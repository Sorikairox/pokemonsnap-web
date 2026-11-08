/* port/gfx: the RSP part of F3DEX2 / L3DEX2 (session S3b; docs/port/RENDERER.md). Original code for this port.
 *
 * Walks display lists in game memory, keeps the RSP state (segment table, matrix stack, MVP with G_MW_MATRIX
 * patches, viewport, vertex buffer, lights, fog, texture scale, geometry mode) and hands screen-space triangles,
 * lines and RDP commands to gfx_rdp.c. Vertex transform and lighting run here on the CPU; clipping is left to the GPU
 * (clip-space vertices). */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <emscripten/heap.h>
#include "gfx_internal.h"

PortGfxStats port_gfx_stats;
uint32_t gfx_geom;
int gfx_tex_on, gfx_tex_tile, gfx_tex_level;
int gfx_log_ops;

/* ------------------------------------------------------------------ logging */
static uint32_t once_keys[512];
void gfx_log_once(unsigned key, const char *fmt, ...) {
    unsigned h = (key * 2654435761u) >> 23;
    for (unsigned i = 0; i < 512; i++) {
        unsigned j = (h + i) & 511;
        if (once_keys[j] == key + 1) return;
        if (!once_keys[j]) { once_keys[j] = key + 1; break; }
    }
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    port_log("[gfx] %s\n", buf);
}
void port_gfx_set_log_ops(int on) { gfx_log_ops = on; }

/* ------------------------------------------------------------------ addresses
 * Address classes in this port (-sGLOBAL_BASE=256MB, the RDRAM mirror at wasm 0x10800000, MEMORY.md):
 *   >= 0x80000000            KSEG0 / KSEG1 console pointer -> mirror (physical part < 8 MiB)
 *   0x10000000..0x7FFFFFFF    wasm pointer stored by game C (also what OS_K0_TO_PHYSICAL leaves of one: < 0x20000000)
 *   0x01000000..0x0FFFFFFF    RSP segment address: segment table entry (itself any class) + offset
 *   < 0x01000000              segment 0: segment 0's base (normally 0) + offset = console physical -> mirror */
static uint32_t segtab[16];
static uint32_t heap_top;

static unsigned char *base_ptr(uint32_t b) {
    if (b >= 0x80000000u) {
        uint32_t p = b & 0x1FFFFFFFu;
        return p < PORT_RDRAM_SIZE ? n64_rdram + p : 0;
    }
    if (b >= 0x10000000u) {
        if (b >= heap_top) heap_top = (uint32_t)emscripten_get_heap_size();   /* memory may have grown */
        return b < heap_top ? (unsigned char *)(uintptr_t)b : 0;
    }
    if (b < PORT_RDRAM_SIZE) return n64_rdram + b;
    return 0;
}
unsigned char *gfx_addr(uint32_t a) {
    unsigned char *p;
    if (a >= 0x10000000u) p = base_ptr(a);
    else {
        uint32_t seg = a >> 24, off = a & 0xFFFFFFu;
        if (seg && !segtab[seg]) { p = 0; gfx_log_once(0x5E60000 | seg, "segment %u used before it was set (0x%08X)", seg, a); }
        else {
            unsigned char *b = base_ptr(segtab[seg]);
            p = b ? b + off : 0;
        }
    }
    if (!p) { port_gfx_stats.bad_addr++; gfx_log_once(0xBAD00000 | (a >> 20), "bad address 0x%08X", a); }
    return p;
}

/* ------------------------------------------------------------------ matrices (row vectors: v' = v * M, like the N64) */
typedef float M4[4][4];
static M4 mv_stack[32], proj, mvp;
static int mv_sp;
static int mvp_dirty = 1;
static uint32_t mvp_fixed[16];   /* the MVP as the RSP keeps it in DMEM (Mtx layout): G_MW_MATRIX patches words */

