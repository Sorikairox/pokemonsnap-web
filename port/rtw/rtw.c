/* retrowser module (session S9; docs/port/RTW.md): the rtw_* exports of the standard, linked only into the retrowser
 * target (port/build/rtw, `port_build.py --rtw`). The test harnesses (port/web, node) use the same game build through
 * their own exports and are not affected by this file.
 *
 * Start-up (standard 4.2): rtw_rom_buffer -> the host copies the .z64 there -> the stored FlashRAM image is copied to
 * rtw_save_ptr() (save.c; the image is the flash chip's array, so it is in place before main) -> rtw_save_load ->
 * rtw_resize -> rtw_start: SHA-1 check, port_set_rom, the harness's own main() with no arguments (mirror, main
 * segment, game_main; returns at once), then the loop.
 *
 * Loop: self-running (no rtw_frame). One call of port_vi_retrace() is one NTSC field (60 Hz). The host calls
 * rtw_input once per display frame; the port-0 call is the frame clock (guide section 3, trap 13): rtw_glue.js starts
 * a due retrace in a microtask after that call (never inside the export) and keeps one timer only to catch up.
 *
 * Input: port 0 is the player's controller (the game has one: pc_pi.c answers "no controller" on ports 2-4).
 * Buttons are OSContPad bits (active-high, as the host sends them); the stick is scaled from the host's -127..127
 * to an N64 stick's +-80. */
#include "pc_os.h"
#include "../save/save.h"

#include <emscripten.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* host imports (env.rtw_*, port/rtw/library_rtw.js -> Module.rtw) */
extern int rtw_log(const char *p, int len);
extern int rtw_build_id(unsigned char *p);
extern int rtw_rumble(int port, int on);
extern int rtw_option_changed(int id, int value);
extern int rtw_pointer_lock(int on);
/* glue (library_rtw.js / rtw_glue.js) */
extern void port_rtw_host_frame(void);
extern void port_rtw_set_paused(int on);
extern void port_rtw_started(void);
extern void port_rtw_refuse(const char *msg);
extern void port_rtw_resize(int w, int h, int dpr);
extern unsigned char *port_rtw_romfill_table(int *len);

int main(int argc, char **argv);
int port_vbe_fix_run(void);
/* the vbe constructors (__vbe_fix, pc_mem.c) wait for the template fill: they convert the very bytes it restores */
int port_vbe_fix_defer = 1;
/* pc_rcp.c's per-task census and [task] log lines: off in play (cost, and a flood of host log messages) */
int port_task_log = 0;
EMSCRIPTEN_KEEPALIVE void port_rtw_test_log(int on) { port_task_log = on; } /* port/rtw/node_host.mjs */
void port_vi_retrace(void);
void port_set_pad(int i, int buttons, int x, int y);
int port_save_poll(void);

#define SNAP_US_SHA1 "edc7c49cc568c045fe48be0d18011c30f393cbaf"

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof buf) n = sizeof buf - 1;
    rtw_log(buf, n);
}

/* ---------------------------------------------------------------- SHA-1 (FIPS 180-4), for the ROM check */
static uint32_t rol(uint32_t x, int n) { return x << n | x >> (32 - n); }
static void sha1(const unsigned char *data, uint32_t len, unsigned char out[20]) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    uint64_t bits = (uint64_t)len * 8;
    uint32_t nblk = (len + 9 + 63) / 64;
    for (uint32_t b = 0; b < nblk; b++) {
        unsigned char blk[64];
        for (int i = 0; i < 64; i++) {
            uint64_t at = (uint64_t)b * 64 + i;
            blk[i] = at < len ? data[at] : at == len ? 0x80 : 0;
        }
        if (b == nblk - 1)
            for (int i = 0; i < 8; i++) blk[56 + i] = (unsigned char)(bits >> (56 - 8 * i));
        uint32_t w[80];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)blk[4 * i] << 24 | blk[4 * i + 1] << 16 | blk[4 * i + 2] << 8 | blk[4 * i + 3];
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (bb & c) | (~bb & d); k = 0x5A827999u; }
            else if (i < 40) { f = bb ^ c ^ d; k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (bb & c) | (bb & d) | (c & d); k = 0x8F1BBCDCu; }
            else { f = bb ^ c ^ d; k = 0xCA62C1D6u; }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(bb, 30); bb = a; a = t;
        }
        h[0] += a; h[1] += bb; h[2] += c; h[3] += d; h[4] += e;
    }
    for (int i = 0; i < 20; i++) out[i] = (unsigned char)(h[i / 4] >> (24 - 8 * (i % 4)));
}

/* ---------------------------------------------------------------- the template fill (port/rtw/romfill.py) */
/* The shipped wasm is a template: the runs of ROM bytes the decomp's C data compiled into it are zeroed, and
 * files.data pokemonsnap.romfill lists them (address, length, offset in the ROM image of one byte order, order).
 * They are copied back from the player's ROM (z64 order, as the host gives it) before anything reads them, and the
 * FNV-1a hash of everything filled must equal the build's. */
