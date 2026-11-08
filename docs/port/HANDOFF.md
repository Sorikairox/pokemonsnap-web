# HANDOFF

Newest session first. Read `docs/port/PLAN.md` (goal, survey, approach, phases, rules) before working.

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
