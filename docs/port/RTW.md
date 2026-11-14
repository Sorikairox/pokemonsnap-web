# Pokémon Snap on retrowser (the retrowser module, session S9)

retrowser (standard: `~/Code/retrowser/spec/retrowser.md`, guide `spec/guide.md`) loads a game build from a GitHub
repository, checks every file by SHA-256 and runs it in a dedicated Web Worker in a sandboxed iframe with the
player's ROM. This is a **separate build target** of the port: the harness build (`port/build/web`, node
`port-test`, `port/web/run.mjs`) is unchanged and links the same objects.

**Published** (since 2026-11-09): public repo github.com/Sorikairox/pokemonsnap-web (remote `pub`, default branch
`web`), MIT licence for the whole repo, manifest `license: MIT` + `source`. The retrowser build is the orphan branch
**`rtw-dist`** (`port/build/rtw/dist/*`, manifest at its root). After any port change it must be rebuilt with
`port/rtw/build.sh` (`rtw.mjs check --rom` OK) before it is pushed; only the orchestrator pushes, only to `pub`, never
to `origin` (ethteck). retrowser deploys need the user's approval each time. Optional `picture` / `icon` (something
the project has the right to use) are not set.

```sh
port/rtw/build.sh            # docker port build --rtw, template + fill, manifest, hash, check --rom (z64 + .n64), romscan
node port/rtw/e2e.mjs        # local retrowser host e2e (PHASES=desk,touch,wrong,offline; out port_work/s9/e2e)
node port/rtw/node_host.mjs port/build/rtw/dist pokemonsnap.z64 --frames 3000 --tap 120 0x8000 --out port_work/s9/nh
```

Needs `./pokemonsnap.z64` (`port/docker/run.sh rom PATH`) and the local retrowser checkout (`~/Code/retrowser`:
`dist/tools/rtw.mjs` = the check tool, `dist/` = the built host, `tools/serve-worker.ts`, `tools/mock-github.ts`,
`node_modules/playwright-core`). Nothing is downloaded.

## Output (`port/build/rtw/dist`, gitignored)

| File | What |
|---|---|
| `retrowser.json` | from `port/rtw/retrowser.json`, hashed by `rtw.mjs hash` |
| `pokemonsnap.wasm` | the **template**: the linked module with every run of ROM bytes in the game objects' data zeroed (below) |
| `pokemonsnap.js` | Emscripten glue, worker only, factory `createPokemonSnap` |
| `pokemonsnap.romfill` | the fill table (`files.data`): offsets only |

## Manifest

