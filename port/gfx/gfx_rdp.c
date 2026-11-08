/* port/gfx: the RDP part, on WebGL2 (session S3b; docs/port/RENDERER.md). Original code for this port.
 *
 *   - TMEM is emulated byte-exact (4 KiB, big-endian byte order): LoadBlock (dxt odd-line word swap), LoadTile (odd
 *     rows swapped), LoadTLUT (entries quadrupled in the upper half), RGBA32 split across the two banks; the sampler
 *     reads TMEM back with the same odd-row swap, so every load sequence decodes as the RDP sees it.
 *   - Tiles are decoded to RGBA8 GL textures (RGBA16/32, CI4/CI8 + RGBA16/IA16 TLUT, IA4/8/16, I4/8, YUV16), cached
 *     by a hash of the TMEM bytes and the tile descriptor.
 *   - The two-cycle colour combiner and the blender are generated GLSL (one program per state key).
 *   - Colour images are GL render targets at the console's resolution, one per colour-image address, rows stored
 *     top row first (GL window y = N64 y). They are kept coherent with RDRAM (PLAN.md 1.1 / 3.3):
 *       * a texture load (LoadBlock / LoadTile / LoadTLUT) from a range a GPU-drawn image covers resolves those rows
 *         into RDRAM first (RGBA16 or RGBA32, the console's pixel format), so the game's copies (photo detector,
 *         score thumbnails) read the pixels the RDP drew;
 *       * port_gfx_inval_dcache (osInvalDCache) resolves the range for CPU reads;
 *       * an image whose rows are all resolved is "clean": RDRAM is authoritative again, and CPU writes into it (the
 *         detector's red dot) are uploaded before the next draw onto it or before it is shown (hash check).
 *   - Z images are GL depth textures, one per Z-image address and size, attached to whichever colour image is drawn
 *     with them, and kept coherent with RDRAM the same way (S6: score.c counts the pixels of each Pokemon in
 *     64x48 Z buffers and copies Z buffers through texture loads): written back in the console's 16-bit Z format
 *     (18-bit z compressed to 3-bit exponent + 11-bit mantissa, dz 0) on texture loads, osInvalDCache and at the
 *     end of each task (except Z images used with framebuffers); loaded from RDRAM when clean and RDRAM changed.
 *     FILL rectangles into a Z image's rows clear its depth texture directly. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <GLES3/gl3.h>
#include <emscripten/heap.h>
#include <emscripten/html5.h>
#include "gfx_internal.h"

int gfx_saw_fullsync;

/* ------------------------------------------------------------------ RDP state */
typedef struct {
    unsigned fmt, siz, line, tmem, palette, cmt, maskt, shiftt, cms, masks, shifts;
    unsigned uls, ult, lrs, lrt;   /* 10.2 */
} Tile;
static Tile tiles[8];
static uint32_t om_h, om_l, cc_w0, cc_w1;
static uint32_t fill_color, fog_color, blend_color, prim_color, env_color;
static unsigned prim_lod_frac, prim_depth;
static int conv_k[6] = {175, -43, -89, 222, 114, 42};
static struct { unsigned fmt, siz, width; unsigned char *addr; } timg, cimg;
static unsigned char *zimg;
static float sc_x0, sc_y0, sc_x1 = 320, sc_y1 = 240;
static unsigned char tmem[4096];
static unsigned tmem_gen;
static int state_dirty = 1;

static int gl_ok = -1;   /* -1 not tried, 0 none, 1 ok */
static EMSCRIPTEN_WEBGL_CONTEXT_HANDLE gl_ctx;

/* ------------------------------------------------------------------ colour images (render targets) */
#define MAX_TARGETS 64
typedef struct {
    unsigned char *addr;
    int width, height, siz;   /* siz: SIZ_16b or SIZ_32b */
    GLuint fbo, tex, depth;
    int dirty_y0, dirty_y1;   /* rows drawn by the GPU and not yet resolved into RDRAM */
    int check_cpu;            /* clean, RDRAM may have been written by the CPU: compare with rdram_hash */
    uint32_t rdram_hash;
    unsigned zatt;            /* serial of the Z image attached to fbo (0 none) */
    unsigned last_use;
    int shown;                /* presented by the VI at least once: a framebuffer */
} Target;
static Target targets[MAX_TARGETS];
static Target *cur_target;
static unsigned use_clock;

#define MAX_ZIMGS 32
typedef struct {
    unsigned char *addr;
    int width, height;
    GLuint tex, fbo;          /* DEPTH_COMPONENT24 texture; a depth-only framebuffer for clears and uploads */
    unsigned serial;
    int dirty_y0, dirty_y1;   /* rows the GPU wrote and RDRAM does not hold yet */
    int synced, check_cpu;    /* synced: the GL copy equals RDRAM (rdram_hash) when clean */
    uint32_t rdram_hash;
    int deferred;             /* used with a framebuffer: not written back at the end of each task */
    unsigned last_use;
} ZImg;
static ZImg zimgs[MAX_ZIMGS];
static unsigned zserial;
static void zimg_resolve(ZImg *z, int y0, int y1);

static void flush(void);

/* ------------------------------------------------------------------ read-back journal (S6: node parity) */
/* Write-backs are the only way the GPU changes what the game's CPU sees. A browser run can record every byte they
 * changed (port_rb_record), grouped by "points" where the CPU can next observe memory: the end of a graphics task,
 * an osInvalDCache, a VI frame. Node has no GL; given the journal (port_rb_load) it writes the same bytes at the same
 * points, so a node run with the same input follows the browser's game state past the detector's first read-back.
 * Record: {u32 seq, u32 kind, u32 addr, u32 len, len bytes padded to 4}; a window that changed memory ends with a
 * {seq, kind, 0, 0} record (kind 1 task, 2 osInvalDCache, 3 VI frame), checked on replay. Raw wasm addresses and
 * bytes in the vbe layout: a journal is only valid for the build that recorded it, and it is ROM-derived
 * (port_work/ only). */
static int rb_mode;            /* 0 off, 1 record, 2 replay */
static uint32_t rb_seq;
static unsigned char *rb_buf;  /* record: the journal; replay: the loaded journal */
static uint32_t rb_len, rb_cap, rb_pos;
static int rb_open;            /* record: this window has entries */
static unsigned char *rb_old;
static uint32_t rb_old_cap;
static unsigned rb_desync;

static void rb_put(const void *p, uint32_t n) {
    if (rb_len + n > rb_cap) {
        uint32_t c = rb_cap ? rb_cap : 1u << 20;
        while (c < rb_len + n) c *= 2;
        unsigned char *b = realloc(rb_buf, c);
        if (!b) { rb_mode = 0; port_log("[readback] journal: out of memory, recording stopped\n"); return; }
        rb_buf = b; rb_cap = c;
    }
    memcpy(rb_buf + rb_len, p, n);
    rb_len += n;
}
static void rb_put_hdr(uint32_t kind, uint32_t addr, uint32_t len) {
    uint32_t h[4] = {rb_seq, kind, addr, len};
    rb_put(h, sizeof h);
}
/* a write-back is about to change [p, p + n): keep the old bytes */
static void rb_begin(const unsigned char *p, uint32_t n) {
    if (rb_mode != 1) return;
    if (n > rb_old_cap) { free(rb_old); rb_old = malloc(n); rb_old_cap = rb_old ? n : 0; }
    if (rb_old) memcpy(rb_old, p, n);
}
/* ... and has: journal the runs that changed */
static void rb_end(const unsigned char *p, uint32_t n) {
    if (rb_mode != 1 || !rb_old || n > rb_old_cap) return;
    uint32_t i = 0;
    while (i < n) {
        if (p[i] == rb_old[i]) { i++; continue; }
        uint32_t j = i + 1, same = 0;
        while (j < n && same < 16) { if (p[j] == rb_old[j]) same++; else same = 0; j++; }
        j -= same;
        uint32_t len = j - i, pad = 0;
        rb_put_hdr(0, (uint32_t)(uintptr_t)(p + i), len);
        rb_put(p + i, len);
        rb_put(&pad, (4 - (len & 3)) & 3);
        rb_open = 1;
        i = j;
    }
}
static void rb_point(uint32_t kind) {
    if (rb_mode == 1) {
        if (rb_open) rb_put_hdr(kind, 0, 0);
        rb_open = 0;
    } else if (rb_mode == 2) {
        while (rb_pos + 16 <= rb_len) {
            uint32_t h[4];
            memcpy(h, rb_buf + rb_pos, 16);
            if (h[0] != rb_seq) break;
            rb_pos += 16;
            if (h[3] == 0) {
                if (h[1] != kind && !rb_desync++)
                    port_log("[readback] desync at point %u: journal kind %u, this run %u\n", rb_seq, h[1], kind);
                continue;
            }
            if (rb_pos + h[3] > rb_len) { rb_pos = rb_len; break; }
            memcpy((void *)(uintptr_t)h[2], rb_buf + rb_pos, h[3]);
            rb_pos += (h[3] + 3) & ~3u;
        }
        if (rb_pos + 16 <= rb_len && ((const uint32_t *)(rb_buf + rb_pos))[0] < rb_seq && !rb_desync++)
            port_log("[readback] desync at point %u: journal behind\n", rb_seq);
    }
    rb_seq++;
}
void port_rb_record(int on) {
    if (on && gl_ok != 0) { rb_mode = 1; rb_seq = 0; rb_len = 0; rb_open = 0; }
    else if (!on && rb_mode == 1) rb_mode = 0;
}
unsigned char *port_rb_journal(void) { return rb_buf; }
uint32_t port_rb_journal_size(void) { return rb_len; }
uint32_t port_rb_point_count(void) { return rb_seq; }
/* node: replay a journal (the bytes are copied); the first point of this run must be the journal's first */
int port_rb_load(const unsigned char *p, uint32_t n) {
    free(rb_buf);
    rb_buf = malloc(n ? n : 1);
    if (!rb_buf) return -1;
    memcpy(rb_buf, p, n);
    rb_len = rb_cap = n; rb_pos = 0; rb_seq = 0; rb_mode = 2; rb_desync = 0;
    port_log("[readback] replaying a journal of %u bytes\n", n);
    return 0;
}


static int bpp_of(int siz) { return siz == SIZ_32b ? 4 : 2; }
static uint32_t hash_words(const unsigned char *p, uint32_t bytes) {
    uint32_t h = 2166136261u;
    const uint32_t *w = (const uint32_t *)p;
    for (uint32_t i = 0; i < bytes / 4; i++) { h ^= w[i]; h *= 16777619u; }
    return h;
}

