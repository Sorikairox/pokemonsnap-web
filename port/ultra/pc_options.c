/* PC layer: port options and the Snap button (session F1; docs/port/notes/F1.md, FEATURES.md).
 *
 * Options are port settings that are NOT part of the game's save: the FlashRAM image stays exactly what the console
 * writes (emulator-compatible). The host keeps them (retrowser: options with "inGame": true in port/rtw/retrowser.json;
 * the harness: run.mjs / node --option ID=VALUE) and passes them in at start with port_option_set (rtw_option); the
 * game's own Options menu (src/main_menu, #ifdef PORT) changes them through port_option_set_by_game, which tells the
 * host (port_option_changed: rtw_option_changed in the retrowser target, a log line in the harness).
 *
 *   id 1  PORT_OPT_SNAP_BUTTON  bool, default 1: the controls.extra button bit 16 "Snap" works
 *
 * The Zoom toggle and the inverted stick Y the player asked for are the game's own Options rows ("Z Button Setup:
 * Hold / Switch", "Control Stick Setup: Reverse / Normal"; player flags in the save, read by app_level/player.c), so
 * they are not port options.
 *
 * Snap button (pad bit 16, PORT_PAD_SNAP): a pad transformation, the game sees only ordinary Z / A presses.
 * While it is held the camera zooms in (Z held, or one Z press with "Z Button Setup: Switch") and the stick aims;
 * when it is let go, A is pressed once (the photo) and the camera zooms out again (Z let go / one more Z press). It
 * acts only while the player controls the camera in a course: app_level/player.c calls port_snap_course() every game
 * frame from handleAnalogStick and the tutorial's wait loop (#ifdef PORT) with the zoom state; anywhere else (menus,
 * pause menu, camera check) the bit is dropped, so it never presses A in a menu. */
#include "pc_os.h"
#include <emscripten.h>
#include <stdlib.h>

enum { PORT_OPT_SNAP_BUTTON = 1, PORT_OPT_COUNT };
#define PORT_PAD_SNAP 0x10000
#define Z_TRIG 0x2000
#define A_BUTTON 0x8000

static int opt_value[PORT_OPT_COUNT] = {0, 1};

static int option_valid(int id, int value) { return id == PORT_OPT_SNAP_BUTTON && (value == 0 || value == 1); }

/* the host (rtw_option, harness --option): the start-up value or a change from outside the game */
EMSCRIPTEN_KEEPALIVE int port_option_set(int id, int value) {
    if (!option_valid(id, value)) {
        port_log("[option] ignored: id %d value %d\n", id, value);
        return -1;
    }
    opt_value[id] = value;
    port_log("[option] %d = %d (host)\n", id, value);
    return 0;
}

EMSCRIPTEN_KEEPALIVE int port_option_get(int id) { return id > 0 && id < PORT_OPT_COUNT ? opt_value[id] : 0; }

/* the retrowser target overrides this with rtw_option_changed (port/rtw/rtw.c) */
__attribute__((weak)) void port_option_changed(int id, int value) { (void)id; (void)value; }

/* the game's Options menu changed an option (src/main_menu/A0E190.c, #ifdef PORT) */
void port_option_set_by_game(int id, int value) {
    if (!option_valid(id, value) || opt_value[id] == value) return;
    opt_value[id] = value;
    port_log("[option] %d = %d (game menu)\n", id, value);
    port_option_changed(id, value);
}

/* ---------------------------------------------------------------- Snap button */
enum { SNAP_IDLE, SNAP_ENTER, SNAP_AIM, SNAP_SHOOT, SNAP_LEAVE };
static const char *const snap_state_name[] = {"idle", "enter", "aim", "shoot", "leave"};
static int snap_state, snap_t, snap_let_go, snap_zoomed_by_us;
/* what player.c reported last (port_snap_course) */
static unsigned course_seen = 0xFFFFFFF0u; /* retrace of the last report */
static int course_dir, course_switch, course_ready;
unsigned port_snap_photos; /* A presses sent */

/* app_level/player.c, every game frame while the player controls the camera: gDirectionIndex (>= 0 zoomed out and
 * facing ahead / a side, -2 zooming in or out, -1 zoomed in), ZoomSwitchMode (0 hold Z, 1 Z toggles) and whether a
 * zoom may start now (zoom enabled, input not disabled) */
void port_snap_course(int direction, int zoom_switch, int ready) {
    course_seen = port_vi_count;
    course_dir = direction;
    course_switch = zoom_switch;
    course_ready = ready;
}

