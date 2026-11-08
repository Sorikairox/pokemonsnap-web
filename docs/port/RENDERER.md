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
  in, so the GPU clips; a triangle with a vertex at w <= 0 or in front of the near plane is first clipped on the CPU
  against the colour image's four side planes, like F3DEX2_NoN (R1, below). RSP state is reset per task (segments,
  matrices, geometry mode; R1: the othermode copy too).
- **RDP on WebGL2** (`gfx_rdp.c`): othermode, combiner, colours, scissor, tiles, byte-exact 4 KiB TMEM (LoadBlock
  with dxt odd-line swaps, LoadTile, LoadTLUT quadrupled, RGBA32 bank split; the sampler reads back with the same
  odd-row swap). Tiles decode to RGBA8 textures (RGBA16/32, CI4/CI8 with RGBA16/IA16 TLUT, IA4/8/16, I4/8, YUV16),
  cached by a hash of the TMEM bytes + descriptor (1024 entries). Wrap/mirror/clamp by mask → GL wrap modes, set on
  every bind from the tile (sampler state, not part of the cached texture: R1).
- **Othermode = the RSP's copy (R1)**: `G_SETOTHERMODE_H/L` merge into the RSP's DMEM copy and the RDP gets the whole
  word; the copy is reloaded from the microcode's data segment (offset 0xC8: `EF080CFF 00000000` in Snap's F3DEX2_NoN
  and L3DEX2 data, read from `OSTask.ucode_data` / the `G_LOAD_UCODE` data at run time) at every task start and
  ucode load. `G_RDPSETOTHERMODE` sets both.
- **Shaders**: one generated GLSL program per (combine, blender bits, cycle type, flags). Two combiner cycles with
  all N64 inputs (NOISE, K4/K5, PRIM_LOD_FRAC; LOD_FRACTION = 0); texels: `G_TF_POINT` and COPY nearest,
  `G_TF_BILERP` the RDP's 3-point filter (R2: three NEAREST fetches in the shader); the blender's first cycle and any cycle without
  memory are evaluated in the shader (fog: `FOG, SHADE_A, IN, 1MA`), a last cycle reading memory with `FORCE_BL`
  maps to GL blending (`SRC_ALPHA, f(B)`). Alpha compare (threshold / dither), `CVG_X_ALPHA + ALPHA_CVG_SEL`
  (texture edge, 0.5), Z compare/update/decal (polygon offset), primitive depth (`z / 32` is a screen z, mapped
  through the current viewport like a vertex: the game's own units, app_render/47380.c). FILL rectangles = flat quads of
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
write-back) to `port/build/gfx_test/out/` (gitignored). Two tasks, 23 checks (S6: `z_writeback`, a 64x48 Z image in
the console format + its COPY into a colour image; R1: `tile_resize_wrap`, `eye_plane_vertex`, `othermode_per_task`,
`prim_depth_units`; R2: `three_point_filter`; each failing without its fix): SP/DP events, culling, no unknown
opcodes / bad addresses, the detector's LoadTile + COPY into an 8-wide image equals the frame centre (64/64),
FILL, Z buffer, a 64-wide image with its own Z copied back via LoadBlock in the same task, CI4 + TLUT + alpha
compare, RGBA16 wrap, RGBA32, IA16, I4, mirror, 2-cycle TEXEL0/TEXEL1, matrix push/mul/pop, `gSPMvpRecalc` +
`G_MW_MATRIX` patch, branch lists; plus fog, lighting, XLU blending, L3DEX2 lines and the CPU red dot by eye.

## Not done / TODO

- Positional lights (`G_LIGHTING_POSITIONAL`), texture LOD / mipmaps / detail (`G_TL_LOD`, LOD_FRACTION = 0: the
  game never asks for them, R2 census), G_TF_AVERAGE (GL bilinear; unused), the filter's 5-bit fraction,
  chroma key, dithering, coverage/AA, YUV combiner path (YUV is
  converted at decode with the SetConvert K0..K3; K4/K5 are passed to the combiner), texgen scale checked only by
  reasoning, L3DEX2 line width formula approximate, NoN: side planes done (R1), but what is nearer than the near
  plane is still removed by GL (the console draws it with a clamped Z; WebGL has no depth clamp), the 16-bit Z format
  (clears map 0xFFFC → far linearly). `G_BRANCH_Z` (R1): the thresholds are `G_DEPTOZS` values for near 10 / far 25600
  (Beach: high detail below ~600 units, low detail to ~4470, nothing beyond); the renderer's screen z of the
  reference vertex matches that formula exactly, so the LOD choice follows the game's design.
- Clamp + mask with a tile larger than the mask (Beach cliffs: mask 64, tile 512x128, clamp bits): GL repeats
  forever; the RDP clamps at the tile edge. Only texels outside the tile differ (not seen on the route).
- Gouraud shade is interpolated perspective-correct by GL; the RDP interpolates it linearly in screen space (GLSL ES
  3.00 has no `noperspective`). Small on ordinary triangles; not emulated.
