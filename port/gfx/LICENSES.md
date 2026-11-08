# port/gfx provenance

Everything in this directory was written for the Pokémon Snap port in session S3b (2026-11-08). No third-party code
was copied or adapted.

- GBI command encodings, constants and macros: the decomp's own `ultralib/include/PR/gbi.h` (F3DEX_GBI_2). The test
  (`test/gfx_test.c`) builds its display lists with those macros and `_Static_assert`s the renderer's constants
  against the header.
- RDP / RSP behaviour (TMEM layout, LoadBlock dxt, TLUT quadrupling, combiner and blender equations, F3DEX2 lighting,
  fog and matrix conventions): public N64 programming documentation (n64brew, the SDK manuals' descriptions).
- Architecture model only (CPU vertex transform + one generated GPU shader per combiner state): Fast3D
  (sm64-port / libultraship, MIT). No code taken.
- `gfx_gl.js` (WebGL2 context on `Module.canvas`) follows the same idea as the Conker web port's
  `port/web/rtw/rtw_lib.js` `port_rtw_gl_create` (the user's own repository); rewritten here.
- Not used and not to be used unless the whole port becomes GPL: GLideN64, parallel-rdp, angrylion, mupen64plus.

If code is ever adapted from another renderer, record its name, URL, commit and licence here.
