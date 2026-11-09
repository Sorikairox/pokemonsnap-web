/* PC layer: PI (cartridge DMA), SI (controllers) and the FlashRAM save; session S3a. Written for this port from
 * libultra's documented behaviour (os_pi.h, os_cont.h, os_flash.h). Design: docs/port/MEMORY.md "DMA".
 *
 * PI: osEPiStartDma copies from the ROM (port_rom_copy) at once and posts the I/O message to its return queue, as
 * the PI manager does when the transfer ends. port_dma_loaded() then relocates what the copy brought in when it is a
 * segment landing at its own VRAM (overlays, asset segments; pc_mem.c).
 * SI: one standard controller in port 1, no pak (status 0: no Controller / Rumble / Transfer Pak); ports 2-4 do not
 * answer. The pad comes from the host (port_set_pad, active-high OSContPad bits). Rumble: port_rumble (S9).
 * Flash (S8): the game's own driver (src/os/osFlash.c) runs on a FlashRAM device model, port/save/flashram.c; DMAs
 * on its handle go there. */
#include "pc_os.h"
#include "../save/save.h"
#include <emscripten.h>
#include <string.h>

/* ---------------------------------------------------------------- PI */
/* the handles the game gets back; their fields are only read by libultra itself */
static uint32_t rom_handle[0x74 / 4];
unsigned port_pi_dmas, port_pi_bytes;

void *osCartRomInit(void) { rom_handle[2] = 0xB0000000u; return rom_handle; }
void osCreatePiManager(int pri, void *cmdQ, void *cmdBuf, int cmdMsgCnt) { (void)pri; (void)cmdQ; (void)cmdBuf; (void)cmdMsgCnt; }

static uint32_t rom_offset(uint32_t dev) {
    if (dev >= 0x10000000u && dev < 0x20000000u) return dev - 0x10000000u; /* PI cart domain address */
    return dev & 0x0FFFFFFFu;                                                 /* ROM offset (the game's usual form) */
}

int osEPiStartDma(void *handle, void *mbv, int direction) {
    N64IoMesg *mb = mbv;
    if (handle != rom_handle && port_flash_is_handle(handle)) return port_flash_dma(mbv, direction); /* S8 */
    if (direction == 0) { /* OS_READ: cart -> RDRAM */
        uint32_t rom = rom_offset(mb->devAddr);
        if (port_rom_copy(mb->dramAddr, rom, mb->size) == 0) port_dma_loaded(mb->dramAddr, rom, mb->size);
        port_pi_dmas++;
        port_pi_bytes += mb->size;
    } else {
        port_log("[pi] write DMA to the cartridge %08X ignored\n", mb->devAddr);
    }
    if (mb->retQueue) pc_send(mb->retQueue, mb, 0);
    return 0;
}

int osPiStartDma(void *mb, int pri, int direction, unsigned devAddr, void *dram, unsigned size, void *mq) {
    N64IoMesg *m = mb;
    (void)pri;
    m->retQueue = mq;
    m->dramAddr = dram;
    m->devAddr = devAddr;
    m->size = size;
    return osEPiStartDma(rom_handle, m, direction);
}

/* ---------------------------------------------------------------- SI: controllers */
static struct { uint16_t buttons; int8_t x, y; } pads[4];
/* host input (exported): pad 0 is the player's; buttons are N64 OSContPad bits (A = 0x8000, START = 0x1000, ...);
 * bits 16-31 are retrowser's controls.extra buttons, turned into OSContPad bits by port_pad_filter (F1, pc_options.c:
 * the Snap button, F4: the Shutter). Called once per retrace. */
int port_pad_filter(int buttons);
EMSCRIPTEN_KEEPALIVE void port_set_pad(int i, int buttons, int x, int y) {
    if (i < 0 || i > 3) return;
    if (i == 0) buttons = port_pad_filter(buttons);
    pads[i].buttons = (uint16_t)buttons;
    pads[i].x = (int8_t)(x < -128 ? -128 : x > 127 ? 127 : x);
    pads[i].y = (int8_t)(y < -128 ? -128 : y > 127 ? 127 : y);
}

