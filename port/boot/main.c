/* Port entry point (replaces src/entry/entry.s: bss clear + jump to game_main). Sessions S2-S3a.
 * The host hands the player's ROM to port_set_rom() before callMain(); main() copies the main segment into the RDRAM
 * mirror (as the boot code does on the console), snapshots the overlays' C data, runs game_main() (which creates
 * and starts the idle thread) and then drives the retrace: --frames N runs N retraces here (node test; default 0 =
 * return to the host, which calls the exported port_vi_retrace() once per display frame). */
#include "pc_os.h"

#include <emscripten.h>
#include <stdlib.h>
#include <string.h>

void game_main(void);
int port_stub_report(void);
void port_icall_report(void);
void port_vi_retrace(void);
int game_sprintf(char *dst, const char *fmt, ...) __asm__("vbe.sprintf"); /* ultralib libc/sprintf.c, vbe-compiled */

/* vbe self-test: the game's own sprintf (libultra xprintf, compiled through vbe.py) formats into game memory; the
 * PC side reads the result back through the lane accessors. The format string is put into game memory the same way. */
static int vbe_selftest(void) {
    static const char fmt[] = "snap %d %x %s %c %.2f %lld";
    static const char want[] = "snap -42 beef zubat ! 3.25 -1234567890123";
    unsigned char *f = n64_rdram + 0x7FF000, *s = n64_rdram + 0x7FF100, *o = n64_rdram + 0x7FF200;
    for (unsigned i = 0; i < sizeof(fmt); i++) vbe_st8(f + i, (unsigned char)fmt[i]);
    for (unsigned i = 0; i < 6; i++) vbe_st8(s + i, (unsigned char)"zubat"[i]);
    int n = game_sprintf((char *)o, (const char *)f, -42, 0xBEEF, s, '!', 3.25, -1234567890123LL);
    char got[64];
    unsigned i = 0;
    for (; i < sizeof(got) - 1 && vbe_ld8(o + i); i++) got[i] = (char)vbe_ld8(o + i);
    got[i] = 0;
    int ok = n == (int)strlen(want) && !strcmp(got, want);
    port_log("[boot] vbe self-test (game sprintf): \"%s\" (%d chars) %s\n", got, n, ok ? "OK" : "MISMATCH");
    memset(n64_rdram + 0x7FF000, 0, 0x300);
    return ok;
}

/* --load-all: copy every segment that has a RAM address from the ROM into the mirror in turn and apply its relocations
 * (a check of port/symbols/relocs.tsv: the loader logs every pointer word that does not resolve). Overlays that
 * share VRAM overwrite each other, as on the console; the main segment is loaded again afterwards. */
static void load_all_test(void) {
    unsigned n = 0, bad = 0;
    for (unsigned i = 0; i < port_segment_count; i++)
        if (port_segments[i].vram_start >= 0x80000000u) {
            int r = port_load_segment(port_segments[i].name);
            if (r >= 0) n++;
            if (r > 0) bad++;
        }
    port_log("[boot] --load-all: %u segments loaded, %u with bad relocations\n", n, bad);
}

/* --bench: two PC-side threads ping-pong through two queues: the cost of one fiber switch at a shallow stack */
static N64MesgQueue bq[2];
static OSMesg_ bmsg[2][1];
static N64Thread bthr[2];
static int bench_n;
static void bench_a(void *arg) {
    (void)arg;
    extern int osSendMesg(void *, void *, int);
    extern int osRecvMesg(void *, void *, int);
    for (int i = 0; i < bench_n; i++) { osSendMesg(&bq[0], 0, 1); osRecvMesg(&bq[1], 0, 1); }
}
static void bench_b(void *arg) {
    (void)arg;
    extern int osSendMesg(void *, void *, int);
    extern int osRecvMesg(void *, void *, int);
    for (int i = 0; i < bench_n; i++) { osRecvMesg(&bq[0], 0, 1); osSendMesg(&bq[1], 0, 1); }
}
static void fiber_bench(int n) {
    extern void osCreateMesgQueue(void *, void *, int);
    extern void osCreateThread(void *, int, void *, void *, void *, int);
    extern void osStartThread(void *);
    extern void osDestroyThread(void *);
    extern void port_run_threads(void);
    extern unsigned port_fiber_swaps;
    osCreateMesgQueue(&bq[0], bmsg[0], 1);
    osCreateMesgQueue(&bq[1], bmsg[1], 1);
    bench_n = n;
    osCreateThread(&bthr[0], 900, (void *)bench_a, 0, 0, 10);
    osCreateThread(&bthr[1], 901, (void *)bench_b, 0, 0, 10);
    osStartThread(&bthr[0]);
    osStartThread(&bthr[1]);
    unsigned s0 = port_fiber_swaps;
    double t0 = emscripten_get_now();
    port_run_threads();
    double ms = emscripten_get_now() - t0;
    unsigned sw = port_fiber_swaps - s0;
    port_log("[time] fiber bench: %u switches in %.1f ms = %.2f us per switch (shallow stacks)\n", sw, ms,
             sw ? ms * 1000.0 / sw : 0.0);
}

int main(int argc, char **argv) {
    int frames = 0, bench = 0;
    port_log("[boot] Pokemon Snap wasm port (S3a); RDRAM mirror at %p\n", (void *)n64_rdram);
    if (!port_rom_ready()) {
        port_log("[boot] no ROM: call port_set_rom(ptr, size) before main\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--load-all")) load_all_test();
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bench")) bench = 1;
    }
    port_mem_init();
    if (port_load_segment("main")) return 1;
    vbe_selftest();
    if (bench) fiber_bench(20000);
    game_main();
    port_log("[boot] game_main returned: %d retraces follow\n", frames);
    extern unsigned port_fiber_swaps, port_fiber_peak, port_fiber_live, port_fiber_created;
    extern unsigned port_gfx_tasks, port_audio_tasks, port_vi_swaps, port_pi_dmas, port_pi_bytes, port_ovl_loads;
    extern unsigned port_ovl_resets, port_cont_reads;
    double t0 = emscripten_get_now(), tlast = t0;
    unsigned swaps_last = 0;
    for (int f = 1; f <= frames; f++) {
        port_vi_retrace();
        if (f % 60 == 0 || f == frames) {
            double t = emscripten_get_now();
            port_log("[frame] retrace %d: %u gfx tasks, %u audio tasks, %u swaps of the framebuffer; threads %u live "
                     "(peak %u, %u created), %u fiber switches; %u PI DMAs (%u bytes), %u overlay loads\n", f,
                     port_gfx_tasks, port_audio_tasks, port_vi_swaps, port_fiber_live, port_fiber_peak,
                     port_fiber_created, port_fiber_swaps, port_pi_dmas, port_pi_bytes, port_ovl_loads);
            port_log("[time] retraces %d..%d: %.2f ms per retrace, %u fiber switches per retrace\n", f - 59 > 1 ? f - 59 : 1,
                     f, (t - tlast) / 60.0, (port_fiber_swaps - swaps_last) / 60);
            tlast = t;
            swaps_last = port_fiber_swaps;
        }
    }
    port_log("[boot] done: %d retraces, %u gfx tasks, %u audio tasks, %u controller reads, %u overlay loads (%u C data "
             "resets)\n", frames, port_gfx_tasks, port_audio_tasks, port_cont_reads, port_ovl_loads, port_ovl_resets);
    port_log("[time] %.1f ms for %d retraces\n", emscripten_get_now() - t0, frames);
    port_icall_report();
    port_stub_report();
    return 0;
}
