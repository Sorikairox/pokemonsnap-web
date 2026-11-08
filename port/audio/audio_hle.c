/* aspMain audio microcode, high-level emulation on the CPU (session S7). Design: docs/port/AUDIO.md.
 *
 * Written for this port from the Audio Binary Interface (ultralib include/PR/abi.h: commands, fields, flags) and the
 * way the game's libaudio (ultralib src/audio: load.c, resample.c, env.c, reverb.c, mainbus.c, auxbus.c, save.c)
 * builds its command lists and expects DMEM to look afterwards. No other HLE implementation was used (in particular
 * none of the GPL ones). Where the ABI leaves the arithmetic open, the choice is written next to the code; the ADPCM
 * decoder is checked bit-exact against the loop states the encoder stored in the game's banks (AUDIO.md "Checks").
 *
 * Memory: DMEM is a private array in the same "virtual big-endian" layout as game memory (port/ultra/pc_mem.h: word
 * native, halfword at a ^ 2, byte at a ^ 3), so the RSP DMAs (LOADBUFF / SAVEBUFF, 8-byte aligned like the SP DMA
 * engine) are plain word copies. DRAM addresses in commands are what the game's osVirtualToPhysical / K0_TO_PHYS
 * give: physical (< 8 MiB, the RDRAM mirror), wasm addresses of C objects or of the mirror (>= 0x10000000, which the
 * 0x1FFFFFFF mask leaves unchanged), or KSEG0. The state structures (ADPCM_STATE, RESAMPLE_STATE, ENVMIX_STATE,
 * POLEF_STATE) are private to the microcode; this HLE keeps its own layout inside their sizes, except ADPCM states,
 * which the game also fills itself (a wave's loop state from the bank: the 16 samples of the frame before the loop
 * point). All arithmetic is integer: identical output on every host. */
#include "audio.h"
#include "pc_mem.h"
#include "pc_os.h"

#include <emscripten.h>
#include <emscripten/heap.h>
#include <string.h>

enum {
    A_SPNOOP = 0, A_ADPCM = 1, A_CLEARBUFF = 2, A_ENVMIXER = 3, A_LOADBUFF = 4, A_RESAMPLE = 5, A_SAVEBUFF = 6,
    A_SEGMENT = 7, A_SETBUFF = 8, A_SETVOL = 9, A_DMEMMOVE = 10, A_LOADADPCM = 11, A_MIXER = 12, A_INTERLEAVE = 13,
    A_POLEF = 14, A_SETLOOP = 15
};
enum { F_INIT = 0x01, F_LOOP = 0x02, F_LEFT = 0x02, F_VOL = 0x04, F_AUX = 0x08 };

#define DMEM_SIZE 0x1000u
/* DMEM plus slack so that a rounded-up copy at the top never leaves the array (addresses wrap at 4 KiB) */
static uint32_t dmem_words[(DMEM_SIZE + 0x1000u) / 4];
#define DMEM ((uint8_t *)dmem_words)

/* RSP state the commands set */
static struct {
    uint32_t in, out, count;               /* A_SETBUFF (main) */
    uint32_t dry_right, wet_left, wet_right; /* A_SETBUFF A_AUX */
    int16_t vol[2], target[2];             /* A_SETVOL: [0] right, [1] left (the A_LEFT flag) */
    int32_t rate[2];
    int16_t dry, wet;
    uint32_t loop;                         /* A_SETLOOP */
    uint32_t segment[16];
    int16_t table[256];                    /* A_LOADADPCM: ADPCM codebook / pole-filter coefficients */
} rsp;

unsigned port_audio_cmds, port_audio_bad;
static double audio_ms, audio_ms_max;
static unsigned audio_tasks, audio_unknown_logged;

/* ---------------------------------------------------------------- memory */
static inline int16_t rd16(uint32_t a) { return (int16_t)*(uint16_t *)(DMEM + ((a & (DMEM_SIZE - 1)) ^ 2)); }
static inline void wr16(uint32_t a, int v) { *(uint16_t *)(DMEM + ((a & (DMEM_SIZE - 1)) ^ 2)) = (uint16_t)v; }
static inline unsigned rd8(uint32_t a) { return DMEM[(a & (DMEM_SIZE - 1)) ^ 3]; }
static inline int16_t sat16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }

