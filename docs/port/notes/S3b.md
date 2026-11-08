# S3b notes (2026-11-08): F3DEX2 / L3DEX2 renderer on WebGL2

Parallel session to S3a (threads / DMA / overlays / VI in port/ultra, port/boot, port/tools). S3b owns only new files
under `port/gfx/` plus `docs/port/RENDERER.md` and this note. Design and interface: **docs/port/RENDERER.md**.

## Done

- `port/gfx/gfx.h` (interface), `gfx_internal.h`, `gfx_rsp.c` (display lists, RSP state on the CPU),
  `gfx_rdp.c` (TMEM, textures, generated combiner/blender shaders, colour-image targets coherent with RDRAM,
  VI blit), `gfx_gl.js` (WebGL2 on `Module.canvas`), `LICENSES.md`.
- Survey: S2DEX2 is not used (gtl.c's ucode table has it NULL); L3DEX2 is switched to mid-list; Snap's
  `gSPMvpRecalc` (G_SPECIAL_1) + `G_MW_MATRIX` patches; YUV sprites (26D80.c, window/8456E0.c).
- Offline harness `port/gfx/test/` (run.sh, gfx_test.c, worker.js, index.html, cdp.mjs): hand-made lists only,
  17 checks all OK, pictures looked at (frame 1: fills, Z, culling, Gouraud, RGBA16 wrap, CI4/TLUT, IA8 copy, fog,
  lighting, XLU, L3DEX2 lines, 64-wide image with own Z copied back, detector 8x8 region, CPU red dot reaching the
  canvas; frame 2: RGBA32, IA16, I4, mirror, 2-cycle TEXEL1, matrix stack, MVP patch, branch lists).
- Bug found by the test and fixed: a new texture was bound on the previous texture unit (2-cycle TEXEL1 clobbered
  TEXEL0).

## Findings / traps

- Headless Chrome `--virtual-time-budget` does not wait for worker computation (the page is dumped while the worker
  is still in WebGL); `--timeout` stops the page. The harness drives Chrome over the DevTools protocol instead and
  waits for `document.title == 'DONE'` in real time (node 24's global WebSocket; no packages).
- gbi.h needs `-D_LANGUAGE_C -DF3DEX_GBI_2` and `PR/mbi.h` (for `_SHIFTL`), `PR/ucode.h` (`SP_UCODE_DATA_SIZE`);
  gbi.h defines `ZMODE_DEC` itself (renamed ours `ZMODE_DECAL_SEL`); `gSPInsertMatrix` is an ERROR macro under
  F3DEX_GBI_2 (render.c uses `gMoveWd(G_MW_MATRIX, ...)`).
- The detector's regions are 128 bytes apart but drawn under a 32x32 scissor: colour targets sized by the scissor
  overlap, so targets must coexist and write back only their dirty rows (tracked per primitive, not per scissor).

## For S3a / the next session

1. `port_build.py`: add `port/gfx/*.c` to the PC-layer sources and `--js-library port/gfx/gfx_gl.js
   -sMAX_WEBGL_VERSION=2 -sMIN_WEBGL_VERSION=2` to the link.
2. `osSpTaskStart(M_GFXTASK)` → `port_gfx_task(task)`; provide `port_sp_task_done()` / `port_dp_full_sync()`.
3. VI retrace / `osViSwapBuffer` → `port_gfx_vi_frame(fb, 320)`; `osInvalDCache` → `port_gfx_inval_dcache`;
   `osWritebackDCache(All)` → `port_gfx_writeback_dcache(p, n | 0)`.
4. A local test page for the game that sets `Module.canvas` (or the retrowser host) to see the first frame.
5. Then: real-frame verification (pictures in `port_work/` only), unknown-opcode log review, performance.

## Downloads

None. Used local tools only: the `pokemonsnap-emsdk` image (emcc), the host's Google Chrome (headless, SwiftShader,
`--mute-audio`), host python3 (http.server) and node 24.