static void target_resolve(Target *t, int y0, int y1) {
    if (!t->fbo || t->dirty_y1 <= t->dirty_y0) return;
    if (y0 < t->dirty_y0) y0 = t->dirty_y0;
    if (y1 > t->dirty_y1) y1 = t->dirty_y1;
    if (y1 <= y0) return;
    flush();
    int w = t->width, n = y1 - y0, bpp = bpp_of(t->siz);
    unsigned char *px = malloc((size_t)w * n * 4);
    if (!px) return;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, t->fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, y0, w, n, GL_RGBA, GL_UNSIGNED_BYTE, px);
    rb_begin(t->addr + (size_t)y0 * w * bpp, (uint32_t)(n * w * bpp));
    for (int y = 0; y < n; y++) {
        unsigned char *row = t->addr + (size_t)(y0 + y) * w * bpp;
        const unsigned char *s = px + (size_t)y * w * 4;
        if (bpp == 2)
            for (int x = 0; x < w; x++, s += 4)
                vbe_st16(row + 2 * x, (unsigned)(s[0] >> 3) << 11 | (unsigned)(s[1] >> 3) << 6 | (unsigned)(s[2] >> 3) << 1 | (s[3] >= 128));
        else
            for (int x = 0; x < w; x++, s += 4)
                vbe_st32(row + 4 * x, (uint32_t)s[0] << 24 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 8 | s[3]);
    }
    rb_end(t->addr + (size_t)y0 * w * bpp, (uint32_t)(n * w * bpp));
    free(px);
    port_gfx_stats.resolves++;
    port_gfx_stats.resolve_px += (unsigned)(w * n);
    {   /* other images over these bytes: their GL copy may now be stale (checked when clean) */
        const unsigned char *a = t->addr + (size_t)y0 * w * bpp, *e = t->addr + (size_t)y1 * w * bpp;
        for (int i = 0; i < MAX_TARGETS; i++) {
            Target *o = &targets[i];
            if (o == t || !o->addr) continue;
            const unsigned char *oe = o->addr + (size_t)o->width * o->height * bpp_of(o->siz);
            if (o->addr < e && a < oe) o->check_cpu = 1;
        }
        for (int i = 0; i < MAX_ZIMGS; i++) {
            ZImg *z = &zimgs[i];
            if (z->addr && z->addr < e && a < z->addr + (size_t)z->width * z->height * 2) z->check_cpu = 1;
        }
    }
    if (y0 <= t->dirty_y0 && y1 >= t->dirty_y1) {
        t->dirty_y0 = t->dirty_y1 = 0;
        t->check_cpu = 0;
        t->rdram_hash = hash_words(t->addr, (uint32_t)(t->width * t->height * bpp));
    } else if (y0 <= t->dirty_y0) t->dirty_y0 = y1;
    else if (y1 >= t->dirty_y1) t->dirty_y1 = y0;
}

static void target_upload(Target *t) {   /* RDRAM -> GL (the image is clean) */
    int w = t->width, h = t->height, bpp = bpp_of(t->siz);
    unsigned char *px = malloc((size_t)w * h * 4);
    if (!px) return;
    for (int y = 0; y < h; y++) {
        const unsigned char *row = t->addr + (size_t)y * w * bpp;
        unsigned char *d = px + (size_t)y * w * 4;
        for (int x = 0; x < w; x++, d += 4) {
            if (bpp == 2) {
                unsigned c = vbe_ld16(row + 2 * x), r = c >> 11, g = (c >> 6) & 31, b = (c >> 1) & 31;
                d[0] = (unsigned char)(r << 3 | r >> 2); d[1] = (unsigned char)(g << 3 | g >> 2);
                d[2] = (unsigned char)(b << 3 | b >> 2); d[3] = (c & 1) ? 255 : 0;
            } else {
                uint32_t c = vbe_ld32(row + 4 * x);
                d[0] = c >> 24; d[1] = c >> 16; d[2] = c >> 8; d[3] = c;
            }
        }
    }
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glBindTexture(GL_TEXTURE_2D, 0);   /* never leave a colour image bound for sampling (feedback loop) */
    state_dirty = 1;
    free(px);
    t->rdram_hash = hash_words(t->addr, (uint32_t)(w * h * bpp));
    t->check_cpu = 0;
    port_gfx_stats.uploads++;
    port_gfx_stats.upload_px += (unsigned)(w * h);
}
static void target_sync_cpu(Target *t) {   /* a clean image: pick up CPU writes */
    if (t->dirty_y1 > t->dirty_y0) return;
    uint32_t h = hash_words(t->addr, (uint32_t)(t->width * t->height * bpp_of(t->siz)));
    if (h != t->rdram_hash) target_upload(t);
    t->check_cpu = 0;
}

static void target_free(Target *t) {
    if (t == cur_target) { flush(); cur_target = 0; }
    target_resolve(t, 0, t->height);
    if (t->fbo) { glDeleteFramebuffers(1, &t->fbo); glDeleteTextures(1, &t->tex); }
    memset(t, 0, sizeof *t);
}

static int in_memory(const unsigned char *p, size_t n) {
    uintptr_t a = (uintptr_t)p;
    return a >= 0x10000000u && a + n <= emscripten_get_heap_size();
}

static Target *target_get(unsigned char *addr, int width, int siz, int min_h) {
    if (siz != SIZ_32b) siz = SIZ_16b;
    int bpp = bpp_of(siz);
    Target *hit = 0;
    for (int i = 0; i < MAX_TARGETS; i++)
        if (targets[i].addr == addr && targets[i].width == width && targets[i].siz == siz) { hit = &targets[i]; break; }
    if (hit && hit->height >= min_h) { hit->last_use = ++use_clock; return hit; }
    int h = width == 320 || width == 640 ? (width == 320 ? 240 : 480) : 0;
    if (hit && hit->height > h) h = hit->height;
    if (h < min_h) h = min_h;
    if (h < 1) h = 1;
    if (h > 1024) h = 1024;
    while (h > 1 && !in_memory(addr, (size_t)width * h * bpp)) h--;
    if (hit) target_free(hit);
    /* the same address with another width / pixel size is replaced (written back first). Other overlapping images
     * stay: only their dirty rows are ever written back (the detector's 8x8 regions are 128 bytes apart and drawn
     * under a 32x32 scissor), and a write-back marks the images it overlaps for a CPU-side check. */
    Target *slot = 0, *lru = &targets[0];
    for (int i = 0; i < MAX_TARGETS; i++) {
        Target *t = &targets[i];
        if (!t->addr) { if (!slot) slot = t; continue; }
        if (t->addr == addr) { target_free(t); if (!slot) slot = t; continue; }
        if (t->last_use < lru->last_use) lru = t;
    }
    if (!slot) { target_free(lru); slot = lru; }
    Target *t = slot;
    t->addr = addr; t->width = width; t->height = h; t->siz = siz; t->last_use = ++use_clock;
    glGenFramebuffers(1, &t->fbo);
    glGenTextures(1, &t->tex);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        gfx_log_once(0xFB000000 | (unsigned)width, "framebuffer incomplete (%dx%d)", width, h);
    target_upload(t);   /* the image starts as RDRAM holds it */
    state_dirty = 1;
    return t;
}

/* the RDRAM rows [p, p + n) are about to be read (texture load, CPU, a Z image loading itself): resolve GPU-drawn
 * colour and Z images there (except `skip`) */
static void resolve_range_skip(const unsigned char *p, uint32_t n, const ZImg *skip) {
    for (int i = 0; i < MAX_TARGETS; i++) {
        Target *t = &targets[i];
        if (!t->addr || t->dirty_y1 <= t->dirty_y0) continue;
        int stride = t->width * bpp_of(t->siz);
        const unsigned char *te = t->addr + (size_t)stride * t->height;
        if (p >= te || p + n <= t->addr) continue;
        int y0 = p > t->addr ? (int)((p - t->addr) / stride) : 0;
        int y1 = p + n < te ? (int)((p + n - t->addr + stride - 1) / stride) : t->height;
        target_resolve(t, y0, y1);
    }
    for (int i = 0; i < MAX_ZIMGS; i++) {
        ZImg *z = &zimgs[i];
        if (z == skip || !z->addr || z->dirty_y1 <= z->dirty_y0) continue;
        int stride = z->width * 2;
        const unsigned char *ze = z->addr + (size_t)stride * z->height;
        if (p >= ze || p + n <= z->addr) continue;
        int y0 = p > z->addr ? (int)((p - z->addr) / stride) : 0;
        int y1 = p + n < ze ? (int)((p + n - z->addr + stride - 1) / stride) : z->height;
        zimg_resolve(z, y0, y1);
    }
}
static void resolve_range(const unsigned char *p, uint32_t n) { resolve_range_skip(p, n, 0); }

void port_gfx_inval_dcache(const void *p, uint32_t len) {
    if (gl_ok == 1) resolve_range((const unsigned char *)p, len);
    rb_point(2);
}
void port_gfx_writeback_dcache(const void *p, uint32_t len) {
    if (gl_ok != 1) return;
    const unsigned char *a = p;
    for (int i = 0; i < MAX_TARGETS; i++) {
        Target *t = &targets[i];
        if (!t->addr) continue;
        const unsigned char *te = t->addr + (size_t)t->width * t->height * bpp_of(t->siz);
        if (!len || (a < te && a + len > t->addr)) t->check_cpu = 1;
    }
    for (int i = 0; i < MAX_ZIMGS; i++) {
        ZImg *z = &zimgs[i];
        if (!z->addr) continue;
        const unsigned char *ze = z->addr + (size_t)z->width * z->height * 2;
        if (!len || (a < ze && a + len > z->addr)) z->check_cpu = 1;
    }
}
void port_gfx_resolve_all(void) {
    if (gl_ok != 1) return;
    for (int i = 0; i < MAX_TARGETS; i++) if (targets[i].addr) target_resolve(&targets[i], 0, targets[i].height);
    for (int i = 0; i < MAX_ZIMGS; i++) if (zimgs[i].addr) zimg_resolve(&zimgs[i], 0, zimgs[i].height);
}

