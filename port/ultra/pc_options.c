/* PC layer: port options and the Snap button (session F1; docs/port/notes/F1.md, FEATURES.md).
 *
 * Options are port settings that are NOT part of the game's save: the FlashRAM image stays exactly what the console
 * writes (emulator-compatible). The host keeps them (retrowser: options with "inGame": true in port/rtw/retrowser.json;
 * the harness: run.mjs / node --option ID=VALUE) and passes them in at start with port_option_set (rtw_option); the
 * game's own Options menu (src/main_menu, #ifdef PORT) changes them through port_option_set_by_game, which tells the
 * host (port_option_changed: rtw_option_changed in the retrowser target, a log line in the harness).
 *
 *   id 1  PORT_OPT_SNAP_BUTTON  bool, default 1: the controls.extra button bit 16 "Snap" works
 *   id 2  PORT_OPT_PIXELS       bool, default 0 (F2): 0 Smooth, 1 Sharp: how the picture is scaled onto the canvas
 *                               (port/gfx vi_frame, port_gfx_set_present_sharp; display only, never the native images
 *                               the game reads back). The Options row previews it live while it is being chosen
 *                               (port_display_preview), as the game's Sound row does with Stereo / Mono.
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
 * pause menu, camera check) the bit is dropped, so it never presses A in a menu.
 *
 * Shutter (pad bit 17, PORT_PAD_SHUTTER, session F4): one tap = one complete picture, through the same state machine.
 * Not zoomed: zoom in (Z held / one Z press, as the Snap button), wait until updateCameraZoomedIn reports that A takes
 * a picture now (port_snap_shoot_ready) for SHUTTER_READY_FRAMES game frames, press A once, zoom back out. Already
 * zoomed in: only the A press. A tap while a sequence runs, while zooming, or outside camera control is ignored
 * (logged), never queued: a queued tap would fire at a moment the player did not choose.
 *
 * Motion aim (session F3): the host's rtw_motion deltas (port/rtw/rtw.c; harness --motion) are summed here for pad 0
 * (port_motion_add, 1/1024 degree) and taken by app_level/player.c every game frame while the player controls the
 * camera (port_motion_take, #ifdef PORT in handleAnalogStick), which turns the view by exactly that angle. Motion that
 * arrives while player.c does not take it (menus, pause, cut-scenes, C-button turns) is dropped. No port option: the
 * host's "Motion aim" toggle (default off, it owns the permission prompt) is the only switch; with it off no motion
 * arrives and nothing changes. */
#include "pc_os.h"
#include <emscripten.h>
#include <stdlib.h>

enum { PORT_OPT_SNAP_BUTTON = 1, PORT_OPT_PIXELS, PORT_OPT_COUNT };
#define PORT_PAD_SNAP 0x10000
#define PORT_PAD_SHUTTER 0x20000
#define Z_TRIG 0x2000
#define A_BUTTON 0x8000

static int opt_value[PORT_OPT_COUNT] = {0, 1, 0};

static int option_valid(int id, int value) { return id > 0 && id < PORT_OPT_COUNT && (value == 0 || value == 1); }

/* option 2: the present mode in use = the menu's live preview while it is shown, else the option */
void port_gfx_set_present_sharp(int on);
static int pixels_preview = -1;
EMSCRIPTEN_KEEPALIVE int port_display_sharp(void) { /* also read by the retrowser glue (canvas size) */
    return pixels_preview >= 0 ? pixels_preview : opt_value[PORT_OPT_PIXELS];
}
static void display_apply(void) { port_gfx_set_present_sharp(port_display_sharp()); }
/* the Options menu (src/main_menu/A0E190.c, #ifdef PORT): -1 ends the preview */
void port_display_preview(int sharp) {
    pixels_preview = sharp == 0 || sharp == 1 ? sharp : -1;
    display_apply();
}

/* the host (rtw_option, harness --option): the start-up value or a change from outside the game */
EMSCRIPTEN_KEEPALIVE int port_option_set(int id, int value) {
    if (!option_valid(id, value)) {
        port_log("[option] ignored: id %d value %d\n", id, value);
        return -1;
    }
    opt_value[id] = value;
    port_log("[option] %d = %d (host)\n", id, value);
    if (id == PORT_OPT_PIXELS) display_apply();
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
    if (id == PORT_OPT_PIXELS) display_apply();
    port_option_changed(id, value);
}

