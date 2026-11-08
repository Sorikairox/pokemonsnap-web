/* Offline checks of the aspMain HLE (port/audio/audio_hle.c), session S7: command lists written here, run through
 * port_audio_task as the game's tasks are, results checked numerically. Run: port/audio/test/run.sh (node in the
 * pokemonsnap-emsdk image). The ROM is read at run time (ADPCM banks, the microcode's resample table); nothing from
 * it is stored. Each check prints "CHECK name: OK|FAIL (numbers)".
 *   1. adpcm_loopstate  every looped ADPCM wave of both banks decoded through A_LOADADPCM / A_LOADBUFF / A_ADPCM in
 *                       chunks (state saved and restored by the HLE between tasks): the 16 samples before the loop
 *                       point must equal the loop state the encoder stored in the bank (bit-exact)
 *   2. resample_sine    a 440 Hz sine resampled at several pitches 160 samples per task with libaudio's input
 *                       count rule (alResamplePull): output frequency 440 x pitch, SNR > 50 dB (a one-sample slip
 *                       at task boundaries drops it below 30 dB); and a mutant with the input count off by one
 *                       must fail
 *   3. envmixer_ramp    a DC input through A_ENVMIXER: start volume, ramp per 8 samples towards the target, stop at
 *                       the target, state continued across tasks, dry / wet split
 *   4. polef_dc         the reverb low-pass (drvrnew.c coefficients) passes DC with unity gain, monotonic step
 *   5. mix_interleave   A_MIXER gain and saturation, A_INTERLEAVE order */
#include "../audio.h"
#include "pc_mem.h"
#include "pc_os.h"

#include <emscripten.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned char n64_rdram[PORT_RDRAM_SIZE] __attribute__((aligned(8)));
unsigned port_vi_count;
void port_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); }
void *port_n64_to_wasm(uint32_t a) { return n64_rdram + (a & 0x7FFFFF); }

static uint8_t *rom;
static long rom_size;
static int fails;
static void check(const char *name, int ok, const char *fmt, ...) {
    char b[256];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    printf("CHECK %s: %s (%s)\n", name, ok ? "OK" : "FAIL", b);
    fails += !ok;
}

/* physical DRAM layout of the tests */
enum { CMDS = 0x100000, TASK = 0x0F0000, UCDATA = 0x0E0000, STATE = 0x0D0000, LSTATE = 0x0D1000, BOOK = 0x0D2000,
       WAVE = 0x200000, OUT = 0x400000 };
static uint32_t ncmd;
static void cmd(uint32_t w0, uint32_t w1) { vbe_st32(n64_rdram + CMDS + 8 * ncmd, w0); vbe_st32(n64_rdram + CMDS + 8 * ncmd + 4, w1); ncmd++; }
static void run(void) {
    N64Task t;
    memset(&t, 0, sizeof t);
    t.type = M_AUDTASK_;
    t.data_ptr = (void *)(uintptr_t)CMDS;
    t.data_size = ncmd * 8;
    t.ucode_data = (void *)(uintptr_t)UCDATA;
    port_audio_task(&t);
    ncmd = 0;
}
static void st16(uint32_t a, int v) { vbe_st16(n64_rdram + a, (unsigned)(uint16_t)v); }
static int16_t ld16(uint32_t a) { return (int16_t)vbe_ld16(n64_rdram + a); }
#define SETBUFF(f, i, o, c) cmd((8u << 24) | ((f) << 16) | ((i) & 0xFFFF), ((uint32_t)(o) << 16) | ((c) & 0xFFFF))

static uint32_t be32(uint32_t o) { return (uint32_t)rom[o] << 24 | rom[o + 1] << 16 | rom[o + 2] << 8 | rom[o + 3]; }
static int16_t be16(uint32_t o) { return (int16_t)(rom[o] << 8 | rom[o + 1]); }