/* ------------------------------------------------------------------ TMEM loads */
static unsigned char *timg_at(unsigned s, unsigned t) {   /* byte address of texel (s, t) of the texture image */
    return timg.addr + (((size_t)t * timg.width + s) << timg.siz >> 1);
}
static void load_block(uint32_t w0, uint32_t w1) {
    Tile *tl = &tiles[(w1 >> 24) & 7];
    unsigned uls = (w0 >> 12) & 0xFFF, ult = w0 & 0xFFF, lrs = (w1 >> 12) & 0xFFF, dxt = w1 & 0xFFF;
    if (!timg.addr) return;
    unsigned texels = lrs - uls + 1, bytes = (texels << timg.siz) >> 1;
    bytes = (bytes + 7) & ~7u;
    unsigned char *src = timg_at(uls, ult);
    if (!in_memory(src, bytes)) { gfx_log_once(0xF3000000, "LoadBlock outside memory"); return; }
    resolve_range(src, bytes);
    unsigned base = tl->tmem * 8, qwords = bytes / 8, acc = 0;
    for (unsigned q = 0; q < qwords; q++) {
        unsigned swap = (acc >> 11) & 1 ? 4 : 0;
        acc += dxt;
        for (unsigned b = 0; b < 8; b++) {
            unsigned v = vbe_ld8(src + q * 8 + b);
            if (timg.siz == SIZ_32b) {   /* RG to the low bank, BA to the high bank */
                unsigned texel = q * 2 + b / 4, half = (b & 3) >> 1;
                unsigned a = ((base / 2 + texel * 2 + (b & 1)) ^ swap) & 0x7FF;
                tmem[a + (half ? 0x800 : 0)] = (unsigned char)v;
            } else tmem[(base + q * 8 + (b ^ swap)) & 0xFFF] = (unsigned char)v;
        }
    }
    tmem_gen++;
}
static void load_tile(uint32_t w0, uint32_t w1) {
    Tile *tl = &tiles[(w1 >> 24) & 7];
    unsigned uls = ((w0 >> 12) & 0xFFF) >> 2, ult = (w0 & 0xFFF) >> 2, lrs = ((w1 >> 12) & 0xFFF) >> 2, lrt = (w1 & 0xFFF) >> 2;
    tl->uls = ((w0 >> 12) & 0xFFF); tl->ult = w0 & 0xFFF; tl->lrs = (w1 >> 12) & 0xFFF; tl->lrt = w1 & 0xFFF;
    if (!timg.addr || lrs < uls || lrt < ult) return;
    unsigned w = lrs - uls + 1, rowbytes = (w << timg.siz) >> 1, line = tl->line * 8, base = tl->tmem * 8;
    if (timg.siz == SIZ_32b) line *= 2;   /* line is in 64-bit words of one bank */
    resolve_range(timg_at(uls, ult), (unsigned)(((size_t)(lrt - ult) * timg.width + w) << timg.siz >> 1));
    for (unsigned y = 0; y <= lrt - ult; y++) {
        unsigned char *src = timg_at(uls, ult + y);
        if (!in_memory(src, rowbytes)) break;
        unsigned swap = (y & 1) ? 4 : 0;
        for (unsigned b = 0; b < rowbytes; b++) {
            unsigned v = vbe_ld8(src + b);
            if (timg.siz == SIZ_32b) {
                unsigned texel = b / 4, half = (b & 3) >> 1;
                unsigned a = ((base / 2 + y * line / 2 + texel * 2 + (b & 1)) ^ swap) & 0x7FF;
                tmem[a + (half ? 0x800 : 0)] = (unsigned char)v;
            } else tmem[(base + y * line + (b ^ swap)) & 0xFFF] = (unsigned char)v;
        }
    }
    tmem_gen++;
}
static void load_tlut(uint32_t w0, uint32_t w1) {
    Tile *tl = &tiles[(w1 >> 24) & 7];
    unsigned uls = ((w0 >> 12) & 0xFFF) >> 2, ult = (w0 & 0xFFF) >> 2, lrs = ((w1 >> 12) & 0xFFF) >> 2;
    if (!timg.addr || lrs < uls) return;
    unsigned n = lrs - uls + 1, base = tl->tmem * 8;
    unsigned char *src = timg_at(uls, ult);
    if (!in_memory(src, n * 2)) return;
    resolve_range(src, n * 2);
    for (unsigned i = 0; i < n; i++) {
        unsigned hi = vbe_ld8(src + 2 * i), lo = vbe_ld8(src + 2 * i + 1);
        for (unsigned k = 0; k < 4; k++) {   /* each entry is written 4 times (one per bank) */
            tmem[(base + i * 8 + k * 2) & 0xFFF] = (unsigned char)hi;
            tmem[(base + i * 8 + k * 2 + 1) & 0xFFF] = (unsigned char)lo;
        }
    }
    tmem_gen++;
}

/* ------------------------------------------------------------------ texel decode */
static unsigned tlut_mode(void) { return (om_h >> OMH_TEXTLUT_SFT) & 3; }   /* 0 none, 2 RGBA16, 3 IA16 */
static void decode16(unsigned c, unsigned fmt_ia, unsigned char *o) {
    if (fmt_ia) { o[0] = o[1] = o[2] = (unsigned char)(c >> 8); o[3] = (unsigned char)c; return; }
    unsigned r = c >> 11, g = (c >> 6) & 31, b = (c >> 1) & 31;
    o[0] = (unsigned char)(r << 3 | r >> 2); o[1] = (unsigned char)(g << 3 | g >> 2); o[2] = (unsigned char)(b << 3 | b >> 2);
    o[3] = (c & 1) ? 255 : 0;
}
static void tlut_entry(unsigned idx, unsigned char *o) {
    unsigned a = 0x800 + (idx & 0xFF) * 8;
    decode16((unsigned)tmem[a] << 8 | tmem[a + 1], tlut_mode() == 3, o);
}
static void texel(const Tile *tl, unsigned s, unsigned t, unsigned char *o) {
    unsigned line = tl->line * 8, base = tl->tmem * 8, swap = (t & 1) ? 4 : 0;
    unsigned tl_on = tlut_mode() != 0;
    switch (tl->siz) {
    case SIZ_4b: {
        unsigned b = tmem[((base + t * line + s / 2) ^ swap) & 0xFFF], v = (s & 1) ? b & 15 : b >> 4;
        if (tl_on || tl->fmt == FMT_CI) { tlut_entry(tl->palette << 4 | v, o); return; }
        if (tl->fmt == FMT_IA) { unsigned i = v >> 1; o[0] = o[1] = o[2] = (unsigned char)(i << 5 | i << 2 | i >> 1); o[3] = (v & 1) ? 255 : 0; return; }
        o[0] = o[1] = o[2] = o[3] = (unsigned char)(v * 17);
        return;
    }
    case SIZ_8b: {
        unsigned v = tmem[((base + t * line + s) ^ swap) & 0xFFF];
        if (tl_on || tl->fmt == FMT_CI) { tlut_entry(v, o); return; }
        if (tl->fmt == FMT_IA) { o[0] = o[1] = o[2] = (unsigned char)((v >> 4) * 17); o[3] = (unsigned char)((v & 15) * 17); return; }
        o[0] = o[1] = o[2] = o[3] = (unsigned char)v;
        return;
    }
    case SIZ_16b: {
        if (tl->fmt == FMT_YUV) {   /* texel pairs U Y0 V Y1; RGB from the SetConvert coefficients */
            unsigned a = ((base + t * line + (s & ~1u) * 2) ^ swap) & 0xFFC;
            int u = tmem[a] - 128, y = tmem[a + ((s & 1) ? 3 : 1)], v = tmem[a + 2] - 128;
            int r = y + (conv_k[0] * v) / 128, g = y + (conv_k[1] * u + conv_k[2] * v) / 128, b = y + (conv_k[3] * u) / 128;
            o[0] = (unsigned char)(r < 0 ? 0 : r > 255 ? 255 : r); o[1] = (unsigned char)(g < 0 ? 0 : g > 255 ? 255 : g);
            o[2] = (unsigned char)(b < 0 ? 0 : b > 255 ? 255 : b); o[3] = 255;
            return;
        }
        unsigned a = ((base + t * line + s * 2) ^ swap) & 0xFFF;
        unsigned c = (unsigned)tmem[a] << 8 | tmem[(a + 1) & 0xFFF];
        if (tl_on && tl->fmt == FMT_CI) { tlut_entry(c >> 8, o); return; }
        decode16(c, tl->fmt == FMT_IA || tl->fmt == FMT_I, o);
        return;
    }
    default: {   /* 32b: RG in the low bank, BA in the high bank */
        unsigned a = ((base / 2 + t * line + s * 2) ^ swap) & 0x7FF;
        o[0] = tmem[a]; o[1] = tmem[(a + 1) & 0x7FF]; o[2] = tmem[a + 0x800]; o[3] = tmem[((a + 1) & 0x7FF) + 0x800];
        return;
    }
    }
}

/* ------------------------------------------------------------------ texture cache */
typedef struct { uint32_t key[4]; GLuint tex; int w, h; unsigned stamp; } TexEnt;
#define TEX_CACHE 1024
static TexEnt tex_cache[TEX_CACHE];
static unsigned tex_clock;
typedef struct { GLuint tex; int w, h; unsigned gen; uint32_t desc; } TileTex;
static TileTex tile_tex[8];

static void tile_dims(const Tile *tl, int *w, int *h, GLint *ws, GLint *wt) {
    int tw = (int)((tl->lrs >> 2) - (tl->uls >> 2)) + 1, th = (int)((tl->lrt >> 2) - (tl->ult >> 2)) + 1;
    if (tw <= 0) tw = 1;
    if (th <= 0) th = 1;
    int mw = tl->masks ? 1 << tl->masks : 0, mh = tl->maskt ? 1 << tl->maskt : 0;
    *ws = *wt = GL_CLAMP_TO_EDGE;
    if (mw && (!(tl->cms & 2) || tw > mw)) { tw = mw; *ws = (tl->cms & 1) ? GL_MIRRORED_REPEAT : GL_REPEAT; }
    if (mh && (!(tl->cmt & 2) || th > mh)) { th = mh; *wt = (tl->cmt & 1) ? GL_MIRRORED_REPEAT : GL_REPEAT; }
    if (tw > 1024) tw = 1024;
    if (th > 1024) th = 1024;
    *w = tw; *h = th;
}
static uint32_t tile_desc(const Tile *tl) {
    return tl->fmt | tl->siz << 3 | tl->line << 5 | tl->tmem << 14 | tl->palette << 23 | tlut_mode() << 27 | tl->cms << 29;
}
static GLuint tile_texture(int ti, int *pw, int *ph) {
    Tile *tl = &tiles[ti & 7];
    int w, h;
    GLint ws, wt;
    tile_dims(tl, &w, &h, &ws, &wt);
    uint32_t desc = tile_desc(tl);
    TileTex *tt = &tile_tex[ti & 7];
    if (tt->tex && tt->gen == tmem_gen && tt->desc == desc && tt->w == w && tt->h == h) { *pw = w; *ph = h; return tt->tex; }
    /* key: TMEM bytes the tile reads + palette + descriptor */
    unsigned line = tl->line * 8, base = tl->tmem * 8;
    uint32_t hsh = 2166136261u;
    unsigned span = line * (unsigned)h;
    if (tl->siz == SIZ_32b || span == 0 || span > 4096) span = 4096, base = 0;
    for (unsigned i = 0; i < span; i++) { hsh ^= tmem[(base + i) & 0xFFF]; hsh *= 16777619u; }
    if (tlut_mode() || tl->fmt == FMT_CI) {
        unsigned p0 = tl->siz == SIZ_4b ? 0x800 + tl->palette * 128 : 0x800, pn = tl->siz == SIZ_4b ? 128 : 2048;
        for (unsigned i = 0; i < pn; i += 8) { hsh ^= tmem[(p0 + i) & 0xFFF] << 8 | tmem[(p0 + i + 1) & 0xFFF]; hsh *= 16777619u; }
    }
    if (tl->fmt == FMT_YUV) { for (int k = 0; k < 4; k++) { hsh ^= (uint32_t)conv_k[k]; hsh *= 16777619u; } }
    uint32_t key[4] = {hsh, desc, (uint32_t)w | (uint32_t)h << 16, (uint32_t)ws << 16 ^ (uint32_t)wt ^ tl->cmt << 30};
    unsigned slot = (hsh ^ desc * 31u) % TEX_CACHE;
    TexEnt *victim = 0;
    for (unsigned probe = 0; probe < 8; probe++) {
        TexEnt *e = &tex_cache[(slot + probe) % TEX_CACHE];
        if (e->tex && !memcmp(e->key, key, sizeof key)) {
            e->stamp = ++tex_clock;
            port_gfx_stats.tex_hits++;
            tt->tex = e->tex; tt->w = w; tt->h = h; tt->gen = tmem_gen; tt->desc = desc;
            *pw = w; *ph = h;
            return e->tex;
        }
        if (!victim || !e->tex || (victim->tex && e->stamp < victim->stamp)) victim = e;
    }
    if (victim->tex) glDeleteTextures(1, &victim->tex);
    unsigned char *px = malloc((size_t)w * h * 4);
    if (!px) return 0;
    for (int t = 0; t < h; t++)
        for (int s = 0; s < w; s++) texel(tl, (unsigned)s, (unsigned)t, px + ((size_t)t * w + s) * 4);
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, ws);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wt);
    free(px);
    memcpy(victim->key, key, sizeof key);
    victim->tex = tex; victim->w = w; victim->h = h; victim->stamp = ++tex_clock;
    port_gfx_stats.tex_decodes++;
    tt->tex = tex; tt->w = w; tt->h = h; tt->gen = tmem_gen; tt->desc = desc;
    *pw = w; *ph = h;
    return tex;
}

