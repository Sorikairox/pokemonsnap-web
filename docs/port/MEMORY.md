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
- C-defined data (code TUs, game-data TUs, libultra C) lives in wasm, not in the mirror. Open point for S3: when the
  game DMAs an overlay again, its C data and bss in wasm are not re-initialised (the mirror copy is).

## ROM loader and relocations

- `port_set_rom(ptr, size)` (exported) takes .z64/.n64/.v64 and keeps one copy in the vbe layout (one bswap32 per
  word). `port_rom_copy(dst, rom, n)` copies into game memory at any alignment.
- `port_load_segment(name)` (pc_mem.c): segment table `gen/pc_segments.c` from `segments.tsv`; copies the ROM range to
  the mirror at the segment's VRAM, clears its bss, applies its relocations.
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
Not done yet: indirect calls (site-typed trampolines, Conker icall.py) -> S3.

## Known gaps (S3)

- Bitfields (IDO MSB-first, clang LSB-first), not yet reversed under `#ifdef PORT`: `include/common_structs.h`
  (photo data: `pokemonID:13, unk_00_13:6, specialID:5`; `levelID:7, unk_00_24:1, unk_00_16:8`),
  `src/tunnel/tunnel.h`, `src/sys/sched.c` (VI feature flags), `src/photo_check/993F80.c`, `src/more_funcs/5BF20.c`.
- Indirect-call trampolines; the address -> function table for function pointers computed at run time.
- Overlay C data / bss re-initialisation on reload; KSEG0 pointer masks / `osVirtualToPhysical` users (audio).
