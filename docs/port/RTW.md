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
| `controls.extra` | bit 16 "Snap" | F1: the Snap button (FEATURES.md) |
| `options` | id 1 "Snap button", bool, default 1, `inGame` | F1: shown in the game's Options menu, kept by the host |
| no `net`, `picture`, `icon` | | nothing needs them yet |

## Build (`port_build.py --rtw`, `link_rtw`)

Same objects and flags as the harness link (ASYNCIFY, `-sGLOBAL_BASE=256MB`, 384 MiB) plus `port/rtw/rtw.c`, with
`-sENVIRONMENT=worker -sDYNAMIC_EXECUTION=0 -sEXPORTED_RUNTIME_METHODS=HEAPU8`, `--js-library port/rtw/library_rtw.js`,
`--pre-js port/rtw/rtw_glue.js`, exports `rtw_*` only (+ `port_rtw_retrace`, `port_rtw_test_log`) ->
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
  retrace), L late`. Measured in headless Chrome (SwiftShader): 59.6-60.1 retraces/s on 56-59 display frames/s,
  1.5-9.8 timer wake-ups/s, every one of them ran a catch-up retrace (no idle wake-ups).
- **Input**: port 0 only (the game has one controller); buttons as sent (OSContPad bits); stick -127..127 scaled to
  an N64 stick's +-80. The first 200 pad changes are logged (`rtw: pad0 buttons XXXX stick X,Y (retrace N)`).
- **Video**: `rtw_resize(w, h, dpr)` -> a 4:3 canvas of the view x dpr (320x240 .. 1280x960), applied before the
  next retrace; the VI blit letterboxes to 4:3 (render only: the N64 images stay at their native size). In the
  headless phone emulation the sandbox reported dpr 1 (canvas 392x294 / 420x315).
- **Audio**: `rtw_audio_rate/drain/buffer` from S7 (32006 Hz, int16 stereo, 16384-frame ring).
- **Save**: `rtw_save_ptr/size/generation` from S8. A whole save happens inside one retrace, and retraces never run
  inside a host call, so the host only ever sees complete images.
- **Rumble**: `__osMotorAccess` -> `port_rumble` -> `rtw_rumble(channel, on)`. Not reached: the port reports no pak,
  and the game never starts the motor (`contRumbleStart` has no caller; only `scPreNMIDefault` sends init + stop).
- **Options** (F1): export `rtw_option(id, value)` -> `port_option_set` (pc_options.c); import `rtw_option_changed`
  called when the game's Options menu changes one. `rtw_input` passes bit 16 (Snap) to `port_set_pad`.
- **Imports**: `rtw_log`, `rtw_build_id`, `rtw_rumble`, `rtw_option_changed`. Not used yet: `rtw_export_file` (album photos as PNG would
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

Checklist items not covered: a real phone (WebKit worker, real dpr), a gamepad, export of the save into an emulator.