/* ------------------------------------------------------------------ shaders: combiner + blender per state key */
typedef struct { uint32_t cc0, cc1, oml, flags; GLuint prog; GLint u_prim, u_env, u_fog, u_blend, u_fill, u_misc, u_t0, u_t1; } Prog;
#define MAX_PROGS 512
static Prog progs[MAX_PROGS];
static int nprogs;
static Prog *cur_prog;

#define PF_CYC_MASK 3u
#define PF_TEX0 4u
#define PF_TEX1 8u
#define PF_ZS_PRIM 16u
#define PF_ACMP_SFT 5          /* 2 bits */
#define PF_TEXEDGE 128u
#define PF_FORCE_BL 256u
#define PF_RECT 512u           /* no shade (rectangles): SHADE = 0 */

static const char *rgb_a[16] = {"comb.rgb", "t0.rgb", "t1.rgb", "uPrim.rgb", "sh.rgb", "uEnv.rgb", "vec3(1.0)", "vec3(noise)",
                                "vec3(0.0)", "vec3(0.0)", "vec3(0.0)", "vec3(0.0)", "vec3(0.0)", "vec3(0.0)", "vec3(0.0)", "vec3(0.0)"};
static const char *rgb_b[16] = {"comb.rgb", "t0.rgb", "t1.rgb", "uPrim.rgb", "sh.rgb", "uEnv.rgb", "vec3(0.0)", "vec3(uMisc.x)",
                                "vec3(0.0)", "vec3(0.0)", "vec3(0.0)", "vec3(0.0)", "vec3(0.0)", "vec3(0.0)", "vec3(0.0)", "vec3(0.0)"};
static const char *rgb_c[32] = {"comb.rgb", "t0.rgb", "t1.rgb", "uPrim.rgb", "sh.rgb", "uEnv.rgb", "vec3(1.0)", "vec3(comb.a)",
                                "vec3(t0.a)", "vec3(t1.a)", "vec3(uPrim.a)", "vec3(sh.a)", "vec3(uEnv.a)", "vec3(0.0)",
                                "vec3(uMisc.z)", "vec3(uMisc.y)"};
static const char *rgb_d[8] = {"comb.rgb", "t0.rgb", "t1.rgb", "uPrim.rgb", "sh.rgb", "uEnv.rgb", "vec3(1.0)", "vec3(0.0)"};
static const char *a_abd[8] = {"comb.a", "t0.a", "t1.a", "uPrim.a", "sh.a", "uEnv.a", "1.0", "0.0"};
static const char *a_c[8] = {"0.0", "t0.a", "t1.a", "uPrim.a", "sh.a", "uEnv.a", "uMisc.z", "0.0"};

static int cc_uses_tex(uint32_t w0, uint32_t w1, int cyc, int which) {   /* which: 1 = TEXEL0, 2 = TEXEL1 */
    unsigned sel[2][8] = {
        {(w0 >> 20) & 15, (w1 >> 28) & 15, (w0 >> 15) & 31, (w1 >> 15) & 7, (w0 >> 12) & 7, (w1 >> 12) & 7, (w0 >> 9) & 7, (w1 >> 9) & 7},
        {(w0 >> 5) & 15, (w1 >> 24) & 15, w0 & 31, (w1 >> 6) & 7, (w1 >> 21) & 7, (w1 >> 3) & 7, (w1 >> 18) & 7, w1 & 7}};
    /* sel: RGB a b c d, alpha a b c d; TEXEL0 = 1, TEXEL1 = 2 everywhere, plus TEXELn_ALPHA = 8 / 9 in RGB c */
    for (int c = 0; c < (cyc == CYC_2 ? 2 : 1); c++) {
        for (int k = 0; k < 8; k++)
            if (sel[c][k] == (unsigned)which) return 1;
        if (sel[c][2] == (unsigned)(which + 7)) return 1;
    }
    return 0;
}

static char *gen_frag(uint32_t w0, uint32_t w1, uint32_t oml, uint32_t flags) {
    static char src[8192];
    int n = 0, cyc = flags & PF_CYC_MASK;
#define P(...) n += snprintf(src + n, sizeof src - n, __VA_ARGS__)
    P("#version 300 es\nprecision highp float;\nin vec4 vCol;\nin vec4 vUV;\nout vec4 oCol;\n"
      "uniform vec4 uPrim, uEnv, uFog, uBlend, uFill, uMisc;\nuniform sampler2D uT0, uT1;\n"
      "void main() {\n vec4 sh = %s;\n", (flags & PF_RECT) ? "vec4(0.0)" : "vCol");
    P(" vec4 t0 = %s;\n vec4 t1 = %s;\n", (flags & PF_TEX0) ? "texture(uT0, vUV.xy)" : "vec4(0.0)",
      (flags & PF_TEX1) ? "texture(uT1, vUV.zw)" : "vec4(0.0)");
    P(" float noise = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);\n vec4 comb = vec4(0.0);\n");
    if (cyc == CYC_FILL) P(" oCol = uFill;\n}\n");
    else if (cyc == CYC_COPY) {
        P(" oCol = t0;\n");
        if (((flags >> PF_ACMP_SFT) & 3) == 1) P(" if (oCol.a < 0.5) discard;\n");   /* copy mode compares texel alpha */
        P("}\n");   /* the texel is copied whole: RGBA16 keeps its alpha (coverage) bit, as score.c's Z copies need */
    } else {
        unsigned sel[2][8] = {
            {(w0 >> 20) & 15, (w1 >> 28) & 15, (w0 >> 15) & 31, (w1 >> 15) & 7, (w0 >> 12) & 7, (w1 >> 12) & 7, (w0 >> 9) & 7, (w1 >> 9) & 7},
            {(w0 >> 5) & 15, (w1 >> 24) & 15, w0 & 31, (w1 >> 6) & 7, (w1 >> 21) & 7, (w1 >> 3) & 7, (w1 >> 18) & 7, w1 & 7}};
        for (int c = 0; c < (cyc == CYC_2 ? 2 : 1); c++) {
            unsigned *s = sel[c];
            P(" comb = clamp(vec4((%s - %s) * %s + %s, (%s - %s) * %s + %s), 0.0, 1.0);\n",
              rgb_a[s[0]], rgb_b[s[1]], s[2] < 16 ? rgb_c[s[2]] : "vec3(0.0)", rgb_d[s[3]],
              a_abd[s[4]], a_abd[s[5]], a_c[s[6]], a_abd[s[7]]);
        }
        unsigned acmp = (flags >> PF_ACMP_SFT) & 3;
        if (acmp == 1) P(" if (comb.a < max(uBlend.a, 1.0 / 255.0)) discard;\n");
        else if (acmp == 3) P(" if (comb.a < noise) discard;\n");
        if (flags & PF_TEXEDGE) P(" if (comb.a < 0.5) discard;\n");
        /* blender: cycle 0 (2-cycle) in the shader, the last cycle in the shader unless it reads memory */
        static const char *pm[4] = {"col.rgb", "col.rgb", "uBlend.rgb", "uFog.rgb"};
        static const char *aa[4] = {"col.a", "uFog.a", "sh.a", "0.0"};
        P(" vec4 col = comb;\n");
        int ncyc = cyc == CYC_2 ? 2 : 1;
        for (int c = 0; c < ncyc; c++) {
            unsigned bp = (oml >> (30 - 2 * c)) & 3, ba = (oml >> (26 - 2 * c)) & 3, bm = (oml >> (22 - 2 * c)) & 3, bb = (oml >> (18 - 2 * c)) & 3;
            int last = c == ncyc - 1;
            int mem = bp == 1 || bm == 1;
            if (last && mem) {
                if ((flags & PF_FORCE_BL) && bm == 1 && bp != 1) P(" col = vec4(%s, %s);\n", pm[bp], aa[ba]);   /* GL blends with MEM */
                break;
            }
            if (mem) continue;   /* a first cycle reading memory: pass IN through */
            static const char *bf[4] = {"(1.0 - A)", "1.0", "1.0", "0.0"};
            P(" { float A = %s; col.rgb = clamp(%s * A + %s * %s, 0.0, 1.0); }\n", aa[ba], pm[bp], pm[bm], bf[bb]);
        }
        if (!(flags & PF_FORCE_BL)) P(" col.a = 1.0;\n");
        P(" oCol = col;\n");
        if (flags & PF_ZS_PRIM) P(" gl_FragDepth = uMisc.w;\n");
        P("}\n");
    }
#undef P
    return src;
}
static const char *vert_src =
    "#version 300 es\nin vec4 aPos;\nin vec4 aCol;\nin vec4 aUV;\nout vec4 vCol;\nout vec4 vUV;\n"
    "void main() { gl_Position = aPos; vCol = aCol; vUV = aUV; }\n";