| Field | Value | Why |
|---|---|---|
| `id` | `pokemonsnap-us` | saves are keyed by it: never change it |
| `platform` | `n64` | N64 touch layout, keys, OSContPad bits |
| `rom.sha1` | `edc7c49c…` (US `.z64`) | the host warns on another ROM; the module refuses it with a plain error |
| `files` | wasm, js (factory `createPokemonSnap`), data `pokemonsnap.romfill` | |
| `video` | `webgl` | WebGL2 on the host's OffscreenCanvas (`Module.canvas`, port/gfx/gfx_gl.js) |
| `hz` | 60 | NTSC; informative (self-running) |
| `save` | `flash`, 131072 | the cartridge's FlashRAM, console byte order (SAVE.md) |
| `license`, `source` | `MIT`, github.com/Sorikairox/pokemonsnap-web | public repo |
| `controls.extra` | bit 16 "Snap", `"when": 1` | F1: the Snap button (FEATURES.md); F5: shown only while option 1 is on |
| | bit 17 "Shutter" | F4: one tap = one picture; an ordinary extra button (the `place: "corner"` zone and "Knock to shoot" were removed by the user) |
| `controls.motion` | `{ "when": 3 }` | F3: the module takes `rtw_motion` (motion aim); F5: switched by option 3 (the game's "Motion Aim" row), the host's ☰ Motion choice hidden |
| `options` | id 1 "Snap button", bool, default 1, `inGame` | F1: shown in the game's Options menu, kept by the host |
| | id 2 "Sharp pixels", bool, default 0, `inGame` | F2: the Options menu's "Pixels Smooth / Sharp" row, kept by the host |
| | id 3 "Motion aim", bool, default 0, `inGame` | F5: the Options menu's "Motion Aim On / Off" row; the host reads the gyroscope only while it is on |
| | id 4 "Mouse aim", bool, default 1, `inGame` | F6: the Options menu's "Mouse Aim On / Off" row; mouse aim needs no manifest key (`rtw_pointer_lock` / `rtw_mouse` are standard) |
| no `net`, `picture`, `icon` | | nothing needs them yet |

## Build (`port_build.py --rtw`, `link_rtw`)

Same objects and flags as the harness link (ASYNCIFY, `-sGLOBAL_BASE=256MB`, 384 MiB) plus `port/rtw/rtw.c`, with
`-sENVIRONMENT=worker -sDYNAMIC_EXECUTION=0 -sEXPORTED_RUNTIME_METHODS=HEAPU8`, `--js-library port/rtw/library_rtw.js`,
`--pre-js port/rtw/rtw_glue.js`, exports `rtw_*` only (F6: + `rtw_mouse`) (+ `port_rtw_retrace`, `port_rtw_test_log`) ->
`port/build/rtw/link/`. The glue takes the wasm from `Module.wasmBinary`; its fetch paths are never reached.

Shared-code hooks (weak defaults keep the harness as it was): `port_vbe_fix_defer` (pc_mem.c), `port_task_log`
(pc_rcp.c: the per-task census / `[task]` lines are off in this target), `port_rumble` (pc_pi.c).

## Module interface (`port/rtw/rtw.c`, `rtw_glue.js`, `library_rtw.js`)

- **Start** (standard 4.2): `rtw_rom_buffer(size)` mallocs; the host copies the `.z64`; the stored save goes straight
  into `rtw_save_ptr()` (the flash chip's array, `port_flash_data()`, so it is in place before `main`), then
  `rtw_save_load` (logged only); `rtw_resize`; `rtw_start`: build id + ROM SHA-1 logged, wrong SHA-1 -> a thrown
  plain error ("This build needs the US release of Pokemon Snap…", shown by the host under "Details"); template
  fill; deferred vbe conversions; `port_set_rom`; the harness's own `main(1, argv)` (mirror, main segment,
  `game_main`, returns); then the loop. Log: `rtw: game started (save from the host | no stored save)`.
- **Loop**: self-running (no `rtw_frame`), `rtw_pause`. One `port_rtw_retrace()` = pad -> `port_vi_retrace()` ->
  `port_save_poll()`. Pacing (traps 13/14): the port-0 `rtw_input` (once per display frame) starts a due retrace
  (up to half a field early) in a **microtask**, never inside the export; one timer at most, armed after each
  retrace for half a field after the next is due and cleared unfired when a display frame comes first; it runs a
  retrace only to catch up (30 Hz host, slow frame) or when no display frames come for 100 ms. Paused: no timer.
  Every 600 retraces: `rtw: pacing N retraces: R retraces/s, D display frames/s, timer wake-ups T/s (K ran a
  retrace), L late`. e2e (F2) checks R = 60 +- 2, wake-ups - K <= 3 + 2 % (no idle chain) and 600 - K >= 60 % of the display
  frames (display-paced); K itself is no limit: it is the display deficit plus every display frame more than half a
  field late (headless Chrome: K 28 / 107 / 193 on one build). Measured in headless Chrome (SwiftShader): 59.6-60.1 retraces/s on 56-59 display frames/s,
  1.5-9.8 timer wake-ups/s, every one of them ran a catch-up retrace (no idle wake-ups).
- **Input**: port 0 only (the game has one controller); buttons as sent (OSContPad bits); stick -127..127 scaled to
  an N64 stick's +-80. The first 200 pad changes are logged (`rtw: pad0 buttons XXXX stick X,Y (retrace N)`).
- **Video**: `rtw_resize(w, h, dpr)` -> a 4:3 canvas of the view x dpr (320x240 .. 1280x960), applied before the
  next retrace; the VI blit letterboxes to 4:3 (render only: the N64 images stay at their native size). F2, option 2
  Sharp: the canvas is the view x dpr itself (any shape, at most 4096 a side) so the page shows it 1:1; the glue reads
  `port_display_sharp()` (exported) before every retrace and resizes when it changes (the menu's live preview too). In the
  headless phone emulation the sandbox reported dpr 1 (canvas 392x294 / 420x315).
- **Audio**: `rtw_audio_rate/drain/buffer` from S7 (32006 Hz, int16 stereo, 16384-frame ring).
- **Save**: `rtw_save_ptr/size/generation` from S8. A whole save happens inside one retrace, and retraces never run
  inside a host call, so the host only ever sees complete images.
- **Rumble**: `__osMotorAccess` -> `port_rumble` -> `rtw_rumble(channel, on)`. Not reached: the port reports no pak,
  and the game never starts the motor (`contRumbleStart` has no caller; only `scPreNMIDefault` sends init + stop).
- **Options** (F1, F2): export `rtw_option(id, value)` -> `port_option_set` (pc_options.c); import
  `rtw_option_changed` called when the game's Options menu changes one (1 Snap button, 2 Pixels, 3 Motion Aim). `rtw_input` passes bit 16 (Snap) to `port_set_pad`.
- **Motion** (F3): export `rtw_motion(port, yaw, pitch)` (port 0; 1/1024 degree, host-corrected for orientation)
  -> `port_motion_add`; logged once (`rtw: motion aim: first rotation from the host`). **Shutter** (F4): `rtw_input`
  keeps bits 0-17; a bit-17 press seen by any `rtw_input` reaches the next retrace.
- **Mouse** (F6): import `rtw_pointer_lock(on)` (`port_pointer_lock`, called by pc_options.c when the module starts or
  stops wanting the mouse: camera control in a course / pause menu, end of the course, option 4 off; logged `rtw:
  pointer lock asked | given back`); export `rtw_mouse(dx, dy, buttons)` -> `port_mouse_add` (logged once: `rtw: mouse
  aim: first mouse event from the host`). Host behaviour read in ~/Code/retrowser (player.tsx, runner.ts): the host
  sends `rtw_mouse` once per display frame while it holds the lock, when the motion is not 0 or the buttons changed
  (movementX/Y summed, clamped to +-32767); the capturing click happens before the lock, so it never reaches the module;
  Esc releases the lock, the buttons go to 0 (one `rtw_mouse` with buttons 0 follows) and the prompt stays while the
  module wants the mouse; `rtw_pointer_lock(0)` exits the lock and removes the prompt.
- **New manifest key and the check tool** (TODO, F3 / F5): `controls.motion` is new in the host's interface, and F5's
  object form `{ "when": 3 }` is newer still (retrowser branch `motion-option`, not deployed yet). A check tool / host
  whose schema does not have a form rejects the manifest (`additionalProperties: false`, `type: boolean`), so
  `build.sh` checks the shipped `retrowser.json` as written and, if the tool rejects `controls.motion`, ships
  `motion: true` instead (prints `rtw: TODO: ...`), then no key; the source manifest keeps the object. With
  `motion: true` the host offers its own ☰ Motion aim and the module still needs option 3 on. `rtw_motion` is reported
  as an unknown export (a warning) by such a tool.
- **Imports**: `rtw_log`, `rtw_build_id`, `rtw_rumble`, `rtw_option_changed`, `rtw_pointer_lock` (F6). Not used yet: `rtw_export_file` (album photos as PNG would
  be a feature behind an option).
- Test hooks: `Module.rtwManual` (no pacing; `node_host.mjs` steps `_port_rtw_retrace` itself), `port_rtw_test_log`.

## ROM-freedom: template + fill (`port/rtw/romfill.py`)

The decomp's own C data is ROM data written as C (sTanTable 8 KiB, sSinTable, Pokémon paths, dialogue text, glyph
tables...): the linked wasm held 24 KB in runs >= 64 bytes, and `rtw.mjs check --rom` failed on two 4 KiB blocks
(sTanTable) with the `.n64` and `.v64` forms. The shipped wasm is a template:

- Build: the ROM in `.z64`, `.n64` (= vbe word order) and `.v64` order is indexed (32-byte windows every 16 bytes,
  >= 8 distinct values); each game object's data (wasm-ld map, `port/build/vobj/*` and the merged string pool;
  adjacent sections of one object merged, so text crossing string literals is found) is slid over; hits are extended
  both ways. Runs >= 16 bytes are zeroed and listed (address, length, ROM offset in that order's image, order).
  Addresses come from the map, file offsets from the final wasm's data segments (the map's offsets are stale after
  wasm-opt). The tool applies its own table and writes nothing unless the result is the original wasm byte for byte.
  Now: 345 runs, 57,575 bytes (47,154 non-zero), table 5,536 bytes.