/* DRAM address of a command -> wasm pointer, n bytes valid; NULL (logged) outside memory */
static uint8_t *dram(uint32_t a, uint32_t n) {
    uint32_t heap = (uint32_t)emscripten_get_heap_size();
    if (a < 0x10000000u) {
        uint32_t base = rsp.segment[(a >> 24) & 0xF];
        a = base + (a & 0x00FFFFFFu);
    }
    if (a >= 0x10000000u && a < 0x80000000u) {
        if (a + n <= heap) return (uint8_t *)(uintptr_t)a;
    } else {
        a &= 0x1FFFFFFFu; /* KSEG0 / KSEG1 / physical */
        if (a + n <= PORT_RDRAM_SIZE) return n64_rdram + a;
    }
    if (port_audio_bad++ < 8) port_log("[audio] bad DRAM address 0x%08X (+%u)\n", a, n);
    return NULL;
}
static inline int16_t dram_rd16(const uint8_t *p, unsigned i) { return (int16_t)vbe_ld16(p + 2 * i); }
static inline void dram_wr16(uint8_t *p, unsigned i, int v) { vbe_st16(p + 2 * i, (unsigned)(uint16_t)v); }

/* SP DMA: both addresses rounded down to 8 bytes, the length up to 8 bytes, as the DMA engine does */
static void dma_load(uint32_t dmem, uint32_t addr, uint32_t len) {
    len = (len + 7) & ~7u;
    if (!len) return;
    uint8_t *p = dram(addr & ~7u, len);
    if (!p) return;
    dmem &= (DMEM_SIZE - 1) & ~7u;
    memcpy(DMEM + dmem, p, len > DMEM_SIZE ? DMEM_SIZE : len);
}
static void dma_save(uint32_t dmem, uint32_t addr, uint32_t len) {
    len = (len + 7) & ~7u;
    if (!len) return;
    uint8_t *p = dram(addr & ~7u, len);
    if (!p) return;
    dmem &= (DMEM_SIZE - 1) & ~7u;
    memcpy(p, DMEM + dmem, len > DMEM_SIZE ? DMEM_SIZE : len);
}

/* ---------------------------------------------------------------- A_ADPCM
 * in: 9-byte frames (header: scale << 4 | predictor; 16 4-bit residuals), out: count bytes of samples decoded after
 * the 16 samples of the previous frame, which are written first (libaudio's alAdpcmPull starts reading at
 * out + lastsam * 2: the not yet consumed samples of that frame). The previous frame comes from the state, from the
 * loop state (A_LOOP: a wave's loop point) or is silence (A_INIT). Prediction per 8 samples with the codebook of the
 * predictor (2 rows of 8 coefficients, Q11): row 0 weights the second-to-last sample, row 1 the last sample and,
 * shifted, the residuals already seen in this half frame. */
static void cmd_adpcm(uint32_t flags, uint32_t state_addr) {
    int16_t prev[16];
    uint8_t *st = dram(state_addr, 32);
    if (!st) return;
    if (flags & F_INIT) memset(prev, 0, sizeof prev);
    else {
        uint8_t *src = (flags & F_LOOP) ? dram(rsp.loop, 32) : st;
        if (!src) return;
        for (int i = 0; i < 16; i++) prev[i] = dram_rd16(src, i);
    }
    uint32_t out = rsp.out, in = rsp.in;
    for (int i = 0; i < 16; i++) wr16(out + 2 * i, prev[i]);
    out += 32;
    int32_t s2 = prev[14], s1 = prev[15];
    unsigned frames = (rsp.count + 31) >> 5;
    for (unsigned f = 0; f < frames; f++) {
        unsigned hdr = rd8(in++), scale = hdr >> 4, pred = hdr & 0xF;
        const int16_t *c0 = &rsp.table[pred * 16], *c1 = c0 + 8;
        int32_t r[16];
        for (int i = 0; i < 8; i++) {
            unsigned b = rd8(in++);
            r[2 * i] = ((int32_t)((b >> 4) ^ 8) - 8) * (1 << scale);
            r[2 * i + 1] = ((int32_t)((b & 15) ^ 8) - 8) * (1 << scale);
        }
        for (int h = 0; h < 2; h++) {
            const int32_t *x = r + 8 * h;
            int16_t y[8];
            for (int j = 0; j < 8; j++) {
                int32_t acc = c0[j] * s2 + c1[j] * s1 + x[j] * 2048;
                for (int k = 0; k < j; k++) acc += c1[j - 1 - k] * x[k];
                y[j] = sat16(acc >> 11);
            }
            for (int j = 0; j < 8; j++) wr16(out + 2 * j, y[j]), prev[8 * h + j] = y[j];
            out += 16;
            s2 = y[6];
            s1 = y[7];
        }
    }
    for (int i = 0; i < 16; i++) dram_wr16(st, i, prev[i]);
}

