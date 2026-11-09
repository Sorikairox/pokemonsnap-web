# Port features

Features the console game does not have, each behind an option (PLAN.md phase 6, skill rule), one commit each. Port
options are **not** part of the save: the FlashRAM image stays exactly what the console writes (emulator-compatible).

## Options

| id | Option | Type | Default | Where it is set | Kept by |
|---|---|---|---|---|---|
| 1 | Snap button | bool | 1 (on) | the game's Options menu, row "Snap Button  On / Off" | the host (`inGame`: `rtw_option` at start, `rtw_option_changed` from the menu); harness `--option 1=V` |
| 2 | Pixels (manifest label "Sharp pixels") | bool | 0 (Smooth) | the game's Options menu, row "Pixels  Smooth / Sharp" (previewed while choosing) | the host, like option 1; harness `--option 2=1` |

No port option for motion aim (F3) or the Shutter (F4): the host's "Motion aim" toggle is the switch for the first,
the host's (opt-in) full-screen landscape layout decides where the second is drawn; see below.

Code: `port/ultra/pc_options.c` (`port_option_set` / `port_option_get` / `port_option_set_by_game`, weak
`port_option_changed`; F2: `port_display_sharp`, `port_display_preview`), `port/rtw/rtw.c` (`rtw_option`, `rtw_option_changed`), `port/rtw/retrowser.json` (`options`,
`controls.extra`). Harness: `node port/web/run.mjs ... --option 1=0`, node `port-test ... --option 1=0`.

## The camera on a touch screen (session F1, docs/port/notes/F1.md)

Taking a picture needs Z (hold to zoom) + stick + A at once.

- **Zoom toggle and inverted stick Y: the game already has them**, in its own Options menu (title: Options):
  "Z Button Setup: Hold / Switch" (Switch: tap Z to zoom in, tap again to zoom out; `ZoomSwitchMode`,
  player flag `PFID_ZOOM_SWITCH`) and "Control Stick Setup: Reverse / Normal" (`IsAxisYInverted`, `PFID_INVERTED_Y`,
  read only by `handleAnalogStick` in a course: menus are not affected). They are player flags in the save (written
  when the game saves, as on the console), so the port adds no option for them.
