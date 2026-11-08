/* port/gfx glue to the PC layer (session S3c): the VI hook of port/ultra/pc_rcp.c -> the renderer's display.
 * pc_rcp.c calls port_vi_present() at every retrace with the framebuffer the VI shows (NULL before the first
 * osViSwapBuffer), the game's OSViMode (game memory, ultralib PR/os_vi.h: u8 type; OSViCommonRegs comRegs = { ctrl
 * at 0x04, width at 0x08, ... }) and the osViBlack state. Words of game memory are native 32-bit loads (vbe layout). */
#include "gfx_internal.h"

void port_vi_present(void *framebuffer, const void *vi_mode, int black) {
    int width = 320;
    if (vi_mode) {
        uint32_t w = ((const uint32_t *)vi_mode)[2];   /* comRegs.width: framebuffer width in pixels */
        if (w >= 16 && w <= 1024) width = (int)w;
    }
    port_gfx_vi_frame(black ? 0 : framebuffer, width);
}

/* the renderer's counters since the last call (exported for the test page, port/web/worker.js) */
void port_gfx_log_stats(void) {
    static PortGfxStats last;
    PortGfxStats s = port_gfx_stats, d;
    unsigned *a = (unsigned *)&s, *b = (unsigned *)&last, *o = (unsigned *)&d;
    for (unsigned i = 0; i < sizeof s / sizeof(unsigned); i++) o[i] = a[i] - b[i];
    last = s;
    port_log("[gfxstat] tasks %u cmds %u tris %u lines %u rects %u fills %u draws %u verts %u culled %u tex %u/%u hits; "
             "resolves %u (%u px) uploads %u (%u px) z resolves %u (%u px) z uploads %u z clears %u clipped %u ucode %u; unknown %u "
             "bad addr %u; presents %u\n",
             d.tasks, d.cmds, d.tris, d.lines, d.rects, d.fills, d.draws, d.verts, d.culled, d.tex_decodes, d.tex_hits,
             d.resolves, d.resolve_px, d.uploads, d.upload_px, d.zresolves, d.zresolve_px, d.zuploads, d.zclears, d.clipped, d.ucode_loads, d.unknown_ops, d.bad_addr, d.presents);
}
