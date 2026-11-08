# HANDOFF

Newest session first. Read `docs/port/PLAN.md` (goal, survey, approach, phases, rules) before working.

## S7 + S8 + R1 — 2026-11-09: audio, FlashRAM save, renderer fixes (parallel worktrees, merged)

Three sessions ran in parallel worktrees from `web` 834301c and were merged (e9471c3: S8 with S7, harness and build
options combined; d15b58f: R1). Session notes: `docs/port/notes/S7.md`, `S8.md`, `R1.md`; designs: AUDIO.md,
SAVE.md, RENDERER.md ("Session R1").

**S7 — audio** (760eb55, fd40ba5, a9b9f28, a360b11): aspMain HLE (`port/audio/audio_hle.c`: libaudio's 15 ABI
commands, ADPCM, resampler table read from aspMain's data, envelope mixer, reverb) and a virtual AI
(`port/audio/audio_ai.c`, `osAi*`, AI_LEN/AI_STATUS reads, `port_ai_retrace`). Before S7 the game read AI_LEN = 0
and sequences ran 1.2x fast; now 528/544-sample frames at 32006 Hz, 0 FIFO-full. Exports `rtw_audio_rate/drain/buffer`
(int16 stereo). Tests `port/audio/test/run.sh` (5 checks), `node_run.js --wav`, `run.mjs --audio [--play]`,
`wav_report.mjs` (tempo / tuning within a few cents, 0 clipped). Cost 0.04-0.08 ms per audio task.

**S8 — save** (4d0d64d, ffae7c4, 7f86f49, e44e1a5, 4a915a3): the game's own `src/os/osFlash.c` compiled as game code
over a Macronix FlashRAM chip model (`port/save/flashram.c`, PI device: commands, status, erase, page program);
128 KiB image in console byte order, generation counter, `rtw_save_ptr/size/generation` (`port/save/save.c`). Save
struct flag words 0x64 / 0x68 MSB-first under `#ifdef PORT`. Harness `--save IN` / `--save-out OUT` (node) and
`--save IN` + OUT/flash.fla (browser); `port/save/snap_save.py` decodes / checks an image (game's MD4 variant).
`UnkIndigoHalibut.unk_18_*` does not reach the save.

**R1 — renderer** (c90a4b4, 5c16425, 03a84dc, 4a205d1, 191d14e, 9c47f03, ccca0e0): texture wrap modes are sampler
state set at every bind (grey cliffs / end tunnel); CPU side-plane clipping of eye-plane triangles like F3DEX2_NoN
(sky box corners); the RSP's othermode copy reloaded per task / ucode load (intro "mist", every world triangle had
leaked `G_ZS_PRIM`); primitive depth in screen-z units; debug pixel probe (`port_gfx_set_probe`, not exported to the
page). The darker band on Oak's check is the game's own sprite (no change). 4 new gfx tests, each failing without
its fix.

Orchestrator verification after merging (d15b58f): `port/gfx/test/run.sh` all OK (22); link OK; full browser route
with `--audio` (S8 route: Beach -> camera check -> Oak's check -> lab -> Save) reaches Oak's check (Pidgey **Total
2810**) and saves in the lab at retrace 17961 (`[save] flash generation 1006`; `snap_save.py`: version_ok + md4_ok);
reload with `--save` shows "I have been waiting for you, AAAAAAA."; matching build `build/pokemonsnap.z64: OK`.

### Remaining / next

1. Album not exercised (no route uses the Album Mark); bitfield structs still in clang order elsewhere (S8 notes 3:
   `cave.h SomeBits`, `app_level.h EnvSound`, `photo_check.h Photo.unk_1A_*`): check whether ROM data meets them.
2. Renderer: near-plane region removed by GL (the console clamps Z); clamp + mask larger than the mask repeats in GL;
   texture LOD / 3-point / dither / chroma key not implemented (none needed so far).
3. No console / emulator reference yet for audio loudness and reverb, score pixel counts, sky shading.
4. Read-back journals are per build: regenerate after every change for node replays.
5. Next: S9 retrowser module (rtw_* exports wired, worker build, manifest, check --rom, local host e2e).

Downloads: none.

## S6 — 2026-11-09: photos, Oak's check scored, node follows the browser (read-back journal)

Root causes, each fixed in the renderer / harness (no game C edits; matching build unchanged), one commit each:

1. **White thumbnails** (2a44b1d): `window/847B60.c` renders each photo into a gtlMalloc'd **320x210** colour image
   and averages it 2x2 into the camera-check / album sprites with the CPU right after `gtlDrawOne`, without
   `osInvalDCache`. The renderer only wrote back images narrower than 320 at task end (the thin sky strip was rows an
   earlier write-back had covered). Now every image the VI never showed is written back at task end.
2. **No Pokémon identified / Oak's Mark disabled** (2e19d56): `score.c` (`func_800A007C`) re-renders the photo at
   64x48 and counts each Pokémon's pixels **in Z buffers** with the CPU (`!= 0xFFFC`, buffer equalities, a Z copy
   through COPY texture rectangles); the renderer's depth never reached RDRAM, so every count was 0,
   `score.pokemonInFocus` 0 (camera_check `func_camera_check_801E3D8C`), Oak's Mark did nothing. Z images are now
   RDRAM-coherent depth textures in the console's 16-bit Z format (RENDERER.md "Z images"); COPY keeps the alpha bit.
   gfx test `z_writeback` (18/18).
3. **Route input** (da9cf3b): the camera-check menus move with the stick only: `--input F:MASK[:LEN[:X[:Y]]]` and
   `--tap-end F`, node and browser.
4. **Node parity** (0c995f2): read-back journal (RENDERER.md): a browser run records every byte write-backs change per
   CPU-observation point; `port-test --readback OUT/readback.bin` replays it. Chosen over a software rasteriser for
   the 8x8 regions: exact by construction (any approximation diverges at the first focus test), no per-region
   special cases, covers Z / photo / score write-backs too. Cost: one journal per route and build (26 MB for Beach).

Route (Beach -> camera check -> Oak's check -> lab), `A=$(for f in $(seq 14400 120 16200); do printf ",%d:0x8000:3" $f; done)`:
`node port/web/run.mjs --out port_work/s6_d --frames 16500 --every 3000 --shots ... --tap 120 0x8000 --tap-end 13300
--input 2940:0x4000:3,4000:0x2000:400,13400:0x4000:3,13460:0:3:0:-80,13500:0:3:0:-80,13560:0x8000:3,13680:0x8000:3,13800:0x8000:3,13920:0x8000:3,14040:0x8000:3,14160:0x8000:3$A`
(B at 13400 keeps the Pidgey pick, stick down x2 -> "To Prof. Oak", A's through the confirmations and Oak's comments).

Verified (screenshots looked at, `port_work/`, never committed):
- `s6_a/f13200.png`: camera check, 3 thumbnails now show the Pidgey photos (rail, cliffs, sky, Pidgey).
- `s6_b/f13350.png`: Oak's Mark -> "I've decided on PIDGEY" with the full-size photo; `s6_c/f13540.png` marked photo,
  "To Prof. Oak"; `f13600` "Show this picture to Prof. Oak?"; `f13720` "PROF. OAK'S CHECK" title fade; `f14050`
  "This is PIDGEY!!"; `s6_d/f14600..f15600`: Size 490, Pose 800, Technique x2 ("right in the middle of the
  frame"), Same PKMN 230, **Total 2810**, "Wonderful! Your work is impeccable."; `f16500` back in Oak's lab.
- Determinism: `s6_d` vs `s6_e` (same route): 29/29 PNGs, `[task]` logs and journals identical.
- Node parity: `s6_node_c` (14400) and `s6_node_d` (16500) with the journals: `[task]` hashes identical to the
  browser (8310 / 10223 tasks); without the journal (`s6_node_c_norb`) they differ from retrace 3833.
- `port/gfx/test/run.sh` 18/18; node `port-test` 5000 retraces OK (`s6_node5000`); matching build OK; browser cost
  unchanged (~7.4 ms per retrace; Z resolves only on score renders).

Verified vs assumed (Oak's score): the arithmetic on screen follows score.c ((490 + 800) x 2 + 230 = 2810), Pidgey is
identified, "well framed" means a Pidgey pixel in the 4x4 centre of the 64x48 Z buffer, the same-Pokémon bonus
matches a second Pidgey visible in the thumbnail. **Assumed, not verified** (no emulator reference): the exact pixel
counts (rasterisation at 64x48 with GL vs the RDP's edge rules: Size 490 = proximity x completeness could differ by
a few %), and absolute Z values (only "far" and equalities are used by score.c). Pose 800 comes from the photo data
(pose table), not from pixels.

Goal 4 (film stuck at 57): **expected with that route**, not a bug. Photos are only taken zoomed in
(`updateCameraZoomedIn`, player.c:1363); the route holds Z only 4000..4400, so after the tutorial's 3 photos the
camera is zoomed out and A does nothing (zoomed-out A = items: none on a first run). `func_8009BBF4()` counts photos
with a Pokémon in focus (`makePhoto(gPokemonInFocus)`, 47380.c `D_800AE27C++`): the tutorial releases input after 3
of those. To take more photos the route must press Z again.

### Remaining bugs / Next

1. Renderer (not read-backs, unchanged): sky box corner rectangles, grey untextured cliffs / end tunnel (S5 notes);
   Oak's check screens show a darker left band (x < 230, `s6_d/f14600.png`): probably a transition overlay, check.
2. Score pixel counts vs the console (emulator run of the same photo, locally) when a reference is available.
3. `camera_check.h UnkIndigoHalibut.unk_18_*` single-bit bitfields are not in PORT LSB-first order: harmless while
   only C reads them; check if they reach the flash save (S8).
4. Journal size (26 MB per Beach route): fine for tests; compress or record only CPU-read ranges if it grows.
5. Next: S7 audio, S8 flash (album photos written there), S9 retrowser module; route Z presses later in the level for
   more photos (Doduo, Pikachu...) to exercise more scoring cases.

Downloads: none.

## S5 — 2026-11-09: Beach played start to end (world, Pokemon, detector, photos) by scripted input

Four root causes, each fixed generally (no game C edits; matching build unchanged), one commit each:

1. **Function identity** (a2547a9): world/block.c compares `gfx->renderFunc` with `&drawSkyBox2Cycle`,
   `&renRenderModelTypeA`... The block descriptors are in beach_assets' unmapped bin (A74F0.bin, no relocations), so
   renderFunc was the mirror address 0x108A1530 (console 800A1530 = renderModelTypeAFogged), never equal to a table
   index: **every world block GObj was deleted** (evidence: `[dbg] block 0 renderFunc 0x108a1530 (A 0x1eb ...)`).
   vbe.py `fptr_eq` + `__port_fptr_canon`: pointer eq/ne tests compare canonical functions (MEMORY.md "Function
   identity"). The "camera never reaches its start position" lead was wrong: the camera moved along the rail all along
   (`gMovementState` and lookAt logged moving); the 1020.22 depths were the first frames' LOD tests.
2. **Void spawn wrappers / v0** (64b25a7): as decided, in the build tools: `port/tools/v0_scan.py` (`run.sh v0`,
   v0 at every `jr ra` of the IDO objects -> `port/symbols/v0.tsv`), `v0.py` + `v0_plan`: 275 of 3288 void functions
   return the v0 of their last call (all 22 spawn wrappers among them; console data-flow and IR must agree);
   `port/v0_report.tsv` lists every void function, direct sites using its value (0) and the resolution; indirect
   value calls of the rest are logged at run time (0 on the route). `[icall] void targets called as values`: 2 -> 0.
3. **bss overflow into clang's neighbours** (7e62e71): `PlayerMatrixStore` (declared 4 bytes + VLA, written 0x34
   bytes) overflowed into IsTutorialEnabled / MainCameraBorder*; `mainCameraSetScissor` then emitted `ED FFF000`
   (ulx saturated) and the world vanished again once Pokemon spawned (evidence: render-target probes black before the
   first detector copy; scissor words in `--logops`; wasm map neighbours). vbe.py `bss_layout`: each TU's bss is laid
   out as on the console (733 globals, aliases into one array per section).
4. **RCP completion timing** (20459ff): SP/DP events were posted inside osSpTaskStart, so the frame's GFX_END task
   never found a running gfx task: no `scPostProcessFunc`, **the Pokemon detector never ran** (evidence: sched.c log
   `fb 0` at DP full sync in the level). Now posted when no thread is runnable (`port_rcp_complete`).
Plus e02252b: `port_game_log` (printf from game C, vbe format string) for debugging.

Verified (screenshots looked at; all in `port_work/`, ROM-derived, never committed):
- Route: `node port/web/run.mjs --out port_work/s5_route_a --frames 12600 --every 300 --tap 120 0x8000 --input
  2940:0x4000:3,4000:0x2000:400` (A every 120, B at 2940 = Beach, Z held at the tutorial). `s5_route_a/sheet.png`
  (contact sheet): f03900 Pidgey + "Press Z to aim", f04200 zoomed view with the CPU red dot and "Press A to shoot",
  then the whole course: Doduo, Pikachu, Butterfree, Snorlax asleep (f06600, Z effect), Meowth/Kangaskhan/Scyther
  areas, river, end tunnel, f12300 the end gate, f12600 fade out. `s5_route_end/f13300.png`: the camera check
  ("I'll choose which pictures to show to Prof. Oak!") with 3 photo frames. Earlier: `s5_web13/f04150.png` zoom +
  red dot + "PIDGEY NEW", film 60 -> 57; `s5_web8/f04200.png` Doduo + Pikachu after the bss fix.
- Detector (S6 owns correctness): Pidgey (16) found in focus at retrace ~3831 (8x8 centre read-backs), Doduo (84)
  later; the tutorial waits for Z, the zoom shows the red dot, A takes photos. Node has no GL, so it never finds a
  Pokemon in focus: node and browser now diverge from the first focus (identical task hashes before).
- Determinism: two browser route runs (`s5_route_a`, `_b`): 42/42 PNGs and task logs identical; node: two 5000-retrace
  runs, tasks.txt identical.
- Threads (PLAN risk 2, node `s5_node14`, Beach start to end): peak 45 live threads, 323 created, 56 switches per
  retrace (max 84), 0.15 ms per retrace without a renderer, 1.31 us per switch (`--bench`): not a risk. Browser
  (SwiftShader): ~7.4 ms per retrace for game + renderer.
- `port/gfx/test/run.sh`: 17/17 checks OK. Matching build: `build/pokemonsnap.z64: OK`.

### Remaining bugs / Next

1. S6 (read-backs): camera-check thumbnails are white with a strip of sky (`s5_route_end/f13300.png`); score and
   photo-check not exercised. Node cannot reproduce the browser's game state past the first detector frame (no GL):
   consider a CPU fallback for the 8x8 centre read-back in node, or route tests in the browser only.
2. Renderer: sky box shows lighter blue rectangles at the top corners (`s5_route_a` f04200..f05400); some cliffs and
   the end tunnel look flat grey / untextured (`s5_web9/f08000.png`, `f11500.png`): check against an emulator.
3. After the tutorial the film stays at 57 although A is tapped every 120 retraces: IsInputDisabled stays true until
   `func_8009BBF4() >= 3` (app_render/47380.c counter); confirm on the console whether the first run behaves so.
4. Indirect value calls of non-promoted void functions are only logged at run time; route the other levels (S10) and
   watch `[icall] void target`. `bss_layout` does not cover initialised `.data` or overflows across TUs.
5. S7 audio, S8 flash, S9 retrowser module as planned.

## S3 (S3a + S3b + S3c) — 2026-11-08: first frames in the browser; title -> menus -> Oak's lab by scripted input

S3a (CPU/OS) and S3b (renderer) ran in parallel worktrees and were merged into `web` (82b30b0); S3c wired them
together and tested the game in a browser. Session notes: `docs/port/notes/S3a.md`, `S3b.md`; designs:
`docs/port/MEMORY.md` (threads, VI/SP/DP/AI, DMA, overlays, indirect calls, bitfields, caches), `RENDERER.md`.

**S3a — libultra on fibers** (51dbefd, 2675be9, 7f06f8d): OSThreads as Emscripten fibers with fixed-address stacks,
libultra priority scheduling, message queues/events, deterministic clock; VI retrace driven by the host
(`port_vi_retrace`), SP/DP tasks synchronous, PI DMA from the ROM with relocation of every DMA'd segment range,
overlay residency + C data reset, controllers (one pad, no paks), FlashRAM image, site-typed indirect-call
trampolines + console-address -> function table (keyed by the resident overlay), `k0_ptrs` (console KSEG0 pointers
-> mirror), MSB-first bitfields under `#ifdef PORT`, the anti-copy check in C (`app_level_CheckIllegalCopy` /* was
func_80364360_504770 */). All 63 S2 stubs have bodies. Node: boot -> intro -> main menu -> Oak's lab (`--tap 120
0x8000`), deterministic task log, 1.0-1.3 us per fiber switch, 0.1-0.7 ms per retrace without a renderer.

**S3b — renderer** (fd99cb0): `port/gfx/` F3DEX2 + L3DEX2 interpreter (RSP on the CPU, RDP on WebGL2: byte-exact
TMEM, generated combiner/blender shaders, Snap's `gSPMvpRecalc` + `G_MW_MATRIX` patches) with colour images kept
coherent with RDRAM at native resolution (detector regions, score thumbnails, CPU red dot). Offline harness
`port/gfx/test/run.sh` (hand-made lists, no ROM): 17 checks.

**S3c — wiring and the browser** (76a8fdf, 584f977, docs commit):
- One owner of the SP/DP events: `pc_rcp.c` posts them after `port_gfx_task` returns; the renderer no longer calls
  `port_sp_task_done` / `port_dp_full_sync` (removed; `port_gfx_stats.fullsyncs` counts G_FULLSYNC, test adjusted).
- `port/gfx/gfx_port.c`: `port_vi_present(fb, vi_mode, black)` -> `port_gfx_vi_frame(fb, comRegs.width)`; NULL fb or
  `osViBlack` = black. `osInvalDCache` / `osWritebackDCache(All)` -> the renderer's cache hooks (KSEG0 -> mirror).
- `port_build.py`: `port/gfx/*.c` in the PC layer, `--js-library port/gfx/gfx_gl.js -sMAX/MIN_WEBGL_VERSION=2`;
  `gen_stubs.py`: `gl*` / `port_gfx_gl_create` are JS imports, weak undefined symbols are references. Node
  `port-test` unchanged (no canvas: lists interpreted, nothing drawn; 0 unknown opcodes, 0 bad addresses).
- **Browser harness `port/web/`** (index.html + worker.js + run.mjs, see RENDERER.md "Testing with the game"): the
  module in a worker on an OffscreenCanvas (retrowser's model), `port_vi_retrace` once per frame, frame-indexed
  input, screenshots at given retraces, `--logops` dumps + `port/tools/dl_decode.py`. Node got the same `--input`,
  plus `--logops` and `--threads` (`port_thread_dump`).
- All address forms from S3a's census are handled by `gfx_addr` (wasm >= 0x10000000, KSEG0, segmented with wasm
  bases unmasked, physical < 8 MiB, `G_SETCIMG 0F000000` and mirror addresses): 0 bad addresses in 6000 retraces.

Verified (screenshots looked at, all in `port_work/`, ROM-derived, never committed):
- `node port/web/run.mjs --out port_work/s3c_web1 --frames 300 --every 30`: f00030 the N64 logo (correct colours
  and shading); f00090..f00300 the intro flyover (sky, hills, river, trees, fog; fades; a faint "Nintendo" logo).
- `... --out port_work/s3c_web2 --frames 1800 --every 60 --tap 120 0x8000`: f00360 title "Pokémon Snap" + PRESS
  START, f00480 New Game / Options, f00540..f01200 name entry (photographer card, alphabet grid, AAAAAA typed),
  f01560 / f01800 **Oak's lab** (background, Oak, dialogue box, side menu). Same overlay sequence as node.
- `... --out port_work/s3c_web7 --frames 6000 --every 150 --tap 120 0x8000 --input 2940:0x4000:3`: Oak's dialogue,
  f03000 the monitor showing a 3D model, f03450 course select (island map, "Should I take this course?"), retrace
  3625 Beach loads (app_level, beach_code/assets, magikarp, pikachu); f03750+ in-level HUD (film 60, crosshair, Z
  icon) on a **black, frozen world** (see Next 1).
- Determinism: two runs of 1800 retraces (`port_work/s3c_det_a`, `_b`): 12/12 PNGs identical, task logs identical;
  node (`port_work/s3c_node4000`) and browser give the same `[task]` hashes for 4000 retraces on one build.
- `port/gfx/test/run.sh`: 17/17 checks OK. Matching build: `build/pokemonsnap.z64: OK`.
- Cost: 5.6-6.2 ms per retrace for game + renderer in headless Chrome on SwiftShader (first 60 retraces 38 ms:
  shader compiles); `--pace raf` runs at the display's 60 Hz.

Commands: `port/docker/run.sh port` (build), `port/docker/run.sh port-test --frames N [--tap N M] [--input F:M:L]
[--logops F] [--threads F] --out port_work/DIR` (node), `node port/web/run.mjs --out port_work/DIR --frames N
--every K [--tap N M] [--input ...] [--logops F] [--pace raf]` (browser), `port/gfx/test/run.sh` (renderer, no ROM).

### Next (S4/S5)

1. **Beach is frozen with a black world** (same in node: not the renderer). Findings: the level's models are drawn
   through `gsSPBranchLessZraw` LOD lists (part 1 loads vertex 31, the model's reference point; `draw_near` if
   screen z <= ~1005, `draw_far` if <= ~1020, else nothing); the reference vertex sits at z 1020.22 > 1020.11 for
   every model, i.e. the camera has not reached its start position. `--threads 3900`: only 7 object threads live
   (updateMusic, vibrateCamera, BumpDetector_Update, 4 beach processes), no Pokémon. **Trap #4 confirmed**: the
   Pokémon spawn wrappers are `void f(...) { Pokemon_Spawn(...); }` (22 such `void` wrappers with the
   `pokemonInit` signature across levels, some non-trivial) and `pokemonAdd()` (503C10.c) reads their result as
   `GObj*` (v0 on the console, 0 here) and stops spawning the block at the first NULL. Returning the value in the
   14 simple Beach wrappers (experiment, reverted) let spawning go on but the world stayed frozen: a second cause
   remains (the camera/rail process; look at player.c's movement process and what it waits for). Fix the v0
   wrappers generally (a vbe/sigs rule: a void function whose return is preceded by a value-returning call returns
   that value when called through a value-typed pointer) or per function with renames.
2. Then the level (S5): object threads at depth, the detector read-backs (S6) on real frames.
3. Renderer: batch texture rectangles across loads; the intro mist strip's hard edges unverified.
4. Audio (S7), flash persistence (S8), retrowser module (S9) as planned.

## S2 — 2026-11-08: port build skeleton (all game C to wasm32, mirror + ROM loader + relocations, vbe, sigs, link)

- **Image `pokemonsnap-emsdk`** (`port/docker/Dockerfile.emsdk`): the local `emscripten/emsdk:4.0.15-arm64` with its
  sysroot libraries prebuilt by two test links. No download. `port/docker/run.sh port-image | port | port-test
  [--load-all] | port-sh`; `run.sh relocs` (toolchain image) writes `port/symbols/relocs.tsv`.
- **`port/tools/port_build.py`** (census -> C->IR -> sigs -> vbe -> -O1 -> PC layer -> stubs -> link), ~10 s clean,
  incremental, deterministic (two clean builds gave the same wasm SHA-1; final build `309d4069…`). Details: **docs/port/MEMORY.md**.
- **Census** `port/census.tsv`: all 996 objects of the matching link, 0 unlinked `src/*.c`. Compiled through vbe and
  OK: 242 game + 14 game-data + 1 mixed (`52DE0.c`, asset includes under `#ifdef PORT`) + 76 libultra C
  (gu/libc/audio/sp/vimodes) = **333 TUs, 0 failures** (4105 functions). Not compiled, each with its reason: 371 asset
  TUs + 39 asm/data + 138 bin (ROM -> mirror), 87 libultra os/io/debug C + 22 libultra asm + `src/os/osFlash.c` +
  `entry.s` (PC layer), 4 `src/rsp/*.s` (HLE).
- **Link OK**: `port/build/web/pokemonsnap.{js,wasm}` (2.29 MB wasm), 0 link warnings, undefined symbols are errors.
  **Stubs list** `port/stubs.tsv`: 63 logged function stubs (all libultra os/io/flash/cont/gbpak: threads, messages,
  VI, SP, AI, PI, cont, flash), 2176 + 3 mirror labels the link needed, 5 libc names used by the PC layer only.
  No game function is stubbed.
- **Mirror + ROM loader** (`port/ultra/pc_mem.c`): `n64_rdram` 8 MiB at wasm 0x10800000 with 15663 labels;
  `port_set_rom` (z64/n64/v64 -> vbe layout), `port_rom_copy`, `port_load_segment` (copy + bss clear + relocations).
  **Relocations** frozen from the matching objects (33785 pointer words of the ROM-loaded data, names/offsets only)
  and resolved at link time (C objects 209, functions 501 by name = right overlay, mirror 32495, absolute 564).
- **Game C edits** (all `#ifdef PORT`, matching build unchanged): `52DE0.c` (asset includes -> externs), 8 local
  array-from-array initialisers (`5612B0.c`, charmander, charmeleon, gate: `PORT_ARRAY_INIT`), `gtl.c`
  (`gSPLoadUcodeL` token paste), `text.c` (`UIText_PrintString` u8* like window.h). The one `func_` touched is
  renamed: `beach_5612B0_WaitForInteraction` /* was func_beach_802C93B8 */ (also in `tools/symbol_addrs.txt`, since
  splat's asm data refers to it; `symbols.tsv` / `relocs.tsv` regenerated: only that name changed).
- Port fixes to the Conker tools: EUC-JP compile of non-ASCII TUs (Japanese character constants), `-funsigned-char`,
  64-bit `va_arg` loads kept native, sigs.py `sret`/`byval` definition parsing, byval kept on rewritten sites, static
  functions never rewritten, libc-clash check (found acosf/asinf/atan2f/atanf/tanf).
- **Tests run**: `port/docker/run.sh port` (link OK); `port-test` in node with `./pokemonsnap.z64`: ROM title read
  through the vbe layout, main segment loaded, **vbe self-test: the game's own sprintf gives
  "snap -42 beef zubat ! 3.25 -1234567890123" OK**, `game_main()` runs to 4 logged stubs (`__osSetWatchLo`,
  `osInitialize`, `osCreateThread`, `osStartThread`) and returns; `port-test --load-all`: 119 segments, 0 bad
  relocation words. Matching build after the edits: `build/pokemonsnap.z64: OK` (SHA-1 `edc7c49c…baf`).
- **Trap found**: a second `configure.py` with a warm `.splache` writes an incomplete `undefined_syms_auto.txt`
  (link: 3501 undefined `D_` symbols). `run.sh build` now runs `configure.py --clean` (~45 s).
- Provenance: `port/tools/vbe.py` (generic pass) and `sigs.py` are adapted from the Conker web port (the user's own
  repo Sorikairox/conker-eu-port, no licence file); everything else written here. The port compiles ultralib's
  gu/libc/audio/sp/vimodes C (SGI copyright headers, no licence): fine for the private build, part of the licence
  question before any publishing.

### Next (S3: first frame)

1. Threads: OSThread as ASYNCIFY fibers with fixed-address stacks, message queues, VI retrace at 60 Hz, `osSpTaskStart`
   -> renderer / audio synchronously; replace the 63 stubs by the PC layer (`port_prefix.h` can `#pragma weak` them).
2. PI DMA (`osEPiStartDma` -> `port_rom_copy`) and `dmaLoadOverlay` -> `port_load_segment` (relocations are ready;
   decide how overlay C data / bss in wasm is re-initialised when an overlay is reloaded).
3. Indirect calls: site-typed trampolines (Conker icall.py + pc_icall.c), plus the address -> function table for
   pointers computed at run time; bitfields MSB-first (MEMORY.md "Known gaps"); `osVirtualToPhysical` must return
   console physical addresses (audio).
4. Minimal F3DEX2 on WebGL2 in a local test page; the first screen, looked at.

## S1 — 2026-11-08: toolchain image, matching build OK, frozen symbol tables

- User answers (PLAN.md section 7): develop privately (no publishing); local branch `web` only, never push;
  downloads approved for S1, all tooling in Docker. ROM: `Pokemon Snap (USA).n64` at the repo root (gitignored).
- **Docker**: Docker Desktop 4.73 runs; its CLI is `/Applications/Docker.app/Contents/Resources/bin/docker`
  (`/usr/local/bin/docker` is a dead OrbStack link). Scripts add that dir to PATH themselves.
- **Image `pokemonsnap-tools`** (838 MB, `port/docker/Dockerfile`, from local `ubuntu:24.04`): binutils-mips-linux-gnu
  2.42, cpp/gcc (pygfxd builds from source), ninja, pigment64 0.6.3 (cargo, Rust 1.85, builder stage), IDO 5.3/7.1
  static recomp **v1.2 linux-arm**, asm-processor 1.0.1 aarch64-linux, uv 0.9.5 (ghcr.io/astral-sh/uv) + the locked
  deps (`uv sync --frozen`, Python 3.11.14) in `/opt/venv`. Deviation from upstream: `configure.py --setup` is not
  used (on linux/aarch64 it fetches x86_64 IDO v1.1 / asm-processor); `run.sh build` copies the arm tools from `/opt`
  into `tools/ido5.3`, `tools/ido7.1`, `tools/asm_proc` (copies, not symlinks: upstream's `tools/ido5.3/` ignore
  rule only matches directories). The checksum proves equivalence.
- `port/docker/run.sh`: `image`, `rom PATH` (`port/tools/baserom.py`: any byte order -> `./pokemonsnap.z64`, SHA-1
  checked, source only read), `build` (configure.py + ninja, ~42 s), `symbols`, `sh`.
- **Matching build reproduced**: `ninja` 2513/2513, `build/pokemonsnap.z64: OK`; host `shasum` = `edc7c49c…baf`.
  Clean rebuild (build/ asm/ assets/ removed) gives the same ROM and byte-identical tables.
- **`port/symbols/`** (names, addresses, ROM offsets, sizes only; `port/tools/gen_symbols.py` = map + ELF, stdlib +
  PyYAML, run in the image):
  - `segments.tsv` 124 segments (54 code, 56 bin, 9 effect sprites, 3 vpk0, ...): ROM range, VRAM, bss, exclusive_ram_id;
  - `objects.tsv` 1422 input sections (object, section, VRAM, size, ROM, segment);
  - `symbols.tsv` 22993 ELF symbols (4269 func, 16431 data, 2293 none) with ROM offset, section, segment, object.
  - Built-in checks: map vs ELF section address/size, `*_ROM_START` vs load address, ROM ranges contiguous to 16 MiB.
    Extra check (scratch script, not committed): all 121 loadable ELF sections equal the ROM bytes at their
    `segments.tsv` offset, and the bytes of all 4252 sized functions match at their `symbols.tsv` ROM offset.
- **`port/symbols/asset_tus.tsv`** (`port/tools/asset_tus.py`): 372 TUs include `assets/…` (5686 includes);
  **371 are data only** (the port does not compile them; their symbols become mirror labels), **1 mixes code and
  assets: `src/app_render/52DE0.c`** (15 functions + 3 gfx / 3 vtx includes: compile with the includes stubbed out).
  Also ROM-derived and not for the port: `src/rsp/*.s` `.incbin` the RSP microcode (aspMain, F3DEX2, L3DEX2, rspboot);
  the 56 `bin` segments (audio, fx, textures) are read from the ROM at run time.
- Commits: branch `web` created locally (from `49e0b80`), one commit with docs, .gitignore, port/docker, port/tools,
  port/symbols. Nothing pushed; ROM, `pokemonsnap.z64`, build/, asm/, assets/, tools/ido*, port_work/ all ignored.

### Next (S2: port build skeleton)

1. Port build target in the local `emscripten/emsdk:4.0.15-arm64` image (no new download): compile all game TUs
   to wasm32, code only, from `objects.tsv` / `asset_tus.tsv`; census of every TU (none silently dropped).
2. Data-only C files that don't include assets but point at asset symbols (model/anim tables): decide per TU between
   compiling them (with relocations into the mirror) and loading them from the ROM; list them from `symbols.tsv`.
3. Mirror + ROM loader keyed by `segments.tsv`; vbe and signature passes (adapt Conker `port/tools/vbe.py`, `sigs.py`).

## S0 — 2026-11-08: survey and plan (no code)

- User: "find a N64 decomp project on GitHub, clone it in ~/Code/reponame and let's try the retrowser skill to make it
  work on retrowser", then "Look for Pokémon snap decomp", then "Write plan and handoff in the repo".
- Cloned **ethteck/pokemonsnap** at `49e0b80` into `~/Code/pokemonsnap` (remote `origin` = ethteck: never push).
  Nothing built, nothing downloaded besides the clone; no ROM on this machine.
- Findings (details in PLAN.md section 1):
  - all 4262 functions are matched C; data 84 % mapped; no licence file;
  - HAL engine (om/gtl/sched; object threads), ~45 overlays sharing VRAM, 372 TUs include ROM-extracted assets;
  - FlashRAM 128 KiB save, Rumble Pak, F3DEX2 + L3DEX2 (+ S2DEX2 in gtl's ucode list), aspMain audio, vpk0;
  - **the photo logic reads back RDP pixels** (pokemon_detect.c focus regions, score.c thumbnails) and the CPU
    draws into the framebuffer: the renderer needs RDRAM-coherent colour images.
- Not done: no branch, no commit (the docs are untracked files in the clone).

### Next (S1)

1. Answers to PLAN.md section 7 (licence, ROM on this machine, private remote and branch).
2. With approval for the downloads (IDO recomp via `configure.py --setup`, uv/Python deps): reproduce the matching
   build in a container, checksum OK.
3. Generate `port/symbols/` from the map (names and offsets only) and list the asset-including TUs the port must not
   compile.
