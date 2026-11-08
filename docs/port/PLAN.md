# Pokémon Snap browser port: plan

Goal: **Pokémon Snap (N64, US) as a browser source port on retrowser.app**. The decomp's C is compiled to
WebAssembly, the N64's OS and hardware are replaced by a PC layer, every asset is read from the player's own ROM at
run time, and the result loads from a GitHub link on retrowser.app (standard 1.2).

Read this first, then `docs/port/HANDOFF.md` (latest state). Method reference (read only): the Conker web port's
playbook `~/conker-wt/web/WEB_PORT_ORCHESTRATOR.md` (cited below as **WPO §n**), the GoldenEye retrowser build
(`~/goldeneye/docs/web/RTW.md`) and retrowser's own guide (`~/Code/retrowser/spec/guide.md`, the `retrowser-setup`
skill).

## 0. Starting point (2026-11-08)

- Upstream: **ethteck/pokemonsnap**, cloned at `49e0b80` (2026-10-25). All **4262 functions are matched C** (code
  100 %), data 84 % mapped (decomp.dev). Matching build: splat + IDO 5.3/7.1 recomp + ninja -> `pokemonsnap.z64`.
- ROM: **US**, `pokemonsnap.z64`, SHA-1 `edc7c49cc568c045fe48be0d18011c30f393cbaf`, 16 MiB, NTSC (60 Hz VI).
  The user owns it (S1: `Pokemon Snap (USA).n64` at the repo root, gitignored). Never commit it or anything from it.
- **No licence file upstream.** retrowser shows a licence and the port must be public to load from a link. User's
  answer (section 7): develop privately, no publishing for now.
- No PC or web port exists. Other Snap projects do not fit: andrewnakas/pokemonsnap-cleanroom runs a rebuilt ROM in
  an N64 emulator; Snap64Recomp / sp00nznet are static recompilations for desktop renderers.

Because every function already matches, the Conker gate "prove each C function equivalent" (WPO §4) is mostly done
upstream: the IDO build of this C *is* the ROM. Our risk is the move from IDO/MIPS to clang/wasm (endianness, UB,
call signatures, pointer tricks), not the C itself.

## 1. What the game is made of (survey, splat.yaml and src/)

| Part | What | Port consequence |
|---|---|---|
| `main` (0x80000400) | HAL's engine (the Smash 64 family): `sys/` main, sched, dma, cont, gtl, om (object manager), render, anim, audio, error; ultralib (libultra decomp) | engine runs as is; libultra replaced by the PC layer |
| `app_render`, `more_funcs`, `app_level` | game-wide code: rendering helpers, score, photo data, flash save (`more_funcs/5D500.c`), the on-rails level app | as is |
| ~45 overlays | `world`, 7 levels (`beach` `tunnel` `cave` `river` `volcano` `valley` `rainbow`: `*_code`, `*_assets`, `*_extra`), Pokémon models (+`_hd`), `window`, menus, `camera_check`, `oaks_lab`, `photo_check`, `pokemon_album`, `pokemon_report`, `gallery`, `credits`, `intro_code`; many share VRAM (`exclusive_ram_id`) | all code linked once (wasm has no overlays); each overlay's **data** is loaded from the ROM into the RAM mirror when the game DMAs it (`dmaLoadOverlay`, sys/dma.c) |
| Assets inside segments | 372 C files `#include "assets/…inc.c"` (vertices, display lists, textures) that splat extracts from the ROM at build time | **must not be compiled into the port** (ROM-free rule): the data comes from the ROM at run time (section 3.1) |
| Raw ROM data | fx scripts and sprites, audio `sbk`, 39 sequences, 2 banks + tables (0xAEFC10…) | read from the ROM by the game's own DMA, unchanged |
| Compression | vpk0 (`AA0650.c`, `main_menu/`, `app_render/46270.c`, `app_level/504770.c`) | keep the game's own decoder |
| Threads | main.c: idle 1, scheduler 3 (`scMain`), audio 4, game 5, controller 6; `om` gives **every game object its own OSThread** (om.c 655/705); credits 0x64; error 8 | cooperative fibers with fixed-address stacks (WPO §3.4); many object threads, so fiber switch cost matters |
| Microcode | F3DEX2 (`gspF3DEX2H.NoN.fifo`), L3DEX2, S2DEX2 enum in gtl.c (check use), aspMain audio; sprite library (ultralib `sp/`), YUV texture loads (`window/8456E0.c`, `26D80.c`) | display-list interpreter for F3DEX2 + L3DEX2 (+ S2DEX2 if used); audio HLE of aspMain (libaudio, not n_audio) |
| Save | **FlashRAM 128 KiB** (`osFlash*`, size 0x20000 in 5D500.c): album photos and progress | `osFlash*` on a 131072-byte native image = retrowser `save: { kind: flash, size: 131072 }` |
| Controller | `sys/cont.c`; Rumble Pak (`osMotor*`); Pfs / Transfer Pak (`osGbpak*`) probes in cont.c | rumble -> `rtw_rumble`; Pfs/Gbpak answer "no pak" (check what cont.c does with them) |