/* ---------------------------------------------------------------- A_RESAMPLE
 * Pitch: 1.15 fixed (0x8000 = unity, libaudio's UNITY_PITCH, at most ~2). The output sample i is a 4-tap filter over
 * the window [4 samples of history, then the input] at position frac + i * pitch, between taps 1 and 2; the filter's
 * 64 phases (Q15) are the microcode's own table, read from the task's ucode_data (offset 0xC0, 64 x 4 halfwords)
 * at run time (from the player's ROM, never stored in the repo). Afterwards the state holds the 4 window samples at
 * the integer position reached and its fraction: the next call's input continues exactly after them, which is how
 * alResamplePull counts input samples (delta + ratio * outCount). State layout (this HLE): [0..3] history,
 * [4] fraction (u16). */
static const uint8_t *resample_table; /* ucode_data + 0xC0 (game memory, vbe layout) */
#define RS_MAX 4096
static int16_t rs_win[4 + 2 * RS_MAX + 8];

static void cmd_resample(uint32_t flags, uint32_t pitch, uint32_t state_addr) {
    uint8_t *st = dram(state_addr, 32);
    if (!st || !resample_table) return;
    unsigned n = rsp.count >> 1;
    if (n > RS_MAX) n = RS_MAX;
    uint32_t inc = pitch << 1; /* Q16 */
    uint32_t pos = 0;
    if (flags & F_INIT) memset(rs_win, 0, 4 * sizeof rs_win[0]);
    else {
        for (int i = 0; i < 4; i++) rs_win[i] = dram_rd16(st, i);
        pos = (uint16_t)dram_rd16(st, 4);
    }
    uint32_t last = (uint32_t)(((uint64_t)pos + (uint64_t)inc * n) >> 16); /* window index reached */
    uint32_t need = last + 4;
    if (need > 4 + 2 * RS_MAX + 4) need = 4 + 2 * RS_MAX + 4;
    for (uint32_t k = 4; k < need; k++) rs_win[k] = rd16(rsp.in + 2 * (k - 4));
    for (unsigned i = 0; i < n; i++) {
        uint32_t idx = pos >> 16;
        const uint8_t *c = resample_table + 8 * ((pos >> 10) & 63);
        if (idx + 3 >= need) idx = need - 4;
        int32_t acc = (int16_t)vbe_ld16(c) * rs_win[idx] + (int16_t)vbe_ld16(c + 2) * rs_win[idx + 1] +
                      (int16_t)vbe_ld16(c + 4) * rs_win[idx + 2] + (int16_t)vbe_ld16(c + 6) * rs_win[idx + 3];
        wr16(rsp.out + 2 * i, sat16((acc + 0x4000) >> 15));
        pos += inc;
    }
    uint32_t idx = pos >> 16;
    if (idx + 3 >= need) idx = need - 4;
    for (int i = 0; i < 4; i++) dram_wr16(st, i, rs_win[idx + i]);
    dram_wr16(st, 4, (int)(pos & 0xFFFF));
}

/* ---------------------------------------------------------------- A_ENVMIXER
 * One voice into the main (dry) and aux (wet) buses: each input sample scaled by the left / right volume, then by
 * the dry and wet amounts (Q15), added with saturation to dry L (out), dry R, wet L, wet R (A_SETBUFF A_AUX). The
 * volumes ramp towards their targets: the rate is a 16.16 factor applied once per 8 samples (env.c: "rate values
 * are actually rate^8"), stopping at the target. Volumes are kept as 16.16 so that a ramp from 1 can grow. A_INIT
 * takes volumes, targets, rates, dry / wet from A_SETVOL; A_CONTINUE from the state. State layout (this HLE, 40
 * halfwords available): volume L, R (32-bit each), target L, R, rate L, R (32-bit), dry, wet. */
static void st32(uint8_t *p, unsigned i, uint32_t v) { dram_wr16(p, i, (int)(v >> 16)); dram_wr16(p, i + 1, (int)(v & 0xFFFF)); }
static uint32_t ld32(const uint8_t *p, unsigned i) { return ((uint32_t)(uint16_t)dram_rd16(p, i) << 16) | (uint16_t)dram_rd16(p, i + 1); }

static int64_t ramp(int64_t v, int32_t rate, int16_t target) {
    int64_t t = (int64_t)target << 16;
    v = (v * (int64_t)(uint32_t)rate) >> 16;
    if ((uint32_t)rate >= 0x10000u) { if (v > t) v = t; }
    else if (v < t) v = t;
    return v;
}

