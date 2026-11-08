/* PC layer: RCP / PI / SI register accesses through constant KSEG1 addresses in game C (IO_READ / IO_WRITE), which
 * vbe.py turns into calls (rcp_io). S2: no register model yet; each register is logged on first access and reads 0.
 * Session S2. */
#include "pc_mem.h"

#define IO_SLOTS 256
static uint32_t seen[IO_SLOTS];
static unsigned nseen;

static void once(const char *what, uint32_t addr) {
    for (unsigned i = 0; i < nseen; i++)
        if (seen[i] == addr) return;
    if (nseen < IO_SLOTS) seen[nseen++] = addr;
    port_log("[io] %s 0x%08X (no register model yet)\n", what, addr);
}

uint32_t __port_io_read(uint32_t addr) {
    once("read", addr);
    return 0;
}

void __port_io_write(uint32_t addr, uint32_t value) { once("write", addr); }