- RDP state that the console keeps across tasks while the port does not (or the reverse) is only checked for
  othermode (R1). Other DMEM state is reset by `rsp_reset` each task.
- CPU writes into an image that still has GPU-dirty rows (only partly written back) are not detected or uploaded;
  Snap's red dot follows a full-frame `osInvalDCache`, so it is not affected.
- Z values are the console's format by construction (linear depth → 18-bit z → compressed); not compared with a
  console capture. dz is always 0.
- Performance: no measurement yet on real frames; per-draw uniform uploads; texture hash per tile change.
- Seen on real frames (S3c): every texture rectangle preceded by a texture load is its own draw (rectangles that
  share a load are batched): ~8000 draws per 150 retraces on the menus; fine on SwiftShader (5.6 ms per retrace for
  the whole game). Batching across loads needs a texture atlas or array (not done; measure on phones first).

## Session R1 (renderer fixes; notes: `docs/port/notes/R1.md`)

| Seen | Root cause (evidence) | Fix |
|---|---|---|
| Beach cliffs, end tunnel flat grey | a CI4 64x64 texture drawn first with a 64x64 tile (clamp), then with a 512x128 tile size over the same TMEM; the tile fast path returned the CLAMP texture: every texel past 64 = the edge column (probe: bound texture wrap `0x812F`, s 376..436) | wrap modes per bind |
| lighter rectangles in the sky's top corners | background mesh vertices at w = 0 (z < -w); GL near-clipped them into w ~ 10 vertices far off screen and SwiftShader filled the strips with one texel and one shade (projective t should go 29 → 6) | CPU side-plane clipping (F3DEX2_NoN) |
| darker left band on Oak's check (x < 232 canvas) | intended: photo_check/98C330.c `func_801DE02C` slides the "black square" sprite (width 132, alpha 128) to x = -12: 450 XLU rectangles of prim alpha 0x80 over x 0..116 | none |
| intro "mist strip" with hard right / bottom edges | a fogged IA8 bush billboard (alpha opaque at s = 0, t = 0: its polygon edges) far behind the hill was drawn over it: the UI's `G_ZS_PRIM` of the previous task leaked (render modes only set the upper 29 bits), so **every world triangle used the primitive depth** | RSP othermode copy reloaded per task / ucode load |
| (uncovered by the above) | primitive depth mapped `z / 32767` while vertices map screen z / 1022: up to 2 screen-z units nearer | `z / 32` through the viewport |

Debug tool: `port_gfx_set_probe(x, y)` (up to 4 points, `x < 0` clears) logs, while `--logops` is on, every
triangle (homogeneous test: triangles crossing w = 0 included) and texture rectangle covering that N64 pixel, with
its vertices and the RDP state. Not exported yet: add `_port_gfx_set_probe` to port_build.py's EXPORTED_FUNCTIONS
and a `cfg.probe` to port/web/worker.js (owned by other sessions).

## Session R2 (renderer polish; notes: `docs/port/notes/R2.md`)

| Seen (S10a list) | Root cause (evidence) | Fix |
|---|---|---|
| Valley / Volcano rocks blurry close up | the game asks for no LOD, mip level or detail texture anywhere (assets + src: every `SetTextureLOD(G_TL_TILE)`, `SetTextureDetail(G_TD_CLAMP)`, `gSPTexture` level 0; Valley frame 7801: othermode H `...AC0F`, no `G_TL_LOD`). The rock is a CI4 32x128 texture (mask) magnified ~3.5x (probe: s 95..112 over 60 px): blurry on the RDP too. The port's only departure was the filter: GL's 4-texel bilinear for `G_TF_BILERP`, which on the RDP is a 3-point filter | **3-point filter in the shader** (NEAREST fetches at texel centres; wrap/mirror/clamp per texel by the sampler); test `three_point_filter` |
| Cave floor "seam" near the end gate | not the renderer: the straight line is the shared rim of the floor and wall triangles; four rim vertices with w 950..1238 all project to screen y 142.1..142.3, i.e. the rim lies in a plane through the eye (the end-scene camera is at the ledge's height). Nothing near the near plane (z/w >= 0.92, w >= 249), no clipping, depths ordered | none |
| Credits 17: dots inside the staff box | not the renderer: the box bitmaps (`UIMem_Allocate(14*140*2)`, credits A937C0.c) of boxes with no photo yet start with the allocator's free-list link (next, prev); the sprite (RGBA16, XLU) shows the pointer halves as texels. Port pointers are wasm addresses (`0x10A5xxxx`, `0x102Bxxxx`: both high halves odd = opaque texels 0 and 2); on the console (`0x8025xxxx`, `0x803A6910`) only texel 0 is opaque and its colour differs. Memory-layout effect (MEMORY.md trap 2) | none in port/gfx (see notes) |
| Tunnel purple / teal / yellow walls | the game's own colours: lighting off (geometry mode `00210405`), shade = the vertex colours, byte-identical to the ROM's tunnel vertex data (116,30,144 / 24,73,98 / 96,93,37 / 155,46,104 found in `tunnel/block*/model.vtx`); combiner TEXEL0 * SHADE, fog colour 12515C | none |
