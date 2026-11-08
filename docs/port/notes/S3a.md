# S3a — 2026-11-08: the CPU/OS side (threads, VI/SP/PI/SI/flash, overlays, indirect calls, bitfields)

Parallel to S3b (renderer, port/gfx/). Worktree branch `worktree-agent-acb22ee9ff700eb19`, based on `web` 16491ab.
Design details are in docs/port/MEMORY.md (sections Threads, Retrace/SP/DP/AI, DMA/controllers/flash, Overlays,
Indirect calls, Bitfields, and "Console addresses as pointers" under RDRAM mirror).

## Result

`port/docker/run.sh port-test --frames 1800` in node: the idle, scheduler, audio, game and controller threads run;
app_render, more_funcs, intro_code and main_menu load (relocated, C data reset); **a graphics task every retrace
(1798 in 1800), an audio task every retrace**; 0 logged stubs left (all 63 libultra stubs of S2 now have PC-layer
bodies); two runs give the same task log (`[task]` lines carry each display list's FNV hash). With scripted input
(`--tap 120 0x8000`, A every 2 s) the game goes on to the new-game menu, world, window and **Oak's lab** (retrace
1501), still deterministic. Matching build: `build/pokemonsnap.z64: OK`.

First display list (task #1, retrace 2, the intro logo): 178 commands, call depth 2, 0 unresolved addresses —
`port_work/s3a_final1/dl_hist.txt`: G_RDPPIPESYNC 25, G_SETOTHERMODE_H 23, G_SETTILE 16, G_SETOTHERMODE_L 11, 8 each of
G_MOVEWORD / G_TEXRECT / G_RDPHALF_1 / G_RDPHALF_2 / G_SETTILESIZE / G_LOADBLOCK / G_SETTIMG / G_RDPLOADSYNC, ...
Main menu frames: ~7100-10200 commands.

Measured: fiber switch 1.0-1.3 us at shallow stacks (`--bench`); peak 16 live threads (menus, Oak's lab), 17-31
switches per retrace; 0.1-0.7 ms per retrace for the whole game without a renderer. Levels (one thread per om
object process) not reached: measure in S5.

## What was built

- `port/ultra/pc_os.c` (threads, queues, events, clock), `pc_rcp.c` (VI, SP/DP tasks, AI, display-list census),
  `pc_pi.c` (PI DMA, controllers, FlashRAM), `pc_os.h` (native views of the libultra structs, static-asserted),
  `pc_icall.c/.h` (signature registry, address -> function table, `port_call_vi`), `pc_mem.c` (DMA-driven relocation,
  overlay residency and C data reset), `pc_io.c` (SP IMEM/DMEM boot values).
- Tools: `port/tools/icall.py` (Conker's, adapted), `icall_gen.py` (dispatcher), vbe.py (icall rewrite, ovl_ sections,
  `k0_ptrs`), port_build.py (per-TU fvram files + section, icall_tab.c, ovl table, exports), gen_stubs.py (sorted
  relocations; `__start_/__stop_` and `emscripten_*` are not stubs), node_run.js (`--out DIR`).
- `port/boot/main.c`: `--frames N`, `--bench`, `--tap N MASK`; per-60-retrace counters.
- Game C (`#ifdef PORT` only): bitfields LSB first (common_structs.h, tunnel.h, sched.c, 993F80.c, 5BF20.c);
  `VPK_VRAM` as `PORT_K0(0x802B5000)` (constants.h); `app_level_CheckIllegalCopy` /* was func_80364360_504770 */
  (504770.c): it decompresses an 18-instruction MIPS routine and calls it; the port does what the routine does
  (result byte 0 when both boot RSP-memory checks passed, else 0xFF). symbols.tsv regenerated (only that name).

## Traps found

1. Hard-coded console addresses (`VPK_VRAM`, `0x80200000`) and **un-relocated pointers in data the decomp has not
   mapped** (rom-bin `.data`, vpk0-decompressed sprite blobs): `RuntimeError: memory access out of bounds` in
   dmaDecodeVPK0, mlHeapInit (heap size = console constant - mirror address), spX2Draw. Fixed generally in vbe.py
   (`k0_ptrs`) plus `PORT_K0` where game C does arithmetic on such a constant.
2. Function pointers from un-mapped data are console addresses: called through the address -> function table
   (~4 per retrace on the menus). Their identity differs from the C function's (open).
3. gen_stubs.py stubbed the linker's `__start_/__stop_` symbols and emscripten's JS-library functions
   (`emscripten_fiber_swap`): excluded.
4. A data section that wasm-ld drops entirely has no `__start_` symbol: the references are weak.

## Interface for the renderer (S3b)

- `void port_gfx_task(void *task)`: weak in pc_rcp.c; the renderer defines it strongly (or a shim calling
  `gfx_run_task((OSTask *)task)`). Called synchronously from the scheduler thread inside `osSpTaskStartGo`, for
  `t->type == M_GFXTASK` only. `task` is the game's `OSTask` in game memory (all fields 32-bit words, native reads);
  `t->data_ptr` = the display list; `t->ucode` = mirror address of `gspF3DEX2_NoN_fifoTextStart` (console 0x8003E7A0,
  F3DEX2) or `gspL3DEX2_fifoTextStart` (0x8003FB30, L3DEX2); compare with `port_wasm_to_n64(t->ucode)`. Return when
  the list is done: the PC layer then posts SP done and DP full sync (do not post them yourself). `G_LOAD_UCODE` in
  the list switches microcode mid-list (gtl.c): its operands are mirror addresses of the same symbols.
- `void port_vi_present(void *framebuffer, const void *vi_mode, int black)`: weak, called at every retrace before the
  threads run, with the framebuffer the VI shows (the pointer the game gave `osViSwapBuffer`: a mirror address,
  NULL before the first swap), the game's `OSViMode *` (game memory) and the `osViBlack` state. 320x240 RGBA16 in the
  game's normal modes (`scViModeNext`, sched.c). Present it (or the GL target that rendered into that address).
- Display-list words: aligned 32-bit words read natively (vbe layout). Address forms (seen, see MEMORY.md): wasm
  addresses (>= 0x10000000: C objects and the mirror at 0x10800000..0x10FFFFFF), raw KSEG0 0x80000000..0x807FFFFF
  (map to `n64_rdram + (a & 0x7FFFFF)`), segmented (top byte 0x01..0x0F, base from `G_MOVEWORD G_MW_SEGMENT`: bases
  are wasm/mirror addresses or 0; do not mask the base to 24 bits), physical < 8 MiB (mirror offset). Texture /
  vertex bytes and halfwords in game memory follow the vbe lanes (`vbe_ld8/16`).
- Colour images: `G_SETCIMG` gets mirror addresses (the framebuffers, the Z buffer used as a colour image to clear
  it) or `0x0F000000` (segment F = current framebuffer). Read-backs (S6) need the RDRAM copy kept coherent.
- No other entry points are needed from the renderer; `osViGetCurrentFramebuffer()` / `port_vi_framebuffer()` give
  the shown buffer at any time.

## Remains (not done here)

- Relocations for code pointers in un-mapped `.data` (rom-bin objects inside code segments: `assets/main_menu/
  A0F660.bin`, ...): a reviewed scan for words equal to function starts, so pointer identity matches (now: fixed up
  at call time only).
- Levels: om object threads (fiber count / switch cost at depth), audio (S7: aspMain HLE, AI queue timing), rumble
  (`__osMotorAccess` -> `rtw_rumble`), flash persistence (S8: `port_flash_image` / `port_flash_generation`).
- Browser driving: `port_vi_retrace()` is exported; the page / retrowser worker calls it once per display frame.
- KSEG1 RDRAM addresses (0xA0000000..) are not mapped; `(u32)p >= 0x80000000` pointer tests not surveyed.
- The SP-done / DP events are immediate; if a later scene depends on a task taking time (yield of a long graphics
  task by an audio task), model it then.
