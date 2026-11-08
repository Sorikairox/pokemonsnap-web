# Renderer: F3DEX2 / L3DEX2 on WebGL2 (port/gfx, session S3b)

How the port draws the game's display lists, and how it keeps colour images coherent with RDRAM (PLAN.md 1.1, 3.3).
Code: `port/gfx/` (`gfx.h` interface, `gfx_rsp.c` display lists + RSP, `gfx_rdp.c` RDP + WebGL2, `gfx_gl.js` context,
`test/` offline harness). Provenance: `port/gfx/LICENSES.md` (own code, from gbi.h and public documentation).

## What the game uses (survey, S3b)

- Microcode: `gtl.c`'s table fills only **F3DEX2_NoN_fifo** (index 2) and **L3DEX2_fifo** (8); S2DEX2 is NULL (gtl
  panics "ucode isn't included" if asked), so **no S2DEX2**. L3DEX2 is entered mid-list with `gSPLoadUcodeL`
  (gtl.c 434/446: `G_LOAD_UCODE` with `G_RDPHALF_1` = data). The renderer recognises L3DEX2 by its text address
  (`gspL3DEX2_fifoTextStart`, mirror label at 0x8003FB30; weak symbol + constant).
- Snap-specific: `gSPMvpRecalc` = `G_SPECIAL_1` (0xD5) with p1 = 1 (render.c), followed by `G_MW_MATRIX` moveword
  patches of the MVP (billboards). Handled: the MVP is kept in the RSP's fixed-point layout so patches land on the
  same words as on the console.
- Sprites: ultralib `sp/` and `26D80.c` (a copy of spDraw) use plain F3DEX2 texture rectangles, including **YUV**
  textures (`gDPLoadTextureBlockYuv`, `G_TC_CONV`).
- GBI macro census of game C (not assets): PipeSync 95, SetRenderMode 64, SetCycleType 63, DisplayList 61, ...,
  FillRectangle 19, LoadTextureBlock(_4b/S/Yuv) 35, TextureRectangle 8 + Scis 2, LoadTLUT 7, Fog 9, Lights 5,
  CullDisplayList 1, BranchList 23, Line3D only in asset lists.

## Model

- **RSP on the CPU** (`gfx_rsp.c`): segment table, modelview stack (32), projection, MVP (lazy, fixed-point copy for
  `G_MW_MATRIX` / `G_MV_MATRIX`), viewport, 64-entry vertex buffer (transform, lighting with up to 7 directional
  lights + ambient, texgen, fog factor into shade alpha), back/front culling in NDC, `G_CULLDL`, `G_BRANCH_Z`,
  `G_MODIFYVTX`, DL call/branch/end (stack 32). Vertices leave as clip-space positions with the N64 viewport folded
  in, so the GPU clips.
- **RDP on WebGL2** (`gfx_rdp.c`): othermode, combiner, colours, scissor, tiles, byte-exact 4 KiB TMEM (LoadBlock
  with dxt odd-line swaps, LoadTile, LoadTLUT quadrupled, RGBA32 bank split; the sampler reads back with the same
  odd-row swap). Tiles decode to RGBA8 textures (RGBA16/32, CI4/CI8 with RGBA16/IA16 TLUT, IA4/8/16, I4/8, YUV16),
  cached by a hash of the TMEM bytes + descriptor (1024 entries). Wrap/mirror/clamp by mask → GL wrap modes.
- **Shaders**: one generated GLSL program per (combine, blender bits, cycle type, flags). Two combiner cycles with
  all N64 inputs (NOISE, K4/K5, PRIM_LOD_FRAC; LOD_FRACTION = 0); the blender's first cycle and any cycle without
  memory are evaluated in the shader (fog: `FOG, SHADE_A, IN, 1MA`), a last cycle reading memory with `FORCE_BL`
  maps to GL blending (`SRC_ALPHA, f(B)`). Alpha compare (threshold / dither), `CVG_X_ALPHA + ALPHA_CVG_SEL`
  (texture edge, 0.5), Z compare/update/decal (polygon offset), primitive depth. FILL rectangles = flat quads of
  the fill colour; COPY rectangles = point-sampled texel copies (dsdx / 4, inclusive lower-right).
- **Batches**: triangles accumulate while the RDP state is unchanged; any state command flushes.

## Address classes (same as MEMORY.md)

`>= 0x80000000` KSEG0/1 → mirror; `0x10000000..0x7FFFFFFF` wasm pointer (what game C stores; `OS_K0_TO_PHYSICAL`
leaves it unchanged since it is < 0x20000000); `0x01..0x0F` top byte → segment base (any class) + offset; segment 0
→ base 0 + physical → mirror. Bad addresses are counted and logged once.

## Colour images coherent with RDRAM (the photo game)

Every colour image is a GL render target (RGBA8 texture + 24-bit depth) **at the console's resolution**, one per
colour-image address/width, rows stored N64 row 0 first (GL window y = N64 y; the VI blit flips). A target starts
as RDRAM holds it (uploaded on creation). It tracks the rows the GPU drew since the last write-back
(`dirty_y0..dirty_y1`, from primitive extents clipped to the scissor).