static GLuint compile(GLenum type, const char *s) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &s, 0);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(sh, sizeof log, 0, log);
        port_log("[gfx] shader compile failed: %s\n%s\n", log, s);
    }
    return sh;
}
static GLuint vert_shader;
static Prog *prog_get(uint32_t w0, uint32_t w1, uint32_t oml, uint32_t flags) {
    for (int i = 0; i < nprogs; i++)
        if (progs[i].cc0 == w0 && progs[i].cc1 == w1 && progs[i].oml == oml && progs[i].flags == flags) return &progs[i];
    if (nprogs == MAX_PROGS) { gfx_log_once(0x9806, "shader cache full"); nprogs = 0; }
    Prog *p = &progs[nprogs++];
    p->cc0 = w0; p->cc1 = w1; p->oml = oml; p->flags = flags;
    GLuint fs = compile(GL_FRAGMENT_SHADER, gen_frag(w0, w1, oml, flags));
    p->prog = glCreateProgram();
    glAttachShader(p->prog, vert_shader);
    glAttachShader(p->prog, fs);
    glBindAttribLocation(p->prog, 0, "aPos");
    glBindAttribLocation(p->prog, 1, "aCol");
    glBindAttribLocation(p->prog, 2, "aUV");
    glLinkProgram(p->prog);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(p->prog, GL_LINK_STATUS, &ok);
    if (!ok) port_log("[gfx] program link failed\n");
    p->u_prim = glGetUniformLocation(p->prog, "uPrim"); p->u_env = glGetUniformLocation(p->prog, "uEnv");
    p->u_fog = glGetUniformLocation(p->prog, "uFog"); p->u_blend = glGetUniformLocation(p->prog, "uBlend");
    p->u_fill = glGetUniformLocation(p->prog, "uFill"); p->u_misc = glGetUniformLocation(p->prog, "uMisc");
    p->u_t0 = glGetUniformLocation(p->prog, "uT0"); p->u_t1 = glGetUniformLocation(p->prog, "uT1");
    glUseProgram(p->prog);
    glUniform1i(p->u_t0, 0);
    glUniform1i(p->u_t1, 1);
    return p;
}

/* ------------------------------------------------------------------ GL setup */
static GLuint vao, vbo;
#define VFLOATS 12   /* pos 4, colour 4, uv0 2, uv1 2 */
#define MAX_BATCH_VERTS 6144
static float batch[MAX_BATCH_VERTS * VFLOATS];
static int batch_n;

extern EMSCRIPTEN_WEBGL_CONTEXT_HANDLE port_gfx_gl_create(void);   /* gfx_gl.js: WebGL2 on Module.canvas */

int port_gfx_init(void) {
    if (gl_ok >= 0) return gl_ok ? 0 : -1;
    gl_ok = 0;
    gl_ctx = emscripten_webgl_get_current_context();
    if (gl_ctx <= 0) gl_ctx = port_gfx_gl_create();
    if (gl_ctx <= 0) { port_log("[gfx] no WebGL2 context: display lists are interpreted, nothing is drawn\n"); return -1; }
    emscripten_webgl_make_context_current(gl_ctx);
    port_log("[gfx] WebGL2: %s / %s\n", (const char *)glGetString(GL_VERSION), (const char *)glGetString(GL_RENDERER));
    vert_shader = compile(GL_VERTEX_SHADER, vert_src);
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof batch, 0, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, VFLOATS * 4, (void *)0);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, VFLOATS * 4, (void *)16);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, VFLOATS * 4, (void *)32);
    gl_ok = 1;
    return 0;
}
int rdp_gl_ready(void) { return gl_ok == 1; }

static void flush(void) {
    if (!batch_n) return;
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)batch_n * VFLOATS * 4, batch);
    glDrawArrays(GL_TRIANGLES, 0, batch_n);
    port_gfx_stats.draws++;
    batch_n = 0;
}
void port_gfx_flush(void) { if (gl_ok == 1) flush(); }

static void set_rgba(GLint u, uint32_t c) {
    glUniform4f(u, (c >> 24) / 255.0f, ((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f);
}

/* ------------------------------------------------------------------ Z images (S6) */
/* The console's 16-bit Z: an 18-bit z (GL depth * 0x3FFFF: both are linear in NDC z) compressed to a 3-bit exponent
 * (leading ones of z) and an 11-bit mantissa, << 2, dz = 0. 0xFFFC (the usual clear) is z 0x3FFFF = depth 1.0. */
static const unsigned zfmt_shift[8] = {6, 5, 4, 3, 2, 1, 0, 0};
static const unsigned zfmt_base[8] = {0, 0x20000, 0x30000, 0x38000, 0x3C000, 0x3E000, 0x3F000, 0x3F800};
static float z16_depth(unsigned v) {
    unsigned e = (v >> 13) & 7, m = (v >> 2) & 0x7FF;
    return (float)(zfmt_base[e] + (m << zfmt_shift[e])) / 262143.0f;
}
static const char *zpass_vert =
    "#version 300 es\nvoid main() { gl_Position = vec4(float((gl_VertexID & 1) << 2) - 1.0, "
    "float((gl_VertexID & 2) << 1) - 1.0, 0.0, 1.0); }\n";
static const char *zenc_frag =
    "#version 300 es\nprecision highp float;\nprecision highp int;\nuniform highp sampler2D uDepth;\nuniform int uY0;\n"
    "out vec4 o;\nvoid main() {\n"
    " float d = texelFetch(uDepth, ivec2(int(gl_FragCoord.x), int(gl_FragCoord.y) + uY0), 0).r;\n"
    " uint z = min(uint(clamp(d, 0.0, 1.0) * 262143.0 + 0.5), 262143u);\n"
    " uint e, m;\n"
    " if (z < 0x20000u) { e = 0u; m = z >> 6; }\n"
    " else if (z < 0x30000u) { e = 1u; m = (z >> 5) & 0x7FFu; }\n"
    " else if (z < 0x38000u) { e = 2u; m = (z >> 4) & 0x7FFu; }\n"
    " else if (z < 0x3C000u) { e = 3u; m = (z >> 3) & 0x7FFu; }\n"
    " else if (z < 0x3E000u) { e = 4u; m = (z >> 2) & 0x7FFu; }\n"
    " else if (z < 0x3F000u) { e = 5u; m = (z >> 1) & 0x7FFu; }\n"
    " else if (z < 0x3F800u) { e = 6u; m = z & 0x7FFu; }\n"
    " else { e = 7u; m = z & 0x7FFu; }\n"
    " uint v = (e << 13) | (m << 2);\n"
    " o = vec4(float(v >> 8) / 255.0, float(v & 255u) / 255.0, 0.0, 1.0);\n}\n";
static const char *zdec_frag =
    "#version 300 es\nprecision highp float;\nuniform highp sampler2D uZ;\n"
    "void main() { gl_FragDepth = texelFetch(uZ, ivec2(gl_FragCoord.xy), 0).r; }\n";
static GLuint zenc_prog, zdec_prog, zpass_vao, zenc_fbo, zenc_tex, zdec_tex;
static GLint zenc_u_y0;
static int zenc_w, zenc_h;

static GLuint zpass_link(const char *frag) {
    GLuint p = glCreateProgram(), vs = compile(GL_VERTEX_SHADER, zpass_vert), fs = compile(GL_FRAGMENT_SHADER, frag);
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) port_log("[gfx] Z program link failed\n");
    return p;
}
static void zpass_init(void) {
    if (zenc_prog) return;
    zenc_prog = zpass_link(zenc_frag);
    zdec_prog = zpass_link(zdec_frag);
    glUseProgram(zenc_prog);
    glUniform1i(glGetUniformLocation(zenc_prog, "uDepth"), 0);
    zenc_u_y0 = glGetUniformLocation(zenc_prog, "uY0");
    glUseProgram(zdec_prog);
    glUniform1i(glGetUniformLocation(zdec_prog, "uZ"), 0);
    glGenVertexArrays(1, &zpass_vao);
    cur_prog = 0;
}
/* GL state a Z pass leaves behind is reset by the next begin_batch (state_dirty) */
static void zpass_state(int w, int h) {
    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glBindVertexArray(zpass_vao);
    glActiveTexture(GL_TEXTURE0);
    state_dirty = 1;
    cur_prog = 0;
}

static void zimg_free(ZImg *z) {
    zimg_resolve(z, 0, z->height);
    if (z->tex) { glDeleteFramebuffers(1, &z->fbo); glDeleteTextures(1, &z->tex); }
    memset(z, 0, sizeof *z);
}

/* GPU -> RDRAM: rows [y0, y1) of the dirty rows, in the console's Z format */
static void zimg_resolve(ZImg *z, int y0, int y1) {
    if (!z->tex || z->dirty_y1 <= z->dirty_y0) return;
    if (y0 < z->dirty_y0) y0 = z->dirty_y0;
    if (y1 > z->dirty_y1) y1 = z->dirty_y1;
    if (y1 <= y0) return;
    flush();
    zpass_init();
    int w = z->width, n = y1 - y0;
    if (w > zenc_w || n > zenc_h) {
        if (zenc_tex) { glDeleteFramebuffers(1, &zenc_fbo); glDeleteTextures(1, &zenc_tex); }
        if (w > zenc_w) zenc_w = w;
        if (n > zenc_h) zenc_h = n;
        glGenTextures(1, &zenc_tex);
        glBindTexture(GL_TEXTURE_2D, zenc_tex);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, zenc_w, zenc_h);
        glBindTexture(GL_TEXTURE_2D, 0);
        glGenFramebuffers(1, &zenc_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, zenc_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, zenc_tex, 0);
    }
    unsigned char *px = malloc((size_t)w * n * 4);
    if (!px) return;
    glBindFramebuffer(GL_FRAMEBUFFER, zenc_fbo);
    zpass_state(w, n);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glUseProgram(zenc_prog);
    glUniform1i(zenc_u_y0, y0);
    glBindTexture(GL_TEXTURE_2D, z->tex);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindTexture(GL_TEXTURE_2D, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, n, GL_RGBA, GL_UNSIGNED_BYTE, px);
    rb_begin(z->addr + (size_t)y0 * w * 2, (uint32_t)(n * w * 2));
    for (int y = 0; y < n; y++) {
        unsigned char *row = z->addr + (size_t)(y0 + y) * w * 2;
        const unsigned char *s = px + (size_t)y * w * 4;
        for (int x = 0; x < w; x++, s += 4) vbe_st16(row + 2 * x, (unsigned)s[0] << 8 | s[1]);
    }
    rb_end(z->addr + (size_t)y0 * w * 2, (uint32_t)(n * w * 2));
    free(px);
    port_gfx_stats.zresolves++;
    port_gfx_stats.zresolve_px += (unsigned)(w * n);
    {   /* images over these bytes may now be stale (checked when clean) */
        const unsigned char *a = z->addr + (size_t)y0 * w * 2, *e = z->addr + (size_t)y1 * w * 2;
        for (int i = 0; i < MAX_TARGETS; i++) {
            Target *o = &targets[i];
            if (o->addr && o->addr < e && a < o->addr + (size_t)o->width * o->height * bpp_of(o->siz)) o->check_cpu = 1;
        }
        for (int i = 0; i < MAX_ZIMGS; i++) {
            ZImg *o = &zimgs[i];
            if (o != z && o->addr && o->addr < e && a < o->addr + (size_t)o->width * o->height * 2) o->check_cpu = 1;
        }
    }
    if (y0 <= z->dirty_y0 && y1 >= z->dirty_y1) {
        z->dirty_y0 = z->dirty_y1 = 0;
        if (z->synced) z->rdram_hash = hash_words(z->addr, (uint32_t)(z->width * z->height * 2));
    } else if (y0 <= z->dirty_y0) z->dirty_y0 = y1;
    else if (y1 >= z->dirty_y1) z->dirty_y1 = y0;
}