static void snap_go(int state) {
    if (state != snap_state)
        port_log("[snap] %s -> %s at retrace %u (direction %d, %s)\n", snap_state_name[snap_state], snap_state_name[state],
                 port_vi_count, course_dir, course_switch ? "switch" : "hold");
    snap_state = state;
    snap_t = 0;
}

#define SNAP_PRESS 4      /* retraces a synthetic press lasts (the game samples the pad once per game frame) */
#define SNAP_STALE 10     /* retraces without a report from player.c: not in a course any more */

/* pc_pi.c port_set_pad: pad 0's buttons as the host sends them (bits 16-31: controls.extra) -> the OSContPad bits the
 * game reads */
int port_pad_filter(int buttons) {
    int held = (buttons & PORT_PAD_SNAP) && opt_value[PORT_OPT_SNAP_BUTTON];
    int out = buttons & 0xFFFF;
    int fresh = port_vi_count - course_seen <= SNAP_STALE;
    if (!fresh) {
        if (snap_state != SNAP_IDLE) snap_go(SNAP_IDLE);
        return out;
    }
    snap_t++;
    if (!held) snap_let_go = 1;
    switch (snap_state) {
    case SNAP_IDLE:
        if (held && course_ready && port_vi_count - course_seen <= 4) {
            snap_let_go = 0;
            if (course_dir == -1) { /* already zoomed in (Z toggled by the player): only the photo */
                snap_zoomed_by_us = 0;
                snap_go(SNAP_AIM);
            } else if (course_dir >= 0) {
                snap_zoomed_by_us = 1;
                snap_go(SNAP_ENTER);
            }
        }
        break;
    case SNAP_ENTER:
        if (course_dir == -1) snap_go(SNAP_AIM);
        else if (snap_t > 90) snap_go(SNAP_LEAVE); /* the zoom did not start: give up without pressing A */
        break;
    case SNAP_AIM:
        if (snap_let_go) snap_go(SNAP_SHOOT);
        else if (course_dir != -1) snap_go(SNAP_IDLE); /* zoomed out by the player (Z) */
        break;
    case SNAP_SHOOT:
        if (snap_t > 10) {
            if (snap_zoomed_by_us) snap_go(SNAP_LEAVE);
            else snap_go(SNAP_IDLE);
        }
        break;
    case SNAP_LEAVE:
        if (course_dir != -1 || snap_t > 30) snap_go(SNAP_IDLE);
        break;
    }
    /* the pad the game sees */
    switch (snap_state) {
    case SNAP_ENTER:
        if (!course_switch || snap_t <= SNAP_PRESS) out |= Z_TRIG;
        break;
    case SNAP_AIM:
        if (!course_switch && snap_zoomed_by_us) out |= Z_TRIG;
        break;
    case SNAP_SHOOT:
        if (!course_switch && snap_zoomed_by_us) out |= Z_TRIG;
        if (snap_t >= 1 && snap_t <= SNAP_PRESS) out |= A_BUTTON;
        if (snap_t == 1) port_snap_photos++;
        break;
    case SNAP_LEAVE:
        if (course_switch && snap_t >= 1 && snap_t <= SNAP_PRESS) out |= Z_TRIG;
        break;
    }
    return out;
}

/* ---------------------------------------------------------------- the Options menu row (src/main_menu, #ifdef PORT) */
/* The game's 12-pixel font (4 bits per pixel, 12 x 12, 370 glyphs in UIText_CharTable order) lives in the window
 * overlay's data (D_80378254_84BA04, used by window/text.c), which the title scene does not load: copied from the
 * player's ROM into port memory (vbe layout, as game code reads it) the first time the menu asks. */
#define FONT12_ROM 0x84BA04u
#define FONT12_SIZE (370 * 72)
void *port_menu_font12(void) {
    static unsigned char *font;
    if (!font) {
        font = malloc(FONT12_SIZE);
        if (!font || port_rom_copy(font, FONT12_ROM, FONT12_SIZE) != 0) {
            port_log("[option] menu font: ROM copy failed\n");
            free(font);
            font = NULL;
        }
    }
    return font;
}

/* image memory for the menu's text sprites (bitmaps + RGBA16 pixels), kept for the session: slot -> buffer */
void *port_menu_alloc(int slot, int bytes) {
    static void *buf[8];
    static int size[8];
    if (slot < 0 || slot >= 8 || bytes <= 0) return NULL;
    if (size[slot] < bytes) {
        free(buf[slot]);
        buf[slot] = malloc((size_t)bytes); /* 8-byte aligned: enough for RGBA16 rows and Bitmap words */
        size[slot] = buf[slot] ? bytes : 0;
    }
    return buf[slot];
}