- Run time (`rtw.c romfill`): before `main`, from the ROM the host gave, FNV-1a of the filled bytes checked
  (`rtw: template filled from the ROM: 345 runs, 57575 bytes, hash e9505431 (OK)`).
- **Trap found**: vbe.py's `__vbe_fix` constructors convert initialised data in place at instantiation, i.e. before
  any fill: the first template build showed word-reversed text ("etnEoy rn ru…") and broken glyph tables. In this
  target the 1602 conversions are queued (`port_vbe_fix_defer = 1`) and run right after the fill
  (`port_vbe_fix_run`), in constructor order.
- Gates: `rtw.mjs check --rom` OK with the `.z64` and the player's `.n64`; `romscan.py` (finer: runs >= 64 bytes, all
  three orders, every file): 0 runs (at 32 bytes: 5 windows, 160 bytes, coincidences in code / short strings).
  Equivalence: `node_host.mjs` 3000 retraces (A every 120) on the template and on the unstripped link: tasks.txt
  identical; vs the harness `port-test` with the same input: task sequence and command counts identical, PCM hashes
  identical (the DL hashes include wasm addresses and differ between the two links).

## Tests (`port/rtw/e2e.mjs`, local host: built `dist/` via serve-worker, GitHub mock, headless muted Chrome)

