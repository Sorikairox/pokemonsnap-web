# Port memory model, endianness and calls (as built in S2)

How the wasm32 build (`port/docker/run.sh port` = `port/tools/port_build.py`) turns the decomp into one module.
Method: docs/port/PLAN.md section 2; the Conker web port's playbook (WPO §3.1–3.3) is the reference.

## Build pipeline

| Phase | Tool | Output (port/build/, gitignored) |
|---|---|---|
| census | port_build.py `census()` | `port/census.tsv` (committed): every object of the matching link + every `src/**/*.c`, category and action |
| C -> IR | emcc `-O0 -Xclang -disable-O0-optnone -fno-discard-value-names` | `ll/<tag>.ll` (incremental: key = flags + source + headers from `-MD`) |
| signatures | sigs.py `scan` / `canonical` | `sigdb.json`, `sig_mismatch.txt`, `short_calls.tsv` |
| vbe + calls | vbe.py (+ sigs.py `normalize`), then emcc `-O1` | `ll/<tag>.v.ll`, `vobj/<tag>.o`, `vbe_report.txt` |
| PC layer | `port/ultra/*.c`, `port/boot/*.c`, `gen/pc_segments.c` (native, no vbe) | `pc/*.o` |
| stubs | gen_stubs.py | `gen/rdram_labels.s`, `gen/stubs.c`, `gen/relocs.c`, `port/stubs.tsv` (committed) |
| link | emcc, `-sERROR_ON_UNDEFINED_SYMBOLS=1`, `-sGLOBAL_BASE=256MB`, ASYNCIFY, MODULARIZE, worker+node | `web/pokemonsnap.{js,wasm}`, `pokemonsnap.map`, `link_log.txt` |

Game TU flags: `-std=gnu89 -nostdinc -fno-builtin -fno-strict-aliasing -fwrapv -funsigned-char` (IDO's char is
unsigned), the include path of configure.py plus `port/include` first (clang `stdarg.h`; `port_prefix.h` is
force-included), `-DPORT`, the IDO-tolerated errors downgraded to warnings. ROM-free guard: a TU whose `-MD` deps
contain `assets/`, `build/` or `asm/` fails the build. TUs with non-ASCII bytes are compiled from an EUC-JP copy
(`port/build/euc/`), as asm-processor feeds IDO: `'あ'` is the 2-byte constant 0xA4A2 in both builds.
The build is deterministic (two clean builds give the same wasm SHA-1) and takes ~10 s on 10 cores.

## RDRAM mirror

- `n64_rdram`: 8 MiB, `.p2align 23`, the first object of the link -> wasm 0x10800000 (`GLOBAL_BASE` = 256 MiB keeps
  0..0x10000000 a null zone). Every **global** data / `none` symbol of `port/symbols/symbols.tsv` in
  0x80000000..0x807FFFFF that no port object defines is a label at `n64_rdram + (vram - 0x80000000)` (15663 labels;
  overlays sharing VRAM simply share addresses).
- Absolute link symbols (not in RAM: `*_ROM_START`, `*_SIZE`, `D_0E000000` segment addresses, ...; 784) are replaced
  in the IR by `inttoptr` constants (vbe.py `abs_syms`, table `port/build/abs_syms.txt`).
- C-defined data (code TUs, game-data TUs, libultra C) lives in wasm, not in the mirror. Overlay TUs' writable
  globals sit in per-overlay wasm sections and are reset when the overlay is loaded again (S3a, "Overlays" below).