/* ---------------------------------------------------------------- 1 */
static void adpcm_bank(uint32_t ctl, uint32_t tbl, unsigned *tested, unsigned *ok) {
    uint32_t bank = ctl + be32(ctl + 4);
    int ninst = be16(bank);
    uint32_t seen[512];
    unsigned nseen = 0;
    for (int i = -1; i < ninst; i++) {
        uint32_t io = i < 0 ? be32(bank + 8) : be32(bank + 12 + 4 * i); /* -1: percussion */
        if (!io) continue;
        uint32_t inst = ctl + io;
        int nsnd = be16(inst + 14);
        for (int s = 0; s < nsnd; s++) {
            uint32_t wt = ctl + be32(ctl + be32(inst + 16 + 4 * s) + 8);
            int dup = 0;
            for (unsigned k = 0; k < nseen; k++) dup |= seen[k] == wt;
            if (dup || nseen >= 512) continue;
            seen[nseen++] = wt;
            if (rom[wt + 8] != 0 || !be32(wt + 12)) continue; /* AL_ADPCM_WAVE with a loop */
            uint32_t base = tbl + be32(wt), lo = ctl + be32(wt + 12), bk = ctl + be32(wt + 16);
            uint32_t start = be32(lo), order = be32(bk), npred = be32(bk + 4);
            uint32_t booksz = 2 * order * npred * 8;
            for (uint32_t k = 0; k < booksz / 2; k++) st16(BOOK + 2 * k, be16(bk + 8 + 2 * k));
            uint32_t frames = (start >> 4) + 1, done = 0;
            for (uint32_t k = 0; k < frames * 9 && base + k < (uint32_t)rom_size; k++) vbe_st8(n64_rdram + WAVE + k, rom[base + k]);
            while (done < frames) {
                uint32_t n = frames - done > 64 ? 64 : frames - done, src = WAVE + 9 * done, al = src & 7;
                cmd(11u << 24 | booksz, BOOK);                        /* A_LOADADPCM */
                SETBUFF(0, 0, 0, (n * 9 + al + 8) & ~7u);
                cmd(4u << 24, src - al);                              /* A_LOADBUFF */
                SETBUFF(0, al, 0x600, n * 32);
                cmd(1u << 24 | (done ? 0 : 1) << 16, STATE);          /* A_ADPCM, A_INIT the first time */
                run();
                done += n;
            }
            int same = 1;
            for (int k = 0; k < 16; k++) same &= ld16(STATE + 2 * k) == be16(lo + 12 + 2 * k);
            (*tested)++;
            *ok += same;
        }
    }
}

/* ---------------------------------------------------------------- 2 */
static double resample_snr(uint32_t pitch, int slip, double *freq_out) {
    const int frames = 120, outn = 160;
    for (int i = 0; i < 160000; i++) st16(WAVE + 2 * i, (int)lrint(16000 * sin(2 * M_PI * 440 * i / 32000.0)));
    double ratio = pitch / 32768.0, delta = 0;
    uint32_t pos = 0;
    for (int f = 0; f < frames; f++) {
        double fin = delta + ratio * outn;
        int incount = (int)fin;
        delta = fin - incount;
        uint32_t src = WAVE + 2 * pos, al = src & 7;
        SETBUFF(0, 0x140, 0, (incount * 2 + al + 8) & ~7u);
        cmd(4u << 24, src - al);
        SETBUFF(0, 0x140 + al, 0x800, outn * 2);
        cmd(5u << 24 | (f ? 0 : 1) << 16 | pitch, STATE + 0x100);   /* A_RESAMPLE */
        SETBUFF(0, 0, 0x800, outn * 2);
        cmd(6u << 24, OUT + 2 * outn * f);                          /* A_SAVEBUFF */
        run();
        pos += incount + (slip && f % 2);
    }
    /* least-squares sine at the expected frequency over frames 4.., residual = noise + distortion */
    double fe = 440 * ratio, best = 0, bestf = fe;
    for (double ft = fe * 0.98; ft <= fe * 1.02; ft += fe * 0.0005) { /* the frequency with the largest projection */
        double c = 0, s = 0;
        for (int i = 4 * outn; i < frames * outn; i++) {
            double x = ld16(OUT + 2 * i), w = 2 * M_PI * ft * i / 32000.0;
            c += x * cos(w); s += x * sin(w);
        }
        if (c * c + s * s > best) best = c * c + s * s, bestf = ft;
    }
    double c = 0, s = 0, e = 0, sig = 0;
    int n = (frames - 4) * outn;
    for (int i = 4 * outn; i < frames * outn; i++) {
        double x = ld16(OUT + 2 * i), w = 2 * M_PI * bestf * i / 32000.0;
        c += x * cos(w); s += x * sin(w);
    }
    c *= 2.0 / n; s *= 2.0 / n;
    for (int i = 4 * outn; i < frames * outn; i++) {
        double x = ld16(OUT + 2 * i), w = 2 * M_PI * bestf * i / 32000.0, y = c * cos(w) + s * sin(w);
        e += (x - y) * (x - y); sig += y * y;
    }
    *freq_out = bestf;
    return 10 * log10(sig / (e + 1e-9));
}