/* RDRAM -> GL (the Z image is clean) */
static void zimg_upload(ZImg *z) {
    int w = z->width, h = z->height;
    float *d = malloc((size_t)w * h * sizeof(float));
    if (!d) return;
    int same = 1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float v = z16_depth(vbe_ld16(z->addr + ((size_t)y * w + x) * 2));
            d[(size_t)y * w + x] = v;
            if (v != d[0]) same = 0;
        }
    flush();
    zpass_init();
    glBindFramebuffer(GL_FRAMEBUFFER, z->fbo);
    zpass_state(w, h);
    glDepthMask(GL_TRUE);
    if (same) {
        glClearDepthf(d[0]);
        glClear(GL_DEPTH_BUFFER_BIT);
    } else {
        if (!zdec_tex) glGenTextures(1, &zdec_tex);
        glBindTexture(GL_TEXTURE_2D, zdec_tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, w, h, 0, GL_RED, GL_FLOAT, d);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_ALWAYS);
        glUseProgram(zdec_prog);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindTexture(GL_TEXTURE_2D, 0);
        glDisable(GL_DEPTH_TEST);
    }
    free(d);
    z->rdram_hash = hash_words(z->addr, (uint32_t)(w * h * 2));
    z->synced = 1;
    z->check_cpu = 0;
    port_gfx_stats.zuploads++;
}

/* before the GPU uses a clean Z image: pick up what other images or the CPU wrote into its bytes */
static void zimg_sync(ZImg *z) {
    if (z->dirty_y1 > z->dirty_y0) return;   /* the GPU copy is newer than RDRAM */
    if (z->synced && !z->check_cpu) return;
    uint32_t bytes = (uint32_t)(z->width * z->height * 2);
    resolve_range_skip(z->addr, bytes, z);
    if (!z->synced || hash_words(z->addr, bytes) != z->rdram_hash) zimg_upload(z);
    z->check_cpu = 0;
}

static ZImg *zimg_get(unsigned char *addr, int width, int height) {
    ZImg *slot = 0, *lru = 0;
    for (int i = 0; i < MAX_ZIMGS; i++) {
        ZImg *z = &zimgs[i];
        if (z->addr == addr && z->width == width && z->height == height) { z->last_use = ++use_clock; return z; }
    }
    for (int i = 0; i < MAX_ZIMGS; i++) {
        ZImg *z = &zimgs[i];
        if (z->addr == addr) zimg_free(z);   /* the same address with another size: written back, replaced */
        if (!z->addr) { if (!slot) slot = z; continue; }
        if (!lru || z->last_use < lru->last_use) lru = z;
    }
    if (!slot) { zimg_free(lru); slot = lru; }
    ZImg *z = slot;
    while (height > 1 && !in_memory(addr, (size_t)width * height * 2)) height--;
    z->addr = addr; z->width = width; z->height = height; z->serial = ++zserial; z->last_use = ++use_clock;
    glGenTextures(1, &z->tex);
    glBindTexture(GL_TEXTURE_2D, z->tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH_COMPONENT24, width, height);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenFramebuffers(1, &z->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, z->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, z->tex, 0);
    {
        GLenum none = GL_NONE;
        glDrawBuffers(1, &none);
        glReadBuffer(GL_NONE);
    }
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        gfx_log_once(0xFA000000 | (unsigned)width, "Z framebuffer incomplete (%dx%d)", width, height);
    state_dirty = 1;
    return z;
}

/* a FILL rectangle over the Z image's rows [y0, y1), columns [x0, x1): its depth texture is cleared */
static void zimg_clear(ZImg *z, int x0, int y0, int x1, int y1, float depth) {
    zimg_sync(z);   /* rows outside the rectangle must hold RDRAM's values when the image becomes dirty */
    flush();
    glBindFramebuffer(GL_FRAMEBUFFER, z->fbo);
    glEnable(GL_SCISSOR_TEST);
    glScissor(x0, y0, x1 - x0, y1 - y0);
    glDepthMask(GL_TRUE);
    glClearDepthf(depth);
    glClear(GL_DEPTH_BUFFER_BIT);
    state_dirty = 1;
    if (z->dirty_y1 <= z->dirty_y0) { z->dirty_y0 = y0; z->dirty_y1 = y1; }
    else { if (y0 < z->dirty_y0) z->dirty_y0 = y0; if (y1 > z->dirty_y1) z->dirty_y1 = y1; }
    port_gfx_stats.zclears++;
}

/* a FILL rectangle into colour image rows [y0, y1) that are (wholly or partly) Z images of the same width: those
 * rows are cleared in the Z images. Returns 1 when every row of the rectangle was a Z image's (no colour to draw). */
static int fill_zimgs(int x0, int y0, int x1, int y1) {
    int stride = (int)cimg.width * 2, n = y1 - y0, left = n;
    static unsigned char cov[1024];
    if (n <= 0 || n > 1024) return 0;
    memset(cov, 0, (size_t)n);
    float depth = z16_depth(fill_color >> 16);
    for (int i = 0; i < MAX_ZIMGS; i++) {
        ZImg *z = &zimgs[i];
        if (!z->addr || z->width != (int)cimg.width) continue;
        ptrdiff_t off = z->addr - cimg.addr;
        if (off % stride) continue;
        int d = (int)(off / stride), a = y0 > d ? y0 : d, b = y1 < d + z->height ? y1 : d + z->height;
        if (a >= b) continue;
        zimg_clear(z, x0, a - d, x1, b - d, depth);
        for (int r = a; r < b; r++) if (!cov[r - y0]) { cov[r - y0] = 1; left--; }
    }
    return left == 0;
}

/* per-batch state: what the current batch was set up with */
static struct { int kind, tex0_tile, tex1_tile; uint32_t geom; int tex_on; } batch_key;
static float cur_tex_w[2] = {1, 1}, cur_tex_h[2] = {1, 1};
static ZImg *cur_zw;   /* the Z image the current batch writes (0 none) */
static int sc_row0, sc_row1;   /* scissor rows of the current batch (clamped to the target) */

/* rows [y0, y1) (N64 pixels, clamped to the scissor) are drawn by the GPU: they are dirty until written back */
static void mark_rows(float fy0, float fy1) {
    Target *t = cur_target;
    int y0 = (int)floorf(fy0), y1 = (int)ceilf(fy1);
    if (y0 < sc_row0) y0 = sc_row0;
    if (y1 > sc_row1) y1 = sc_row1;
    if (y1 <= y0) return;
    if (t->dirty_y1 <= t->dirty_y0) { t->dirty_y0 = y0; t->dirty_y1 = y1; }
    else { if (y0 < t->dirty_y0) t->dirty_y0 = y0; if (y1 > t->dirty_y1) t->dirty_y1 = y1; }
    ZImg *z = cur_zw;
    if (z) {
        if (y1 > z->height) y1 = z->height;
        if (y1 <= y0) return;
        if (z->dirty_y1 <= z->dirty_y0) { z->dirty_y0 = y0; z->dirty_y1 = y1; }
        else { if (y0 < z->dirty_y0) z->dirty_y0 = y0; if (y1 > z->dirty_y1) z->dirty_y1 = y1; }
    }
}

/* kind: 0 triangles, 1 rectangle (1/2-cycle), 2 copy rect, 3 fill rect (1/2-cycle draws use 1) */
static int begin_batch(int kind, int tile) {
    if (gl_ok != 1 || !cimg.addr) return 0;
    uint32_t geom = gfx_geom & (GM_ZBUFFER);
    int t0 = kind == 0 ? gfx_tex_tile : tile;
    if (!state_dirty && batch_key.kind == kind && batch_key.tex0_tile == t0 && batch_key.geom == geom &&
        batch_key.tex_on == gfx_tex_on && batch_n < MAX_BATCH_VERTS - 6)
        return 1;
    flush();
    state_dirty = 0;
    batch_key.kind = kind; batch_key.tex0_tile = t0; batch_key.geom = geom; batch_key.tex_on = gfx_tex_on;
    unsigned cyc = (om_h >> OMH_CYCLETYPE_SFT) & 3;
    /* target */
    int need_h = (int)ceilf(sc_y1);
    Target *t = target_get(cimg.addr, (int)cimg.width, (int)cimg.siz, need_h);
    if (t->dirty_y1 <= t->dirty_y0 && t->check_cpu) target_sync_cpu(t);
    if (t != cur_target) { cur_target = t; }
    /* depth: the Z image (a depth texture of the target's size) is brought up to date and attached */
    int zs_prim = (om_l & OML_ZS_PRIM) != 0;
    int ztest = cyc < CYC_COPY && (om_l & OML_Z_CMP) && zimg && (kind == 0 ? (gfx_geom & GM_ZBUFFER) != 0 : zs_prim);
    int zwrite = cyc < CYC_COPY && (om_l & OML_Z_UPD) && zimg && (kind == 0 ? (gfx_geom & GM_ZBUFFER) != 0 : zs_prim);
    ZImg *z = 0;
    if (ztest || zwrite) {
        z = zimg_get(zimg, t->width, t->height);
        zimg_sync(z);
        if (t->shown) z->deferred = 1;
    }
    cur_zw = zwrite ? z : 0;
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    if (z && t->zatt != z->serial) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, z->tex, 0);
        t->zatt = z->serial;
    }
    glViewport(0, 0, t->width, t->height);
    int x0 = (int)floorf(sc_x0), y0 = (int)floorf(sc_y0), x1 = (int)ceilf(sc_x1), y1 = (int)ceilf(sc_y1);
    if (x1 > t->width) x1 = t->width;
    if (y1 > t->height) y1 = t->height;
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    glEnable(GL_SCISSOR_TEST);
    glScissor(x0, y0, x1 - x0, y1 - y0);
    sc_row0 = y0; sc_row1 = y1;
    if (ztest || zwrite) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(ztest ? GL_LEQUAL : GL_ALWAYS);
    } else glDisable(GL_DEPTH_TEST);
    glDepthMask(zwrite ? GL_TRUE : GL_FALSE);
    if (OML_ZMODE(om_l) == ZMODE_DECAL_SEL && ztest) { glEnable(GL_POLYGON_OFFSET_FILL); glPolygonOffset(-1, -2); }
    else glDisable(GL_POLYGON_OFFSET_FILL);
    /* program */
    uint32_t flags = cyc, w0 = cc_w0, w1 = cc_w1, oml = om_l & 0xFFFF0000u;
    if (cyc == CYC_COPY) { w0 = w1 = 0; oml = 0; flags |= PF_TEX0; }
    else if (cyc == CYC_FILL) { w0 = w1 = 0; oml = 0; }
    else {
        int use_tex = kind != 0 || gfx_tex_on;
        if (use_tex && cc_uses_tex(cc_w0, cc_w1, (int)cyc, 1)) flags |= PF_TEX0;
        if (use_tex && cc_uses_tex(cc_w0, cc_w1, (int)cyc, 2)) flags |= PF_TEX1;
        if (om_l & OML_FORCE_BL) flags |= PF_FORCE_BL;
        if (zs_prim && ztest) flags |= PF_ZS_PRIM;
        if (kind != 0) flags |= PF_RECT;
        if ((om_l & OML_CVG_X_ALPHA) && (om_l & OML_ALPHA_CVG_SEL) && !(om_l & OML_FORCE_BL)) flags |= PF_TEXEDGE;
    }
    flags |= (om_l & OML_ALPHACOMPARE_MASK) << PF_ACMP_SFT;
    Prog *p = prog_get(w0, w1, oml, flags);
    if (p != cur_prog) glUseProgram(p->prog);
    cur_prog = p;
    set_rgba(p->u_prim, prim_color); set_rgba(p->u_env, env_color); set_rgba(p->u_fog, fog_color);
    set_rgba(p->u_blend, blend_color);
    if (cimg.siz == SIZ_32b) set_rgba(p->u_fill, fill_color);
    else {
        float c[4];
        unsigned f = fill_color >> 16;
        c[0] = (f >> 11) / 31.0f; c[1] = ((f >> 6) & 31) / 31.0f; c[2] = ((f >> 1) & 31) / 31.0f; c[3] = f & 1;
        glUniform4f(p->u_fill, c[0], c[1], c[2], c[3]);
    }
    float zp = (prim_depth >> 16 & 0x7FFF) / 32767.0f;
    glUniform4f(p->u_misc, (conv_k[4] & 0x1FF) / 255.0f, (conv_k[5] & 0x1FF) / 255.0f, prim_lod_frac / 255.0f, zp);
    /* blending: the last blender cycle reads memory and FORCE_BL */
    unsigned last = cyc == CYC_2 ? 1 : 0;
    unsigned bp = (om_l >> (30 - 2 * last)) & 3, bm = (om_l >> (22 - 2 * last)) & 3, bb = (om_l >> (18 - 2 * last)) & 3;
    if (cyc < CYC_COPY && (om_l & OML_FORCE_BL) && bm == 1 && bp != 1) {
        static const GLenum dst[4] = {GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE, GL_ZERO};
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, dst[bb], GL_ZERO, GL_ONE);
    } else glDisable(GL_BLEND);
    /* textures */
    for (int u = 0; u < 2; u++) {
        if (!(flags & (u ? PF_TEX1 : PF_TEX0))) continue;
        int w, h;
        int ti = u ? (t0 + 1) & 7 : t0;
        glActiveTexture(GL_TEXTURE0 + u);   /* before tile_texture: a new texture is created on the active unit */
        GLuint tex = tile_texture(ti, &w, &h);
        glBindTexture(GL_TEXTURE_2D, tex);
        {
            int dw, dh;
            GLint ws, wt;
            tile_dims(&tiles[ti & 7], &dw, &dh, &ws, &wt);
        }
        unsigned filt = (om_h >> OMH_TEXTFILT_SFT) & 3;
        GLint f = (cyc == CYC_COPY || filt == 0) ? GL_NEAREST : GL_LINEAR;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
        cur_tex_w[u] = (float)w; cur_tex_h[u] = (float)h;
    }
    glActiveTexture(GL_TEXTURE0);
    return 1;
}