- **Console addresses as pointers (S3a, vbe.py `k0_ptrs`)**: game C hard-codes KSEG0 addresses and the unmapped /
  compressed data holds them as pointer words. In game code every integer -> pointer conversion and every pointer
  loaded from memory (except plain locals and the TU's own C globals) maps 0x80000000..0x807FFFFF to the same offset
  in `n64_rdram`; constant `inttoptr` of such an address becomes `n64_rdram + offset` (16 constants, 109 run-time
  conversions, 10490 pointer loads). Arithmetic on a hard-coded address together with a symbol address needs the
  source to say so: `PORT_K0(a)` (port_prefix.h), used by `VPK_VRAM` (include/constants.h). Not mapped: KSEG1
  0xA0000000.. (a cheap test would also catch wasm addresses past 512 MiB), and tests like `(u32)p >= 0x80000000` on
  pointers (none known in Snap yet; Conker had some).

## ROM loader and relocations

- `port_set_rom(ptr, size)` (exported) takes .z64/.n64/.v64 and keeps one copy in the vbe layout (one bswap32 per
  word). `port_rom_copy(dst, rom, n)` copies into game memory at any alignment.
- `port_load_segment(name)` (pc_mem.c; the boot loader of `main`, and `--load-all`): segment table
  `gen/pc_segments.c` from `segments.tsv`; copies the ROM range to the mirror at the segment's VRAM, clears its bss,
  then relocates through `port_dma_loaded` like any DMA (S3a, "DMA" below).
- Relocations: `port/symbols/relocs.tsv` (gen_relocs.py, frozen from the matching build's objects: every R_MIPS_32
  in `.data`/`.rodata` of the objects the port does **not** compile; 33785 words, offsets and names only, addends stay
  in the ROM). gen_stubs.py resolves each against the link: target is a C object/function of the port -> its wasm
  address / table index + offset (209 data, 501 functions: the symbol name picks the overlay); target in RAM ->
  mirror address (32495); absolute target -> kept (564); code without a port body (libultra asm) -> kept (16).
  `port-test --load-all` loads all 119 RAM segments: 0 bad words (every function word is its function's start).

## Endianness (vbe.py)

Virtual big-endian memory, the Conker pass adapted: aligned words native, halfwords at `a ^ 2`, bytes at `a ^ 3`,
64-bit with swapped halves, unaligned and odd-sized copies through `__vbe_*` helpers (pc_mem.c), global initialisers
with 1/2/8-byte scalars converted by a constructor (`__vbe_fix`). Snap additions:
- **va_arg**: the variadic buffer is written by the wasm backend in native layout, so 64-bit loads through clang's
  `%argp.cur*` pointers are not half-swapped (`va64`; needs `-fno-discard-value-names`). Checked at boot: the game's
  own sprintf formats `%d %x %s %c %.2f %lld` correctly ("vbe self-test ... OK").
- **libc names**: game definitions of libc names (`LIBC_CLASH`: acosf asinf atan2f atanf cosf ldiv lldiv memcpy sinf
  sprintf strchr strlen tanf) and libultra-asm libc calls (`LIBC_VBE`: bcopy bzero bcmp sqrtf, defined in
  `port/ultra/pc_libc.c`) are renamed `vbe.<name>`; gen_stubs.py fails the build on a new clash.
- RCP register accesses through constant KSEG1 addresses -> `__port_io_read/write` (pc_io.c, logged; 4 sites).

## Calls (sigs.py)

One canonical signature per function (the definition's; for functions without a body the most common call site);
mismatching direct call sites and declarations are rewritten as o32 registers would pass them, keeping
`zeroext`/`signext` on narrow parameters. Snap fixes: definitions with `sret(...)`/`byval(...)` parameters are parsed
with balanced parentheses (the lazy regex truncated them); a site's own argument text (byval) is kept when the type
is unchanged; calls to a function the TU defines `static` are never rewritten (libaudio's `_getRate` / `_getVol`
exist in two TUs with different signatures). Result: 41 rewritten sites, 12 functions with mismatching sites
(`port/build/sig_mismatch.txt`), **0 short K&R calls** (`port/build/short_calls.tsv`).

## Indirect calls (S3a: icall.py, icall_gen.py, pc_icall.c)

- **Site-typed trampolines** (adapted from Conker): every indirect call in a game TU (184 sites) calls
  `__icall.<hash>(fp, args...)` with the site's own type. Fast path: the target's registered wasm signature equals the
  site's (or is unknown: PC-layer functions are not registered) -> plain `call_indirect`. Slow path:
  `__port_icall_slow` (generated, one noinline case per target signature, 109, in groups of 16) converts arguments and
  result like sigs.py does for direct calls; mismatches are logged (`[icall] signature mismatch`), void targets
  called for a value too (they read 0 here, v0 on the N64: audit each). Each TU's constructor registers its
  functions' signatures (`__port_fsig_reg`, 4133 functions).
- **Address -> function table**: each TU also registers {function, console address, segment index} from
  `port/symbols/symbols.tsv` (4085 functions; port_build.py writes `port/build/fvram/<tag>.txt`). A trampoline whose
  pointer is >= 1 MiB (not a table index: a console KSEG0 address, or a mirror address after the K0 mapping) calls
  `__port_fptr_fix`, which returns the function at that console address **in the overlay resident there** (pc_mem.c
  tracks the last segment loaded over each VRAM range; overlays share VRAM). Logged: `[icall] function pointer ...`.
  Seen at boot: the scene setups and process tables in un-mapped `.data` (`assets/main_menu/A0F660.bin` = rom-bin:
  no relocations) hold console function addresses: ~4 fix-ups per retrace on the menus. Their **identity** differs
  from the C function's table index (a compare `p == func` would fail): give rom-bin data relocations (a scan for
  function-start words, reviewed) or map those data at load time (open, S4).
- PC-layer calls of game function pointers (thread entry points): `port_call_vi(fp, a0)` (same rules).

## Calls: v0 of void functions (S5: v0_scan.py, v0.py, port_build.py v0_plan)

On MIPS a `void` function whose every return follows a call of a value-returning function leaves that result in v0,
and IDO-era callers read it (Snap: the Pokemon spawn wrappers `void f(...) { Pokemon_Spawn(...); }`, called through
`GObj* (*init)(...)`; pokemonAdd() stopped at the first 0, no Pokemon spawned). General rule, no per-function edits:
- `port/docker/run.sh v0` (toolchain image) -> `port/symbols/v0.tsv`: from the matching build's IDO objects
  (`objdump -dr`), a data-flow per function of where v0 comes from at each `jr ra` (`call:F`, `set`, `entry`,
  `icall`, `jr` = jump table; delay slots and branch-likely handled). Ground truth, not the C.
- port_build.py `v0_plan`: a void function is **promoted** (canonical return i32) when the console's v0 is only call
  results, the -O0 IR's last calls before each return name the same callees (an IR-only path with no call is accepted:
  IDO proved it impossible, e.g. a rotated counted loop), and each callee leaves a value in v0 (int/ptr return, or
  promoted: fixpoint). vbe.py applies `v0.promote` (each `ret void` -> `ret i32` of the last call's result, phi nodes
  over the CFG) and rewrites the TU's own calls to promoted statics. 275 of 3288 void functions.
- Void callers drop the result (sigs.py rewrites direct sites; icall trampolines of void sites accept the promoted
  `i:...` signature on a fast path; `port_call_vi` accepts `i:i`).
- `port/v0_report.tsv` (committed): every void function, the direct sites that use its value (0 today), the console
  v0 at its returns and what the port returns. Indirect value calls of a non-promoted void function still read 0 and
  are logged at run time (`[icall] void target ... called as ...`; 0 on the Beach route).

## Function identity (S5: vbe.py fptr_eq, pc_icall.c __port_fptr_canon)

Data without relocations (the `bin` blobs inside code segments: world block descriptors, Pokemon tables, scene
setups) holds functions as console addresses: calls work (`__port_fptr_fix`), but `==` with a function did not
(world/block.c `&drawSkyBox2Cycle == gfx->renderFunc`: every world block GObj was deleted, the Beach was black). Every
pointer `icmp eq/ne` with a non-constant operand maps it through `__port_fptr_canon`: a console / mirror address that
starts a function of the overlay resident there (bitmap of compiled functions' console starts) becomes that function's
table index, anything else is unchanged; pointers compare equal exactly when they do on the console (184 sites).
`port/tools` survey (scratch, S5): rom-bin data holding function words: the asset bins of every level (render
functions), cave/river/valley/tunnel code bins (Pokemon tables), main_menu scene setups.

## Console bss layout (S5: vbe.py bss_layout)

The decomp declares some objects smaller than the game uses them and keeps console addresses with unreferenced filler:
player.c `struct DObjDynamicStore PlayerMatrixStore; // size 0x38` (4 bytes + VLA; the player's DObj writes 0x34
bytes of matrix data) is followed by filler `D_803AE4A5..D_803AE4A8[0x30]`. clang put IsTutorialEnabled and
MainCameraBorder* after it: the borders took float bits, `mainCameraSetScissor` emitted `ED FFF000` and every world
draw was scissored away. Every zero-initialised global that the matching build puts in a bss section now lives at its
console offset inside one byte array per TU and console section (`@__bss.<tu>.<section>`, 16-aligned at a 16-aligned
console address, so vbe's sub-word addressing matches the mirror's); the global is an alias of its place (733
globals; layout from `symbols.tsv` -> `port/build/fvram/<tag>.bss`). Overflows reach the console's neighbours.
Not covered: initialised `.data` and overflows across TUs (none known).

## Debugging from game C (S5)

`port_game_log(fmt, ...)` (pc_mem.c): printf from game C under `#ifdef PORT` (the format literal is in vbe layout and
is unswizzled; pass numbers only, `%s` arguments would be vbe too). `port_function_vram(fn)` gives a function's
console address for logs. Remove such logs before committing.

## Threads (S3a: pc_os.c)

- `OSThread` = an Emscripten fiber (ASYNCIFY) with **static, fixed-address stacks** (256 slots x 128 KiB C stack +
  64 KiB Asyncify stack; slot tag in `OSThread.thprof`): their addresses end up in game memory, so they must not
  depend on heap history. The console stack passed to `osCreateThread` is unused (the game's stack probes still
  pass: nothing writes there).
- libultra scheduling: highest priority first, FIFO among equals (by the time each became runnable), preemption as
  soon as a higher-priority thread becomes runnable (`osSendMesg` waking a receiver, `osStartThread`,
  `osSetThreadPri`), blocking in `osRecvMesg` / `osSendMesg(BLOCK)` / `osStopThread` / `osYieldThread`. Threads
  switch to each other directly (one fiber swap); the host context runs only when nothing is runnable. Priority 0 is
  never run (the idle thread lowers itself to 0 and spins `while (1)`). Threads whose entry returns are destroyed.
- `OSThread.priority/id/state` (state via the vbe halfword) and the `tlnext` list (`__osGetActiveQueue`, error.c)
  are kept in game memory; `OSMesgQueue` is fully the libultra one (validCount/first/msgCount/msg in game memory),
  waiters are tracked on the PC side (`mtqueue`/`fullqueue` point at a `__osThreadTail`-like sentinel).
- Events: `osSetEventMesg` table; `pc_post_event(e)` sends (NOBLOCK) from the PC layer.
- Clock: deterministic: `osGetCount` = retraces x 781250 + 64 per read (never past the next retrace); `osGetTime` the
  same 64-bit, `osSetTime` an offset. No wall time anywhere in game state.
- Measured (node, S3a): fiber switch **~1.0-1.3 us** at shallow stacks (`--bench`: 40002 switches, two threads
  ping-ponging); boot -> main menu -> Oak's lab: **peak 16 live threads** (49 created over 3600 retraces), 17-31
  switches per retrace, 0.1-0.7 ms per retrace for the whole game with no renderer. Levels (one om thread per object
  process, `omCreateProcess` kind 0) not reached yet: measure there (S5).
- Measured in the level (node, S5, Beach start to end, `s5_node14`): **peak 45 live threads** (323 created by retrace
  12500; one om thread per Pokemon process), 56 fiber switches per retrace on average (max 84), 0.15 ms per retrace
  (max 0.32) for the game without a renderer; `--bench` 1.31 us per switch. Fiber count and switch cost are not a
  risk (PLAN risk 2). Browser (SwiftShader, game + renderer + screenshots): ~7.4 ms per retrace.

## Retrace, SP, DP, AI (S3a: pc_rcp.c)

- `port_vi_retrace()` (exported; node's main loops it, the page's worker calls it once per frame): the next
  framebuffer becomes current, `port_vi_present(fb, vi_mode, black)` (port/gfx/gfx_port.c since S3c), the
  `osViSetEvent` message every `retraceCount` retraces, then the threads
  run until idle. `osViSwapBuffer` / `osViGetCurrentFramebuffer` / `osViGetNextFramebuffer` as libultra.
- `osSpTaskLoad` + `osSpTaskStartGo` (= `osSpTaskStart`): a graphics task (`M_GFXTASK`) runs `port_gfx_task(task)`
  (weak hook; the renderer's strong definition replaces it); SP done then DP full sync are posted **when no thread is
  runnable any more** (S5: `port_rcp_pending` / `port_rcp_complete` in `port_run_threads`): on the console the RDP takes
  milliseconds and the game thread queues the frame's `SC_TASK_TYPE_GFX_END` meanwhile, which sched.c attaches to the
  running gfx task so that DP full sync calls `scPostProcessFunc` (the Pokemon detector) and swaps; posted at once,
  the detector never ran. An audio task posts SP done at once (audio HLE: S7). Never yielded (`osSpTaskYielded` 0). `osDpSetNextBuffer` posts DP. Each graphics
  task is walked (display-list census: opcode counts and an FNV hash, `[task]` lines; the first one's histogram
  `[dlhist]`). `osAiSetFrequency` returns the real DAC rate; `osAiSetNextBuffer` accepts and drops (S7).
- Display-list address forms seen (for the renderer): wasm addresses of C objects (matrices in bss, 0x1006xxxx),
  mirror addresses (0x108xxxxx-0x10Fxxxxx: framebuffers, Z, heaps), **raw KSEG0 addresses** in DLs from un-mapped
  data (`G_VTX 803717A8`, `G_SETTIMG 8035D2B0`: the renderer maps 0x80000000.. into the mirror), segmented addresses
  (`G_SETCIMG 0F000000`; segment bases set by `G_MOVEWORD` are wasm/mirror addresses or 0), physical (< 8 MiB:
  `osVirtualToPhysical` of mirror pointers). `OS_K0_TO_PHYSICAL(&gspF3DEX2_NoN_fifoTextStart)` leaves a mirror address
  (0x1083E7A0): the mask 0x1FFFFFFF does not change wasm addresses below 512 MiB.

## DMA, controllers, flash (S3a: pc_pi.c, pc_mem.c)

- `osEPiStartDma`: OS_READ copies ROM bytes (`devAddr` = ROM offset, or PI 0x1xxxxxxx / KSEG1 0xBxxxxxxx forms)
  into game memory at once, then posts the I/O message to `retQueue`; writes are logged and ignored.
- **Relocation on DMA** (`port_dma_loaded`): a copy from a segment's ROM range to the same offset of its VRAM is a
  load of that part of the segment: the pointer words in the copied range are relocated (relocs.tsv, sorted per
  segment, binary search). This covers `dmaLoadOverlay`, `dmaReadRom` of whole asset segments (`46270.c`), and the
  boot load of main, with no game edit.
- Controllers: one standard pad in port 1, no pak (status 0, so the game never probes Rumble / Controller / Transfer
  Paks; their calls answer "no pak"), ports 2-4 CONT_NO_RESPONSE_ERROR. `osContStartQuery` / `osContStartReadData`
  post the SI event; `osContGetReadData` writes the host pad (`port_set_pad(i, buttons, x, y)`, exported, OSContPad
  bits). Node: `--tap N MASK` presses MASK for 3 retraces every N retraces (frame-indexed, deterministic).
- FlashRAM: 131072-byte image (console byte order, erased 0xFF), 128-byte pages, 16 KiB sector erase, programming
  ANDs bits; `osFlashReadId` = Macronix MX_C. `port_flash_image()` / `port_flash_generation()` exported for the host
  (persistence: S8).
- `osVirtualToPhysical`: mirror pointer -> console physical address (0..8 MiB); KSEG0 -> physical; any other
  pointer (C object) unchanged.
- Caches (S3c): `osInvalDCache(p, n)` -> `port_gfx_inval_dcache` (GPU-drawn colour images in range are written into
  RDRAM before the CPU reads them), `osWritebackDCache(p, n)` / `osWritebackDCacheAll` -> `port_gfx_writeback_dcache`
  (images in range re-uploaded if the CPU changed them); KSEG0 pointers are mapped to the mirror first.
- RCP registers: `SP_IMEM_START` reads 6103 and `SP_DMEM_START` -1, as after the console's boot code
  (`check_sp_imem/dmem`, flags the anti-tamper code in app_level/504770.c reads); others read 0.

## Overlays (S3a)

- Code is linked once; "loading" an overlay = its ROM bytes in the mirror + relocation (above) + its **C data reset**:
  vbe.py puts every writable global of an overlay TU into the wasm data section `ovl_<segment>` (3035 globals, 31
  sections; main is not reset), wasm-ld gives `__start_/__stop_ovl_<segment>`; `port_mem_init()` snapshots each
  section after the constructors (vbe fix-ups) and `port_dma_loaded` restores it when a DMA brings the segment's
  first byte to its VRAM, which is what the console's DMA of `.data` + bzero of `.bss` does. Sections a TU's globals
  never reach (gc'd) are weak and empty.
- Residency: the same DMA makes the segment resident at its VRAM and every overlapping segment not (address ->
  function table). Seen: app_render, more_funcs, intro_code, main_menu, world, window, menu_new_game, oaks_lab;
  world loaded twice (reset exercised).

## Bitfields (S3a)

IDO allocates bitfields from the MSB, clang (little-endian) from the LSB; in vbe memory a word holds its big-endian
value, so every bitfield run is declared LSB first under `#ifdef PORT`, padded to whole 32-bit units, with sub-word
members that share the word (`poseID`, `unk_02`) in an anonymous union: `include/common_structs.h`
(PokemonPhotoData, PhotoData), `src/tunnel/tunnel.h`, `src/sys/sched.c` (ViSettings), `src/photo_check/993F80.c`,
`src/more_funcs/5BF20.c`. Checked in the IR: `pokemonID` reads `ashr i32 w, 19` as IDO's bits 31..19. gbi.h/abi.h
bitfield structs are not used by game C (macros with shifts).