static void cmd_envmixer(uint32_t flags, uint32_t state_addr) {
    uint8_t *st = dram(state_addr, 80);
    if (!st) return;
    int64_t vl, vr;
    int16_t tl, tr, dry, wet;
    int32_t rl, rr;
    if (flags & F_INIT) {
        vl = (int64_t)rsp.vol[1] << 16; vr = (int64_t)rsp.vol[0] << 16;
        tl = rsp.target[1]; tr = rsp.target[0];
        rl = rsp.rate[1]; rr = rsp.rate[0];
        dry = rsp.dry; wet = rsp.wet;
    } else {
        vl = (int32_t)ld32(st, 0); vr = (int32_t)ld32(st, 2);
        tl = dram_rd16(st, 4); tr = dram_rd16(st, 5);
        rl = (int32_t)ld32(st, 6); rr = (int32_t)ld32(st, 8);
        dry = dram_rd16(st, 10); wet = dram_rd16(st, 11);
    }
    unsigned n = rsp.count >> 1;
    uint32_t in = rsp.in, dl = rsp.out, dr = rsp.dry_right, wl = rsp.wet_left, wr = rsp.wet_right;
    int aux = (flags & F_AUX) != 0;
    for (unsigned i = 0; i < n; i += 8) {
        int32_t gl = (int32_t)(vl >> 16), gr = (int32_t)(vr >> 16);
        for (unsigned j = i; j < i + 8 && j < n; j++) {
            int32_t s = rd16(in + 2 * j);
            int32_t l = (s * gl) >> 15, r = (s * gr) >> 15;
            uint32_t o = 2 * j;
            wr16(dl + o, sat16(rd16(dl + o) + ((l * dry) >> 15)));
            wr16(dr + o, sat16(rd16(dr + o) + ((r * dry) >> 15)));
            if (aux) {
                wr16(wl + o, sat16(rd16(wl + o) + ((l * wet) >> 15)));
                wr16(wr + o, sat16(rd16(wr + o) + ((r * wet) >> 15)));
            }
        }
        vl = ramp(vl, rl, tl);
        vr = ramp(vr, rr, tr);
    }
    st32(st, 0, (uint32_t)vl); st32(st, 2, (uint32_t)vr);
    dram_wr16(st, 4, tl); dram_wr16(st, 5, tr);
    st32(st, 6, (uint32_t)rl); st32(st, 8, (uint32_t)rr);
    dram_wr16(st, 10, dry); dram_wr16(st, 11, wet);
}

/* ---------------------------------------------------------------- A_POLEF
 * The reverb's low-pass (reverb.c _filterBuffer, coefficients from drvrnew.c _init_lpfilter, loaded by
 * A_LOADADPCM): a one-pole filter computed 8 samples at a time like the ADPCM predictor, in Q14 (libaudio's SCALE
 * 16384): row 0 (zeros) weights y[-2], row 1 = fc^(j+1) weights y[-1] and, shifted, the inputs; each input is first
 * scaled by the gain (fgain = 1 - fc, unity at DC). State (this HLE): y[-2], y[-1]. */
static void cmd_polef(uint32_t flags, int16_t gain, uint32_t state_addr) {
    uint8_t *st = dram(state_addr, 8);
    if (!st) return;
    int32_t y2 = 0, y1 = 0;
    if (!(flags & F_INIT)) { y2 = dram_rd16(st, 0); y1 = dram_rd16(st, 1); }
    const int16_t *h0 = rsp.table, *h1 = rsp.table + 8;
    unsigned n = rsp.count >> 1;
    for (unsigned i = 0; i < n; i += 8) {
        int32_t x[8];
        int16_t y[8];
        for (int j = 0; j < 8; j++) x[j] = (rd16(rsp.in + 2 * (i + j)) * gain) >> 14;
        for (int j = 0; j < 8; j++) {
            int32_t acc = h0[j] * y2 + h1[j] * y1 + x[j] * 16384;
            for (int k = 0; k < j; k++) acc += h1[j - 1 - k] * x[k];
            y[j] = sat16(acc >> 14);
        }
        for (int j = 0; j < 8; j++) wr16(rsp.out + 2 * (i + j), y[j]);
        y2 = y[6];
        y1 = y[7];
    }
    dram_wr16(st, 0, y2);
    dram_wr16(st, 1, y1);
    dram_wr16(st, 2, 0);
    dram_wr16(st, 3, 0);
}

