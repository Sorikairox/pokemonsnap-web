/* port/gfx offline test (S3b): hand-written F3DEX2 / L3DEX2 display lists, built with the decomp's own gbi.h macros,
 * rendered by port/gfx into a test RDRAM, then shown on the canvas and written back to RDRAM. No ROM data.
 * Built and run by port/gfx/test/run.sh (headless Chrome, SwiftShader). Checks printed as "CHECK name OK|FAIL".
 *
 * Memory model as in the game build: the test RDRAM n64_rdram stands for the mirror (KSEG0 / physical addresses map
 * into it), the module is linked with -sGLOBAL_BASE=256MB so C arrays have wasm addresses >= 0x10000000, and game
 * memory is virtual big-endian (words native, halfwords at a ^ 2, bytes at a ^ 3). */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <PR/mbi.h>
#include <PR/sptask.h>
#include <PR/ucode.h>
#include "../gfx_internal.h"
#define gSPMvpRecalc(pkt) gImmp21((pkt), G_SPECIAL_1, 0, 1, 0)   /* src/sys/render.c */

/* the renderer's constants are the header's */
_Static_assert(GM_ZBUFFER == G_ZBUFFER && GM_SHADE == G_SHADE && GM_CULL_BACK == G_CULL_BACK && GM_CULL_FRONT == G_CULL_FRONT, "geom");
_Static_assert(GM_FOG == G_FOG && GM_LIGHTING == G_LIGHTING && GM_TEXTURE_GEN == G_TEXTURE_GEN && GM_SHADING_SMOOTH == G_SHADING_SMOOTH, "geom2");
_Static_assert(GM_TEXTURE_GEN_LINEAR == G_TEXTURE_GEN_LINEAR && GM_LOD == G_LOD && GM_CLIPPING == G_CLIPPING, "geom3");
_Static_assert(OMH_CYCLETYPE_SFT == G_MDSFT_CYCLETYPE && OMH_TEXTLUT_SFT == G_MDSFT_TEXTLUT && OMH_TEXTFILT_SFT == G_MDSFT_TEXTFILT, "omh");
_Static_assert(OMH_TEXTCONV_SFT == G_MDSFT_TEXTCONV && OMH_TEXTPERSP_SFT == G_MDSFT_TEXTPERSP, "omh2");
_Static_assert(OML_ZS_PRIM == G_ZS_PRIM && OML_Z_CMP == Z_CMP && OML_Z_UPD == Z_UPD && OML_AA_EN == AA_EN && OML_IM_RD == IM_RD, "oml");
_Static_assert(OML_CVG_X_ALPHA == CVG_X_ALPHA && OML_ALPHA_CVG_SEL == ALPHA_CVG_SEL && OML_FORCE_BL == FORCE_BL, "oml2");
_Static_assert((ZMODE_DECAL_SEL << 10) == ZMODE_DEC && FMT_CI == G_IM_FMT_CI && FMT_IA == G_IM_FMT_IA && FMT_YUV == G_IM_FMT_YUV, "fmt");
_Static_assert(G_MV_VIEWPORT == 8 && G_MV_LIGHT == 10 && G_MV_MATRIX == 14 && G_MW_SEGMENT == 6 && G_MW_FOG == 8, "mv");

/* ------------------------------------------------------------------ the test machine */
unsigned char n64_rdram[PORT_RDRAM_SIZE] __attribute__((aligned(64)));
void port_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

#define K0(phys) (0x80000000u | (phys))
#define FB_PHYS 0x300000u
#define Z_PHYS 0x340000u
#define REGION_PHYS 0x380000u    /* 8-wide detector region (like PokemonDetector_Regions) */
#define SMALL_PHYS 0x390000u     /* 64-wide image with its own Z (like score.c) */
#define SMALLZ_PHYS 0x398000u
#define ZCOPY_PHYS 0x39A000u     /* score.c's "Z before": the Z image copied into a colour image */
#define TEX_PHYS 0x200000u       /* textures, reached through segment 6 */
#define L3DEX2_TEXT 0x0003FB30u  /* where the game's gspL3DEX2_fifoTextStart lives (physical) */

static Gfx dl[8192];
static Gfx sub_dl[64];
static uint32_t vtx_mem[64 * 4 * 16];
static uint32_t mtx_mem[16 * 16];
static uint32_t vp_mem[4];
static uint32_t light_mem[16];
static unsigned vtx_used, mtx_used;

static void *rd(uint32_t phys) { return n64_rdram + phys; }

/* a Vtx in the vbe layout (s16 ob[3], u16 flag, s16 tc[2], u8 cn[4]) */
static uint32_t *vtx(int n) { uint32_t *v = &vtx_mem[vtx_used * 4]; vtx_used += (unsigned)n; return v; }
static void vset(uint32_t *v, int i, int x, int y, int z, int s, int t, uint32_t rgba) {
    v[i * 4 + 0] = (uint32_t)(x & 0xFFFF) << 16 | (uint32_t)(y & 0xFFFF);
    v[i * 4 + 1] = (uint32_t)(z & 0xFFFF) << 16;
    v[i * 4 + 2] = (uint32_t)(s & 0xFFFF) << 16 | (uint32_t)(t & 0xFFFF);
    v[i * 4 + 3] = rgba;
}
/* Mtx (s15.16, integer halves then fractions) from a float matrix, row vectors like guMtxF2L */
static uint32_t *mtx(const float m[4][4]) {
    uint32_t *w = &mtx_mem[mtx_used++ * 16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j += 2) {
            int32_t a = (int32_t)(m[i][j] * 65536.0f), b = (int32_t)(m[i][j + 1] * 65536.0f);
            w[i * 2 + j / 2] = ((uint32_t)a & 0xFFFF0000u) | ((uint32_t)b >> 16);
            w[8 + i * 2 + j / 2] = ((uint32_t)a << 16) | ((uint32_t)b & 0xFFFFu);
        }
    return w;
}
static void ident(float m[4][4]) { for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) m[i][j] = i == j; }
/* ortho: x 0..320 -> -1..1, y 0..240 (down) -> 1..-1, z -1000..1000 -> -1..1 */
static void ortho(float m[4][4]) {
    ident(m);
    m[0][0] = 1 / 160.0f; m[3][0] = -1;
    m[1][1] = -1 / 120.0f; m[3][1] = 1;
    m[2][2] = 1 / 1000.0f;
}
static uint16_t rgba16(int r, int g, int b, int a) { return (uint16_t)((r >> 3) << 11 | (g >> 3) << 6 | (b >> 3) << 1 | (a ? 1 : 0)); }
static uint16_t fb_px(uint32_t phys, int w, int x, int y) { return (uint16_t)vbe_ld16(rd(phys + (uint32_t)(y * w + x) * 2)); }