int main(int argc, char **argv) {
    FILE *f = argc > 1 ? fopen(argv[1], "rb") : NULL;
    if (!f) { printf("usage: hle_test ROM.z64\n"); return 2; }
    fseek(f, 0, SEEK_END); rom_size = ftell(f); fseek(f, 0, SEEK_SET);
    rom = malloc(rom_size);
    if (fread(rom, 1, rom_size, f) != (size_t)rom_size) return 2;
    fclose(f);
    if (rom[0] != 0x80) { printf("expects the big-endian .z64 (port/docker/run.sh rom)\n"); return 2; }
    /* the microcode's data (aspMainDataStart, ROM 0x457A0 in the US ROM, port/symbols/symbols.tsv) -> UCDATA */
    for (int i = 0; i < 0x2C0; i++) vbe_st8(n64_rdram + UCDATA + i, rom[0x457A0 + i]);

    unsigned tested = 0, ok = 0;
    adpcm_bank(0xAFEEE0, 0xB04430, &tested, &ok); /* audio_bank1 / audio_table1 (sound effects) */
    adpcm_bank(0xBA6C20, 0xBB6940, &tested, &ok); /* audio_bank2 / audio_table2 (music) */
    check("adpcm_loopstate", tested > 50 && ok == tested, "%u of %u looped waves bit-exact", ok, tested);

    static const uint32_t pitches[] = {0x8000, 0x6000, 0xA000, 0x4321, 0xFFFF, 0x2000};
    double worst = 1e9, fo;
    int fok = 1;
    for (unsigned i = 0; i < sizeof pitches / sizeof pitches[0]; i++) {
        double snr = resample_snr(pitches[i], 0, &fo);
        double want = 440 * pitches[i] / 32768.0;
        printf("  pitch 0x%04X: %.2f Hz (want %.2f), SNR %.1f dB\n", pitches[i], fo, want, snr);
        if (snr < worst) worst = snr;
        fok &= fabs(fo - want) < want * 0.001;
    }
    double snr_slip = resample_snr(0x6000, 1, &fo);
    check("resample_sine", worst > 50 && fok && snr_slip < 30, "worst SNR %.1f dB; mutant (input slips 1 sample every 2nd "
          "task) %.1f dB", worst, snr_slip);

    /* 3: envmixer. DC 0x4000 in, start volume 0x1000 -> target 0x4000 at rate 1.25 per 8 samples, dry 0x7FFF wet 0x4000 */
    for (int i = 0; i < 160; i++) st16(0, 0); /* nothing */
    int eok = 1;
    int16_t first = 0, last = 0;
    int mono = 1;
    for (int task = 0; task < 4; task++) {
        /* DMEM: input at 0x000 (160 samples), dry L 0x200, dry R 0x400, wet L 0x600, wet R 0x800 */
        for (int i = 0; i < 160; i++) st16(WAVE + 2 * i, 0x4000);
        SETBUFF(0, 0, 0, 320); cmd(4u << 24, WAVE);
        cmd(2u << 24 | 0x200, 0x800); /* A_CLEARBUFF 0x200..0xA00 */
        if (!task) {
            cmd(9u << 24 | (0x02 | 0x04) << 16 | 0x1000, 0);           /* left volume 0x1000 */
            cmd(9u << 24 | 0x04 << 16 | 0x0800, 0);                    /* right volume 0x0800 */
            cmd(9u << 24 | 0x02 << 16 | 0x4000, 0x00014000);           /* left target 0x4000, rate 1.25 */
            cmd(9u << 24 | 0x00 << 16 | 0x0400, 0x0000C000);           /* right target 0x400, rate 0.75 */
            cmd(9u << 24 | 0x08 << 16 | 0x7FFF, 0x4000);               /* dry, wet */
        }
        SETBUFF(0, 0, 0x200, 320);
        SETBUFF(0x08, 0x400, 0x600, 0x800);
        cmd(3u << 24 | ((task ? 0 : 1) | 0x08) << 16, STATE + 0x200); /* A_ENVMIXER */
        SETBUFF(0, 0, 0x200, 0x800); cmd(6u << 24, OUT + 0x800 * task);
        run();
        for (int i = 0; i < 160; i++) {
            int16_t l = ld16(OUT + 0x800 * task + 2 * i);
            if (!task && !i) first = l;
            if (i && l < ld16(OUT + 0x800 * task + 2 * i - 2) && !(task == 0 && i == 0)) mono = 0;
            last = l;
        }
    }
    int16_t r_end = ld16(OUT + 0x800 * 3 + 0x200 + 2 * 159), wl_end = ld16(OUT + 0x800 * 3 + 0x400 + 2 * 159);
    /* expected: start 0x4000*0x1000>>15 = 0x800, x dry 0x7FFF>>15 -> 2047; end at target 0x4000 -> 8191; right ends at
     * 0x400 -> 0x200 -> 511; wet left at target 0x2000 x 0x4000 >> 15 = 4096 */
    eok = first == 2047 && last == 8191 && mono && r_end == 511 && wl_end == 4096;
    check("envmixer_ramp", eok, "first %d (want 2047), last %d (8191), monotonic %d, right end %d (511), wet left %d (4096)",
          first, last, mono, r_end, wl_end);

    /* 4: pole filter, fc 0.5 (0x2000) as drvrnew.c builds it: fccoef[8 + j] = fc^(j+1) Q14, gain 1 - fc */
    for (int j = 0; j < 8; j++) st16(BOOK + 2 * j, 0), st16(BOOK + 16 + 2 * j, (int)(16384 * pow(0.5, j + 1)));
    int pmono = 1;
    int16_t p_end = 0;
    for (int task = 0; task < 3; task++) {
        for (int i = 0; i < 160; i++) st16(WAVE + 2 * i, 10000);
        SETBUFF(0, 0x100, 0, 320); cmd(4u << 24, WAVE);
        SETBUFF(0, 0x100, 0x100, 320);
        cmd(11u << 24 | 32, BOOK);
        cmd(14u << 24 | (task ? 0 : 1) << 16 | 0x2000, STATE + 0x300);
        SETBUFF(0, 0, 0x100, 320); cmd(6u << 24, OUT + 0x400 * task);
        run();
        for (int i = 1; i < 160; i++) pmono &= ld16(OUT + 0x400 * task + 2 * i) >= ld16(OUT + 0x400 * task + 2 * i - 2);
        p_end = ld16(OUT + 0x400 * task + 2 * 159);
    }
    int16_t p0 = ld16(OUT), p1 = ld16(OUT + 2);
    check("polef_dc", pmono && abs(p_end - 10000) <= 4 && p0 == 5000 && p1 == 7500,
          "y0 %d (5000), y1 %d (7500), settles at %d (10000), monotonic %d", p0, p1, p_end, pmono);

    /* 5: mixer and interleave */
    for (int i = 0; i < 16; i++) st16(WAVE + 2 * i, 20000), st16(WAVE + 32 + 2 * i, -1000 * i);
    SETBUFF(0, 0x000, 0, 64); cmd(4u << 24, WAVE);           /* 0x000: 20000 x16, 0x020: -1000 i */
    SETBUFF(0, 0, 0, 32);
    cmd(12u << 24 | 0x4000, 0x0000 << 16 | 0x0020);          /* 0x020 += 0x000 * 0.5 */
    cmd(12u << 24 | 0x7FFF, 0x0000 << 16 | 0x0000);          /* 0x000 += 0x000 * ~1: saturates */
    SETBUFF(0, 0, 0x100, 32);
    cmd(13u << 24, 0x0000 << 16 | 0x0020);                   /* interleave L = 0x000, R = 0x020 */
    SETBUFF(0, 0, 0x100, 64); cmd(6u << 24, OUT);
    run();
    int mok = ld16(OUT) == 32767 && ld16(OUT + 2) == 10000 && ld16(OUT + 4 * 15 + 2) == 10000 - 15000 && ld16(OUT + 4 * 15) == 32767;
    check("mix_interleave", mok, "L0 %d R0 %d L15 %d R15 %d", ld16(OUT), ld16(OUT + 2), ld16(OUT + 60), ld16(OUT + 62));

    printf("%s: %d failed\n", fails ? "FAIL" : "ALL OK", fails);
    return fails != 0;
}