static int romfill(const unsigned char *rom, uint32_t len) {
    int tlen = 0;
    unsigned char *t = port_rtw_romfill_table(&tlen);
    if (!t || tlen < 16 || memcmp(t, "SNRF", 4)) { free(t); return -1; }
    uint32_t hdr[4], n, want, h = 2166136261u, bytes = 0;
    memcpy(hdr, t, 16);
    n = hdr[2];
    want = hdr[3];
    if (hdr[1] != 1 || (uint32_t)tlen != 16 + 16 * n) { free(t); return -1; }
    for (uint32_t k = 0; k < n; k++) {
        uint32_t r[4];
        memcpy(r, t + 16 + 16 * k, 16);
        unsigned char *dst = (unsigned char *)(uintptr_t)r[0];
        uint32_t xr = r[3] == 1 ? 3 : r[3] == 2 ? 1 : 0;
        if (r[3] > 2 || r[2] + r[1] > len || r[0] < 0x10000000u) { free(t); return -1; }
        for (uint32_t i = 0; i < r[1]; i++) {
            unsigned char b = rom[(r[2] + i) ^ xr];
            dst[i] = b;
            h = (h ^ b) * 16777619u;
        }
        bytes += r[1];
    }
    free(t);
    logf_("rtw: template filled from the ROM: %u runs, %u bytes, hash %08x (%s)", n, bytes, h, h == want ? "OK" : "MISMATCH");
    return h == want ? 0 : -1;
}

/* ---------------------------------------------------------------- start */
static unsigned char *rom_buf;
static uint32_t rom_len;
static int started, save_from_host;

EMSCRIPTEN_KEEPALIVE int rtw_rom_buffer(int size) {
    if (size <= 0 || size > (64 << 20)) return 0;
    free(rom_buf);
    rom_buf = malloc((size_t)size);
    rom_len = rom_buf ? (uint32_t)size : 0;
    return (int)(uintptr_t)rom_buf;
}

/* the stored save is already at rtw_save_ptr() (the flash array itself): nothing to copy, only noted */
EMSCRIPTEN_KEEPALIVE int rtw_save_load(int ptr, int size) {
    save_from_host = size;
    port_log("[save] flash image from the host (%d bytes at %s)\n", size,
             (unsigned char *)(uintptr_t)ptr == port_flash_data() ? "the flash array" : "another address");
    return 0;
}

EMSCRIPTEN_KEEPALIVE int rtw_start(void) {
    if (started || !rom_buf) return 0;
    unsigned char id[32], dig[20];
    char hex[65], sh[41];
    rtw_build_id(id);
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", id[i]);
    sha1(rom_buf, rom_len, dig);
    for (int i = 0; i < 20; i++) snprintf(sh + 2 * i, 3, "%02x", dig[i]);
    logf_("rtw: Pokemon Snap (US) port, build id %s; ROM %u bytes, SHA-1 %s", hex, rom_len, sh);
    if (strcmp(sh, SNAP_US_SHA1)) {
        logf_("rtw: wrong ROM (expected SHA-1 " SNAP_US_SHA1 ")");
        port_rtw_refuse("This build needs the US release of Pokemon Snap (N64). The ROM given has a different "
                        "SHA-1, so the game cannot start.");
        return 0;
    }
    if (romfill(rom_buf, rom_len) != 0) {
        port_rtw_refuse("The game data could not be rebuilt from the ROM (pokemonsnap.romfill does not match this "
                        "build).");
        return 0;
    }
    int nfix = port_vbe_fix_run();
    logf_("rtw: %d deferred vbe conversions run after the fill", nfix);
    if (nfix < 0) {
        port_rtw_refuse("Internal error: the data conversions ran before the fill (queue full).");
        return 0;
    }
    if (port_set_rom(rom_buf, rom_len) != 0) {
        port_rtw_refuse("The ROM could not be read.");
        return 0;
    }
    free(rom_buf); /* port_set_rom keeps its own copy (vbe layout) */
    rom_buf = NULL;
    static char arg0[] = "pokemonsnap";
    char *argv[] = {arg0, NULL};
    int rc = main(1, argv);
    if (rc) {
        logf_("rtw: boot failed (%d)", rc);
        port_rtw_refuse("The game failed to boot (see the log).");
        return 0;
    }
    started = 1;
    logf_("rtw: game started (%s)", save_from_host ? "save from the host" : "no stored save");
    port_rtw_started();
    return 0;
}

/* ---------------------------------------------------------------- input, pause, view */
static int pad_buttons, pad_x, pad_y, pad_logged;
/* F4: a Shutter press (bit 17) seen by any rtw_input since the last retrace reaches that retrace, so a tap shorter
 * than a field is not lost between two retraces */
static int pad_shutter_seen;

static int stick(int v) {
    if (v < -127) v = -127;
    if (v > 127) v = 127;
    return v * 80 / 127;
}