static int checks_ok = 1;
static void check(const char *name, int ok, const char *fmt, ...) {
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    printf("CHECK %s %s %s\n", name, ok ? "OK" : "FAIL", buf);
    if (!ok) checks_ok = 0;
}

/* ------------------------------------------------------------------ textures (hand-made, in RDRAM via segment 6) */
static void make_textures(void) {
    /* 32x32 RGBA16 checkerboard (8-texel squares) with a coloured border texel row */
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) {
            int c = ((x >> 3) ^ (y >> 3)) & 1;
            uint16_t p = c ? rgba16(255, 255, 255, 1) : rgba16(255, 128, 0, 1);
            if (y == 0) p = rgba16(0, 255, 0, 1);
            vbe_st16(rd(TEX_PHYS + (uint32_t)(y * 32 + x) * 2), p);
        }
    /* 16x16 CI4 "ring" + 16-entry palette */
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x += 2) {
            int i0 = ((x - 8) * (x - 8) + (y - 8) * (y - 8)) / 5, i1 = ((x - 7) * (x - 7) + (y - 8) * (y - 8)) / 5;
            if (i0 > 15) i0 = 15;
            if (i1 > 15) i1 = 15;
            vbe_st8(rd(TEX_PHYS + 0x1000 + (uint32_t)(y * 8 + x / 2)), (unsigned)(i0 << 4 | i1));
        }
    for (int i = 0; i < 16; i++) vbe_st16(rd(TEX_PHYS + 0x1100 + (uint32_t)i * 2), i == 15 ? 0 : rgba16(i * 16, 64, 255 - i * 16, 1));
    /* 32x16 IA8 gradient (intensity across, alpha full) */
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 32; x++) vbe_st8(rd(TEX_PHYS + 0x1200 + (uint32_t)(y * 32 + x)), (unsigned)((x / 2) << 4 | 15));
}