desk (ROM picker with the player's `.n64`, remembered): template OK, boot, input (A, Start, stick), audio drained and
played by the host's AudioWorklet, pacing, pause (no retrace while paused), save import -> restart with the image ->
Continue -> Oak's lab -> Save (the game writes the flash: generation 129, 1 erase + 128 programs; the host stores
`pokemonsnap-us:flash`, 131072 bytes, `snap_save.py` version + MD4 OK), reload -> started with the stored save ->
"I have been waiting for you, AAAAAAA."; touch (390x844 portrait, 844x390 landscape, isMobile + hasTouch): the
host's N64 layout around the picture, the touch A reaches pad 0; wrong (one byte changed): the host warns, then the
module refuses; offline (network off, host and mock stopped): reload boots from the caches and the remembered ROM,
no request reaches the mock.

motion (F3 + F4, opt-in: `RTW_REPO=~/Code/retrowser-wt-motion PHASES=...,motion`, a host with ☰ -> Motion "Off /
Motion aim" and Full-screen landscape; place it before `offline`, which stops the servers): phone landscape 844x390,
dpr 3, touch, the screen angle set through CDP, DeviceMotionEvent.requestPermission stubbed the iOS way (first grant
only from a tap). The host's ☰ is opened by a real touch tap (CDP touchStart/touchEnd, not page.click) at 90 and 270
in the normal layout and in Full-screen landscape while the game runs: the menu opens and stays open (the host acts on
pointerup and swallows the click after it). The devicemotion events are a simulated upright phone: rotationRate from a
device-frame rate and accelerationIncludingGravity = gravity turned by that rotation, so the host's label/unit
calibration can lock (a roll about the screen's normal: gravity turns, the aim does not). Full-screen landscape (no
bar, Shutter an ordinary extra button) and Motion aim chosen in the host's menu; the SAVE continued to the Beach (A
every 2 s for 75 s, then Z held + A until the cart moves: the tutorial stops it at "Press Z to aim" until 3 photos).
The roll locks "axes: spec (locked)"; a turn about gravity at 90 deg/s for 1 s -> `[motion] rotation stopped ...
yaw +90.4` within 5 % of rate x time, left the same negative, likewise at landscape 270 (a quarter turn in the
pictures); Off -> no `[motion]` line. A touch tap on the Shutter button takes exactly one picture: `[snap] idle ->
enter -> aim -> shoot -> leave -> idle (shutter)` and `[snap] photo taken ..., film left N (shutter)` one below the
previous, again with a finger held on the stick. Then a fresh page with WebKit's rotationRate labels (alpha/beta/gamma
= rates about x/y/z): Chrome is not WebKit, so the host guesses the spec's labels (the roll turns the camera a few
degrees before the lock), the roll locks "axes: webkit (locked)" and the turn right is again +90 within 5 %. Test
notes: a Playwright screenshot puts its own emulated orientation (90) back over the CDP override, so none is taken
between the turns at 270; a turn the game drops (a message or a Pokemon's scene: "... degrees dropped outside camera
control") is tried again. (The host's earlier corner trigger zone and "Knock to shoot" were removed by the user.)

F5: `options` also turns Motion Aim On (option 3) and checks that the Snap touch button (`"when": 1`) leaves the
touch layout at once; `menu` (opt-in) takes phone portrait / landscape pictures of the Options menu; `motionopt`
(opt-in, `RTW_REPO=~/Code/retrowser-wt-motionopt`, a build made with that tool so `motion: { when: 3 }` ships): no ☰
Motion choice, no motion while option 3 is Off, the host's Allow prompt when the game turns it On, rtw_motion after
the tap, a reload passes it back and asks again, Off stops the sensors.

F6: `mouse` (opt-in, desktop 1024x768, main host): option 4 Off then On in the game's Options menu (reported,
passed back after reloads), no pointer lock in the title / menus; the SAVE continued to the Beach: `rtw: pointer lock
asked` and the host's prompt in the course, the capturing click not seen by the game, left clicks take the tutorial's
pictures (`[snap] photo taken ... (mouse click)`) until the cart moves, 400 px right / left = +-32 degrees and 200 px up
= +16 (the `[mouse] movement stopped` line, +-5 %), right button held = zoom + a left click's picture + zoomed out on
release, a middle click's apple, the pause menu gives the mouse back (no prompt) and camera control asks again, Esc
(`document.exitPointerLock`: the Escape key does not release it in headless Chrome) leaves the prompt and the module
does not call again, the click that captures again takes no picture. The `options` phase steps over the Mouse Aim row;
`menu` visits 4 port rows.

Checklist items not covered: a real phone (WebKit worker, real dpr), a gamepad, export of the save into an emulator.
