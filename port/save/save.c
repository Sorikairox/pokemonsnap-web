/* PC layer: the save's host interface (session S8, docs/port/SAVE.md).
 *
 * The save is the cartridge's 128 KiB FlashRAM image in console byte order (flashram.c). Hosts:
 *   - retrowser (guide section 3, standard 4.2): rtw_save_ptr / rtw_save_size / rtw_save_generation; the host copies
 *     the stored image to rtw_save_ptr before the game boots and persists it whenever the generation changes;
 *   - the test harnesses (port/tools/node_run.js --save, port/web/run.mjs --save): port_save_load(ptr, size) before
 *     main(), then port_flash_image() / port_flash_generation() to dump it.
 * The game reads the whole flash once at boot (more_funcs/5BF20.c, func_800C05D4_5D474), so an image loaded after
 * boot is only seen at the next boot. */
#include "pc_mem.h"
#include "save.h"
#include <emscripten.h>
#include <string.h>

EMSCRIPTEN_KEEPALIVE unsigned char *port_flash_image(void) { return port_flash_data(); }
EMSCRIPTEN_KEEPALIVE unsigned port_flash_generation(void) { return port_flash_gen; }

/* copy a stored image in (a shorter one is padded with 0xFF, the erased state); 0 = OK */
EMSCRIPTEN_KEEPALIVE int port_save_load(const unsigned char *p, unsigned n) {
    unsigned char *d = port_flash_data();
    if (n > PORT_FLASH_SIZE) n = PORT_FLASH_SIZE;
    memcpy(d, p, n);
    memset(d + n, 0xFF, PORT_FLASH_SIZE - n);
    port_log("[save] flash image loaded by the host (%u bytes)\n", n);
    return 0;
}

/* once per retrace from the harnesses (main.c's loop, port/web/worker.js): 1 when the image changed since the last
 * call (the harness then dumps it), with a deterministic log line */
EMSCRIPTEN_KEEPALIVE int port_save_poll(void) {
    static unsigned seen, erases, programs;
    if (port_flash_gen == seen) return 0;
    port_log("[save] retrace %u: flash generation %u (%u sector/chip erases, %u page programs since the last change)\n",
             port_vi_count, port_flash_gen, port_flash_erases - erases, port_flash_programs - programs);
    seen = port_flash_gen;
    erases = port_flash_erases;
    programs = port_flash_programs;
    return 1;
}

/* retrowser save exports (guide section 3): the native image, its size, the generation counter */
EMSCRIPTEN_KEEPALIVE int rtw_save_ptr(void) { return (int)(uintptr_t)port_flash_data(); }
EMSCRIPTEN_KEEPALIVE int rtw_save_size(void) { return (int)PORT_FLASH_SIZE; }
EMSCRIPTEN_KEEPALIVE int rtw_save_generation(void) { return (int)port_flash_gen; }