static void mtx_mul(M4 r, M4 a, M4 b) {   /* r = a * b */
    M4 t;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
    memcpy(r, t, sizeof t);
}
static void mtx_from_fixed(M4 m, const uint32_t *w) {   /* Mtx: 8 words of integer halves, then 8 of fractions */
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j += 2) {
            uint32_t iw = w[i * 2 + j / 2], fw = w[8 + i * 2 + j / 2];
            m[i][j] = (int32_t)((iw & 0xFFFF0000u) | (fw >> 16)) / 65536.0f;
            m[i][j + 1] = (int32_t)((iw << 16) | (fw & 0xFFFFu)) / 65536.0f;
        }
}
static void mtx_to_fixed(uint32_t *w, M4 m) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j += 2) {
            int32_t a = (int32_t)lrintf(m[i][j] * 65536.0f), b = (int32_t)lrintf(m[i][j + 1] * 65536.0f);
            w[i * 2 + j / 2] = ((uint32_t)a & 0xFFFF0000u) | ((uint32_t)b >> 16);
            w[8 + i * 2 + j / 2] = ((uint32_t)a << 16) | ((uint32_t)b & 0xFFFFu);
        }
}
static int mtx_read(M4 m, uint32_t addr) {
    const uint32_t *p = (const uint32_t *)gfx_addr(addr);
    if (!p) return 0;
    uint32_t w[16];
    for (int i = 0; i < 16; i++) w[i] = vbe_ld32(p + i);
    mtx_from_fixed(m, w);
    return 1;
}
static void mvp_update(void) {
    if (!mvp_dirty) return;
    mtx_mul(mvp, mv_stack[mv_sp], proj);
    mtx_to_fixed(mvp_fixed, mvp);
    mvp_dirty = 0;
}

/* ------------------------------------------------------------------ viewport, lights, fog, texture scale */
static float vp_sx = 160, vp_sy = 120, vp_sz = 511, vp_tx = 160, vp_ty = 120, vp_tz = 511;   /* N64 pixels / z units */
static unsigned char lightbuf[10 * 24];   /* DMEM light area: lookat X (0), lookat Y (24), lights 1..8 (48 + 24 n) */
static int num_lights;
static float fog_mul, fog_off;
static float tex_scale_s = 1, tex_scale_t = 1;
static uint32_t rdphalf1;
static int ucode_l3dex2;

static GfxVtx vbuf[64];
static float vndc[64][3];      /* NDC x, y (y up) and w for culling / branch z */

static void light_dir_model(int slot, float out[3]) {   /* direction of light slot (byte offset) in model space */
    const unsigned char *d = lightbuf + slot + 8;
    float x = (signed char)d[0], y = (signed char)d[1], z = (signed char)d[2];
    M4 *m = &mv_stack[mv_sp];
    float a = (*m)[0][0] * x + (*m)[0][1] * y + (*m)[0][2] * z;
    float b = (*m)[1][0] * x + (*m)[1][1] * y + (*m)[1][2] * z;
    float c = (*m)[2][0] * x + (*m)[2][1] * y + (*m)[2][2] * z;
    float l = sqrtf(a * a + b * b + c * c);
    if (l > 0) { a /= l; b /= l; c /= l; }
    out[0] = a; out[1] = b; out[2] = c;
}