static float shift_scale(unsigned sh) { return sh == 0 ? 1.0f : sh <= 10 ? 1.0f / (float)(1 << sh) : (float)(1 << (16 - sh)); }
static void tile_uv(int ti, int u, float s, float t, float *ou, float *ov) {
    const Tile *tl = &tiles[ti & 7];
    *ou = (s * shift_scale(tl->shifts) - tl->uls / 4.0f) / cur_tex_w[u];
    *ov = (t * shift_scale(tl->shiftt) - tl->ult / 4.0f) / cur_tex_h[u];
}
static void put_vtx(float x, float y, float z, float w, const float *col, float s, float t) {
    float *o = batch + (size_t)batch_n * VFLOATS;
    float W = (float)cur_target->width, H = (float)cur_target->height;
    o[0] = x * (2.0f / W) - w;
    o[1] = y * (2.0f / H) - w;
    o[2] = z;
    o[3] = w;
    o[4] = col[0]; o[5] = col[1]; o[6] = col[2]; o[7] = col[3];
    int t0 = batch_key.tex0_tile;
    tile_uv(t0, 0, s, t, &o[8], &o[9]);
    tile_uv((t0 + 1) & 7, 1, s, t, &o[10], &o[11]);
    batch_n++;
}

/* debug (port_gfx_set_probe): while commands are logged, every primitive covering the probe pixel (N64 pixels of the
 * current colour image) is logged with the state it is drawn with */
static float probe_x = -1, probe_y = -1;
void port_gfx_set_probe(int x, int y) { probe_x = (float)x + 0.5f; probe_y = (float)y + 0.5f; }
static void probe_state(void) {
    for (int i = 0; i < 8; i++) {
        const Tile *tl = &tiles[i];
        port_log("[probe]   tile %d fmt %u siz %u line %u tmem %u pal %u cms %u masks %u shifts %u cmt %u maskt %u shiftt %u "
                 "(%.2f,%.2f)-(%.2f,%.2f)\n", i, tl->fmt, tl->siz, tl->line, tl->tmem, tl->palette, tl->cms, tl->masks,
                 tl->shifts, tl->cmt, tl->maskt, tl->shiftt, tl->uls / 4.0, tl->ult / 4.0, tl->lrs / 4.0, tl->lrt / 4.0);
    }
    port_log("[probe]   omh %08X oml %08X cc %08X %08X prim %08X env %08X fog %08X geom %08X tex on %d tile %d lvl %d\n",
             om_h, om_l, cc_w0, cc_w1, prim_color, env_color, fog_color, gfx_geom, gfx_tex_on, gfx_tex_tile, gfx_tex_level);
}
static void probe_tri(const GfxVtx *v[3]) {
    if (probe_x < 0 || !gfx_log_ops) return;
    /* homogeneous 2D rasterisation (no clipping needed): e_i = (v_j x v_k) . (px, py, 1), inside when all e_i have
     * the sign of det = v_0 . (v_1 x v_2) */
    double V[3][3], e[3], det;
    for (int i = 0; i < 3; i++) { V[i][0] = v[i]->x; V[i][1] = v[i]->y; V[i][2] = v[i]->w; }
    for (int i = 0; i < 3; i++) {
        const double *a = V[(i + 1) % 3], *b = V[(i + 2) % 3];
        double cx = a[1] * b[2] - a[2] * b[1], cy = a[2] * b[0] - a[0] * b[2], cw = a[0] * b[1] - a[1] * b[0];
        e[i] = cx * probe_x + cy * probe_y + cw;
        if (i == 0) det = cx * V[0][0] + cy * V[0][1] + cw * V[0][2];
    }
    if (det == 0) return;
    for (int i = 0; i < 3; i++) if (e[i] / det < 0) return;
    port_log("[probe] tri hit (cimg %p w %u)\n", (void *)cimg.addr, cimg.width);
    for (int i = 0; i < 3; i++)
        port_log("[probe]   v%d scr %.2f %.2f z/w %.6f w %.2f rgba %.3f %.3f %.3f %.3f st %.2f %.2f\n", i, v[i]->x / v[i]->w,
                 v[i]->y / v[i]->w, v[i]->z / v[i]->w, v[i]->w, v[i]->r, v[i]->g, v[i]->b, v[i]->a, v[i]->s, v[i]->t);
    probe_state();
}
static void probe_rect(float x0, float y0, float x1, float y1, float s, float t, float dsdx, float dtdy) {
    if (probe_x < 0 || !gfx_log_ops || probe_x < x0 || probe_x >= x1 || probe_y < y0 || probe_y >= y1) return;
    port_log("[probe] rect hit (cimg %p w %u) (%.2f,%.2f)-(%.2f,%.2f) st %.2f %.2f d %.4f %.4f\n", (void *)cimg.addr,
             cimg.width, x0, y0, x1, y1, s, t, dsdx, dtdy);
    probe_state();
}

void rdp_tri(const GfxVtx *a, const GfxVtx *b, const GfxVtx *c) {
    if (!begin_batch(0, 0)) return;
    { const GfxVtx *pv[3] = {a, b, c}; probe_tri(pv); }
    const GfxVtx *v[3] = {a, b, c};
    for (int i = 0; i < 3; i++) put_vtx(v[i]->x, v[i]->y, v[i]->z, v[i]->w, &v[i]->r, v[i]->s, v[i]->t);
    if (a->w > 0 && b->w > 0 && c->w > 0) {
        float ya = a->y / a->w, yb = b->y / b->w, yc = c->y / c->w;
        mark_rows(fminf(ya, fminf(yb, yc)), fmaxf(ya, fmaxf(yb, yc)));
    } else mark_rows(-1e9f, 1e9f);
}
void rdp_line(const GfxVtx *a, const GfxVtx *b, float width) {
    if (a->w <= 0 || b->w <= 0) return;
    if (!begin_batch(0, 0)) return;
    float ax = a->x / a->w, ay = a->y / a->w, bx = b->x / b->w, by = b->y / b->w;
    float dx = bx - ax, dy = by - ay, l = sqrtf(dx * dx + dy * dy);
    if (l <= 0) return;
    float nx = -dy / l * width * 0.5f, ny = dx / l * width * 0.5f;
    float az = a->z / a->w, bz = b->z / b->w;
    put_vtx(ax - nx, ay - ny, az, 1, &a->r, a->s, a->t);
    put_vtx(ax + nx, ay + ny, az, 1, &a->r, a->s, a->t);
    put_vtx(bx + nx, by + ny, bz, 1, &b->r, b->s, b->t);
    put_vtx(ax - nx, ay - ny, az, 1, &a->r, a->s, a->t);
    put_vtx(bx + nx, by + ny, bz, 1, &b->r, b->s, b->t);
    put_vtx(bx - nx, by - ny, bz, 1, &b->r, b->s, b->t);
    mark_rows(fminf(ay, by) - width, fmaxf(ay, by) + width);
}

static void quad(float x0, float y0, float x1, float y1, float s0, float t0, float s1, float t1, int flip) {
    static const float black[4] = {0, 0, 0, 0};
    float z = ((prim_depth >> 16 & 0x7FFF) / 32767.0f) * 2.0f - 1.0f;
    /* corners: (x0,y0) (x1,y0) (x1,y1) (x0,y1); flip swaps the s/t directions */
    float cs[4], ct[4];
    if (!flip) { cs[0] = s0; ct[0] = t0; cs[1] = s1; ct[1] = t0; cs[2] = s1; ct[2] = t1; cs[3] = s0; ct[3] = t1; }
    else { cs[0] = s0; ct[0] = t0; cs[1] = s0; ct[1] = t1; cs[2] = s1; ct[2] = t1; cs[3] = s1; ct[3] = t0; }
    float X[4] = {x0, x1, x1, x0}, Y[4] = {y0, y0, y1, y1};
    static const int idx[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; i++) put_vtx(X[idx[i]], Y[idx[i]], z, 1, black, cs[idx[i]], ct[idx[i]]);
    mark_rows(y0, y1);
}

