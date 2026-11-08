# HANDOFF

Newest session first. Read `docs/port/PLAN.md` (goal, survey, approach, phases, rules) before working.

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