| When | What |
|---|---|
| texture load (LoadBlock / LoadTile / LoadTLUT) from bytes a GPU-drawn image covers | those rows are read back (`glReadPixels`) and written into RDRAM as **RGBA16** (5551, alpha bit = coverage ≥ 0.5) or RGBA32 before TMEM is filled → the detector's 8x8 LoadTile of the frame centre, score.c's copies of 64-wide images |
| end of every task | every image the VI never showed is written back (S6; before: width ≠ 320/640 only): detector regions, score images, and the 320x210 photo renders of `window/847B60.c`, which the CPU averages into the camera-check / album sprites right after `gtlDrawOne` without `osInvalDCache` (they came out white) |
| `osInvalDCache(p, n)` → `port_gfx_inval_dcache` | rows in range written back (CPU reads: `PokemonDetector_PostProcessImage`) |
| an image fully written back | "clean": RDRAM is authoritative; before the next draw onto it or before the VI shows it, RDRAM is hashed and re-uploaded if the CPU changed it (the red focus dot) |
| `osWritebackDCache(p, n)` → `port_gfx_writeback_dcache` | forces that hash check on images in range (n = 0: all) |
| write-back of rows that overlap another image | that image is flagged for the same check |
| FILL rectangle over a Z image's rows (same width) | that Z image's depth texture is cleared in the rectangle (S6); rows of no known Z image are drawn as colour |

**Z images (S6).** `score.c` (`func_800A007C`, called by the camera check and Oak's check through
`window/847B60.c func_8037452C`) renders each photo again into a 64x48 image, gives every Pokémon its own 64x48 Z
buffer (segment 0x0A), copies the current Z buffer into a "before" image with COPY texture rectangles (0x0C → 0x0B)
and then **counts pixels in the Z buffers with the CPU** (`!= 0xFFFC`, equality between buffers): size, completeness,
"well framed" and the same-Pokémon bonus all come from these counts. Z images are therefore GL depth textures
(`DEPTH_COMPONENT24`) keyed by address and size, attached to whichever colour image is drawn with them, with the
same coherence rules as colour images: dirty rows, written back on texture loads / `osInvalDCache` / at the end of
each task (except Z images used with framebuffers: never read), loaded from RDRAM when clean and RDRAM changed
(all-equal → clear, else a `gl_FragDepth` pass from an R32F texture). Format: the console's 16-bit Z, GL depth ×
0x3FFFF = the 18-bit z (both linear in NDC z), compressed to a 3-bit exponent (leading ones) + 11-bit mantissa,
`<< 2`, dz 0; 0xFFFC = depth 1.0 both ways. Equalities between buffers hold because the same geometry gives the same
GL depth; the absolute values are not verified against a console. COPY mode keeps the texel's alpha (coverage) bit,
so a copied Z buffer is bit-exact.

**Read-back journal (S6, node parity).** Write-backs are the only GPU effect the game's CPU can see. A browser run
records every byte they change (`port_rb_record`, gfx_rdp.c), grouped by the next point where the CPU can observe
memory (end of a graphics task, `osInvalDCache`, VI frame); `port/web/run.mjs` saves it as `OUT/readback.bin` and
`port/docker/run.sh port-test --readback OUT/readback.bin` (same `--frames/--tap/--tap-end/--input`, same build)
writes the same bytes at the same points in node (the point kind is checked; `[readback] desync` is logged).
Measured: Beach route to Oak's check, 14400 retraces: 26 MB journal, node gives the browser's 8310 `[task]` hashes
(without it 5596 differ from retrace 3833, the first detector frame). ROM-derived: `port_work/` only.

Because targets are native size, read-backs equal what the console's RDP would leave in RDRAM up to rasterisation
rounding — detection (≥ 32 of 64 pixels changed) and thumbnails don't depend on the display. A later "high
resolution" option must keep a native-resolution path for these images (render them at 1x, or resolve with a
1x downsample); render-only options must never change read-backs.

Costs: a screen image write-back is a synchronous `glReadPixels` (GPU stall). The detector does up to 20 tiny
(8-row) read-backs per frame while aiming; measure on phones (S6). Clean-image hash: 150 KB per frame shown.

## Interface with the PC layer (S3a)

Declared in `port/gfx/gfx.h`:

| Function | Caller | Notes |
|---|---|---|
| `void port_gfx_task(void *ostask)` | `pc_rcp.c osSpTaskStartGo` for `M_GFXTASK` | runs the whole list synchronously; reads `OSTask` words (type 0x00, ucode 0x10, data_ptr 0x30). **The PC layer posts SP done and DP full sync after it returns (S3c: one owner of the events)**; the renderer only counts `G_FULLSYNC` (`port_gfx_stats.fullsyncs`) |
| `void port_gfx_vi_frame(const void *fb, int width)` | `gfx_port.c port_vi_present(fb, vi_mode, black)`, called by `pc_rcp.c port_vi_retrace` once per retrace: width = `OSViMode.comRegs.width`; NULL fb or `osViBlack` = black screen | blits colour image `fb` (any class: KSEG0/physical/wasm) of width 320/640 to the canvas, 4:3 letterboxed; CPU-drawn frames are uploaded from RDRAM |
| `void port_gfx_inval_dcache(const void *p, uint32_t n)` | `pc_os.c osInvalDCache` (KSEG0 mapped to the mirror) | write-back for CPU reads |
| `void port_gfx_writeback_dcache(const void *p, uint32_t n)` | `pc_os.c osWritebackDCache` (n) / `osWritebackDCacheAll` (n = 0) | CPU-write hint |
| `int port_gfx_init(void)` | optional at boot | creates the WebGL2 context on `Module.canvas` (`gfx_gl.js`); lazy otherwise; without a canvas (node `port-test`) lists are still interpreted and nothing is drawn |