- **Snap button** (option 1, default on: the button is always on the host's touch layout, and does nothing unless
  pressed; off makes it a dead button): `controls.extra` bit 16 "Snap" (touch button; the host's keyboard default is
  key 1, a standard gamepad's Back/Select button). Held: the camera zooms in (Z held, or one Z press in Switch mode)
  and the stick aims; let go: A once (the picture), then the camera zooms out (Z let go / one Z press). If the
  camera was already zoomed in (Switch mode), letting go only takes the picture. A pad transformation in the PC layer
  (`port_pad_filter`, called by `port_set_pad` every retrace): the game sees ordinary Z / A presses. It acts only while
  `app_level/player.c` reports, every game frame, that the player controls the camera (`port_snap_course`, #ifdef
  PORT, in `handleAnalogStick`, `updateCameraZoomedIn` and the tutorial's "Press Z" wait); in menus, the pause menu
  and the camera check the bit is dropped, so it never presses A there. `[snap]` log lines show each step.

## Shutter: one tap, one picture (session F4, docs/port/notes/F3.md)

`controls.extra` bit 17 "Shutter" with `place: "corner"`: the host draws it as a trigger zone in the top-right corner
of its full-screen landscape layout (tapped with the right index finger like a camera's shutter), elsewhere as an
ordinary extra button. A **tap** (the press; its length does not matter) while the player controls the camera is a
whole picture through F1's `port_pad_filter`: not zoomed -> zoom in (Z held, or one Z press with "Z Button Setup:
Switch") -> wait until `updateCameraZoomedIn` reports that A takes a picture now (`port_snap_shoot_ready`, #ifdef
PORT: no zoomed-in C-button turn, the game's minimum time between photos over) for 3 game frames, so the frame the
Pokémon detector analysed is a zoomed-in one -> A once -> zoom back out. Already zoomed in: only the A press. A tap
during a sequence, while zooming, while Snap is held, or outside camera control is **ignored** (a `[snap] shutter tap
ignored` line), never queued: a queued tap would take a picture at a moment the player did not choose. Not tied to
option 1 (that is the Snap button's switch). In the retrowser module a bit-17 press seen by any `rtw_input` reaches
the next retrace even if released before it. A tap takes ~25 retraces (0.4 s): zoom 11, ready 2-3, A + hold 11, zoom
out. Harness: `--input F:0x20000:3`.

## Motion aim (session F3, docs/port/notes/F3.md)

Manifest `controls.motion: true`; export `rtw_motion(port, yaw, pitch)`: the device's rotation since the previous
call in 1/1024 degree, corrected for the screen's orientation by the host (yaw > 0 turned right, pitch > 0 tilted
up), sent only while the player has turned "Motion aim" on in the host's menu (default off). `pc_options.c`
`port_motion_add` sums it; `app_level/player.c` takes it every game frame in `handleAnalogStick` (#ifdef PORT,
`port_motionAim`) and turns the view **1:1** (10 degrees of phone = 10 degrees of view), combined with the stick:
- yaw -> `PlayerViewYaw`. Zoomed out it is relative to the facing; past +-0.3 pi the facing turns a quarter
  (`gDirectionIndex`, `CurrentDirectionYaw`) exactly as the game does for the stick, but without the game's snap to
  -+0.2 pi, so no rotation is lost (turning the phone 90 degrees = one C-button turn). Zoomed in: relative to the
  cart, kept in [0, 2 pi). There is no yaw limit in the game (zoomed out the facing changes, zoomed in it wraps).
- pitch -> `ViewPitch`, kept within the game's `MinPitch`..`MaxPitch` (-22.5 / +45 degrees by default,
  `setPitchLimits`); past a limit the view stays at it (tilting back moves it at once, like a mouse look).
- "Control Stick Setup" (inverted Y) is a stick preference and is **not** applied: tilting the phone up always looks
  up.
- Held while zooming in or out and during a zoomed-in C-button turn (applied when it ends); dropped while input is
  disabled (tutorial messages, cut-scenes), paused, in menus and during a zoomed-out C-button turn (handleAnalogStick
  does not run: the motion of those frames is lost, the view keeps its game-driven turn). Sums held at +-90 degrees.
- `[motion]` log lines: when motion starts turning the camera, after 20 game frames without motion ("rotation
  stopped": the angle of that one turn), every 600 game frames and when camera control ends (degrees applied, degrees
  dropped). Every picture the game takes logs `[snap] photo taken at retrace N, film left F (shutter | Snap button |
  A)` (player.c, #ifdef PORT, after makePhoto).
Harness: `run.mjs` / node `port-test` `--motion F:YAW:PITCH[:LEN],...` (per retrace, before the pad, as the host).
The local test page (port/web/index.html) is a scripted harness without live input: no DeviceMotionEvent there.

## Sharp pixels (session F2, docs/port/notes/F2.md)

The game draws 320x240 (or 640x480); the VI blit scales it onto the canvas. **Smooth** (default, unchanged since S9):
one `GL_LINEAR` blit to the 4:3 fit, which on a phone (~3.7x) softens everything, HUD and text included. **Sharp**:
"sharp bilinear" (the retrowser host does the same for framebuffer games): a nearest-neighbour upscale by
n = ceil(fit scale) into an intermediate image, then one `GL_LINEAR` blit down to the 4:3 fit; a whole-number fit is a
single nearest blit. Every N64 pixel becomes a square of one size, only its edges blend (at most one device pixel).
Why not the alternatives on a phone (390 css px at dpr 3 = 1170 device px): integer 3x with a letterbox is 960 px
wide, a fifth of the width lost; plain nearest at 3.66x makes columns 3 and 4 pixels wide in an irregular pattern
(visible in text and when the camera pans). In Sharp mode the retrowser glue sizes the canvas to the view in device
pixels (at most 4096 a side, any shape; `vi_frame` letterboxes) so the page shows it 1:1 and the browser adds no
smoothing of its own; Smooth keeps the old 4:3 canvas (320x240 .. 1280x960).

Display only: `port/gfx/gfx_rdp.c` `vi_frame` / `present_sharp_blit` touch the canvas, never the colour images the
game renders and reads back (photo detection, scoring, album): the Beach route's read-back journal, flash image and
audio are byte-identical with `--option 2=1`. The Options row previews the choice live (like Sound's Stereo / Mono;
`port_display_preview`), B while choosing restores it, Return commits it (`rtw_option_changed 2`). Test:
`port/gfx/test` `present_sharp` (blended pixels across an edge at 640x480 and 1100x825; fails if the option is
ignored).

### The Options menu with the port's rows

Rows from the top: Screen, Sound, Z Button Setup, Control Stick Setup, Snap Button (5), Pixels (6), Return (4).
"Return" and the description box (its lines, sides and every description) moved down 32 / 16 pixels: the box spans y
182-213 of 240, just inside the "Screen" adjustment frame.