/* ---------------------------------------------------------------- the task */
void port_audio_task(void *task) {
    double t0 = emscripten_get_now();
    const N64Task *t = task;
    uint32_t data = (uint32_t)(uintptr_t)t->data_ptr, size = t->data_size;
    uint32_t ucode_data = (uint32_t)(uintptr_t)t->ucode_data;
    const uint32_t *cmd = (const uint32_t *)dram(data, size);
    resample_table = dram(ucode_data + 0xC0, 0x200);
    memset(rsp.segment, 0, sizeof rsp.segment);
    if (!cmd) return;
    for (uint32_t i = 0; i + 1 < size / 4; i += 2) {
        uint32_t w0 = cmd[i], w1 = cmd[i + 1], op = w0 >> 24, flags = (w0 >> 16) & 0xFF;
        port_audio_cmds++;
        switch (op) {
        case A_SPNOOP: break;
        case A_ADPCM: cmd_adpcm(flags, w1); break;
        case A_CLEARBUFF: {
            uint32_t d = w0 & 0xFFFF, n = w1 & 0xFFFF;
            for (uint32_t k = 0; k < n; k++) DMEM[((d + k) & (DMEM_SIZE - 1)) ^ 3] = 0;
            break;
        }
        case A_ENVMIXER: cmd_envmixer(flags, w1); break;
        case A_LOADBUFF: dma_load(rsp.in, w1, rsp.count); break;
        case A_RESAMPLE: cmd_resample(flags, w0 & 0xFFFF, w1); break;
        case A_SAVEBUFF: dma_save(rsp.out, w1, rsp.count); break;
        case A_SEGMENT: rsp.segment[(w1 >> 24) & 0xF] = w1 & 0x00FFFFFFu; break;
        case A_SETBUFF:
            if (flags & F_AUX) { rsp.dry_right = w0 & 0xFFFF; rsp.wet_left = w1 >> 16; rsp.wet_right = w1 & 0xFFFF; }
            else { rsp.in = w0 & 0xFFFF; rsp.out = w1 >> 16; rsp.count = w1 & 0xFFFF; }
            break;
        case A_SETVOL: {
            int side = (flags & F_LEFT) ? 1 : 0;
            int16_t v = (int16_t)(w0 & 0xFFFF);
            if (flags & F_AUX) { rsp.dry = v; rsp.wet = (int16_t)(w1 & 0xFFFF); }
            else if (flags & F_VOL) rsp.vol[side] = v;
            else { rsp.target[side] = v; rsp.rate[side] = (int32_t)w1; }
            break;
        }
        case A_DMEMMOVE: {
            uint32_t s = w0 & 0xFFFF, d = w1 >> 16, n = w1 & 0xFFFF;
            for (uint32_t k = 0; k < n; k++)
                DMEM[((d + k) & (DMEM_SIZE - 1)) ^ 3] = DMEM[((s + k) & (DMEM_SIZE - 1)) ^ 3];
            break;
        }
        case A_LOADADPCM: {
            uint32_t n = (w0 & 0xFFFFFF) >> 1;
            if (n > 256) n = 256;
            uint8_t *p = dram(w1, n * 2);
            if (p) for (uint32_t k = 0; k < n; k++) rsp.table[k] = dram_rd16(p, k);
            break;
        }
        case A_MIXER: {
            int32_t g = (int16_t)(w0 & 0xFFFF);
            uint32_t s = w1 >> 16, d = w1 & 0xFFFF, n = rsp.count >> 1;
            for (uint32_t k = 0; k < n; k++) wr16(d + 2 * k, sat16(rd16(d + 2 * k) + ((rd16(s + 2 * k) * g) >> 15)));
            break;
        }
        case A_INTERLEAVE: {
            static int16_t lr[2][RS_MAX];
            uint32_t l = w1 >> 16, r = w1 & 0xFFFF, n = rsp.count >> 1;
            if (n > RS_MAX) n = RS_MAX;
            for (uint32_t k = 0; k < n; k++) lr[0][k] = rd16(l + 2 * k), lr[1][k] = rd16(r + 2 * k);
            for (uint32_t k = 0; k < n; k++) wr16(rsp.out + 4 * k, lr[0][k]), wr16(rsp.out + 4 * k + 2, lr[1][k]);
            break;
        }
        case A_POLEF: cmd_polef(flags, (int16_t)(w0 & 0xFFFF), w1); break;
        case A_SETLOOP: rsp.loop = w1; break;
        default:
            if (audio_unknown_logged++ < 8) port_log("[audio] unknown command %08X %08X\n", w0, w1);
        }
    }
    double ms = emscripten_get_now() - t0;
    audio_ms += ms;
    if (ms > audio_ms_max) audio_ms_max = ms;
    if (++audio_tasks % 600 == 0) {
        port_log("[time] audio: %u tasks, %.3f ms per task (max %.3f) over the last 600\n", audio_tasks,
                 audio_ms / 600, audio_ms_max);
        audio_ms = audio_ms_max = 0;
    }
}