/* ---------------------------------------------------------------- Snap button */
enum { SNAP_IDLE, SNAP_ENTER, SNAP_AIM, SNAP_SHOOT, SNAP_LEAVE };
static const char *const snap_state_name[] = {"idle", "enter", "aim", "shoot", "leave"};
static int snap_state, snap_t, snap_let_go, snap_zoomed_by_us;
static int snap_shutter; /* F4: this sequence was started by a Shutter tap (bit 17), not by holding Snap (bit 16) */
static int shutter_was_down;
/* what player.c reported last (port_snap_course) */
static unsigned course_seen = 0xFFFFFFF0u; /* retrace of the last report */
static int course_dir, course_switch, course_ready;
/* F4: game frames in a row in which updateCameraZoomedIn would take a picture on A (port_snap_shoot_ready) */
static unsigned shoot_seen = 0xFFFFFFF0u;
static int shoot_frames;
unsigned port_snap_photos; /* A presses sent */

/* app_level/player.c, every game frame while the player controls the camera: gDirectionIndex (>= 0 zoomed out and
 * facing ahead / a side, -2 zooming in or out, -1 zoomed in), ZoomSwitchMode (0 hold Z, 1 Z toggles) and whether a
 * zoom may start now (zoom enabled, input not disabled) */
void port_snap_course(int direction, int zoom_switch, int ready) {
    course_seen = port_vi_count;
    if (direction != -1) shoot_frames = 0;
    course_dir = direction;
    course_switch = zoom_switch;
    course_ready = ready;
}

/* F4: updateCameraZoomedIn, every zoomed-in game frame: 1 when an A press now takes a picture (no C-button turn, the
 * game's minimum time between two photos is over) */
void port_snap_shoot_ready(int ready) {
    if (!ready) shoot_frames = 0;
    else if (shoot_seen != port_vi_count) shoot_frames++;
    shoot_seen = port_vi_count;
}

static void snap_go(int state) {
    if (state != snap_state)
        port_log("[snap] %s -> %s at retrace %u (direction %d, %s%s)\n", snap_state_name[snap_state],
                 snap_state_name[state], port_vi_count, course_dir, course_switch ? "switch" : "hold",
                 snap_shutter ? ", shutter" : "");
    snap_state = state;
    snap_t = 0;
}

#define SNAP_PRESS 4      /* retraces a synthetic press lasts (the game samples the pad once per game frame) */
#define SNAP_STALE 10     /* retraces without a report from player.c: not in a course any more */
/* F4: zoomed-in game frames (A accepted) before the shutter presses A. The photo and the Pokemon it scores are what the
 * detector last analysed (pokemon_detect.c: a post-process of the drawn frame, copied to the game a frame later), which
 * lags the camera; 3 frames make that a zoomed-in frame rather than the last step of the zoom. */
#define SHUTTER_READY_FRAMES 3

/* pc_pi.c port_set_pad: pad 0's buttons as the host sends them (bits 16-31: controls.extra) -> the OSContPad bits the
 * game reads */