void rdp_texrect(uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3, int flip) {
    port_gfx_stats.rects++;
    unsigned cyc = (om_h >> OMH_CYCLETYPE_SFT) & 3;
    float lrx = ((w0 >> 12) & 0xFFF) / 4.0f, lry = (w0 & 0xFFF) / 4.0f;
    int tile = (w1 >> 24) & 7;
    float ulx = ((w1 >> 12) & 0xFFF) / 4.0f, uly = (w1 & 0xFFF) / 4.0f;
    float s = (int16_t)(w2 >> 16) / 32.0f, t = (int16_t)w2 / 32.0f;
    float dsdx = (int16_t)(w3 >> 16) / 1024.0f, dtdy = (int16_t)w3 / 1024.0f;
    if (cyc == CYC_COPY) { dsdx /= 4.0f; lrx = floorf(lrx) + 1; lry = floorf(lry) + 1; }
    if (cyc == CYC_FILL) return;
    if (lrx <= ulx || lry <= uly) return;
    if (!begin_batch(cyc == CYC_COPY ? 2 : 1, tile)) return;
    probe_rect(ulx, uly, lrx, lry, s, t, dsdx, dtdy);
    float s1, t1;
    if (!flip) { s1 = s + dsdx * (lrx - ulx); t1 = t + dtdy * (lry - uly); }
    else { s1 = s + dsdx * (lry - uly); t1 = t + dtdy * (lrx - ulx); }
    quad(ulx, uly, lrx, lry, s, t, s1, t1, flip);
}

static void fill_rect(uint32_t w0, uint32_t w1) {
    port_gfx_stats.fills++;
    unsigned cyc = (om_h >> OMH_CYCLETYPE_SFT) & 3;
    float lrx = ((w0 >> 12) & 0xFFF) / 4.0f, lry = (w0 & 0xFFF) / 4.0f;
    float ulx = ((w1 >> 12) & 0xFFF) / 4.0f, uly = (w1 & 0xFFF) / 4.0f;
    if (cyc >= CYC_COPY) { lrx = floorf(lrx) + 1; lry = floorf(lry) + 1; }
    if (lrx <= ulx || lry <= uly) return;
    /* clip to the scissor */
    float x0 = ulx > sc_x0 ? ulx : sc_x0, y0 = uly > sc_y0 ? uly : sc_y0, x1 = lrx < sc_x1 ? lrx : sc_x1, y1 = lry < sc_y1 ? lry : sc_y1;
    if (x1 <= x0 || y1 <= y0) return;
    /* a fill into Z images' rows (render.c and score.c clear Z buffers as 16-bit colour images): their depth
     * textures are cleared; rows that belong to no known Z image are drawn as colour */
    if (cyc == CYC_FILL && cimg.addr && cimg.siz == SIZ_16b && gl_ok == 1 &&
        fill_zimgs((int)floorf(x0), (int)floorf(y0), (int)ceilf(x1), (int)ceilf(y1)))
        return;
    if (!begin_batch(cyc == CYC_FILL ? 3 : 1, 0)) return;
    quad(x0, y0, x1, y1, 0, 0, 0, 0, 0);
}

/* ------------------------------------------------------------------ RDP commands */
static void rdp_end_task_gl(void);
void rdp_reset_task(void) {
    state_dirty = 1;
    cur_target = 0;
    cur_zw = 0;
}
void rdp_end_task(void) {
    rdp_end_task_gl();
    rb_point(1);
}
static void rdp_end_task_gl(void) {
    if (gl_ok != 1) return;
    flush();
    /* colour images that are not framebuffers (never shown by the VI) are written back at the end of every task:
     * the game reads them with the CPU or copies them without invalidating first (detector regions, score images,
     * and the 320x210 photo renders of window/847B60.c that the CPU averages into album / camera-check sprites) */
    for (int i = 0; i < MAX_TARGETS; i++) {
        Target *t = &targets[i];
        if (t->addr && !t->shown) target_resolve(t, 0, t->height);
    }
    /* Z images too (score.c counts Pokemon pixels in its Z buffers with the CPU), except those of framebuffers */
    for (int i = 0; i < MAX_ZIMGS; i++) {
        ZImg *z = &zimgs[i];
        if (z->addr && !z->deferred) zimg_resolve(z, 0, z->height);
    }
}

void rdp_cmd(uint32_t w0, uint32_t w1, const uint32_t *next) {
    unsigned op = w0 >> 24;
    (void)next;
    switch (op) {
    case 0xE2: case 0xE3: {   /* G_SETOTHERMODE_L / _H: 32 - shift - len, len - 1 */
        unsigned len = (w0 & 0xFF) + 1, sft = 32 - ((w0 >> 8) & 0xFF) - len;
        uint32_t mask = (len >= 32 ? 0xFFFFFFFFu : ((1u << len) - 1)) << sft;
        if (op == 0xE2) om_l = (om_l & ~mask) | (w1 & mask); else om_h = (om_h & ~mask) | (w1 & mask);
        state_dirty = 1;
        break;
    }
    case 0xE6: case 0xE7: case 0xE8: break;   /* load / pipe / tile sync */
    case 0xE9: gfx_saw_fullsync = 1; break;
    case 0xEA: case 0xEB: break;               /* key GB / R: chroma key not emulated */
    case 0xEC:                                 /* G_SETCONVERT */
        conv_k[0] = (int)((w0 >> 13) & 0x1FF); conv_k[1] = (int)((w0 >> 4) & 0x1FF);
        conv_k[2] = (int)((w0 & 15) << 5 | w1 >> 27); conv_k[3] = (int)((w1 >> 18) & 0x1FF);
        conv_k[4] = (int)((w1 >> 9) & 0x1FF); conv_k[5] = (int)(w1 & 0x1FF);
        for (int i = 0; i < 6; i++) if (conv_k[i] & 0x100) conv_k[i] -= 0x200;
        tmem_gen++;   /* YUV decodes depend on it */
        state_dirty = 1;
        break;
    case 0xED:   /* G_SETSCISSOR (10.2) */
        sc_x0 = ((w0 >> 12) & 0xFFF) / 4.0f; sc_y0 = (w0 & 0xFFF) / 4.0f;
        sc_x1 = ((w1 >> 12) & 0xFFF) / 4.0f; sc_y1 = (w1 & 0xFFF) / 4.0f;
        state_dirty = 1;
        break;
    case 0xEE: prim_depth = w1; state_dirty = 1; break;
    case 0xEF: om_h = (om_h & 0xFF000000u) | (w0 & 0x00FFFFFFu); om_l = w1; state_dirty = 1; break;
    case 0xF0: load_tlut(w0, w1); state_dirty = 1; break;
    case 0xF2: {
        Tile *t = &tiles[(w1 >> 24) & 7];
        t->uls = (w0 >> 12) & 0xFFF; t->ult = w0 & 0xFFF; t->lrs = (w1 >> 12) & 0xFFF; t->lrt = w1 & 0xFFF;
        state_dirty = 1;
        break;
    }
    case 0xF3: load_block(w0, w1); state_dirty = 1; break;
    case 0xF4: load_tile(w0, w1); state_dirty = 1; break;
    case 0xF5: {
        Tile *t = &tiles[(w1 >> 24) & 7];
        t->fmt = (w0 >> 21) & 7; t->siz = (w0 >> 19) & 3; t->line = (w0 >> 9) & 0x1FF; t->tmem = w0 & 0x1FF;
        t->palette = (w1 >> 20) & 15; t->cmt = (w1 >> 18) & 3; t->maskt = (w1 >> 14) & 15; t->shiftt = (w1 >> 10) & 15;
        t->cms = (w1 >> 8) & 3; t->masks = (w1 >> 4) & 15; t->shifts = w1 & 15;
        state_dirty = 1;
        break;
    }
    case 0xF6: fill_rect(w0, w1); break;
    case 0xF7: fill_color = w1; state_dirty = 1; break;
    case 0xF8: fog_color = w1; state_dirty = 1; break;
    case 0xF9: blend_color = w1; state_dirty = 1; break;
    case 0xFA: prim_color = w1; prim_lod_frac = w0 & 0xFF; state_dirty = 1; break;
    case 0xFB: env_color = w1; state_dirty = 1; break;
    case 0xFC: cc_w0 = w0 & 0xFFFFFF; cc_w1 = w1; state_dirty = 1; break;
    case 0xFD: timg.fmt = (w0 >> 21) & 7; timg.siz = (w0 >> 19) & 3; timg.width = (w0 & 0xFFF) + 1; timg.addr = gfx_addr(w1); break;
    case 0xFE: zimg = gfx_addr(w1); state_dirty = 1; break;
    case 0xFF:
        flush();
        cimg.fmt = (w0 >> 21) & 7; cimg.siz = (w0 >> 19) & 3; cimg.width = (w0 & 0xFFF) + 1; cimg.addr = gfx_addr(w1);
        state_dirty = 1;
        break;
    default: port_gfx_stats.unknown_ops++; gfx_log_once(op << 24, "unknown RDP opcode 0x%02X (0x%08X 0x%08X)", op, w0, w1);
    }
}
int rdp_target_width(void) { return cur_target ? cur_target->width : 320; }
int rdp_target_height(void) { return cur_target ? cur_target->height : 240; }

/* ------------------------------------------------------------------ display (VI) */
static void vi_frame(const void *fb, int width);
void port_gfx_vi_frame(const void *fb, int width) {
    vi_frame(fb, width);
    rb_point(3);
}
static void vi_frame(const void *fb, int width) {
    if (port_gfx_init() < 0) return;
    flush();
    unsigned char *p = fb ? gfx_addr((uint32_t)(uintptr_t)fb) : 0;
    if (!p) {   /* no framebuffer yet, or the VI is blanked (osViBlack): a black screen */
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        port_gfx_stats.presents++;
        state_dirty = 1;
        return;
    }
    if (width <= 0) width = 320;
    Target *t = target_get(p, width, SIZ_16b, width == 640 ? 480 : 240);
    if (t->dirty_y1 <= t->dirty_y0) target_sync_cpu(t);   /* clean: RDRAM is authoritative (CPU-drawn frames) */
    t->shown = 1;
    int cw = 0, ch = 0;
    emscripten_webgl_get_drawing_buffer_size(gl_ctx, &cw, &ch);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    int sh = width == 640 ? 480 : 240;
    if (sh > t->height) sh = t->height;
    float k = fminf((float)cw / 4.0f, (float)ch / 3.0f);
    int dw = (int)(k * 4), dh = (int)(k * 3), dx = (cw - dw) / 2, dy = (ch - dh) / 2;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, t->fbo);
    /* N64 row 0 is GL row 0 in the target: flip onto the canvas */
    glBlitFramebuffer(0, 0, t->width, sh, dx, dy + dh, dx + dw, dy, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    port_gfx_stats.presents++;
    state_dirty = 1;
}