### 1.1 The trap that decides the renderer: the game reads its own pixels

Snap's photo logic reads what the RDP drew (why emulators need "framebuffer emulation" for Snap):

- `app_level/pokemon_detect.c`: after each Pokémon is drawn, `PokemonDetector_SaveRegion` copies the framebuffer's
  centre 8×8 (156..163, 116..123; colour image at segment 0xF) into an 8-pixel-wide RGBA16 colour image in RDRAM.
  `PokemonDetector_FindPokemonInFocus` compares consecutive regions: ≥ 32 of 64 pixels changed = that Pokémon is in
  focus. Photo scoring and the focus marker depend on it.
- `PokemonDetector_PostProcessImage` (via `scSetPostProcessFunc`, sched.c ~914) **writes pixels into the
  framebuffer with the CPU** (the red focus dot).
- `app_render/score.c`: colour images 64 wide at segments 0xB/0xC copied from the framebuffer (photo thumbnails
  that end up in the album and the flash save).
- `app_level/player.c`, `sys/render.c`: colour/depth image switches (`viZBuffer`, `0x0F000000`).

So the renderer must keep **N64 colour images coherent with RDRAM**: a colour image the game later reads (as a
texture, by the CPU, or as a copy source) is resolved into the RAM mirror in RGBA16 at N64 resolution; CPU writes
into a displayed framebuffer reach the picture. Detection must give the same answer as the console, so the
detector's draws and copies run at native 320×240 whatever the display resolution. Online/feature rule: render-only
options must never change these read-backs (WPO trap 22).

## 2. Approach (decided, WPO §1 and §3)

Source port to **wasm32** (32-bit pointers like the N64), Emscripten, built in the local
`emscripten/emsdk:4.0.15-arm64` image (Docker; nothing installed on the host without asking). Conker's solutions are
reused, adapted rather than copied blind:

1. **Memory (WPO §3.1)**: an 8 MiB RDRAM mirror at a fixed, size-aligned wasm address; every data symbol without a C
   definition (the asset objects, the 16 % unmapped data) is a label in the mirror at its console address; segment
   data is copied from the ROM into the mirror when the game loads it; a build-time **relocation list** turns
   pointer words into wasm addresses / table indices. Review relocations for words that only look like pointers
   (WPO trap 1). `-sGLOBAL_BASE` null zone.
2. **Endianness (WPO §3.2)**: virtual big-endian memory via an LLVM IR pass on game TUs (Conker `port/tools/vbe.py`);
   ROM bytes enter with one bswap32 per word. Bitfields MSB-first under `#ifdef PORT`.
