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
 *   id 3  PORT_OPT_MOTION_AIM   bool, default 0 (F5): motion aim on. The host reads the gyroscope only while it is on
 *                               (manifest controls.motion = { "when": 3 }); the module also drops rtw_motion while it
 *                               is off, so with a host that still has its own ☰ Motion choice both must be on.
 *   id 4  PORT_OPT_MOUSE_AIM    bool, default 1 (F6): mouse aim on. While the player controls the camera the module
 *                               asks the host for the mouse (rtw_pointer_lock); its motion turns the view, a left
 *                               click takes a picture, the right button held zooms, a middle click throws an apple.
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
 * arrives while player.c does not take it (menus, pause, cut-scenes, C-button turns) is dropped. F5: option 3 (the
 * Options menu's "Motion Aim" row) is the switch: the host reads the sensors (and asks for the permission) only while
 * it is on, and motion that arrives while it is off is dropped here; with it off nothing changes. */
#include "pc_os.h"
#include <emscripten.h>
#include <stdlib.h>

enum { PORT_OPT_SNAP_BUTTON = 1, PORT_OPT_PIXELS, PORT_OPT_MOTION_AIM, PORT_OPT_MOUSE_AIM, PORT_OPT_COUNT };
#define PORT_PAD_SNAP 0x10000
#define PORT_PAD_SHUTTER 0x20000
#define Z_TRIG 0x2000
#define A_BUTTON 0x8000

static int opt_value[PORT_OPT_COUNT] = {0, 1, 0, 0, 1};

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
/* F6, the mouse's buttons in this state machine (port_pad_filter; the mouse itself: "mouse aim" below) */
static int snap_rzoom;        /* the sequence was started by the right button: zoom while held, no picture */
static int snap_mouse;        /* the sequence was started by a left click (a Shutter tap) */
static int mouse_a_t;         /* retraces of the mouse's synthetic A press so far (0: none) */
static unsigned mouse_a_start = 0xFFFFFFF0u;
static int mouse_shot_pending; /* a left click waiting for the zoomed camera (port_pad_filter) */
static unsigned mouse_shot_at;
static void mouse_lock_update(int fresh);
static int mouse_take_presses(int *held);
static void mouse_a_press(const char *what);
static void mouse_shot_poll(void);
static void mouse_reset(void);
#define SNAP_PRESS 4      /* retraces a synthetic press lasts (the game samples the pad once per game frame) */
#define SNAP_STALE 10     /* retraces without a report from player.c: not in a course any more */
/* what player.c reported last (port_snap_course) */
static unsigned course_seen = 0xFFFFFFF0u; /* retrace of the last report */
static int course_dir, course_switch, course_ready;
/* F6: what player.c's updatePlayer reported last (port_course_frame): a course is running, and whether it is paused */
static unsigned course_frame_seen = 0xFFFFFFF0u;
static int course_is_paused;
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

/* F6: app_level/player.c updatePlayer, every game frame of a course (its scenes, messages and pause menu included) */
void port_course_frame(int paused) {
    course_frame_seen = port_vi_count;
    course_is_paused = paused != 0;
}

/* F4: updateCameraZoomedIn, every zoomed-in game frame: 1 when an A press now takes a picture (no C-button turn, the
 * game's minimum time between two photos is over) */
void port_snap_shoot_ready(int ready) {
    if (!ready) shoot_frames = 0;
    else if (shoot_seen != port_vi_count) shoot_frames++;
    shoot_seen = port_vi_count;
}

/* app_level/player.c (#ifdef PORT): the game took a picture, whatever pressed A; filmLeft: the film counter after it */
void port_snap_photo_taken(int film_left) {
    port_log("[snap] photo taken at retrace %u, film left %d (%s)\n", port_vi_count, film_left,
             port_vi_count - mouse_a_start <= SNAP_PRESS + 2        ? "mouse click"
             : snap_state == SNAP_SHOOT && snap_mouse            ? "mouse click"
             : snap_state == SNAP_SHOOT                          ? (snap_shutter ? "shutter" : "Snap button")
                                                                 : "A");
}

static void snap_go(int state) {
    if (state != snap_state)
        port_log("[snap] %s -> %s at retrace %u (direction %d, %s%s)\n", snap_state_name[snap_state],
                 snap_state_name[state], port_vi_count, course_dir, course_switch ? "switch" : "hold",
                 snap_mouse ? ", mouse click" : snap_shutter ? ", shutter" : snap_rzoom ? ", right button" : "");
    snap_state = state;
    snap_t = 0;
}

/* F4: zoomed-in game frames (A accepted) before the shutter presses A. The photo and the Pokemon it scores are what the
 * detector last analysed (pokemon_detect.c: a post-process of the drawn frame, copied to the game a frame later), which
 * lags the camera; 3 frames make that a zoomed-in frame rather than the last step of the zoom. */
#define SHUTTER_READY_FRAMES 3

/* pc_pi.c port_set_pad: pad 0's buttons as the host sends them (bits 16-31: controls.extra) -> the OSContPad bits the
 * game reads. F6: the mouse's buttons (port_mouse_add) act here too: a left click is a Shutter tap (zoomed out) or an
 * A press (zoomed in), the right button held zooms like Snap without the picture, a middle click is A while zoomed out
 * (an apple). */
int port_pad_filter(int buttons) {
    int held = (buttons & PORT_PAD_SNAP) && opt_value[PORT_OPT_SNAP_BUTTON];
    int shutter_down = (buttons & PORT_PAD_SHUTTER) != 0;
    int tap = shutter_down && !shutter_was_down; /* F4: the press is the tap, however long it is held */
    int out = buttons & 0xFFFF;
    int fresh = port_vi_count - course_seen <= SNAP_STALE;
    int rheld, mclicks, ltap = 0;
    shutter_was_down = shutter_down;
    mouse_lock_update(fresh);
    mclicks = mouse_take_presses(&rheld); /* F6: presses since the last retrace; rheld: the right button is down */
    if (!fresh) {
        if (snap_state != SNAP_IDLE) snap_go(SNAP_IDLE);
        if (tap) port_log("[snap] shutter tap ignored at retrace %u (no camera control)\n", port_vi_count);
        if (mclicks) port_log("[mouse] click ignored at retrace %u (no camera control)\n", port_vi_count);
        mouse_reset();
        return out;
    }
    /* F6: the left click. Zoomed in (or zooming in for the right button): an A press as soon as the camera takes
     * pictures (mouse_shot_poll; whether A takes one is the game's: updateCameraZoomedIn). Zoomed out: a Shutter tap
     * (one whole picture), unless one is already running. */
    if (mclicks & 1) {
        if (course_dir == -1 || (snap_rzoom && snap_state == SNAP_ENTER)) {
            if (snap_state == SNAP_SHOOT || mouse_a_t || mouse_shot_pending)
                port_log("[mouse] click ignored at retrace %u (a picture is being taken)\n", port_vi_count);
            else {
                mouse_shot_pending = 1;
                mouse_shot_at = port_vi_count;
            }
        } else if (!course_ready) {
            port_log("[mouse] click ignored at retrace %u (the camera is not the player's now)\n", port_vi_count);
        } else if (tap || held || snap_state != SNAP_IDLE) {
            port_log("[mouse] click ignored at retrace %u (%s busy)\n", port_vi_count,
                     snap_state == SNAP_IDLE ? "Snap" : snap_state_name[snap_state]);
        } else {
            ltap = 1;
        }
    }
    /* F6: the middle click: A while zoomed out (the game throws an apple) */
    if (mclicks & 4) {
        if (course_ready && course_dir >= 0 && snap_state == SNAP_IDLE && !mouse_a_t && !ltap && !tap && !held)
            mouse_a_press("middle click: apple");
        else
            port_log("[mouse] middle click ignored at retrace %u (only zoomed out, nothing else running)\n",
                     port_vi_count);
    }
    snap_t++;
    if (!(snap_rzoom ? rheld : held)) snap_let_go = 1;
    if (tap && (snap_state != SNAP_IDLE || held))
        port_log("[snap] shutter tap ignored at retrace %u (%s busy)\n", port_vi_count,
                 held && snap_state == SNAP_IDLE ? "Snap" : snap_state_name[snap_state]);
    switch (snap_state) {
    case SNAP_IDLE:
        if ((held || tap || ltap || rheld) && course_ready && port_vi_count - course_seen <= 4) {
            snap_let_go = 0;
            snap_shutter = !held && (tap || ltap);
            snap_mouse = !held && !tap && ltap;
            snap_rzoom = !held && !tap && !ltap;
            if (course_dir == -1) { /* already zoomed in (Z toggled / held by the player): only the photo */
                snap_zoomed_by_us = 0;
                if (!snap_rzoom) snap_go(SNAP_AIM); /* the right button: zoomed in already, nothing to do */
            } else if (course_dir >= 0) {
                snap_zoomed_by_us = 1;
                snap_go(SNAP_ENTER);
            } else if (snap_shutter) {
                port_log("[snap] %s ignored at retrace %u (zooming)\n", snap_mouse ? "mouse click" : "shutter tap",
                         port_vi_count);
            }
        } else if ((tap || ltap) && !held) {
            port_log("[snap] %s ignored at retrace %u (zoom not available now)\n", ltap ? "mouse click" : "shutter tap",
                     port_vi_count);
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
        } else if (snap_rzoom) { /* F6: zoomed while the right button is held */
            if (course_dir != -1) snap_go(SNAP_IDLE); /* zoomed out by the player (Z) */
            else if (snap_let_go) snap_go(SNAP_LEAVE);
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
    if (snap_state == SNAP_IDLE) snap_mouse = snap_rzoom = 0;
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
    /* F6: the mouse's own A press (a click while zoomed in, a middle click) */
    mouse_shot_poll();
    if (mouse_a_t) {
        out |= A_BUTTON;
        if (++mouse_a_t > SNAP_PRESS) mouse_a_t = 0;
    }
    return out;
}

/* ---------------------------------------------------------------- motion aim (F3) and mouse aim (F6) */
/* Two sources turn the view the same way (app_level/player.c port_motionAim): the device's rotation (F3, rtw_motion)
 * and the mouse (F6, rtw_mouse). Each keeps the sums of its deltas not yet taken by the game, in 1/1024 degree (yaw > 0:
 * turn right, pitch > 0: look up), held to +-90 degrees: a host that keeps sending while the game holds the motion
 * (zooming) cannot wind it up. Each logs on its own ([motion] / [mouse]). */
#define MOTION_MAX (90 * 1024)
typedef struct {
    const char *tag, *stopped; /* log prefix; what the end of one turn is called */
    int yaw, pitch;            /* not yet taken */
    int active;                /* reaching the camera (for the log) */
    long long sum_yaw, sum_pitch, dropped;
    unsigned frames, still;    /* game frames with motion / without since the last one (for the log) */
} AimSource;
static AimSource aim_motion = {"[motion]", "rotation stopped"}, aim_mouse = {"[mouse]", "movement stopped"};
static AimSource *const aim_sources[2] = {&aim_motion, &aim_mouse};
static int motion_latched_pitch;
static unsigned motion_taken = 0xFFFFFFF0u; /* retrace of player.c's last port_motion_take (any mode) */
/* game frames in camera control without motion after which the turn so far is reported ("rotation stopped"): the
 * log then gives the angle of each separate turn of the phone (e2e.mjs motion phase reads it; F6: the mouse phase
 * reads "[mouse] movement stopped") */
#define MOTION_STILL_FRAMES 20

static int motion_clamp(int v) { return v > MOTION_MAX ? MOTION_MAX : v < -MOTION_MAX ? -MOTION_MAX : v; }
static long long iabs(long long v) { return v < 0 ? -v : v; }

static void motion_report(AimSource *a, const char *why) {
    port_log("%s %s at retrace %u: %u game frames turned the view by yaw %+.2f, pitch %+.2f degrees (before the "
             "game's pitch limits); %.2f degrees dropped outside camera control\n", a->tag, why, port_vi_count, a->frames,
             a->sum_yaw / 1024.0, a->sum_pitch / 1024.0, a->dropped / 1024.0);
    a->sum_yaw = a->sum_pitch = a->dropped = 0;
    a->frames = 0;
}

static void aim_add(AimSource *a, int yaw, int pitch) {
    if (port_vi_count - motion_taken > SNAP_STALE) { /* the camera is not the player's: drop */
        a->dropped += iabs(yaw) + iabs(pitch);
        if (a->active) {
            a->active = 0;
            motion_report(a, "camera control ended");
        }
        a->yaw = a->pitch = 0;
        return;
    }
    a->yaw = motion_clamp(a->yaw + yaw);
    a->pitch = motion_clamp(a->pitch + pitch);
}

/* the host (rtw_motion, port 0; harness --motion): the device's rotation since the previous call */
EMSCRIPTEN_KEEPALIVE void port_motion_add(int yaw, int pitch) {
    if (!opt_value[PORT_OPT_MOTION_AIM]) { /* F5: motion aim is off (the Options menu's row) */
        static int told;
        if (!told) port_log("[motion] dropped: option 3 (Motion Aim) is off\n");
        told = 1;
        aim_motion.yaw = aim_motion.pitch = 0;
        return;
    }
    aim_add(&aim_motion, yaw, pitch);
}

/* app_level/player.c handleAnalogStick, every game frame (#ifdef PORT). mode 0: take the motion of every source
 * (returns the yaw; port_motion_pitch then gives the pitch; 1/1024 degree); 1: keep it for a later frame (zooming in
 * or out, a C-button turn while zoomed in); 2: drop it (input disabled) */
int port_motion_take(int mode) {
    int yaw = 0, i;
    int stale = port_vi_count - motion_taken > SNAP_STALE; /* what was summed came before camera control */
    motion_taken = port_vi_count;
    motion_latched_pitch = 0;
    for (i = 0; i < 2; i++) {
        AimSource *a = aim_sources[i];
        if (stale) a->yaw = a->pitch = 0;
        if (mode == 2) {
            a->dropped += iabs(a->yaw) + iabs(a->pitch);
            a->yaw = a->pitch = 0;
        } else if (mode == 0 && (a->yaw || a->pitch)) {
            a->still = 0;
            yaw += a->yaw;
            motion_latched_pitch += a->pitch;
            if (!a->active) {
                a->active = 1;
                port_log("%s turning the camera from retrace %u\n", a->tag, port_vi_count);
            }
            a->sum_yaw += a->yaw;
            a->sum_pitch += a->pitch;
            a->yaw = a->pitch = 0;
            if (++a->frames >= 600) motion_report(a, "so far");
        } else if (mode == 0 && a->active && ++a->still >= MOTION_STILL_FRAMES) {
            a->active = 0;
            a->still = 0;
            motion_report(a, a->stopped);
        }
    }
    return yaw;
}
int port_motion_pitch(void) { return motion_latched_pitch; }

/* ---------------------------------------------------------------- mouse aim (F6) */
/* The host (rtw_mouse, port/rtw/rtw.c; harness --mouse): the mouse's motion since the previous call in CSS pixels and
 * its buttons (bit 0 left, 1 right, 2 middle), sent only while the host holds the pointer lock the module asked for
 * (rtw_pointer_lock, see mouse_lock_update). Motion turns the view through the motion-aim path at a fixed
 * MOUSE_UNITS_PER_PX; the buttons act in port_pad_filter. */
#define MOUSE_UNITS_PER_PX 82 /* 1/1024 degree per pixel: 0.08 degree (about the picture's width = its field of view) */
#define MOUSE_ZOOM_NUM 3      /* zoomed in (fovy 40 instead of 55): 3/4 of that, so a pixel moves the picture alike */
#define MOUSE_ZOOM_DEN 4
#define MOUSE_SHOT_WAIT 60    /* retraces a click waits for the zoomed camera to take pictures before it is dropped */
static int mouse_lock;        /* what the module asked the host for (rtw_pointer_lock) */
static int mouse_buttons;     /* the host's latest button state */
static int mouse_pressed;     /* presses seen since the last retrace (a click shorter than a field still counts) */

/* the retrowser target overrides this with rtw_pointer_lock (port/rtw/rtw.c); the harness only logs */
__attribute__((weak)) void port_pointer_lock(int on) { (void)on; }

EMSCRIPTEN_KEEPALIVE void port_mouse_add(int dx, int dy, int buttons) {
    if (!opt_value[PORT_OPT_MOUSE_AIM] || !mouse_lock) { /* not asked for (a late message after the release) */
        mouse_buttons = 0;
        return;
    }
    mouse_pressed |= buttons & ~mouse_buttons & 7;
    mouse_buttons = buttons & 7;
    if (dx || dy) {
        int units = course_dir == -1 ? MOUSE_UNITS_PER_PX * MOUSE_ZOOM_NUM / MOUSE_ZOOM_DEN : MOUSE_UNITS_PER_PX;
        if (dx > 32767) dx = 32767;
        if (dx < -32767) dx = -32767;
        if (dy > 32767) dy = 32767;
        if (dy < -32767) dy = -32767;
        aim_add(&aim_mouse, dx * units, -dy * units); /* the mouse moved up (dy < 0): look up */
    }
}

/* every retrace (port_pad_filter): ask for the mouse once the player controls the camera in a course, keep it for
 * the rest of the course (its own scenes, messages and zoomed-out C-button turns pause the camera only for a while:
 * giving the mouse back there would make the player click again after each), give it back in the pause menu, when the
 * course ends (camera check, menus) and when option 4 goes off. fresh: player.c reported camera control within
 * SNAP_STALE retraces. The host captures the mouse on the player's next click and shows "Click to use the mouse" while
 * the module wants it; after Esc it is the player's choice to click again: the module does not call again. */
static void mouse_lock_update(int fresh) {
    int in_course = port_vi_count - course_frame_seen <= SNAP_STALE;
    int want;
    const char *why;
    if (!opt_value[PORT_OPT_MOUSE_AIM]) {
        want = 0;
        why = "option 4 (Mouse Aim) is off";
    } else if (in_course && course_is_paused) {
        want = 0;
        why = "pause menu";
    } else if (fresh) {
        want = 1;
        why = "camera control";
    } else if (in_course) {
        want = mouse_lock;
        why = "the course goes on";
    } else {
        want = 0;
        why = "not in a course";
    }
    if (want == mouse_lock) return;
    mouse_lock = want;
    port_log("[mouse] %s at retrace %u (%s)\n", want ? "pointer lock asked" : "mouse given back", port_vi_count, why);
    if (!want) {
        mouse_buttons = mouse_pressed = 0;
        mouse_shot_pending = 0;
    }
    port_pointer_lock(want);
}

static void mouse_a_press(const char *what) {
    mouse_a_t = 1;
    mouse_a_start = port_vi_count;
    port_log("[mouse] A (%s) at retrace %u\n", what, port_vi_count);
}

/* port_pad_filter, once per retrace: the presses since the last retrace (bit 0 left, 2 middle) and whether the right
 * button is down; nothing unless the module holds the mouse */
static int mouse_take_presses(int *held) {
    int p = mouse_lock ? mouse_pressed : 0;
    mouse_pressed = 0;
    *held = mouse_lock && (mouse_buttons & 2);
    return p;
}

/* no camera control: nothing of the mouse's carries over */
static void mouse_reset(void) {
    if (mouse_shot_pending) port_log("[mouse] click dropped at retrace %u (no camera control)\n", port_vi_count);
    mouse_shot_pending = 0;
    mouse_a_t = 0;
}

/* a left click while zoomed in: A once the zoomed camera takes pictures (port_snap_shoot_ready, as the Shutter waits
 * for it: the frame the detector analysed is then a zoomed-in one), within MOUSE_SHOT_WAIT retraces */
static void mouse_shot_poll(void) {
    if (!mouse_shot_pending) return;
    if (course_dir == -1 && shoot_frames >= SHUTTER_READY_FRAMES && !mouse_a_t && snap_state != SNAP_SHOOT) {
        mouse_shot_pending = 0;
        mouse_a_press("left click: picture");
    } else if (course_dir >= 0 && !(snap_rzoom && snap_state == SNAP_ENTER)) {
        mouse_shot_pending = 0;
        port_log("[mouse] click dropped at retrace %u (zoomed out before the camera was ready)\n", port_vi_count);
    } else if (port_vi_count - mouse_shot_at > MOUSE_SHOT_WAIT) {
        mouse_shot_pending = 0;
        port_log("[mouse] click dropped at retrace %u (the camera took no picture in time)\n", port_vi_count);
    }
}

/* ---------------------------------------------------------------- the Options menu rows (src/main_menu, #ifdef PORT) */
/* image memory for the menu's text sprites (bitmaps + IA16 pixels), kept for the session: slot -> buffer */
void *port_menu_alloc(int slot, int bytes) {
    static void *buf[32];
    static int size[32];
    if (slot < 0 || slot >= 32 || bytes <= 0) return NULL;
    if (size[slot] < bytes) {
        free(buf[slot]);
        buf[slot] = malloc((size_t)bytes); /* 8-byte aligned: enough for IA16 rows and Bitmap words */
        size[slot] = buf[slot] ? bytes : 0;
    }
    return buf[slot];
}
