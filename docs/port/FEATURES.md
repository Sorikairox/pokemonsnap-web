# Port features

Features the console game does not have, each behind an option (PLAN.md phase 6, skill rule), one commit each. Port
options are **not** part of the save: the FlashRAM image stays exactly what the console writes (emulator-compatible).

## Options

| id | Option | Type | Default | Where it is set | Kept by |
|---|---|---|---|---|---|
| 1 | Snap button | bool | 1 (on) | the game's Options menu, row "Snap Button  On / Off" | the host (`inGame`: `rtw_option` at start, `rtw_option_changed` from the menu); harness `--option 1=V` |

Code: `port/ultra/pc_options.c` (`port_option_set` / `port_option_get` / `port_option_set_by_game`, weak
`port_option_changed`), `port/rtw/rtw.c` (`rtw_option`, `rtw_option_changed`), `port/rtw/retrowser.json` (`options`,
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
