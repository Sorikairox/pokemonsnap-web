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
| end of every task | all non-screen images (width ≠ 320/640) are written back (detector regions, score thumbnails: read by the CPU after the task) |
| `osInvalDCache(p, n)` → `port_gfx_inval_dcache` | rows in range written back (CPU reads: `PokemonDetector_PostProcessImage`) |
| an image fully written back | "clean": RDRAM is authoritative; before the next draw onto it or before the VI shows it, RDRAM is hashed and re-uploaded if the CPU changed it (the red focus dot) |
| `osWritebackDCache(p, n)` → `port_gfx_writeback_dcache` | forces that hash check on images in range (n = 0: all) |
| write-back of rows that overlap another image | that image is flagged for the same check |
| FILL rectangle into a Z image's address | the depth buffer of every image used with that Z image is cleared to the fill value at its next bind (generation counter); 0xFFFC = far |

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
| `void port_gfx_task(void *ostask)` | S3a `osSpTaskStart` for `M_GFXTASK` | runs the whole list synchronously; reads `OSTask` words (type 0x00, ucode 0x10, data_ptr 0x30); then calls `port_sp_task_done()` and, if `G_FULLSYNC` was reached, `port_dp_full_sync()` |
| `void port_sp_task_done(void)` | **S3a provides** | raise the SP interrupt (scheduler's SP message); weak in the renderer |
| `void port_dp_full_sync(void)` | **S3a provides** | raise the DP interrupt (`OS_EVENT_DP`); weak in the renderer |
| `void port_gfx_vi_frame(const void *fb, int width)` | S3a VI, once per retrace (or on `osViSwapBuffer`) | blits colour image `fb` (any class: KSEG0/physical/wasm) of width 320/640 to the canvas, 4:3 letterboxed; CPU-drawn frames are uploaded from RDRAM |
| `void port_gfx_inval_dcache(const void *p, uint32_t n)` | S3a `osInvalDCache` | write-back for CPU reads |
| `void port_gfx_writeback_dcache(const void *p, uint32_t n)` | S3a `osWritebackDCache` (n) / `osWritebackDCacheAll` (n = 0) | CPU-write hint |
| `int port_gfx_init(void)` | optional at boot | creates the WebGL2 context on `Module.canvas` (`gfx_gl.js`); lazy otherwise; without a canvas (node `port-test`) lists are still interpreted and nothing is drawn |

Build integration (S3a owns `port/tools/port_build.py`; not edited here):
1. compile `port/gfx/gfx_rsp.c` and `port/gfx/gfx_rdp.c` with the PC-layer flags (they include `../ultra/pc_mem.h`;
   clean under `-Wall -Wextra`): add `port/gfx/*.c` to `pc_srcs`;
2. link with `--js-library port/gfx/gfx_gl.js -sMAX_WEBGL_VERSION=2 -sMIN_WEBGL_VERSION=2`;
3. the host (retrowser worker, a page, or the test) sets `Module.canvas` to an (Offscreen)Canvas before the first
   frame. retrowser: `video: "webgl"` gives exactly that.

## Testing (no ROM)

`port/gfx/test/run.sh [OUT]` builds `test/gfx_test.c` (display lists written with gbi.h's own macros, data in a
test RDRAM; `-sGLOBAL_BASE=256MB` so the address classes match the game), serves it on 127.0.0.1, runs the host's
Chrome headless (SwiftShader, muted) over the DevTools protocol (`test/cdp.mjs`, node's built-in WebSocket), and
writes `log.txt` (CHECK lines), `shown.png` / `shown2.png` (canvas) and `rdram.png` / `rdram2.png` (RDRAM after
write-back) to `port/build/gfx_test/out/` (gitignored). Two tasks, 17 checks: SP/DP events, culling, no unknown
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
- A Z image's RDRAM is never written (no game code reads it, as far as the survey shows).
- Performance: no measurement yet on real frames; per-draw uniform uploads; texture hash per tile change.