static void load_vtx(uint32_t w0, uint32_t w1) {
    int n = (w0 >> 12) & 0xFF, v0 = (int)((w0 >> 1) & 0x7F) - n;
    const uint32_t *src = (const uint32_t *)gfx_addr(w1);
    if (!src || v0 < 0 || v0 + n > 64) { gfx_log_once(0xD7000000 | (w0 & 0xFFFFFF), "bad G_VTX 0x%08X 0x%08X", w0, w1); return; }
    mvp_update();
    float ldir[9][3], lcol[9][3], amb[3] = {0, 0, 0}, lax[3] = {0}, lay[3] = {0};
    int nl = 0;
    if (gfx_geom & GM_LIGHTING) {
        nl = num_lights > 7 ? 7 : num_lights;
        for (int i = 0; i < nl; i++) {
            light_dir_model(48 + 24 * i, ldir[i]);
            for (int k = 0; k < 3; k++) lcol[i][k] = lightbuf[48 + 24 * i + k] / 255.0f;
        }
        for (int k = 0; k < 3; k++) amb[k] = lightbuf[48 + 24 * nl + k] / 255.0f;
        if (gfx_geom & GM_TEXTURE_GEN) { light_dir_model(0, lax); light_dir_model(24, lay); }
    }
    for (int i = 0; i < n; i++, src += 4) {
        uint32_t a = vbe_ld32(src), b = vbe_ld32(src + 1), c = vbe_ld32(src + 2), d = vbe_ld32(src + 3);
        float x = (int16_t)(a >> 16), y = (int16_t)a, z = (int16_t)(b >> 16);
        float s = (int16_t)(c >> 16), t = (int16_t)c;
        GfxVtx *v = &vbuf[v0 + i];
        int texgen = 0;
        float cx = x * mvp[0][0] + y * mvp[1][0] + z * mvp[2][0] + mvp[3][0];
        float cy = x * mvp[0][1] + y * mvp[1][1] + z * mvp[2][1] + mvp[3][1];
        float cz = x * mvp[0][2] + y * mvp[1][2] + z * mvp[2][2] + mvp[3][2];
        float cw = x * mvp[0][3] + y * mvp[1][3] + z * mvp[2][3] + mvp[3][3];
        /* N64 viewport: sx = tx + ndc.x * sx, sy = ty - ndc.y * sy (y down); kept homogeneous for GPU clipping */
        v->x = vp_tx * cw + cx * vp_sx;
        v->y = vp_ty * cw - cy * vp_sy;
        v->z = cz;
        v->w = cw;
        float iw = cw != 0 ? 1.0f / cw : 0;
        vndc[v0 + i][0] = cx * iw; vndc[v0 + i][1] = cy * iw; vndc[v0 + i][2] = cw;
        if (gfx_geom & GM_LIGHTING) {
            float nx = (signed char)(d >> 24), ny = (signed char)(d >> 16), nz = (signed char)(d >> 8);
            float l = sqrtf(nx * nx + ny * ny + nz * nz);
            if (l > 0) { nx /= l; ny /= l; nz /= l; }
            float r = amb[0], g = amb[1], bb = amb[2];
            for (int k = 0; k < nl; k++) {
                float dt = nx * ldir[k][0] + ny * ldir[k][1] + nz * ldir[k][2];
                if (dt > 0) { r += dt * lcol[k][0]; g += dt * lcol[k][1]; bb += dt * lcol[k][2]; }
            }
            v->r = r > 1 ? 1 : r; v->g = g > 1 ? 1 : g; v->b = bb > 1 ? 1 : bb;
            if (gfx_geom & GM_TEXTURE_GEN) {
                float dx = nx * lax[0] + ny * lax[1] + nz * lax[2], dy = nx * lay[0] + ny * lay[1] + nz * lay[2];
                if (gfx_geom & GM_TEXTURE_GEN_LINEAR) {   /* acos mapping, scaled like the linear case */
                    dx = 1.0f - acosf(dx < -1 ? -1 : dx > 1 ? 1 : dx) * (2.0f / 3.14159265f);
                    dy = 1.0f - acosf(dy < -1 ? -1 : dy > 1 ? 1 : dy) * (2.0f / 3.14159265f);
                }
                /* texgen: (dot + 1) / 4 of the raw G_TEXTURE scale is the 10.5 coordinate (gSPTexture(0x07C0,
                 * 0x07C0) spans ~31 texels); TODO verify against hardware */
                texgen = 1;
                s = (dx + 1.0f) * 0.25f * tex_scale_s * 65536.0f;
                t = (dy + 1.0f) * 0.25f * tex_scale_t * 65536.0f;
            }
        } else {
            v->r = (d >> 24) / 255.0f; v->g = ((d >> 16) & 0xFF) / 255.0f; v->b = ((d >> 8) & 0xFF) / 255.0f;
        }
        v->a = (d & 0xFF) / 255.0f;
        if (gfx_geom & GM_FOG) {   /* the RSP replaces shade alpha by the fog factor */
            float f = (cw != 0 ? cz / cw : 0) * fog_mul + fog_off;
            if (cw < 0) f = 255;   /* behind the eye */
            v->a = (f < 0 ? 0 : f > 255 ? 255 : f) / 255.0f;
        }
        v->s = (texgen ? s : s * tex_scale_s) / 32.0f;
        v->t = (texgen ? t : t * tex_scale_t) / 32.0f;
    }
    port_gfx_stats.verts += n;
}