3. **Calls (WPO §3.3)**: one canonical signature per function, call-site rewriting keeping `zeroext`/`signext`;
   site-typed trampolines for indirect calls (om's object callbacks, render function tables, `scPostProcessFunc`).
4. **Threads (WPO §3.4)**: OSThread = fibers (ASYNCIFY), priority scheduling, the browser drives VI retraces at 60 Hz;
   `osSpTaskStart` runs the renderer / audio HLE synchronously.
5. **Renderer (WPO §3.5 + 1.1)**: F3DEX2/L3DEX2 interpreter on **WebGL2** (fast3d / libultraship family as a base
   if the licence allows, else our own), with RDRAM-coherent colour images and CPU framebuffer writes (1.1).
6. **Audio (WPO §3.6)**: aspMain HLE from documentation (ADPCM, resample, envelope mixer, reverb); the library's
   physical addresses map into the mirror.
7. **ROM, DMA, save, input (WPO §3.7)**: `osEPiStartDma` copies ROM bytes into the mirror; FlashRAM image; pads from
   `rtw_input` (N64 OSContPad bits, stick −127..127).
8. **retrowser module (guide §3–§6)**: `module.loop: asyncify`, `video: webgl`, `hz: 60`, `save: flash 131072`,
   `rom.sha1` above, `id: "pokemonsnap-us"` (never change it: saves are keyed by it), a separate build target so a
   later standalone page stays possible. Host imports `rtw_log`, `rtw_build_id`, `rtw_rumble`, `rtw_export_file`
   (album photos as PNG: a natural Snap feature), `rtw_pointer_lock` (unused unless a mouse camera is added).

## 3. Key mechanisms to design (write each down in docs/port/MEMORY.md etc. when done)

### 3.1 Getting the data from the ROM

Ground truth for "which bytes of which segment live where" is the **matching build's map**: run the upstream build
once locally with the user's ROM (`uv run configure.py --setup`, `uv run configure.py`, `ninja`, checksum OK), then
generate from `build/pokemonsnap.map` + splat's segment table frozen tables under `port/symbols/` (symbol, VRAM,
ROM offset, size, segment): offsets and names only, never bytes. The port build compiles **code only** for those
TUs (data definitions that are asset includes become mirror labels); at run time `dmaLoadOverlay` and the main
segment's load copy the ROM ranges into the mirror and apply that segment's relocations.

### 3.2 Overlays sharing VRAM

Many segments share an address range (`exclusive_ram_id`: `level`, `unk8011B050`, ...). Code is all linked at once,
so the two cases differ: **code pointers** resolve per segment (a function address is ambiguous across overlays
loaded at the same VRAM: the address -> function table must be keyed by the overlay loaded there, like the GS port's
codemap), **data** lives in the mirror and is simply overwritten as on the console.

### 3.3 Render-to-RDRAM and read-backs

Colour images: the main framebuffers (`scFrameBuffers[3]`), the Z buffer, the detector regions (8 wide), score
images (64 wide). Design: one GL render target per active colour-image address; mark ranges "GPU-owned"; resolve to
the mirror (readPixels -> RGBA16) when the game or a later draw reads them (texture from that address, `osInvalDCache`
of the range, a copy); upload CPU writes back before the next draw/display. Measure the cost: up to 20 detector copies
per frame while aiming.

## 4. Phases (each a numbered session or a few; one feature per session)