Build integration (done in S3c): `port_build.py` compiles `port/gfx/*.c` with the PC-layer flags and links with
`--js-library port/gfx/gfx_gl.js -sMAX_WEBGL_VERSION=2 -sMIN_WEBGL_VERSION=2`; `gen_stubs.py` treats `gl*` and
`port_gfx_gl_create` as JS-library imports. Without a canvas (node `port-test`) the lists are interpreted and nothing
is drawn. The host (retrowser's worker, `port/web/worker.js`) sets `Module.canvas` to an OffscreenCanvas.

## Testing with the game (S3c, needs the player's ROM)

`node port/web/run.mjs --out port_work/DIR --frames N [--every K | --shots a,b] [--tap N MASK] [--input F:MASK[:LEN],...]
[--logops F,...] [--pace fast|raf]` serves `port/web/` + `port/build/web/` + the ROM (from `--rom`, default
`./pokemonsnap.z64`, never copied) on 127.0.0.1, runs host Chrome headless (muted, SwiftShader) over the DevTools
protocol and writes `DIR/fNNNNN.png` (canvas after retrace N; ROM-derived: `port_work/` only) and `DIR/log.txt`
(module log, `[gfxstat]` counters per shot, `[gfx]` warnings). `--logops F` dumps every command of the tasks run in
retrace F; `python3 port/tools/dl_decode.py DIR/log.txt` decodes them (combiner, render modes, tiles). Pace `fast`
is deterministic (same PNG hashes run to run); node gives the same `[task]` hashes as the page on the same build
(display lists contain C-object addresses: compare runs of one build only).

## Testing (no ROM)

`port/gfx/test/run.sh [OUT]` builds `test/gfx_test.c` (display lists written with gbi.h's own macros, data in a
test RDRAM; `-sGLOBAL_BASE=256MB` so the address classes match the game), serves it on 127.0.0.1, runs the host's
Chrome headless (SwiftShader, muted) over the DevTools protocol (`test/cdp.mjs`, node's built-in WebSocket), and
writes `log.txt` (CHECK lines), `shown.png` / `shown2.png` (canvas) and `rdram.png` / `rdram2.png` (RDRAM after
write-back) to `port/build/gfx_test/out/` (gitignored). Two tasks, 18 checks (S6: `z_writeback`, a 64x48 Z image in the console format + its COPY into a colour image): SP/DP events, culling, no unknown
opcodes / bad addresses, the detector's LoadTile + COPY into an 8-wide image equals the frame centre (64/64),
FILL, Z buffer, a 64-wide image with its own Z copied back via LoadBlock in the same task, CI4 + TLUT + alpha
compare, RGBA16 wrap, RGBA32, IA16, I4, mirror, 2-cycle TEXEL0/TEXEL1, matrix push/mul/pop, `gSPMvpRecalc` +
`G_MW_MATRIX` patch, branch lists; plus fog, lighting, XLU blending, L3DEX2 lines and the CPU red dot by eye.

## Not done / TODO

- Positional lights (`G_LIGHTING_POSITIONAL`), texture LOD / mipmaps / detail (`G_TL_LOD`, LOD_FRACTION = 0),
  chroma key, dithering, coverage/AA, the 3-point bilinear filter (GL bilinear is used), YUV combiner path (YUV is
  converted at decode with the SetConvert K0..K3; K4/K5 are passed to the combiner), texgen scale checked only by
  reasoning, L3DEX2 line width formula approximate, NoN (no near clipping: GL clips at near; WebGL has no depth
  clamp), the 16-bit Z format (clears map 0xFFFC → far linearly), `G_BRANCH_Z` threshold units untested.
- CPU writes into an image that still has GPU-dirty rows (only partly written back) are not detected or uploaded;
  Snap's red dot follows a full-frame `osInvalDCache`, so it is not affected.
- Z values are the console's format by construction (linear depth → 18-bit z → compressed); not compared with a
  console capture. dz is always 0.
- Performance: no measurement yet on real frames; per-draw uniform uploads; texture hash per tile change.
- Seen on real frames (S3c): every texture rectangle is its own draw (a texture load between rectangles sets
  `state_dirty`): ~8000 draws per 150 retraces on the menus; fine on SwiftShader (5.6 ms per retrace for the whole
  game), batch across loads when phones need it. The intro's mist strip (IA8, XLU, alpha = TEXEL0) has hard right /
  bottom edges: not verified against a console capture.
