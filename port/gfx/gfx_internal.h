/* port/gfx internal state shared by gfx_rsp.c (display lists, RSP: matrices, vertices, lights) and gfx_rdp.c (RDP:
 * TMEM, tiles, combiner, blender, colour images, WebGL2). Constants: values from ultralib/include/PR/gbi.h with
 * F3DEX_GBI_2 (checked against the header by port/gfx/test/gfx_test.c, which includes it). */
#ifndef PORT_GFX_INTERNAL_H
#define PORT_GFX_INTERNAL_H
#include <stdint.h>
#include "gfx.h"
#include "../ultra/pc_mem.h"

/* geometry mode (F3DEX2) */
#define GM_ZBUFFER 0x00000001u
#define GM_SHADE 0x00000004u
#define GM_CULL_FRONT 0x00000200u
#define GM_CULL_BACK 0x00000400u
#define GM_FOG 0x00010000u
#define GM_LIGHTING 0x00020000u
#define GM_TEXTURE_GEN 0x00040000u
#define GM_TEXTURE_GEN_LINEAR 0x00080000u
#define GM_LOD 0x00100000u
#define GM_SHADING_SMOOTH 0x00200000u
#define GM_CLIPPING 0x00800000u

/* othermode H shifts / L bits */
#define OMH_CYCLETYPE_SFT 20
#define OMH_TEXTPERSP_SFT 19
#define OMH_TEXTLUT_SFT 14
#define OMH_TEXTFILT_SFT 12
#define OMH_TEXTCONV_SFT 9
#define CYC_1 0
#define CYC_2 1
#define CYC_COPY 2
#define CYC_FILL 3
#define OML_ALPHACOMPARE_MASK 3u
#define OML_ZS_PRIM 0x4u
#define OML_AA_EN 0x8u
#define OML_Z_CMP 0x10u
#define OML_Z_UPD 0x20u
#define OML_IM_RD 0x40u
#define OML_CVG_X_ALPHA 0x1000u
#define OML_ALPHA_CVG_SEL 0x2000u
#define OML_FORCE_BL 0x4000u
#define OML_ZMODE(l) (((l) >> 10) & 3u)
#define ZMODE_DECAL_SEL 3

/* image formats / sizes */
#define FMT_RGBA 0
#define FMT_YUV 1
#define FMT_CI 2
#define FMT_IA 3
#define FMT_I 4
#define SIZ_4b 0
#define SIZ_8b 1
#define SIZ_16b 2
#define SIZ_32b 3

/* ------------------------------------------------------------------ addresses (gfx_rsp.c) */
/* display-list address -> pointer into wasm memory (segments, KSEG0, physical, wasm), 0 if invalid */
unsigned char *gfx_addr(uint32_t a);

/* ------------------------------------------------------------------ vertices handed to the RDP side */
typedef struct {
    float x, y, z, w;       /* clip space, x/y already through the N64 viewport (N64 pixels * w), z NDC * w */
    float r, g, b, a;       /* shade, 0..1 (a = fog factor when G_FOG) */
    float s, t;             /* texture coordinates in texels (10.5 / 32 after G_TEXTURE scale), before tile shift */
} GfxVtx;

/* RSP state the RDP side needs */
extern uint32_t gfx_geom;                 /* geometry mode */
extern int gfx_tex_on, gfx_tex_tile, gfx_tex_level;

/* ------------------------------------------------------------------ RDP side (gfx_rdp.c) */
void rdp_reset_task(void);
void rdp_ucode_data(const unsigned char *data);   /* a microcode's data segment was loaded into DMEM (task start, G_LOAD_UCODE) */
void rdp_cmd(uint32_t w0, uint32_t w1, const uint32_t *next); /* an RDP command (0xE4..0xFF etc.); next = following words */
void rdp_tri(const GfxVtx *a, const GfxVtx *b, const GfxVtx *c);
void rdp_line(const GfxVtx *a, const GfxVtx *b, float width);
void rdp_texrect(uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3, int flip); /* w2 = s<<16|t, w3 = dsdx<<16|dtdy */
void rdp_end_task(void);
int rdp_gl_ready(void);
int rdp_target_width(void), rdp_target_height(void);
extern int gfx_saw_fullsync;
extern int gfx_log_ops;   /* port_gfx_set_log_ops: log every command (debug) */

void gfx_log_once(unsigned key, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#endif