| Phase | Goal | Done when |
|---|---|---|
| **S1 Toolchain + matching build** | Docker images (emsdk; the decomp's Python + IDO recomp run where upstream expects); upstream build reproduces the ROM (checksum OK) with the user's ROM | `ninja` OK; `port/symbols/*.tsv` generated from the map (offsets/names only); ROM-free audit of what the port must not compile (the 372 asset-including TUs listed) |
| **S2 Port build skeleton** | wasm32 build of all game TUs (code only), stubs for unported libultra calls (logged once), mirror + ROM loader, vbe pass, signature pass | the module links; census of TUs (none silently dropped); stubs list (done S2: docs/port/MEMORY.md; indirect calls and bitfields moved to S3) |
| **S3 First frame** | fibers + VI, DMA, dmaLoadOverlay, minimal F3DEX2 on WebGL2 in a local test page | the N64/HAL logos or first screen visible (screenshot looked at, not assumed) |
| **S4 Title and menus** | controller input, more GBI, sprites/S2DEX, YUV, text | title -> main menu -> Professor Oak's lab by real input |
| **S5 First level** | Beach: object threads, models, animation, on-rails camera | play the Beach course start to end |
| **S6 Photos** | render-to-RDRAM, detector read-backs, score thumbnails, photo check scoring | a photo of a Pokémon in focus scores like the console (compare with an emulator run of the same inputs, locally) |
| **S7 Audio** | aspMain HLE, sequences, sound effects; virtual AI queue | music and effects at the right pitch and tempo |
| **S8 Save** | FlashRAM image, album, progress; save survives reload | album and progress persist |
| **S9 retrowser module** | the `retrowser-setup` steps: rtw exports/imports, `retrowser.json`, touch layout check, `rtw.mjs check --rom` OK (no ROM runs in any file) | local retrowser host e2e (boot, input, save, reload + continue, offline) all pass |
| **S10 All courses + polish** | every level, Rainbow Cloud, gallery, report, credits | full play-through route |
| later | features (each optional: widescreen, high frame rate, photo export as PNG), regressions/recordings (WPO §11) | |

Determinism modes and route tests (WPO §5) are added at S4 and grow with each phase.

## 5. Rules (from the user; apply to every session)

- **Never commit ROM-derived bytes** (assets, extracted files, screenshots, captures, saves). Generated asset dirs
  (`assets/`, `build/`, `asm/`) are already gitignored upstream; port work output goes to a gitignored `port_work/`.
- Never modify or copy the ROM into the repo; read it from its own path.
- **Never push upstream** (ethteck/pokemonsnap). Back up to a private remote of the user's only when they create one.
- No system installs or downloads without the user's approval (IDO recomp download by `configure.py --setup`,
  Docker images, npm/pip packages: ask first). No git stash.
- Never pkill/pgrep; kill only PIDs you started. Headless Chrome muted; timing-sensitive runs alone.
- At most 2–3 agents at once, each in its own worktree, disjoint files; one bounded slice per session.
- Rename every function and variable you touch to a readable name, keep `/* was func_XXXXXXXX */`.
- Every feature behind an option (default off unless asked), one commit per feature.
- Touch controls on phones come from the retrowser host (N64 layout); test portrait and landscape before calling a
  build done.
- End each session with a `docs/port/HANDOFF.md` entry and a `docs/progress/sessions.tsv` row.
- retrowser deploys and Cloudflare/DNS changes need the user's explicit approval each time.

## 6. Risks

1. Framebuffer read-backs (1.1): without them photos never find a Pokémon. Highest risk, own phase (S6).
2. Many object threads (om): fiber count and switch cost under ASYNCIFY; measure early (S5).
3. Shared-VRAM overlays (3.2): function pointers stored in data must resolve to the right overlay's function.
4. 16 % of data not yet mapped upstream: unknown pointers in those blobs need the relocation review (WPO trap 1).
5. Phone performance in a WebKit worker (retrowser on iPhone): budget the renderer; WebGL2 on OffscreenCanvas.
6. Licence (section 7).

## 7. Open questions for the user

Answered by the user at S1 (2026-11-08):

1. **Licence**: develop privately, no publishing (revisit before S9 / any retrowser link: needs a licence upstream).
2. **ROM**: the user's ROM is `Pokemon Snap (USA).n64` at the repo root (byte-swapped .n64; gitignored by `*.n64`;
   never modify, move or commit it). The build's big-endian baserom `pokemonsnap.z64` (gitignored by `*.z64`) is
   written from it by `port/docker/run.sh rom`.
3. **Remote / branch**: a local branch **`web`** only; **no push ever** (no remote chosen). Commit code, tooling,
   names/offsets tables and docs; never the ROM or anything built from it (check `git status` before each commit).
4. **Toolchain**: downloads approved for S1; everything runs in Docker (Docker Desktop CLI at
   `/Applications/Docker.app/Contents/Resources/bin/docker`; `/usr/local/bin/docker` is a dead OrbStack link).
