/* Port entry point (replaces src/entry/entry.s: bss clear + jump to game_main). Session S2 skeleton: the host hands
 * the player's ROM to port_set_rom() before callMain(); main() copies the main segment into the RDRAM mirror (as
 * the boot code does on the console), then runs game_main(), which today only reaches logged stubs (threads are
 * S3). */
#include "pc_mem.h"

#include <string.h>

void game_main(void);
int port_stub_report(void);
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
    return ok;
}

/* --load-all: copy every segment that has a RAM address from the ROM into the mirror in turn and apply its relocations
 * (a check of port/symbols/relocs.tsv: the loader logs every pointer word that does not resolve). Overlays that
 * share VRAM overwrite each other, as on the console; the main segment is loaded again afterwards. */
static void load_all_test(void) {
    unsigned n = 0;
    for (unsigned i = 0; i < port_segment_count; i++)
        if (port_segments[i].vram_start >= 0x80000000u && !port_load_segment(port_segments[i].name)) n++;
    port_log("[boot] --load-all: %u segments loaded\n", n);
}

int main(int argc, char **argv) {
    port_log("[boot] Pokemon Snap wasm port (S2 skeleton); RDRAM mirror at %p\n", (void *)n64_rdram);
    if (!port_rom_ready()) {
        port_log("[boot] no ROM: call port_set_rom(ptr, size) before main\n");
        return 1;
    }
    if (argc > 1 && !strcmp(argv[1], "--load-all")) load_all_test();
    if (port_load_segment("main")) return 1;
    vbe_selftest();
    game_main();
    port_log("[boot] game_main returned (no threads yet)\n");
    port_stub_report();
    return 0;
}
