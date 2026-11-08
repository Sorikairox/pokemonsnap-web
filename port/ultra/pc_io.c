/* PC layer: RCP / PI / SI register accesses through constant KSEG1 addresses in game C (IO_READ / IO_WRITE), which
 * vbe.py turns into calls (rcp_io). Sessions S2-S3a. Each register is logged on first access; reads give what the
 * console holds after the boot code (S3a: the two the game checks at boot, sys/main.c check_sp_imem / check_sp_dmem,
 * whose flags the game copies next to its anti-tamper code: app_level/504770.c), else 0. */
#include "pc_mem.h"
#include "../audio/audio.h"

#define IO_SLOTS 256
static uint32_t seen[IO_SLOTS];
static unsigned nseen;

static void once(const char *what, uint32_t addr, uint32_t v) {
    for (unsigned i = 0; i < nseen; i++)
        if (seen[i] == addr) return;
    if (nseen < IO_SLOTS) seen[nseen++] = addr;
    port_log("[io] %s 0x%08X = 0x%X (first access)\n", what, addr, v);
}

uint32_t __port_io_read(uint32_t addr) {
    uint32_t v = 0;
    if (addr == 0xA4001000u) v = 6103;             /* SP_IMEM_START: the CIC-6103 boot code leaves 6103 here */
    else if (addr == 0xA4000000u) v = 0xFFFFFFFFu; /* SP_DMEM_START: -1 after the boot code */
    else if (addr == 0xA4500004u) v = port_ai_get_length(); /* S7: AI_LEN_REG (sys/audio.c sizes its frames by it) */
    else if (addr == 0xA450000Cu) v = port_ai_get_status(); /* S7: AI_STATUS_REG */
    once("read", addr, v);
    return v;
}

void __port_io_write(uint32_t addr, uint32_t value) { once("write", addr, value); }