static int cull_tri(int i0, int i1, int i2) {
    uint32_t cm = gfx_geom & (GM_CULL_FRONT | GM_CULL_BACK);
    if (!cm) return 0;
    if (cm == (GM_CULL_FRONT | GM_CULL_BACK)) return 1;
    if (vndc[i0][2] <= 0 || vndc[i1][2] <= 0 || vndc[i2][2] <= 0) return 0;   /* crosses w = 0: let the GPU clip */
    float ax = vndc[i1][0] - vndc[i0][0], ay = vndc[i1][1] - vndc[i0][1];
    float bx = vndc[i2][0] - vndc[i0][0], by = vndc[i2][1] - vndc[i0][1];
    float area = ax * by - ay * bx;   /* > 0: counter-clockwise in NDC (y up) = front */
    if (vp_sx * vp_sy < 0) area = -area;
    return (cm & GM_CULL_BACK) ? area <= 0 : area >= 0;
}
static void tri(unsigned a, unsigned b, unsigned c) {
    a /= 2; b /= 2; c /= 2;
    if (a >= 64 || b >= 64 || c >= 64) return;
    if (cull_tri(a, b, c)) { port_gfx_stats.culled++; return; }
    port_gfx_stats.tris++;
    if (!(gfx_geom & GM_SHADING_SMOOTH)) {   /* flat: the first vertex's colour (F3DEX2) */
        GfxVtx vb = vbuf[b], vc = vbuf[c];
        vb.r = vc.r = vbuf[a].r; vb.g = vc.g = vbuf[a].g; vb.b = vc.b = vbuf[a].b; vb.a = vc.a = vbuf[a].a;
        rdp_tri(&vbuf[a], &vb, &vc);
    } else rdp_tri(&vbuf[a], &vbuf[b], &vbuf[c]);
}

/* ------------------------------------------------------------------ the display-list walker */
extern char gspL3DEX2_fifoTextStart[] __attribute__((weak));
#define L3DEX2_TEXT_VRAM 0x8003FB30u   /* port/symbols/symbols.tsv (the test build has no game symbols) */

static int is_l3dex2(uint32_t text) {
    unsigned char *p = gfx_addr(text);
    if (!p) return 0;
    if (gspL3DEX2_fifoTextStart && p == (unsigned char *)gspL3DEX2_fifoTextStart) return 1;
    return p == n64_rdram + (L3DEX2_TEXT_VRAM & 0x7FFFFF);
}