/* OSContStatus[4]: u16 type, u8 status, u8 errno (sub-word fields: vbe accessors) */
static void fill_status(unsigned char *st) {
    for (int i = 0; i < 4; i++, st += 4) {
        vbe_st16(st, i == 0 ? 0x0005 : 0);  /* CONT_TYPE_NORMAL */
        vbe_st8(st + 2, 0);                  /* no pak */
        vbe_st8(st + 3, i == 0 ? 0 : 0x8);   /* CONT_NO_RESPONSE_ERROR */
    }
}

int osContInit(void *mq, void *bitpattern, void *status) {
    (void)mq; /* libultra waits on mq internally for its own SI transfers */
    vbe_st8(bitpattern, 0x1);
    fill_status(status);
    return 0;
}
int osContStartQuery(void *mq) { (void)mq; pc_post_event(N64_EVENT_SI); return 0; }
void osContGetQuery(void *status) { fill_status(status); }
int osContStartReadData(void *mq) { (void)mq; pc_post_event(N64_EVENT_SI); return 0; }
unsigned port_cont_reads;
void osContGetReadData(void *padv) {
    unsigned char *p = padv;
    port_cont_reads++;
    for (int i = 0; i < 4; i++, p += 6) { /* OSContPad: u16 button, s8 stick_x, s8 stick_y, u8 errno */
        vbe_st16(p, i == 0 ? pads[0].buttons : 0);
        vbe_st8(p + 2, i == 0 ? (uint8_t)pads[0].x : 0);
        vbe_st8(p + 3, i == 0 ? (uint8_t)pads[0].y : 0);
        vbe_st8(p + 4, i == 0 ? 0 : 0x8);
    }
}
/* paks: none present (the game only probes them when status says CONT_CARD_ON, which it never does) */
int __osContRamRead(void *mq, int ch, int addr, void *buf) { (void)mq; (void)ch; (void)addr; (void)buf; return 1; }
int __osContRamWrite(void *mq, int ch, int addr, void *buf, int f) { (void)mq; (void)ch; (void)addr; (void)buf; (void)f; return 1; }
/* S9: motor on/off goes to port_rumble (the retrowser target's port/rtw/rtw.c forwards it to rtw_rumble). Not reached
 * while osMotorInit reports no pak; the game never starts the motor anyway (only sched.c's init + stop at reset). */
__attribute__((weak)) void port_rumble(int channel, int on) { (void)channel; (void)on; }
int __osMotorAccess(void *pfs, int on) {
    port_rumble((int)vbe_ld32((unsigned char *)pfs + 8), on); /* OSPfs.channel */
    return 0;
}
int osMotorInit(void *mq, void *pfs, int ch) { (void)mq; (void)pfs; (void)ch; return 1; }  /* PFS_ERR_NOPACK */
int osPfsInitPak(void *mq, void *pfs, int ch) { (void)mq; (void)pfs; (void)ch; return 1; }
int osPfsFileState(void *pfs, int f, void *st) { (void)pfs; (void)f; (void)st; return 1; }
int osGbpakInit(void *mq, void *pfs, int ch) { (void)mq; (void)pfs; (void)ch; return 1; }
int osGbpakPower(void *pfs, int on) { (void)pfs; (void)on; return 1; }
int osGbpakReadId(void *pfs, void *id, void *st) { (void)pfs; (void)id; (void)st; return 1; }
int osGbpakCheckConnector(void *pfs, void *st) { (void)pfs; (void)st; return 1; }
int osGbpakReadWrite(void *pfs, int dir, int addr, void *buf, int n) { (void)pfs; (void)dir; (void)addr; (void)buf; (void)n; return 1; }
