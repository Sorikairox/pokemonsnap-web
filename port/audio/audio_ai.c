/* Virtual AI (audio interface) of the wasm port, and the PCM the host plays (session S7). Design: docs/port/AUDIO.md.
 *
 * The console's AI plays one DMA buffer of s16 stereo from RDRAM at the DAC rate and holds one more in its FIFO
 * (osAiSetNextBuffer fails with -1 while both slots are taken). Here DAC time advances with the retraces only:
 * every port_vi_retrace consumes rate / 60 stereo frames (an exact integer accumulator), so everything the game
 * sees (AI_LEN_REG, which sys/audio.c reads to size its next audio frame, and the FIFO state) depends on the retrace
 * count, never on wall time. Samples are read from game memory when the DAC reaches them, as the AI's DMA does; an
 * empty FIFO plays silence. The frames played go to the host (rtw_audio_rate / rtw_audio_drain / rtw_audio_buffer,
 * retrowser standard 4.6) and, for tests, to an optional capture buffer (node: port/tools/node_run.js --wav). */
#include "audio.h"
#include "pc_mem.h"

#include <emscripten.h>
#include <stdlib.h>
#include <string.h>

#define VI_NTSC_CLOCK 48681812u
#define RETRACE_HZ 60u

static struct { uint8_t *ptr; uint32_t len, pos; } fifo[2]; /* [0] playing, [1] queued */
static unsigned fifo_n;
static uint32_t dac_hz, dac_acc;
unsigned port_ai_buffers, port_ai_full, port_ai_underruns;
static uint32_t pcm_hash = 2166136261u;
static uint64_t pcm_frames;

/* host side: the frames made since the last rtw_audio_drain (int16 L, R) */
#define HOST_FRAMES 16384
static int16_t host_pcm[2 * HOST_FRAMES];
static uint32_t host_n;
/* test capture (node --wav, browser cfg.audio): every frame played since port_audio_capture(1) */
static int16_t *cap;
static uint32_t cap_n, cap_cap;
static int cap_on;

int port_ai_set_frequency(int hz) {
    if (hz <= 0) return 0;
    uint32_t dacrate = (uint32_t)((double)VI_NTSC_CLOCK / hz + 0.5);
    if (dacrate < 132) return -1; /* AI_MIN_DAC_RATE */
    dac_hz = VI_NTSC_CLOCK / dacrate;
    port_log("[audio] AI rate %u Hz (asked %d)\n", dac_hz, hz);
    return (int)dac_hz;
}

int port_ai_set_next_buffer(void *buf, uint32_t size) {
    if (fifo_n >= 2) { port_ai_full++; return -1; }
    uint32_t a = (uint32_t)(uintptr_t)buf;
    uint8_t *p = a >= 0x80000000u ? port_n64_to_wasm(a) : a < PORT_RDRAM_SIZE ? n64_rdram + a : (uint8_t *)buf;
    fifo[fifo_n].ptr = p;
    fifo[fifo_n].len = size & ~3u;
    fifo[fifo_n].pos = 0;
    fifo_n++;
    port_ai_buffers++;
    return 0;
}

uint32_t port_ai_get_length(void) { return fifo_n ? fifo[0].len - fifo[0].pos : 0; }
uint32_t port_ai_get_status(void) {
    return (fifo_n >= 2 ? 0x80000000u : 0) | (fifo_n ? 0x40000000u : 0); /* AI_STATUS_FIFO_FULL, AI_STATUS_DMA_BUSY */
}

static void emit(int16_t l, int16_t r) {
    pcm_hash = (pcm_hash ^ (uint16_t)l) * 16777619u;
    pcm_hash = (pcm_hash ^ (uint16_t)r) * 16777619u;
    pcm_frames++;
    if (host_n == HOST_FRAMES) { /* the host stopped draining: keep the newest */
        memmove(host_pcm, host_pcm + HOST_FRAMES, sizeof host_pcm / 2);
        host_n = HOST_FRAMES / 2;
    }
    host_pcm[2 * host_n] = l;
    host_pcm[2 * host_n + 1] = r;
    host_n++;
    if (cap_on) {
        if (cap_n == cap_cap) {
            uint32_t nc = cap_cap ? cap_cap * 2 : 1u << 20;
            int16_t *c = realloc(cap, (size_t)nc * 4);
            if (!c) { cap_on = 0; return; }
            cap = c;
            cap_cap = nc;
        }
        cap[2 * cap_n] = l;
        cap[2 * cap_n + 1] = r;
        cap_n++;
    }
}

void port_ai_retrace(void) {
    if (!dac_hz) return;
    dac_acc += dac_hz;
    uint32_t n = dac_acc / RETRACE_HZ;
    dac_acc %= RETRACE_HZ;
    while (n) {
        if (!fifo_n) {
            port_ai_underruns++;
            while (n--) emit(0, 0);
            break;
        }
        uint32_t left = (fifo[0].len - fifo[0].pos) >> 2, take = left < n ? left : n;
        for (uint32_t k = 0; k < take; k++) {
            const uint8_t *s = fifo[0].ptr + fifo[0].pos + 4 * k;
            emit((int16_t)vbe_ld16(s), (int16_t)vbe_ld16(s + 2));
        }
        fifo[0].pos += take * 4;
        n -= take;
        if (fifo[0].pos >= fifo[0].len) {
            fifo[0] = fifo[1];
            fifo_n--;
        }
    }
    if (port_vi_count % 600 == 0)
        port_log("[audio] retrace %u: %llu frames played, PCM hash %08X; %u AI buffers, %u refused (FIFO full), "
                 "%u underruns\n", port_vi_count, (unsigned long long)pcm_frames, pcm_hash, port_ai_buffers,
                 port_ai_full, port_ai_underruns);
}

/* ---------------------------------------------------------------- host exports (retrowser standard 4.6) */
EMSCRIPTEN_KEEPALIVE int rtw_audio_rate(void) { return (int)dac_hz; }
EMSCRIPTEN_KEEPALIVE int rtw_audio_drain(void) { uint32_t n = host_n; host_n = 0; return (int)n; }
EMSCRIPTEN_KEEPALIVE int rtw_audio_buffer(void) { return (int)(uintptr_t)host_pcm; }

/* ---------------------------------------------------------------- test capture */
EMSCRIPTEN_KEEPALIVE void port_audio_capture(int on) { cap_on = on; }
EMSCRIPTEN_KEEPALIVE int port_audio_capture_ptr(void) { return (int)(uintptr_t)cap; }
EMSCRIPTEN_KEEPALIVE int port_audio_capture_frames(void) { return (int)cap_n; }
EMSCRIPTEN_KEEPALIVE unsigned port_audio_pcm_hash(void) { return pcm_hash; }