static void movemem(uint32_t w0, uint32_t w1) {
    unsigned idx = w0 & 0xFF, ofs = ((w0 >> 8) & 0xFF) * 8, len = ((w0 >> 19) & 0x1F) * 8 + 8;
    unsigned char *p = gfx_addr(w1);
    if (!p) return;
    switch (idx) {
    case 8: {   /* G_MV_VIEWPORT: Vp { s16 vscale[4], vtrans[4] }, x/y in quarter pixels */
        vp_sx = (int16_t)vbe_ld16(p) / 4.0f; vp_sy = (int16_t)vbe_ld16(p + 2) / 4.0f; vp_sz = (int16_t)vbe_ld16(p + 4);
        vp_tx = (int16_t)vbe_ld16(p + 8) / 4.0f; vp_ty = (int16_t)vbe_ld16(p + 10) / 4.0f; vp_tz = (int16_t)vbe_ld16(p + 12);
        (void)vp_sz; (void)vp_tz;
        break;
    }
    case 10:   /* G_MV_LIGHT */
        for (unsigned i = 0; i < len && ofs + i < sizeof lightbuf; i++) lightbuf[ofs + i] = vbe_ld8(p + i);
        break;
    case 14: { /* G_MV_MATRIX (gSPForceMatrix): the MVP itself */
        for (int i = 0; i < 16; i++) mvp_fixed[i] = vbe_ld32(p + 4 * i);
        mtx_from_fixed(mvp, mvp_fixed);
        mvp_dirty = 0;
        break;
    }
    case 2: mtx_read(mv_stack[mv_sp], w1); mvp_dirty = 1; break;   /* G_MV_MMTX */
    case 6: mtx_read(proj, w1); mvp_dirty = 1; break;              /* G_MV_PMTX */
    default: gfx_log_once(0xDC000000 | idx, "G_MOVEMEM index %u not handled", idx);
    }
}
static void moveword(uint32_t w0, uint32_t w1) {
    unsigned idx = (w0 >> 16) & 0xFF, off = w0 & 0xFFFF;
    switch (idx) {
    case 0x00:   /* G_MW_MATRIX: patch one word of the MVP */
        mvp_update();
        if (off < 64) { mvp_fixed[off / 4] = w1; mtx_from_fixed(mvp, mvp_fixed); }
        break;
    case 0x02: num_lights = (int)(w1 / 24); break;            /* G_MW_NUMLIGHT: NUML(n) = n * 24 */
    case 0x04: break;                                          /* G_MW_CLIP: clip ratio (GPU clips) */
    case 0x06: segtab[(off / 4) & 15] = w1; break;             /* G_MW_SEGMENT */
    case 0x08: fog_mul = (int16_t)(w1 >> 16); fog_off = (int16_t)w1; break;   /* G_MW_FOG */
    case 0x0A:   /* G_MW_LIGHTCOL: offset from light 1 */
        if (48 + off + 4 <= sizeof lightbuf)
            for (int i = 0; i < 4; i++) lightbuf[48 + off + i] = (unsigned char)(w1 >> (24 - 8 * i));
        break;
    case 0x0C: break;                                          /* G_MW_FORCEMTX (follows the G_MV_MATRIX) */
    case 0x0E: break;                                          /* G_MW_PERSPNORM: fixed-point only */
    default: gfx_log_once(0xDB000000 | idx, "G_MOVEWORD index 0x%02X not handled", idx);
    }
}
static void modify_vtx(uint32_t w0, uint32_t w1) {
    unsigned where = (w0 >> 16) & 0xFF, i = (w0 & 0xFFFF) / 2;
    if (i >= 64) return;
    GfxVtx *v = &vbuf[i];
    switch (where) {
    case 0x10: v->r = (w1 >> 24) / 255.0f; v->g = ((w1 >> 16) & 0xFF) / 255.0f; v->b = ((w1 >> 8) & 0xFF) / 255.0f;
               v->a = (w1 & 0xFF) / 255.0f; break;
    case 0x14: v->s = (int16_t)(w1 >> 16) / 32.0f; v->t = (int16_t)w1 / 32.0f; break;
    case 0x18: {   /* screen x, y (s13.2) */
        float sx = (int16_t)(w1 >> 16) / 4.0f, sy = (int16_t)w1 / 4.0f;
        v->x = sx * v->w; v->y = sy * v->w;
        break;
    }
    case 0x1C: {   /* screen z (16.16 of the 0..0x3FF range) */
        float zs = w1 / 65536.0f;
        v->z = (vp_sz != 0 ? (zs - vp_tz) / vp_sz : 0) * v->w;
        break;
    }
    default: gfx_log_once(0x02000000 | where, "G_MODIFYVTX where 0x%02X", where);
    }
}
static int cull_dl(uint32_t w0, uint32_t w1) {   /* 1 = every vertex outside the same clip plane */
    unsigned a = (w0 & 0xFFFF) / 2, b = (w1 & 0xFFFF) / 2;
    unsigned all = 0x3F;
    for (unsigned i = a; i <= b && i < 64; i++) {
        const GfxVtx *v = &vbuf[i];
        float cx = (v->x - vp_tx * v->w) / (vp_sx ? vp_sx : 1), cy = -(v->y - vp_ty * v->w) / (vp_sy ? vp_sy : 1);
        unsigned o = 0;
        if (cx < -v->w) o |= 1;
        if (cx > v->w) o |= 2;
        if (cy < -v->w) o |= 4;
        if (cy > v->w) o |= 8;
        if (v->z > v->w) o |= 16;
        if (v->w < 0) o |= 32;
        all &= o;
    }
    return all != 0;
}

