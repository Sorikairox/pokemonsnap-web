# Port features

Features the console game does not have, each behind an option (PLAN.md phase 6, skill rule), one commit each. Port
options are **not** part of the save: the FlashRAM image stays exactly what the console writes (emulator-compatible).

## Options

| id | Option | Type | Default | Where it is set | Kept by |
|---|---|---|---|---|---|
| 1 | Snap button | bool | 1 (on) | the game's Options menu, row "Snap Button  On / Off" | the host (`inGame`: `rtw_option` at start, `rtw_option_changed` from the menu); harness `--option 1=V` |
| 2 | Pixels (manifest label "Sharp pixels") | bool | 0 (Smooth) | the game's Options menu, row "Pixels  Smooth / Sharp" (previewed while choosing) | the host, like option 1; harness `--option 2=1` |

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
