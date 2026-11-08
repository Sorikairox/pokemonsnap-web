/* PC layer: PI (cartridge DMA), SI (controllers) and the FlashRAM save; session S3a. Written for this port from
 * libultra's documented behaviour (os_pi.h, os_cont.h, os_flash.h). Design: docs/port/MEMORY.md "DMA".
 *
 * PI: osEPiStartDma copies from the ROM (port_rom_copy) at once and posts the I/O message to its return queue, as
 * the PI manager does when the transfer ends. port_dma_loaded() then relocates what the copy brought in when it is a
 * segment landing at its own VRAM (overlays, asset segments; pc_mem.c).
 * SI: one standard controller in port 1, no pak (status 0: no Controller / Rumble / Transfer Pak); ports 2-4 do not
 * answer. The pad comes from the host (port_set_pad, active-high OSContPad bits). Rumble: later (rtw_rumble).
 * Flash: a 131072-byte image in memory (FlashRAM, 128-byte pages, 16 KiB sectors), erased = 0xFF; the retrowser
 * host will own its persistence (port_flash_image / port_flash_generation, S8). */
#include "pc_os.h"
#include <emscripten.h>
#include <string.h>

/* ---------------------------------------------------------------- PI */
/* the handles the game gets back; their fields are only read by libultra itself */
static uint32_t rom_handle[0x74 / 4], flash_handle[0x74 / 4];
unsigned port_pi_dmas, port_pi_bytes;

void *osCartRomInit(void) { rom_handle[2] = 0xB0000000u; return rom_handle; }
void osCreatePiManager(int pri, void *cmdQ, void *cmdBuf, int cmdMsgCnt) { (void)pri; (void)cmdQ; (void)cmdBuf; (void)cmdMsgCnt; }

static uint32_t rom_offset(uint32_t dev) {
    if (dev >= 0x10000000u && dev < 0x20000000u) return dev - 0x10000000u; /* PI cart domain address */
    return dev & 0x0FFFFFFFu;                                                 /* ROM offset (the game's usual form) */
}

int osEPiStartDma(void *handle, void *mbv, int direction) {
    N64IoMesg *mb = mbv;
    if (handle == flash_handle) { port_log("[pi] DMA on the flash handle\n"); return -1; }
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
/* host input (exported): pad 0 is the player's; buttons are N64 OSContPad bits (A = 0x8000, START = 0x1000, ...) */
EMSCRIPTEN_KEEPALIVE void port_set_pad(int i, int buttons, int x, int y) {
    if (i < 0 || i > 3) return;
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
int __osMotorAccess(void *pfs, int on) { (void)pfs; (void)on; return 1; }
int osMotorInit(void *mq, void *pfs, int ch) { (void)mq; (void)pfs; (void)ch; return 1; }  /* PFS_ERR_NOPACK */
int osPfsInitPak(void *mq, void *pfs, int ch) { (void)mq; (void)pfs; (void)ch; return 1; }
int osPfsFileState(void *pfs, int f, void *st) { (void)pfs; (void)f; (void)st; return 1; }
int osGbpakInit(void *mq, void *pfs, int ch) { (void)mq; (void)pfs; (void)ch; return 1; }
int osGbpakPower(void *pfs, int on) { (void)pfs; (void)on; return 1; }
int osGbpakReadId(void *pfs, void *id, void *st) { (void)pfs; (void)id; (void)st; return 1; }
int osGbpakCheckConnector(void *pfs, void *st) { (void)pfs; (void)st; return 1; }
int osGbpakReadWrite(void *pfs, int dir, int addr, void *buf, int n) { (void)pfs; (void)dir; (void)addr; (void)buf; (void)n; return 1; }

/* ---------------------------------------------------------------- FlashRAM (128 KiB) */
#define FLASH_SIZE 0x20000
#define FLASH_PAGE 128
static unsigned char flash[FLASH_SIZE];   /* console byte order */
static unsigned char flash_page_buf[FLASH_PAGE];
static int flash_ready;
static unsigned flash_generation;
EMSCRIPTEN_KEEPALIVE unsigned char *port_flash_image(void) { return flash; }
EMSCRIPTEN_KEEPALIVE unsigned port_flash_generation(void) { return flash_generation; }

void *osFlashInit(void) {
    if (!flash_ready) { memset(flash, 0xFF, sizeof flash); flash_ready = 1; }
    return flash_handle;
}
void osFlashReadId(void *type, void *maker) {
    vbe_st32(type, 0x11118001u);  /* FLASH_TYPE_MAGIC */
    vbe_st32(maker, 0x00C2001Eu); /* FLASH_VERSION_MX_C */
}
int osFlashAllErase(void) { memset(flash, 0xFF, sizeof flash); flash_generation++; return 0; }
int osFlashSectorErase(int page) {
    unsigned off = ((unsigned)page & ~0x7Fu) * FLASH_PAGE;
    if (off >= FLASH_SIZE) return -1;
    memset(flash + off, 0xFF, 0x4000);
    flash_generation++;
    return 0;
}
int osFlashReadArray(void *mbv, int pri, int page, void *dram, int pages, void *mq) {
    N64IoMesg *mb = mbv;
    (void)pri;
    unsigned off = (unsigned)page * FLASH_PAGE, n = (unsigned)pages * FLASH_PAGE;
    if (off <= FLASH_SIZE && n <= FLASH_SIZE - off)
        for (unsigned i = 0; i < n; i++) vbe_st8((unsigned char *)dram + i, flash[off + i]);
    mb->retQueue = mq;
    mb->dramAddr = dram;
    mb->devAddr = off;
    mb->size = n;
    if (mq) pc_send(mq, mb, 0);
    return 0;
}
int osFlashWriteBuffer(void *mbv, int pri, void *dram, void *mq) {
    N64IoMesg *mb = mbv;
    (void)pri;
    for (unsigned i = 0; i < FLASH_PAGE; i++) flash_page_buf[i] = (unsigned char)vbe_ld8((unsigned char *)dram + i);
    mb->retQueue = mq;
    mb->dramAddr = dram;
    mb->size = FLASH_PAGE;
    if (mq) pc_send(mq, mb, 0);
    return 0;
}
int osFlashWriteArray(int page) {
    unsigned off = (unsigned)page * FLASH_PAGE;
    if (off >= FLASH_SIZE) return -1;
    for (unsigned i = 0; i < FLASH_PAGE; i++) flash[off + i] &= flash_page_buf[i]; /* programming only clears bits */
    flash_generation++;
    return 0;
}
