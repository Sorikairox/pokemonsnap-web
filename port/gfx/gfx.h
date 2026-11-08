/* port/gfx: F3DEX2 + L3DEX2 display-list interpreter on WebGL2 (session S3b). Design: docs/port/RENDERER.md.
 * Written for this port from the GBI headers (ultralib/include/PR/gbi.h) and public N64 documentation; no
 * third-party code (port/gfx/LICENSES.md).
 *
 * Memory: game memory is virtual big-endian (docs/port/MEMORY.md): display-list / matrix / vertex words are plain
 * 32-bit loads, bytes are at a ^ 3, halfwords at a ^ 2 (pc_mem.h accessors).
 *
 * Interface with the PC layer (S3a: port/ultra, port/boot):
 *   port_gfx_task(t)          osSpTaskStart of an M_GFXTASK: runs the whole display list synchronously, then calls
 *                             port_sp_task_done() and, if the list reached G_FULLSYNC, port_dp_full_sync() (both
 *                             provided by the PC layer, weak here so the renderer links and tests alone).
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

/* provided by the PC layer (S3a); weak references in the renderer */
void port_sp_task_done(void);
void port_dp_full_sync(void);

/* tools / tests */
void port_gfx_run_dl(const void *dl, int ucode_l3dex2);  /* interpret one list (no SP/DP events) */
void port_gfx_flush(void);                                /* submit pending draws */
void port_gfx_resolve_all(void);                          /* every GPU-drawn colour image -> RDRAM */
void port_gfx_set_log_ops(int on);                        /* log every command (debug) */

typedef struct {
    unsigned tasks, dls, cmds, tris, lines, rects, fills, draws, verts, tex_decodes, tex_hits, unknown_ops;
    unsigned resolves, resolve_px, uploads, upload_px, ucode_loads, bad_addr, culled, presents;
} PortGfxStats;
extern PortGfxStats port_gfx_stats;

#endif