#define DL_DEPTH 32
static void run_dl(uint32_t start) {
    const uint32_t *stack[DL_DEPTH];
    int sp = 0;
    const uint32_t *pc = (const uint32_t *)gfx_addr(start);
    if (!pc) return;
    port_gfx_stats.dls++;
    heap_top = (uint32_t)emscripten_get_heap_size();
    for (unsigned guard = 0; pc && guard < 4000000; guard++) {
        uint32_t w0 = vbe_ld32(pc), w1 = vbe_ld32(pc + 1);
        unsigned op = w0 >> 24;
        port_gfx_stats.cmds++;
        if (gfx_log_ops) port_log("[gfx] %p %08X %08X\n", (const void *)pc, w0, w1);
        pc += 2;
        switch (op) {
        case 0x00: break;   /* G_NOOP */
        case 0x01: load_vtx(w0, w1); break;
        case 0x02: modify_vtx(w0, w1); break;
        case 0x03: if (cull_dl(w0, w1)) goto enddl; break;
        case 0x04: {   /* G_BRANCH_Z: rdphalf1 = list, w1 = z (16.16 screen z) */
            unsigned i = (w0 & 0xFFF) / 2;
            if (i < 64) {
                const GfxVtx *v = &vbuf[i];
                float zs = (v->w != 0 ? v->z / v->w : 0) * vp_sz + vp_tz;
                if (gfx_log_ops) port_log("[gfx] branch_z v%u x %f y %f z %f w %f -> screen z %f vs %f (vp z %f %f)\n", i,
                                      v->x, v->y, v->z, v->w, zs, w1 / 65536.0f, vp_sz, vp_tz);
                if (zs * 65536.0f <= (float)w1 || v->w <= 0) {
                    const uint32_t *t = (const uint32_t *)gfx_addr(rdphalf1);
                    if (t) pc = t;
                }
            }
            break;
        }
        case 0x05: tri((w0 >> 16) & 0xFF, (w0 >> 8) & 0xFF, w0 & 0xFF); break;
        case 0x06: tri((w0 >> 16) & 0xFF, (w0 >> 8) & 0xFF, w0 & 0xFF); tri((w1 >> 16) & 0xFF, (w1 >> 8) & 0xFF, w1 & 0xFF); break;
        case 0x07: tri((w0 >> 16) & 0xFF, (w0 >> 8) & 0xFF, w0 & 0xFF); tri((w1 >> 16) & 0xFF, (w1 >> 8) & 0xFF, w1 & 0xFF); break;
        case 0x08: {   /* G_LINE3D (L3DEX2): v0, v1, width */
            unsigned a = ((w0 >> 16) & 0xFF) / 2, b = ((w0 >> 8) & 0xFF) / 2, wd = w0 & 0xFF;
            if (!ucode_l3dex2) gfx_log_once(0x08000001, "G_LINE3D while the ucode is F3DEX2");
            if (a < 64 && b < 64) { port_gfx_stats.lines++; rdp_line(&vbuf[a], &vbuf[b], 1.0f + wd * 0.5f); }
            break;
        }
        case 0xD5:   /* G_SPECIAL_1: Snap's gSPMvpRecalc (render.c: gImmp21(G_SPECIAL_1, 0, 1, 0)) */
            if ((w0 & 0xFF) == 1) { mvp_dirty = 1; mvp_update(); }
            else gfx_log_once(0xD5000000 | (w0 & 0xFFFFFF), "G_SPECIAL_1 0x%08X", w0);
            break;
        case 0xD3: case 0xD4: case 0xD6: gfx_log_once(op << 24, "special/DMA_IO opcode 0x%02X ignored", op); break;
        case 0xD7:   /* G_TEXTURE */
            tex_scale_s = (w1 >> 16) / 65536.0f; tex_scale_t = (w1 & 0xFFFF) / 65536.0f;
            gfx_tex_level = (w0 >> 11) & 7; gfx_tex_tile = (w0 >> 8) & 7; gfx_tex_on = (w0 >> 1) & 0x7F;
            break;
        case 0xD8: {   /* G_POPMTX: w1 = bytes */
            int n = (int)(w1 / 64);
            mv_sp = mv_sp >= n ? mv_sp - n : 0;
            mvp_dirty = 1;
            break;
        }
        case 0xD9: gfx_geom = (gfx_geom & (w0 & 0xFFFFFF)) | w1; break;
        case 0xDA: {   /* G_MTX: params ^ G_MTX_PUSH */
            unsigned prm = (w0 & 0xFF) ^ 1;
            M4 m;
            if (!mtx_read(m, w1)) break;
            if (prm & 4) {   /* projection */
                if (prm & 2) memcpy(proj, m, sizeof m); else mtx_mul(proj, m, proj);
            } else {
                if ((prm & 1) && mv_sp < 31) { memcpy(mv_stack[mv_sp + 1], mv_stack[mv_sp], sizeof(M4)); mv_sp++; }
                if (prm & 2) memcpy(mv_stack[mv_sp], m, sizeof m); else mtx_mul(mv_stack[mv_sp], m, mv_stack[mv_sp]);
            }
            mvp_dirty = 1;
            break;
        }
        case 0xDB: moveword(w0, w1); break;
        case 0xDC: movemem(w0, w1); break;
        case 0xDD:   /* G_LOAD_UCODE: w1 = text, rdphalf1 = data */
            port_gfx_stats.ucode_loads++;
            ucode_l3dex2 = is_l3dex2(w1);
            break;
        case 0xDE: {
            const uint32_t *t = (const uint32_t *)gfx_addr(w1);
            if (!t) break;
            if (!((w0 >> 16) & 0xFF)) {   /* G_DL_PUSH */
                if (sp >= DL_DEPTH) { gfx_log_once(0xDE000001, "display-list stack overflow"); break; }
                stack[sp++] = pc;
            }
            port_gfx_stats.dls++;
            pc = t;
            break;
        }
        case 0xDF:
        enddl:
            pc = sp ? stack[--sp] : 0;
            break;
        case 0xE0: break;   /* G_SPNOOP */
        case 0xE1: rdphalf1 = w1; break;
        case 0xF1: break;   /* G_RDPHALF_2 outside a texture rectangle */
        case 0xE4: case 0xE5: {   /* texture rectangle: + G_RDPHALF_1 (s, t) + G_RDPHALF_2 (dsdx, dtdy) */
            uint32_t w2 = vbe_ld32(pc + 1), w3 = vbe_ld32(pc + 3);
            pc += 4;
            rdp_texrect(w0, w1, w2, w3, op == 0xE5);
            break;
        }
        default:
            if (op >= 0xE2) rdp_cmd(w0, w1, pc);
            else { port_gfx_stats.unknown_ops++; gfx_log_once(op << 24, "unknown opcode 0x%02X (0x%08X 0x%08X)", op, w0, w1); }
        }
    }
}