/* ------------------------------------------------------------------ the frame */
static void build_frame(Gfx **pg) {
    Gfx *g = *pg;
    float m[4][4];
    gSPSegment(g++, 0x06, K0(TEX_PHYS));
    gSPSegment(g++, 0x0F, FB_PHYS);   /* physical, like the scheduler's segment F patch */
    /* viewport 320x240 */
    vp_mem[0] = (uint32_t)640 << 16 | 480; vp_mem[1] = (uint32_t)511 << 16;
    vp_mem[2] = (uint32_t)640 << 16 | 480; vp_mem[3] = (uint32_t)511 << 16;
    gSPViewport(g++, vp_mem);
    gDPPipeSync(g++);
    gDPSetCycleType(g++, G_CYC_FILL);
    gDPSetRenderMode(g++, G_RM_NOOP, G_RM_NOOP2);
    gDPSetScissor(g++, G_SC_NON_INTERLACE, 0, 0, 320, 240);
    /* clear Z (the Z image as a colour image, like render.c) */
    gDPSetColorImage(g++, G_IM_FMT_RGBA, G_IM_SIZ_16b, 320, K0(Z_PHYS));
    gDPSetDepthImage(g++, K0(Z_PHYS));
    gDPSetFillColor(g++, GPACK_ZDZ(G_MAXFBZ, 0) << 16 | GPACK_ZDZ(G_MAXFBZ, 0));
    gDPFillRectangle(g++, 0, 0, 319, 239);
    gDPPipeSync(g++);
    /* clear the frame through segment F */
    gDPSetColorImage(g++, G_IM_FMT_RGBA, G_IM_SIZ_16b, 320, 0x0F000000);
    gDPSetFillColor(g++, (uint32_t)rgba16(16, 24, 64, 1) << 16 | rgba16(16, 24, 64, 1));
    gDPFillRectangle(g++, 0, 0, 319, 239);
    /* fill bars (FILL mode) */
    const uint16_t bars[4] = {rgba16(255, 0, 0, 1), rgba16(0, 255, 0, 1), rgba16(0, 0, 255, 1), rgba16(255, 255, 255, 1)};
    for (int i = 0; i < 4; i++) {
        gDPPipeSync(g++);
        gDPSetFillColor(g++, (uint32_t)bars[i] << 16 | bars[i]);
        gDPFillRectangle(g++, 8 + i * 20, 8, 8 + i * 20 + 15, 31);
    }
    /* centre target for the detector test: a magenta square with a yellow quarter */
    gDPPipeSync(g++);
    gDPSetFillColor(g++, (uint32_t)rgba16(255, 0, 255, 1) << 16 | rgba16(255, 0, 255, 1));
    gDPFillRectangle(g++, 150, 110, 169, 129);
    gDPPipeSync(g++);
    gDPSetFillColor(g++, (uint32_t)rgba16(255, 255, 0, 1) << 16 | rgba16(255, 255, 0, 1));
    gDPFillRectangle(g++, 160, 120, 169, 129);

    /* 3D state */
    gDPPipeSync(g++);
    gDPSetCycleType(g++, G_CYC_1CYCLE);
    gDPSetRenderMode(g++, G_RM_ZB_OPA_SURF, G_RM_ZB_OPA_SURF2);
    gDPSetCombineMode(g++, G_CC_SHADE, G_CC_SHADE);
    gSPClearGeometryMode(g++, 0xFFFFFFFF);
    gSPSetGeometryMode(g++, G_ZBUFFER | G_SHADE | G_SHADING_SMOOTH | G_CULL_BACK);
    gSPTexture(g++, 0, 0, 0, G_TX_RENDERTILE, G_OFF);
    ortho(m);
    gSPMatrix(g++, mtx(m), G_MTX_PROJECTION | G_MTX_LOAD | G_MTX_NOPUSH);
    ident(m);
    gSPMatrix(g++, mtx(m), G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
    /* Z test: a near green triangle drawn first, then a far red one over it (must stay hidden where they overlap);
     * plus a back-facing triangle that must be culled */
    uint32_t *v = vtx(9);
    vset(v, 0, 20, 60, -100, 0, 0, 0x00FF00FF); vset(v, 1, 100, 140, -100, 0, 0, 0x00C000FF); vset(v, 2, 100, 60, -100, 0, 0, 0x008000FF);
    vset(v, 3, 40, 50, 100, 0, 0, 0xFF0000FF); vset(v, 4, 120, 120, 100, 0, 0, 0xFF0000FF); vset(v, 5, 120, 50, 100, 0, 0, 0xFF4040FF);
    vset(v, 6, 20, 150, 0, 0, 0, 0xFFFFFFFF); vset(v, 7, 60, 150, 0, 0, 0, 0xFFFFFFFF); vset(v, 8, 20, 200, 0, 0, 0, 0xFFFFFFFF);
    gSPVertex(g++, v, 9, 0);
    /* screen y is down: (20,60) (100,140) (100,60) is clockwise on screen = counter-clockwise in NDC = front */
    gSP1Triangle(g++, 0, 1, 2, 0);
    gSP1Triangle(g++, 3, 4, 5, 0);
    gSP1Triangle(g++, 6, 7, 8, 0);   /* (20,150) (60,150) (20,200): back-facing, culled */
    /* Gouraud triangle with a G_DL call through segment 6-free wasm pointer */
    {
        Gfx *s = sub_dl;
        uint32_t *w = vtx(3);
        vset(w, 0, 130, 200, 0, 0, 0, 0xFF0000FF); vset(w, 1, 220, 200, 0, 0, 0, 0x0000FFFF); vset(w, 2, 175, 140, 0, 0, 0, 0x00FF00FF);
        gSPVertex(s++, w, 3, 0);
        gSP1Triangle(s++, 0, 1, 2, 0);
        gSPEndDisplayList(s++);
        gSPDisplayList(g++, sub_dl);
    }
    /* textured quad: RGBA16 32x32 via LoadBlock (dxt), point sampled, G_CC_DECALRGBA, s/t 0..64 texels (wraps 2x) */
    gDPPipeSync(g++);
    gDPSetCombineMode(g++, G_CC_DECALRGBA, G_CC_DECALRGBA);
    gDPSetTextureFilter(g++, G_TF_POINT);
    gDPSetTexturePersp(g++, G_TP_PERSP);
    gSPTexture(g++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON);
    gDPLoadTextureBlock(g++, 0x06000000, G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, 32, 0, G_TX_WRAP, G_TX_WRAP, 5, 5, G_TX_NOLOD, G_TX_NOLOD);
    v = vtx(4);
    vset(v, 0, 230, 40, 0, 0, 0, 0xFFFFFFFF); vset(v, 1, 230, 104, 0, 0, 64 << 5, 0xFFFFFFFF);
    vset(v, 2, 294, 104, 0, 64 << 5, 64 << 5, 0xFFFFFFFF); vset(v, 3, 294, 40, 0, 64 << 5, 0, 0xFFFFFFFF);
    gSPVertex(g++, v, 4, 0);
    gSP2Triangles(g++, 0, 1, 2, 0, 0, 2, 3, 0);
    /* CI4 texrect with TLUT, scaled 2x, 1-cycle, alpha compare (palette entry 15 is transparent) */
    gDPPipeSync(g++);
    gDPSetTextureLUT(g++, G_TT_RGBA16);
    gDPLoadTLUT_pal16(g++, 0, 0x06001100);
    gDPLoadTextureBlock_4b(g++, 0x06001000, G_IM_FMT_CI, 16, 16, 0, G_TX_CLAMP, G_TX_CLAMP, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
    gDPSetRenderMode(g++, G_RM_OPA_SURF, G_RM_OPA_SURF2);
    gDPSetCombineMode(g++, G_CC_DECALRGBA, G_CC_DECALRGBA);
    gDPSetAlphaCompare(g++, G_AC_THRESHOLD);
    gDPSetBlendColor(g++, 0, 0, 0, 1);
    gSPTextureRectangle(g++, 230 << 2, 120 << 2, 262 << 2, 152 << 2, G_TX_RENDERTILE, 0, 0, 1 << 9, 1 << 9);
    gDPSetAlphaCompare(g++, G_AC_NONE);
    gDPSetTextureLUT(g++, G_TT_NONE);
    /* IA8 gradient, COPY mode texrect 32x16 at (270, 120) */
    gDPPipeSync(g++);
    gDPSetCycleType(g++, G_CYC_COPY);
    gDPSetRenderMode(g++, G_RM_NOOP, G_RM_NOOP2);
    gDPLoadTextureBlock(g++, 0x06001200, G_IM_FMT_IA, G_IM_SIZ_8b, 32, 16, 0, G_TX_CLAMP, G_TX_CLAMP, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
    gSPTextureRectangle(g++, 270 << 2, 120 << 2, 301 << 2, 135 << 2, G_TX_RENDERTILE, 0, 0, 4 << 10, 1 << 10);
    /* fog: 2-cycle G_RM_FOG_SHADE_A, white fog over a blue strip whose z goes from near to far */
    gDPPipeSync(g++);
    gDPSetCycleType(g++, G_CYC_2CYCLE);
    gDPSetRenderMode(g++, G_RM_FOG_SHADE_A, G_RM_OPA_SURF2);
    gDPSetCombineMode(g++, G_CC_SHADE, G_CC_PASS2);
    gDPSetFogColor(g++, 255, 255, 255, 255);
    gSPFogPosition(g++, 0, 1000);
    gSPSetGeometryMode(g++, G_FOG);
    gSPClearGeometryMode(g++, G_ZBUFFER | G_CULL_BACK);
    gSPTexture(g++, 0, 0, 0, G_TX_RENDERTILE, G_OFF);
    v = vtx(4);
    vset(v, 0, 10, 210, -1000, 0, 0, 0x2040FFFF); vset(v, 1, 10, 230, -1000, 0, 0, 0x2040FFFF);
    vset(v, 2, 120, 230, 1000, 0, 0, 0x2040FFFF); vset(v, 3, 120, 210, 1000, 0, 0, 0x2040FFFF);
    gSPVertex(g++, v, 4, 0);
    gSP2Triangles(g++, 0, 1, 2, 0, 0, 2, 3, 0);
    gSPClearGeometryMode(g++, G_FOG);
    /* lighting: one directional light from +x, ambient dark; normals fan from -x to +x across a quad */
    gDPPipeSync(g++);
    gDPSetCycleType(g++, G_CYC_1CYCLE);
    gDPSetRenderMode(g++, G_RM_OPA_SURF, G_RM_OPA_SURF2);
    gDPSetCombineMode(g++, G_CC_SHADE, G_CC_SHADE);
    gSPSetGeometryMode(g++, G_LIGHTING);
    /* Light: col rgb pad, colc rgb pad, dir xyz pad (bytes, BE order in words); Ambient: col, colc */
    light_mem[0] = 0xFFE0C000; light_mem[1] = 0xFFE0C000; light_mem[2] = 0x7F000000; light_mem[3] = 0;
    light_mem[4] = 0x20202000; light_mem[5] = 0x20202000;
    gSPNumLights(g++, NUMLIGHTS_1);
    gSPLight(g++, &light_mem[0], 1);
    gSPLight(g++, &light_mem[4], 2);
    v = vtx(4);
    vset(v, 0, 130, 210, 0, 0, 0, 0x81000000); vset(v, 1, 130, 232, 0, 0, 0, 0x81000000);   /* normal -x */
    vset(v, 2, 220, 232, 0, 0, 0, 0x7F000000); vset(v, 3, 220, 210, 0, 0, 0, 0x7F000000);   /* normal +x */
    gSPVertex(g++, v, 4, 0);
    gSP2Triangles(g++, 0, 1, 2, 0, 0, 2, 3, 0);
    gSPClearGeometryMode(g++, G_LIGHTING);
    /* translucent prim-colour rectangle (XLU, alpha 0.5) over the bars */
    gDPPipeSync(g++);
    gDPSetRenderMode(g++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
    gDPSetCombineMode(g++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);
    gDPSetPrimColor(g++, 0, 0, 255, 255, 255, 128);
    gDPFillRectangle(g++, 4, 20, 100, 40);
    /* L3DEX2: switch ucode, draw lines, switch back */
    gDPPipeSync(g++);
    gDPSetRenderMode(g++, G_RM_OPA_SURF, G_RM_OPA_SURF2);
    gDPSetCombineMode(g++, G_CC_SHADE, G_CC_SHADE);
    gSPLoadUcode(g++, L3DEX2_TEXT, 0x00045280);
    v = vtx(4);
    vset(v, 0, 240, 160, 0, 0, 0, 0xFFFF00FF); vset(v, 1, 310, 200, 0, 0, 0, 0xFF00FFFF);
    vset(v, 2, 240, 200, 0, 0, 0, 0x00FFFFFF); vset(v, 3, 310, 160, 0, 0, 0, 0xFFFFFFFF);
    gSPVertex(g++, v, 4, 0);
    gSPLine3D(g++, 0, 1, 0);
    gSPLineW3D(g++, 2, 3, 4, 0);
    gSPLoadUcode(g++, 0x0003E7A0, 0x00044E60);
    *pg = g;
}

/* the detector's two lists (pokemon_detect.c), copied: LoadTile of the frame's centre 8x8, COPY into an 8-wide image */
static void build_detector(Gfx **pg) {
    Gfx *g = *pg;
    gDPPipeSync(g++);
    gDPSetTextureLUT(g++, G_TT_NONE);
    gDPSetTextureImage(g++, G_IM_FMT_RGBA, G_IM_SIZ_16b, 320, 0x0F000000);
    gDPSetTile(g++, G_IM_FMT_RGBA, G_IM_SIZ_16b, 2, 0x0000, G_TX_LOADTILE, 0, G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMASK, G_TX_NOLOD);
    gDPLoadSync(g++);
    gDPLoadTile(g++, G_TX_LOADTILE, 156 << 2, 116 << 2, 163 << 2, 123 << 2);
    gDPPipeSync(g++);
    gDPSetTile(g++, G_IM_FMT_RGBA, G_IM_SIZ_16b, 2, 0x0000, G_TX_RENDERTILE, 0, G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMASK, G_TX_NOLOD);
    gDPSetTileSize(g++, G_TX_RENDERTILE, 0, 0, 7 << 2, 7 << 2);
    gDPSetColorImage(g++, G_IM_FMT_RGBA, G_IM_SIZ_16b, 8, K0(REGION_PHYS));
    gDPSetCycleType(g++, G_CYC_COPY);
    gDPSetRenderMode(g++, G_RM_NOOP, G_RM_NOOP2);
    gDPSetTexturePersp(g++, G_TP_NONE);
    gDPSetAlphaCompare(g++, G_AC_NONE);
    gDPSetScissor(g++, G_SC_NON_INTERLACE, 0, 0, 32, 32);
    gSPTextureRectangle(g++, 0 << 2, 0 << 2, 7 << 2, 7 << 2, G_TX_RENDERTILE, 0, 0, 4 << 10, 1 << 10);
    gDPPipeSync(g++);
    gDPSetCycleType(g++, G_CYC_2CYCLE);
    gDPSetRenderMode(g++, G_RM_FOG_SHADE_A, G_RM_AA_ZB_OPA_SURF2);
    gDPSetTexturePersp(g++, G_TP_PERSP);
    gDPSetColorImage(g++, G_IM_FMT_RGBA, G_IM_SIZ_16b, 320, 0x0F000000);
    gDPSetScissor(g++, G_SC_NON_INTERLACE, 0, 0, 320, 240);
    *pg = g;
}

/* score.c style: render into a 64x48 image with its own Z, then copy it into the frame from RDRAM in the same task */
static void build_small(Gfx **pg) {
    Gfx *g = *pg;
    float m[4][4];
    gDPPipeSync(g++);
    gDPSetCycleType(g++, G_CYC_FILL);
    gDPSetRenderMode(g++, G_RM_NOOP, G_RM_NOOP2);
    gDPSetColorImage(g++, G_IM_FMT_RGBA, G_IM_SIZ_16b, 64, K0(SMALLZ_PHYS));
    gDPSetFillColor(g++, 0xFFFCFFFC);
    gDPSetScissor(g++, G_SC_NON_INTERLACE, 0, 0, 64, 48);
    gDPFillRectangle(g++, 0, 0, 63, 47);
    gDPPipeSync(g++);
    gDPSetColorImage(g++, G_IM_FMT_RGBA, G_IM_SIZ_16b, 64, K0(SMALL_PHYS));
    gDPSetDepthImage(g++, K0(SMALLZ_PHYS));
    gDPSetFillColor(g++, (uint32_t)rgba16(0, 96, 0, 1) << 16 | rgba16(0, 96, 0, 1));
    gDPFillRectangle(g++, 0, 0, 63, 47);
    gDPPipeSync(g++);
    gDPSetCycleType(g++, G_CYC_1CYCLE);
    gDPSetRenderMode(g++, G_RM_ZB_OPA_SURF, G_RM_ZB_OPA_SURF2);
    gDPSetCombineMode(g++, G_CC_SHADE, G_CC_SHADE);
    gSPSetGeometryMode(g++, G_ZBUFFER | G_SHADE | G_SHADING_SMOOTH);
    /* viewport 64x48 */
    static uint32_t vp_small[4];
    vp_small[0] = (uint32_t)128 << 16 | 96; vp_small[1] = (uint32_t)511 << 16;
    vp_small[2] = (uint32_t)128 << 16 | 96; vp_small[3] = (uint32_t)511 << 16;
    gSPViewport(g++, vp_small);
    ortho(m);
    gSPMatrix(g++, mtx(m), G_MTX_PROJECTION | G_MTX_LOAD | G_MTX_NOPUSH);
    ident(m);
    gSPMatrix(g++, mtx(m), G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
    uint32_t *v = vtx(3);   /* in the 320x240 ortho space: maps onto the 64x48 image */
    vset(v, 0, 40, 200, 0, 0, 0, 0xFFFF00FF); vset(v, 1, 280, 200, 0, 0, 0, 0xFF8000FF); vset(v, 2, 160, 30, 0, 0, 0, 0xFFFFFFFF);
    gSPVertex(g++, v, 3, 0);
    gSP1Triangle(g++, 0, 1, 2, 0);
    /* score.c: the Z image's upper half copied into a 64-wide colour image through a texture load, same task */
    gDPPipeSync(g++);
    gDPSetCycleType(g++, G_CYC_COPY);
    gDPSetRenderMode(g++, G_RM_NOOP, G_RM_NOOP2);
    gDPSetTexturePersp(g++, G_TP_NONE);
    gDPLoadTextureBlock(g++, K0(SMALLZ_PHYS), G_IM_FMT_RGBA, G_IM_SIZ_16b, 64, 24, 0, G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
    gDPSetColorImage(g++, G_IM_FMT_RGBA, G_IM_SIZ_16b, 64, K0(ZCOPY_PHYS));
    gSPTextureRectangle(g++, 0, 0, 0x00FC, 0x005C, G_TX_RENDERTILE, 0, 0, 0x1000, 0x0400);
    /* back to the frame, copy the small image to (248, 184) */
    gDPPipeSync(g++);
    gSPViewport(g++, vp_mem);
    gDPSetColorImage(g++, G_IM_FMT_RGBA, G_IM_SIZ_16b, 320, 0x0F000000);
    gDPSetDepthImage(g++, K0(Z_PHYS));
    gDPSetScissor(g++, G_SC_NON_INTERLACE, 0, 0, 320, 240);
    gDPSetCycleType(g++, G_CYC_COPY);
    gDPSetRenderMode(g++, G_RM_NOOP, G_RM_NOOP2);
    gDPSetTexturePersp(g++, G_TP_NONE);
    gDPLoadTextureBlock(g++, K0(SMALL_PHYS), G_IM_FMT_RGBA, G_IM_SIZ_16b, 64, 24, 0, G_TX_CLAMP, G_TX_CLAMP, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
    gSPTextureRectangle(g++, 248 << 2, 184 << 2, 311 << 2, 207 << 2, G_TX_RENDERTILE, 0, 0, 4 << 10, 1 << 10);
    gDPLoadTextureBlock(g++, K0(SMALL_PHYS + 64 * 24 * 2), G_IM_FMT_RGBA, G_IM_SIZ_16b, 64, 24, 0, G_TX_CLAMP, G_TX_CLAMP, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
    gSPTextureRectangle(g++, 248 << 2, 208 << 2, 311 << 2, 231 << 2, G_TX_RENDERTILE, 0, 0, 4 << 10, 1 << 10);
    *pg = g;
}

/* OSTask image (32-bit fields): type, flags, boot, boot size, ucode, size, data, size, stack, size, out, out size,
 * data_ptr, data_size, yield, yield size */
static uint32_t task[16];
static unsigned char rgba_out[320 * 240 * 4];
static unsigned char *to_rgba(uint32_t fb) {
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 320; x++) {
            uint16_t c = fb_px(fb, 320, x, y);
            unsigned char *o = rgba_out + (y * 320 + x) * 4;
            o[0] = (unsigned char)((c >> 11) << 3); o[1] = (unsigned char)(((c >> 6) & 31) << 3);
            o[2] = (unsigned char)(((c >> 1) & 31) << 3); o[3] = 255;
        }
    return rgba_out;
}

unsigned char *gfx_test_run(void) {
    printf("gfx test: n64_rdram at %p, dl at %p (address classes: mirror / wasm)\n", (void *)n64_rdram, (void *)dl);
    make_textures();
    Gfx *g = dl;
    build_frame(&g);
    build_detector(&g);
    build_small(&g);
    gDPFullSync(g++);
    gSPEndDisplayList(g++);
    memset(task, 0, sizeof task);
    task[0] = M_GFXTASK;
    task[4] = 0x0003E7A0;   /* F3DEX2 text */
    task[12] = (uint32_t)(uintptr_t)dl;
    port_gfx_task(task);
    printf("stats: cmds %u tris %u lines %u rects %u fills %u draws %u verts %u culled %u tex decodes %u hits %u "
           "resolves %u (%u px) uploads %u unknown %u bad addr %u\n",
           port_gfx_stats.cmds, port_gfx_stats.tris, port_gfx_stats.lines, port_gfx_stats.rects, port_gfx_stats.fills,
           port_gfx_stats.draws, port_gfx_stats.verts, port_gfx_stats.culled, port_gfx_stats.tex_decodes,
           port_gfx_stats.tex_hits, port_gfx_stats.resolves, port_gfx_stats.resolve_px, port_gfx_stats.uploads,
           port_gfx_stats.unknown_ops, port_gfx_stats.bad_addr);
    check("events", port_gfx_stats.tasks == 1 && port_gfx_stats.fullsyncs == 1, "tasks %u full syncs %u",
          port_gfx_stats.tasks, port_gfx_stats.fullsyncs);
    check("culling", port_gfx_stats.culled == 1, "%u culled", port_gfx_stats.culled);
    check("unknown_ops", port_gfx_stats.unknown_ops == 0 && port_gfx_stats.bad_addr == 0, "%u unknown, %u bad addresses",
          port_gfx_stats.unknown_ops, port_gfx_stats.bad_addr);

    /* the detector region was written back at the end of the task (a small image) */
    port_gfx_inval_dcache(rd(FB_PHYS), 320 * 240 * 2);   /* the CPU reads the frame (PostProcessImage) */
    int same = 0;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) same += fb_px(REGION_PHYS, 8, x, y) == fb_px(FB_PHYS, 320, 156 + x, 116 + y);
    uint16_t c0 = fb_px(REGION_PHYS, 8, 0, 0), c7 = fb_px(REGION_PHYS, 8, 7, 7);
    check("detector_region", same == 64 && c0 == rgba16(255, 0, 255, 1) && c7 == rgba16(255, 255, 0, 1),
          "%d/64 pixels equal the frame's centre; (0,0)=%04X (7,7)=%04X", same, c0, c7);
    uint16_t bar = fb_px(FB_PHYS, 320, 70, 12), clr = fb_px(FB_PHYS, 320, 150, 236);
    check("fill_readback", bar == rgba16(255, 255, 255, 1) && clr == rgba16(16, 24, 64, 1), "bar %04X clear %04X", bar, clr);
    uint16_t ztest = fb_px(FB_PHYS, 320, 90, 70);
    check("zbuffer", (ztest >> 11) < 8 && ((ztest >> 6) & 31) > 8, "overlap pixel %04X (green expected)", ztest);
    uint16_t small = fb_px(FB_PHYS, 320, 248 + 32, 184 + 20), smallbg = fb_px(FB_PHYS, 320, 249, 185);
    check("small_image_copy", (small >> 11) > 20 && smallbg == rgba16(0, 96, 0, 1), "copied pixel %04X background %04X", small, smallbg);
    {   /* the 64x48 Z image was written back in the console's Z format at the end of the task (score.c reads it) */
        uint16_t zin = fb_px(SMALLZ_PHYS, 64, 32, 20), zout = fb_px(SMALLZ_PHYS, 64, 1, 1);
        int zsame = 0;
        for (int y = 0; y < 24; y++)
            for (int x = 0; x < 64; x++) zsame += fb_px(ZCOPY_PHYS, 64, x, y) == fb_px(SMALLZ_PHYS, 64, x, y);
        check("z_writeback", zin == 0x2000 && zout == 0xFFFC && zsame == 64 * 24,
              "triangle z %04X (2000 expected) clear %04X, copy equal %d/1536 (copy %04X %04X)", zin, zout, zsame, fb_px(ZCOPY_PHYS, 64, 32, 20), fb_px(ZCOPY_PHYS, 64, 1, 1));
    }
    uint16_t ci = fb_px(FB_PHYS, 320, 246, 136), ciedge = fb_px(FB_PHYS, 320, 231, 121);
    check("ci4_tlut", ci != rgba16(16, 24, 64, 1) && ciedge == rgba16(16, 24, 64, 1), "centre %04X corner (transparent) %04X", ci, ciedge);
    uint16_t chk0 = fb_px(FB_PHYS, 320, 232, 50), chk1 = fb_px(FB_PHYS, 320, 242, 50);
    check("rgba16_wrap", chk0 == rgba16(255, 255, 255, 1) && chk1 == rgba16(255, 128, 0, 1), "%04X %04X", chk0, chk1);

    /* CPU writes into the frame (the detector's red dot), then the VI shows it */
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 5; j++)
            if ((i != 0 && i != 4) || (j != 0 && j != 4)) vbe_st16(rd(FB_PHYS + (uint32_t)((117 + i) * 320 + 157 + j) * 2), rgba16(255, 0, 0, 1));
    port_gfx_writeback_dcache(0, 0);
    port_gfx_vi_frame(rd(FB_PHYS), 320);
    printf("uploads after the CPU write: %u\n", port_gfx_stats.uploads);

    printf("CHECKS %s\n", checks_ok ? "ALL OK" : "SOME FAILED");
    return to_rgba(FB_PHYS);
}

/* ------------------------------------------------------------------ frame 2: formats, mirror, 2-cycle TEXEL1,
 * matrix stack, G_MW_MATRIX patch after gSPMvpRecalc, branch lists (second task, second framebuffer) */
#define FB2_PHYS 0x3C0000u
static Gfx dl2[2048], sub2[16], branch2[16];
static void tex2(void) {
    for (int y = 0; y < 8; y++)   /* RGBA32 8x8: r = 32x, g = 32y, b = 128 */
        for (int x = 0; x < 8; x++) vbe_st32(rd(TEX_PHYS + 0x2000 + (uint32_t)(y * 8 + x) * 4), (uint32_t)(x * 32) << 24 | (uint32_t)(y * 32) << 16 | 128u << 8 | 255u);
    for (int y = 0; y < 16; y++)   /* IA16 16x16: I = 16x, A = 255 */
        for (int x = 0; x < 16; x++) vbe_st16(rd(TEX_PHYS + 0x2200 + (uint32_t)(y * 16 + x) * 2), (unsigned)(x * 16) << 8 | 255u);
    for (int y = 0; y < 16; y++)   /* I4 16x16: I = x */
        for (int x = 0; x < 16; x += 2) vbe_st8(rd(TEX_PHYS + 0x2400 + (uint32_t)(y * 8 + x / 2)), (unsigned)(x << 4 | (x + 1)));
    for (int y = 0; y < 16; y++)   /* RGBA16 16x16: left half red, right half blue (mirror test) */
        for (int x = 0; x < 16; x++) vbe_st16(rd(TEX_PHYS + 0x2500 + (uint32_t)(y * 16 + x) * 2), x < 8 ? rgba16(255, 0, 0, 1) : rgba16(0, 0, 255, 1));
    for (int i = 0; i < 256; i++) {   /* two solid 16x16 RGBA16: red, blue (2-cycle lerp) */
        vbe_st16(rd(TEX_PHYS + 0x2800 + (uint32_t)i * 2), rgba16(255, 0, 0, 1));
        vbe_st16(rd(TEX_PHYS + 0x2A00 + (uint32_t)i * 2), rgba16(0, 0, 255, 1));
    }
}
static void fillrect(Gfx **pg, uint16_t c, int x0, int y0, int x1, int y1) {
    Gfx *g = *pg;
    gDPPipeSync(g++);
    gDPSetCycleType(g++, G_CYC_FILL);
    gDPSetRenderMode(g++, G_RM_NOOP, G_RM_NOOP2);
    gDPSetFillColor(g++, (uint32_t)c << 16 | c);
    gDPFillRectangle(g++, x0, y0, x1, y1);
    *pg = g;
}
static void texrect_1cyc(Gfx **pg, int x, int y, int w, int h, int dsdx) {
    Gfx *g = *pg;
    gSPTextureRectangle(g++, x << 2, y << 2, (x + w) << 2, (y + h) << 2, G_TX_RENDERTILE, 0, 0, dsdx, dsdx);
    *pg = g;
}
unsigned char *gfx_test_run2(void) {
    float m[4][4];
    tex2();
    Gfx *g = dl2;
    gSPSegment(g++, 0x06, K0(TEX_PHYS));
    gSPViewport(g++, vp_mem);
    gDPSetScissor(g++, G_SC_NON_INTERLACE, 0, 0, 320, 240);
    gDPSetColorImage(g++, G_IM_FMT_RGBA, G_IM_SIZ_16b, 320, K0(FB2_PHYS));
    fillrect(&g, rgba16(40, 40, 40, 1), 0, 0, 319, 239);
    gDPPipeSync(g++);
    gDPSetCycleType(g++, G_CYC_1CYCLE);
    gDPSetRenderMode(g++, G_RM_OPA_SURF, G_RM_OPA_SURF2);
    gDPSetCombineMode(g++, G_CC_DECALRGBA, G_CC_DECALRGBA);
    gDPSetTextureFilter(g++, G_TF_POINT);
    gDPSetTexturePersp(g++, G_TP_NONE);
    gDPLoadTextureBlock(g++, 0x06002000, G_IM_FMT_RGBA, G_IM_SIZ_32b, 8, 8, 0, G_TX_CLAMP, G_TX_CLAMP, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
    texrect_1cyc(&g, 10, 10, 32, 32, 1 << 8);   /* 4x */
    gDPLoadTextureBlock(g++, 0x06002200, G_IM_FMT_IA, G_IM_SIZ_16b, 16, 16, 0, G_TX_CLAMP, G_TX_CLAMP, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
    texrect_1cyc(&g, 60, 10, 32, 32, 1 << 9);
    gDPLoadTextureBlock_4b(g++, 0x06002400, G_IM_FMT_I, 16, 16, 0, G_TX_CLAMP, G_TX_CLAMP, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
    texrect_1cyc(&g, 110, 10, 32, 32, 1 << 9);
    gDPLoadTextureBlock(g++, 0x06002500, G_IM_FMT_RGBA, G_IM_SIZ_16b, 16, 16, 0, G_TX_MIRROR | G_TX_WRAP, G_TX_CLAMP, 4, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
    texrect_1cyc(&g, 160, 10, 64, 16, 1 << 10);   /* 64 texels across a 16-texel mirrored tile */
    /* R1 (Beach cliffs): the same TMEM drawn first with a 16x16 clamped tile, then with a 64x16 tile size (clamp bit
     * + mask 16: wraps every 16 texels inside the tile). The second draw must wrap, not reuse the clamped texture */
    gDPLoadTextureBlock(g++, 0x06002500, G_IM_FMT_RGBA, G_IM_SIZ_16b, 16, 16, 0, G_TX_CLAMP, G_TX_CLAMP, 4, 4, G_TX_NOLOD, G_TX_NOLOD);
    texrect_1cyc(&g, 160, 110, 16, 16, 1 << 10);
    gDPSetTileSize(g++, G_TX_RENDERTILE, 0, 0, 63 << 2, 15 << 2);
    texrect_1cyc(&g, 160, 130, 64, 16, 1 << 10);
    /* 2-cycle: (TEXEL1 - TEXEL0) * ENV_ALPHA + TEXEL0, then * SHADE (white) */
    gDPPipeSync(g++);
    gDPSetCycleType(g++, G_CYC_2CYCLE);
    gDPSetRenderMode(g++, G_RM_PASS, G_RM_OPA_SURF2);
    gDPSetCombineLERP(g++, TEXEL1, TEXEL0, ENV_ALPHA, TEXEL0, 0, 0, 0, 1, COMBINED, 0, SHADE, 0, 0, 0, 0, 1);
    gDPSetEnvColor(g++, 0, 0, 0, 128);
    gDPLoadTextureBlock(g++, 0x06002800, G_IM_FMT_RGBA, G_IM_SIZ_16b, 16, 16, 0, G_TX_WRAP, G_TX_WRAP, 4, 4, G_TX_NOLOD, G_TX_NOLOD);
    gDPLoadMultiBlock(g++, 0x06002A00, 256, 1, G_IM_FMT_RGBA, G_IM_SIZ_16b, 16, 16, 0, G_TX_WRAP, G_TX_WRAP, 4, 4, G_TX_NOLOD, G_TX_NOLOD);
    gSPClearGeometryMode(g++, 0xFFFFFFFF);
    gSPSetGeometryMode(g++, G_SHADE | G_SHADING_SMOOTH);
    gSPTexture(g++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON);
    ortho(m);
    gSPMatrix(g++, mtx(m), G_MTX_PROJECTION | G_MTX_LOAD | G_MTX_NOPUSH);
    ident(m);
    gSPMatrix(g++, mtx(m), G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
    uint32_t *v = vtx(4);
    vset(v, 0, 10, 60, 0, 0, 0, 0xFFFFFFFF); vset(v, 1, 10, 100, 0, 0, 16 << 5, 0xFFFFFFFF);
    vset(v, 2, 50, 100, 0, 16 << 5, 16 << 5, 0xFFFFFFFF); vset(v, 3, 50, 60, 0, 16 << 5, 0, 0xFFFFFFFF);
    gSPVertex(g++, v, 4, 0);
    gSP2Triangles(g++, 0, 1, 2, 0, 0, 2, 3, 0);
    /* matrix stack: the triangle at (10..50, 150..190) pushed + translated by 100, then popped and drawn as is */
    gDPPipeSync(g++);
    gDPSetCycleType(g++, G_CYC_1CYCLE);
    gDPSetRenderMode(g++, G_RM_OPA_SURF, G_RM_OPA_SURF2);
    gDPSetCombineMode(g++, G_CC_SHADE, G_CC_SHADE);
    gSPTexture(g++, 0, 0, 0, G_TX_RENDERTILE, G_OFF);
    uint32_t *tv = vtx(3);
    vset(tv, 0, 10, 150, 0, 0, 0, 0xFFFFFFFF); vset(tv, 1, 10, 190, 0, 0, 0, 0xFFFFFFFF); vset(tv, 2, 50, 190, 0, 0, 0, 0xFFFFFFFF);
    ident(m); m[3][0] = 100;
    gSPMatrix(g++, mtx(m), G_MTX_MODELVIEW | G_MTX_MUL | G_MTX_PUSH);
    gSPVertex(g++, tv, 3, 0);
    gSP1Triangle(g++, 0, 1, 2, 0);
    gSPPopMatrix(g++, G_MTX_MODELVIEW);
    gSPVertex(g++, tv, 3, 0);
    gSP1Triangle(g++, 0, 1, 2, 0);
    /* render.c's billboard trick: recalculate the MVP, then patch its words (x translation int part -> 0 = +160 px) */
    gSPMvpRecalc(g++);
    gMoveWd(g++, G_MW_MATRIX, G_MWO_MATRIX_XX_XY_I + 0x18, 0x00000001);   /* as render.c does */
    uint32_t *gv = vtx(3);
    vset(gv, 0, 10, 150, 0, 0, 0, 0x00FF00FF); vset(gv, 1, 10, 190, 0, 0, 0, 0x00FF00FF); vset(gv, 2, 50, 190, 0, 0, 0, 0x00FF00FF);
    gSPVertex(g++, gv, 3, 0);
    gSP1Triangle(g++, 0, 1, 2, 0);
    /* R1 (Beach sky box): a vertex on the eye plane (w = 0, z < -w). Projection: clip = (x / 160, -y / 120, z - 1, z),
     * no Z. A = (100, 0, 0) is the point at infinity to the right, B, C at w = 1 project to (230, 100) and (230, 140): the
     * triangle is the strip x >= 230, 100 <= y <= 140. Shade: A white, B, C black. Projective interpolation at pixel
     * (x, 120): n = (x + 0.5 - 160) / 160, lambda_A = (n - 0.4375) / 0.625 per unit of w, a = lambda_A / (lambda_A + 1):
     * 24 at x = 240, 105 at x = 300 (out of 255) */
    gDPPipeSync(g++);
    gDPSetCombineMode(g++, G_CC_SHADE, G_CC_SHADE);
    gSPClearGeometryMode(g++, G_CULL_BACK | G_CULL_FRONT | G_ZBUFFER);
    ident(m);
    m[0][0] = 1 / 160.0f; m[1][1] = -1 / 120.0f; m[2][2] = 1; m[3][2] = -1; m[2][3] = 1; m[3][3] = 0;
    gSPMatrix(g++, mtx(m), G_MTX_PROJECTION | G_MTX_LOAD | G_MTX_NOPUSH);
    ident(m);
    gSPMatrix(g++, mtx(m), G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
    uint32_t *ev = vtx(3);
    vset(ev, 0, 100, 0, 0, 0, 0, 0xFFFFFFFF); vset(ev, 1, 70, -20, 1, 0, 0, 0x000000FF); vset(ev, 2, 70, 20, 1, 0, 0, 0x000000FF);
    gSPVertex(g++, ev, 3, 0);
    gSP1Triangle(g++, 0, 1, 2, 0);
    /* branch: a called list branches to another, whose end returns to this one */
    {
        Gfx *b = branch2;
        fillrect(&b, rgba16(255, 255, 0, 1), 250, 150, 269, 169);
        gSPEndDisplayList(b++);
        Gfx *s2 = sub2;
        gSPBranchList(s2++, branch2);
        fillrect(&s2, rgba16(255, 0, 0, 1), 0, 0, 319, 239);   /* never reached */
        gSPEndDisplayList(s2++);
        gSPDisplayList(g++, sub2);
    }
    fillrect(&g, rgba16(0, 255, 255, 1), 280, 150, 299, 169);
    gDPFullSync(g++);
    gSPEndDisplayList(g++);
    unsigned cmds0 = port_gfx_stats.cmds;
    memset(task, 0, sizeof task);
    task[0] = M_GFXTASK;
    task[4] = 0x0003E7A0;
    task[12] = (uint32_t)(uintptr_t)dl2;
    port_gfx_task(task);
    printf("frame 2: %u commands, unknown %u, bad addr %u\n", port_gfx_stats.cmds - cmds0, port_gfx_stats.unknown_ops, port_gfx_stats.bad_addr);
    port_gfx_inval_dcache(rd(FB2_PHYS), 320 * 240 * 2);
    uint16_t p;
    p = fb_px(FB2_PHYS, 320, 10 + 4 * 5 + 1, 10 + 4 * 2 + 1);
    check("rgba32", p == rgba16(160, 64, 128, 1), "%04X (texel (5,2))", p);
    p = fb_px(FB2_PHYS, 320, 60 + 2 * 10 + 1, 11);
    check("ia16", p == rgba16(160, 160, 160, 1), "%04X (I 160)", p);
    p = fb_px(FB2_PHYS, 320, 110 + 2 * 9 + 1, 11);
    check("i4", p == rgba16(153, 153, 153, 1), "%04X (I 9 * 17)", p);
    uint16_t m1 = fb_px(FB2_PHYS, 320, 160 + 17, 12), m2 = fb_px(FB2_PHYS, 320, 160 + 30, 12), m0 = fb_px(FB2_PHYS, 320, 161, 12);
    check("mirror", m0 == rgba16(255, 0, 0, 1) && m1 == rgba16(0, 0, 255, 1) && m2 == rgba16(255, 0, 0, 1), "%04X %04X %04X", m0, m1, m2);
    {
        uint16_t c0 = fb_px(FB2_PHYS, 320, 160 + 20, 112), w0 = fb_px(FB2_PHYS, 320, 160 + 20, 132), w1 = fb_px(FB2_PHYS, 320, 160 + 28, 132);
        check("tile_resize_wrap", c0 == rgba16(40, 40, 40, 1) && w0 == rgba16(255, 0, 0, 1) && w1 == rgba16(0, 0, 255, 1),
              "first rect ends at 16 (%04X), second wraps: texel 20 = 4 %04X (red), texel 28 = 12 %04X (blue)", c0, w0, w1);
    }
    p = fb_px(FB2_PHYS, 320, 30, 80);
    check("two_cycle_texel1", (p >> 11) >= 14 && (p >> 11) <= 17 && ((p >> 1) & 31) >= 14 && ((p >> 1) & 31) <= 17 && ((p >> 6) & 31) == 0, "%04X (half red, half blue)", p);
    uint16_t a = fb_px(FB2_PHYS, 320, 15, 185), b = fb_px(FB2_PHYS, 320, 115, 185), c = fb_px(FB2_PHYS, 320, 175, 185), d = fb_px(FB2_PHYS, 320, 65, 185);
    check("matrix_stack", a == 0xFFFF && b == 0xFFFF && d == rgba16(40, 40, 40, 1), "orig %04X pushed %04X between %04X", a, b, d);
    check("mw_matrix_patch", c == rgba16(0, 255, 0, 1), "%04X (green at +160)", c);
    {
        uint16_t e1 = fb_px(FB2_PHYS, 320, 240, 120), e2 = fb_px(FB2_PHYS, 320, 300, 120), e3 = fb_px(FB2_PHYS, 320, 300, 95);
        int g1 = (e1 >> 6) & 31, g2 = (e2 >> 6) & 31;
        check("eye_plane_vertex", g1 >= 1 && g1 <= 5 && g2 >= 11 && g2 <= 15 && e3 == rgba16(40, 40, 40, 1),
              "x 240: %04X (green %d, 3 expected), x 300: %04X (green %d, 13 expected), above the strip %04X", e1, g1, e2, g2, e3);
    }
    uint16_t y = fb_px(FB2_PHYS, 320, 255, 155), cy = fb_px(FB2_PHYS, 320, 285, 155), bg = fb_px(FB2_PHYS, 320, 5, 230);
    check("branch_list", y == rgba16(255, 255, 0, 1) && cy == rgba16(0, 255, 255, 1) && bg == rgba16(40, 40, 40, 1), "%04X %04X %04X", y, cy, bg);
    port_gfx_vi_frame(rd(FB2_PHYS), 320);
    printf("CHECKS2 %s\n", checks_ok ? "ALL OK" : "SOME FAILED");
    return to_rgba(FB2_PHYS);
}
