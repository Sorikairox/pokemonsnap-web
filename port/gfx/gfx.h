/* port/gfx: F3DEX2 + L3DEX2 display-list interpreter on WebGL2 (session S3b). Design: docs/port/RENDERER.md.
 * Written for this port from the GBI headers (ultralib/include/PR/gbi.h) and public N64 documentation; no
 * third-party code (port/gfx/LICENSES.md).
 *
 * Memory: game memory is virtual big-endian (docs/port/MEMORY.md): display-list / matrix / vertex words are plain
 * 32-bit loads, bytes are at a ^ 3, halfwords at a ^ 2 (pc_mem.h accessors).
 *
 * Interface with the PC layer (S3a: port/ultra, port/boot):
 *   port_gfx_task(t)          osSpTaskStart of an M_GFXTASK: runs the whole display list synchronously; the caller
 *                             (pc_rcp.c osSpTaskStartGo) then posts SP done and DP full sync (S3c: the PC layer
 *                             owns the events; port_gfx_stats.fullsyncs counts lists that reached G_FULLSYNC).
 *   port_gfx_vi_frame(fb, w)  once per VI retrace (or osViSwapBuffer): show colour image fb (any game pointer:
 *                             KSEG0, physical or wasm) of width w on the canvas.
 *   port_gfx_inval_dcache(p, n)    osInvalDCache(p, n): the CPU will read [p, p+n): GPU-drawn colour images in it are
 *                             resolved into RDRAM as the console's pixels (RGBA16/32, native resolution).
 *   port_gfx_writeback_dcache(p, n) osWritebackDCache(p, n) (n = 0: osWritebackDCacheAll): the CPU may have written
 *                             colour images in the range: they are re-uploaded before the next draw / display.
 *   port_gfx_init()           optional: creates the WebGL2 context on Module.canvas (the host's OffscreenCanvas, or
 *                             the page's canvas) now; otherwise done on first use. 0 = ok, < 0 = no GL (headless:
 *                             display lists are still interpreted, nothing is drawn). */
#ifndef PORT_GFX_H
#define PORT_GFX_H
#include <stdint.h>

void port_gfx_task(void *ostask);
void port_gfx_vi_frame(const void *fb, int width);
void port_gfx_inval_dcache(const void *p, uint32_t len);
void port_gfx_writeback_dcache(const void *p, uint32_t len);
int port_gfx_init(void);

/* tools / tests */
void port_gfx_run_dl(const void *dl, int ucode_l3dex2);  /* interpret one list (no SP/DP events) */
void port_gfx_flush(void);                                /* submit pending draws */
void port_gfx_resolve_all(void);                          /* every GPU-drawn colour image -> RDRAM */
void port_gfx_set_log_ops(int on);                        /* log every command (debug) */
void port_gfx_set_probe(int x, int y);                    /* with log_ops: log primitives covering N64 pixel (x, y) */
void port_gfx_log_stats(void);                            /* log the counters since the last call (gfx_port.c) */

typedef struct {
    unsigned tasks, dls, cmds, tris, lines, rects, fills, draws, verts, tex_decodes, tex_hits, unknown_ops;
    unsigned resolves, resolve_px, uploads, upload_px, ucode_loads, bad_addr, culled, presents, fullsyncs;
    unsigned zresolves, zresolve_px, zuploads, zclears;   /* Z images (S6) */
    unsigned clipped;                                    /* triangles clipped on the CPU (R1: NoN side planes) */
} PortGfxStats;
extern PortGfxStats port_gfx_stats;

#endif