static void rsp_reset(void) {
    memset(segtab, 0, sizeof segtab);
    mv_sp = 0;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) proj[i][j] = mv_stack[0][i][j] = i == j;
    mvp_dirty = 1;
    gfx_geom = 0;
    ucode_l3dex2 = 0;
}

/* OSTask (ultralib PR/sptask.h, 32-bit fields): type 0x00, flags 0x04, ucode_boot 0x08, ucode 0x10, ucode_data 0x18,
 * output_buff 0x28, data_ptr 0x30, data_size 0x34 */

void port_gfx_run_dl(const void *dl, int l3dex2) {
    port_gfx_init();
    rsp_reset();
    rdp_reset_task();
    ucode_l3dex2 = l3dex2;
    run_dl((uint32_t)(uintptr_t)dl);
    rdp_end_task();
}

void port_gfx_task(void *task) {
    const uint32_t *t = (const uint32_t *)task;
    port_gfx_init();
    port_gfx_stats.tasks++;
    uint32_t type = vbe_ld32(t), ucode = vbe_ld32(t + 4), data = vbe_ld32(t + 12);
    gfx_saw_fullsync = 0;
    if (type == 1) {   /* M_GFXTASK */
        rsp_reset();
        rdp_reset_task();
        heap_top = (uint32_t)emscripten_get_heap_size();
        ucode_l3dex2 = is_l3dex2(ucode);
        run_dl(data);
        rdp_end_task();
    } else gfx_log_once(0x7A5C0000 | type, "port_gfx_task: task type %u is not a graphics task", type);
    /* the PC layer (port/ultra/pc_rcp.c osSpTaskStartGo) posts SP done and DP full sync after this returns (S3c:
     * one owner of the events); the test counts G_FULLSYNC through port_gfx_stats.fullsyncs */
    if (gfx_saw_fullsync) port_gfx_stats.fullsyncs++;
}