int port_pad_filter(int buttons) {
    int held = (buttons & PORT_PAD_SNAP) && opt_value[PORT_OPT_SNAP_BUTTON];
    int shutter_down = (buttons & PORT_PAD_SHUTTER) != 0;
    int tap = shutter_down && !shutter_was_down; /* F4: the press is the tap, however long it is held */
    int out = buttons & 0xFFFF;
    int fresh = port_vi_count - course_seen <= SNAP_STALE;
    shutter_was_down = shutter_down;
    if (!fresh) {
        if (snap_state != SNAP_IDLE) snap_go(SNAP_IDLE);
        if (tap) port_log("[snap] shutter tap ignored at retrace %u (no camera control)\n", port_vi_count);
        return out;
    }
    snap_t++;
    if (!held) snap_let_go = 1;
    if (tap && (snap_state != SNAP_IDLE || held))
        port_log("[snap] shutter tap ignored at retrace %u (%s busy)\n", port_vi_count,
                 held && snap_state == SNAP_IDLE ? "Snap" : snap_state_name[snap_state]);
    switch (snap_state) {
    case SNAP_IDLE:
        if ((held || tap) && course_ready && port_vi_count - course_seen <= 4) {
            snap_let_go = 0;
            snap_shutter = !held;
            if (course_dir == -1) { /* already zoomed in (Z toggled / held by the player): only the photo */
                snap_zoomed_by_us = 0;
                snap_go(SNAP_AIM);
            } else if (course_dir >= 0) {
                snap_zoomed_by_us = 1;
                snap_go(SNAP_ENTER);
            } else if (!held) {
                port_log("[snap] shutter tap ignored at retrace %u (zooming)\n", port_vi_count);
            }
        } else if (tap && !held) {
            port_log("[snap] shutter tap ignored at retrace %u (zoom not available now)\n", port_vi_count);
        }
        break;
    case SNAP_ENTER:
        if (course_dir == -1) snap_go(SNAP_AIM);
        else if (snap_t > 90) snap_go(SNAP_LEAVE); /* the zoom did not start: give up without pressing A */
        break;
    case SNAP_AIM:
        if (snap_shutter) { /* F4: shoot as soon as the zoomed camera takes pictures */
            if (course_dir != -1) snap_go(SNAP_IDLE);
            else if (shoot_frames >= SHUTTER_READY_FRAMES) snap_go(SNAP_SHOOT);
            else if (snap_t > 90) snap_go(snap_zoomed_by_us ? SNAP_LEAVE : SNAP_IDLE); /* never ready: no A */
        } else if (snap_let_go) snap_go(SNAP_SHOOT);
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

/* ---------------------------------------------------------------- motion aim (F3) */
/* Sums of the host's deltas not yet taken by the game, in 1/1024 degree (yaw > 0: turn right, pitch > 0: look up).
 * Held to +-90 degrees: a host that keeps sending while the game holds the motion (zooming) cannot wind it up. */
#define MOTION_MAX (90 * 1024)
static int motion_yaw, motion_pitch, motion_latched_pitch;
static unsigned motion_taken = 0xFFFFFFF0u; /* retrace of player.c's last port_motion_take (any mode) */
static int motion_active;                    /* motion is reaching the camera (for the log) */
static long long motion_sum_yaw, motion_sum_pitch, motion_dropped;
static unsigned motion_frames;

static int motion_clamp(int v) { return v > MOTION_MAX ? MOTION_MAX : v < -MOTION_MAX ? -MOTION_MAX : v; }
static long long iabs(long long v) { return v < 0 ? -v : v; }

static void motion_report(const char *why) {
    port_log("[motion] %s at retrace %u: %u game frames turned the view by yaw %+.2f, pitch %+.2f degrees (before the "
             "game's pitch limits); %.2f degrees dropped outside camera control\n", why, port_vi_count, motion_frames,
             motion_sum_yaw / 1024.0, motion_sum_pitch / 1024.0, motion_dropped / 1024.0);
    motion_sum_yaw = motion_sum_pitch = motion_dropped = 0;
    motion_frames = 0;
}

/* the host (rtw_motion, port 0; harness --motion): the device's rotation since the previous call */
EMSCRIPTEN_KEEPALIVE void port_motion_add(int yaw, int pitch) {
    if (port_vi_count - motion_taken > SNAP_STALE) { /* the camera is not the player's: drop */
        motion_dropped += iabs(yaw) + iabs(pitch);
        if (motion_active) {
            motion_active = 0;
            motion_report("camera control ended");
        }
        motion_yaw = motion_pitch = 0;
        return;
    }
    motion_yaw = motion_clamp(motion_yaw + yaw);
    motion_pitch = motion_clamp(motion_pitch + pitch);
}

/* app_level/player.c handleAnalogStick, every game frame (#ifdef PORT). mode 0: take the motion (returns the yaw;
 * port_motion_pitch then gives the pitch; 1/1024 degree); 1: keep it for a later frame (zooming in or out, a C-button
 * turn while zoomed in); 2: drop it (input disabled) */
int port_motion_take(int mode) {
    int yaw = 0;
    if (port_vi_count - motion_taken > SNAP_STALE) motion_yaw = motion_pitch = 0; /* from before camera control */
    motion_taken = port_vi_count;
    motion_latched_pitch = 0;
    if (mode == 2) {
        motion_dropped += iabs(motion_yaw) + iabs(motion_pitch);
        motion_yaw = motion_pitch = 0;
    } else if (mode == 0 && (motion_yaw || motion_pitch)) {
        yaw = motion_yaw;
        motion_latched_pitch = motion_pitch;
        motion_yaw = motion_pitch = 0;
        if (!motion_active) {
            motion_active = 1;
            port_log("[motion] turning the camera from retrace %u\n", port_vi_count);
        }
        motion_sum_yaw += yaw;
        motion_sum_pitch += motion_latched_pitch;
        if (++motion_frames >= 600) motion_report("so far");
    }
    return yaw;
}
int port_motion_pitch(void) { return motion_latched_pitch; }

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
    static void *buf[16];
    static int size[16];
    if (slot < 0 || slot >= 16 || bytes <= 0) return NULL;
    if (size[slot] < bytes) {
        free(buf[slot]);
        buf[slot] = malloc((size_t)bytes); /* 8-byte aligned: enough for RGBA16 rows and Bitmap words */
        size[slot] = buf[slot] ? bytes : 0;
    }
    return buf[slot];
}