EMSCRIPTEN_KEEPALIVE int rtw_input(int port, int buttons, int sx, int sy) {
    if (port != 0) return 0; /* one controller */
    /* bits 16-31: controls.extra (bit 16 "Snap", F1; bit 17 "Shutter", F4): port_set_pad -> port_pad_filter
     * (pc_options.c) */
    int b = buttons & 0x3FFFF, x = stick(sx), y = stick(sy);
    pad_shutter_seen |= b & 0x20000;
    if ((b != pad_buttons || x != pad_x || y != pad_y) && pad_logged < 200) {
        pad_logged++;
        logf_("rtw: pad0 buttons %04x stick %d,%d (retrace %u)", b, x, y, port_vi_count);
    }
    pad_buttons = b;
    pad_x = x;
    pad_y = y;
    if (started) port_rtw_host_frame(); /* the frame clock: rtw_glue.js decides, the retrace runs after this returns */
    return 0;
}

/* F3, motion aim (manifest controls.motion): the device's rotation since the previous call in 1/1024 degree, already
 * turned to the screen's orientation (yaw > 0: turned right, pitch > 0: tilted up). The host calls it at most once
 * per display frame, just before rtw_input, and only while the player has "Motion aim" on in the host's menu. Summed
 * for the game (pc_options.c port_motion_add), which applies it while the player controls the camera. */
void port_motion_add(int yaw, int pitch);
static int motion_logged;
EMSCRIPTEN_KEEPALIVE int rtw_motion(int port, int yaw, int pitch) {
    if (port != 0) return 0; /* one controller */
    if (!motion_logged && (yaw || pitch)) {
        motion_logged = 1;
        logf_("rtw: motion aim: first rotation from the host (yaw %d, pitch %d / 1024 degree, retrace %u)", yaw, pitch,
              port_vi_count);
    }
    if (started) port_motion_add(yaw, pitch);
    return 0;
}

/* F6, mouse aim (pc_options.c): the module asks for the mouse while the player controls the camera
 * (port_pointer_lock -> rtw_pointer_lock; the host captures it on the player's next click, Esc gives it back) and
 * gives it back elsewhere. rtw_mouse: the motion since the previous call (CSS pixels, summed by the host over a display
 * frame) and the buttons (bit 0 left, 1 right, 2 middle), sent only while the host holds the lock. The click that
 * captures the mouse is the host's, not the game's. */
void port_mouse_add(int dx, int dy, int buttons);
static int mouse_logged;
EMSCRIPTEN_KEEPALIVE int rtw_mouse(int dx, int dy, int buttons) {
    if (!mouse_logged) {
        mouse_logged = 1;
        logf_("rtw: mouse aim: first mouse event from the host (dx %d, dy %d, buttons %d, retrace %u)", dx, dy, buttons,
              port_vi_count);
    }
    if (started) port_mouse_add(dx, dy, buttons);
    return 0;
}
void port_pointer_lock(int on) { /* pc_options.c (weak there) */
    logf_("rtw: pointer lock %s (retrace %u)", on ? "asked" : "given back", port_vi_count);
    rtw_pointer_lock(on ? 1 : 0);
}

/* options (standard 4.7, F1): every option of retrowser.json is "inGame" (the game's Options menu shows it); the host
 * calls rtw_option for each at start-up with the value it remembers and is told of every change from the menu */
int port_option_set(int id, int value);
EMSCRIPTEN_KEEPALIVE int rtw_option(int id, int value) {
    logf_("rtw: option %d = %d from the host", id, value);
    return port_option_set(id, value);
}
void port_option_changed(int id, int value) { /* pc_options.c (weak there): the game's Options menu changed it */
    logf_("rtw: option %d = %d changed in the game's Options menu", id, value);
    rtw_option_changed(id, value);
}

EMSCRIPTEN_KEEPALIVE int rtw_pause(int on) {
    port_rtw_set_paused(on ? 1 : 0);
    return 0;
}

EMSCRIPTEN_KEEPALIVE int rtw_resize(int w, int h, int dpr) {
    port_rtw_resize(w, h, dpr);
    return 0;
}

/* one retrace, called by rtw_glue.js (outside every host export) */
EMSCRIPTEN_KEEPALIVE void port_rtw_retrace(void) {
    if (!started) return;
    port_set_pad(0, pad_buttons | pad_shutter_seen, pad_x, pad_y);
    pad_shutter_seen = 0;
    port_vi_retrace();
    port_save_poll(); /* a [save] log line per flash change (the host reads rtw_save_generation itself) */
}

/* pc_pi.c's motor hook (weak there). Pokemon Snap never starts the motor (contRumbleStart has no caller; sched.c only
 * sends init + stop at reset) and the port reports no pak, so this is not reached in play; kept so a pak model or a
 * game change reaches the host. */
void port_rumble(int channel, int on) { rtw_rumble(channel, on); }
