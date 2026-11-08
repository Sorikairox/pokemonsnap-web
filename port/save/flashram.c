/* PC layer: the cartridge's FlashRAM as a PI device (session S8). Design: docs/port/SAVE.md.
 *
 * The game's own driver (src/os/osFlash.c with ultralib's flashreadstatus / flashclearstatus / flashwritebuffer,
 * compiled through vbe like any game TU) talks to this device exactly as it talks to the chip on the console:
 * 32-bit command words written with osEPiWriteIo to base | 0x10000, the status register read with osEPiReadIo at
 * base, the silicon id / array read and the page buffer written by PI DMA (osEPiStartDma on the flash handle,
 * routed here by port/ultra/pc_pi.c). The model is a 1 Mbit Macronix part (MX29L1100, id FLASH_VERSION_MX_C):
 *
 *   command (top byte)        effect
 *   0x3C chip erase mode      the next 0x78 erases all 128 KiB
 *   0x4B | page sector mode   the next 0x78 erases the 16 KiB sector (128 pages) holding `page`
 *   0x78 execute erase        bytes -> 0xFF; status bit 0x08 (erase done)
 *   0xB4 page program mode    the next PI write DMA fills the 128-byte page buffer
 *   0xA5 | page program page  page &= buffer (programming only clears bits); status bit 0x04 (write done)
 *   0xD2 status mode          reads of base return the status word; a write to base sets the status (clear: 0)
 *   0xE1 id mode              a PI read DMA returns the 8-byte silicon id 11118001 00C2001E
 *   0xF0 read array mode      a PI read DMA returns array bytes at devAddr * 2 (MX parts address 16-bit units:
 *                             the driver passes page * __osFlashVersion = page * 0x40 for this id)
 *
 * Status word = 0x11118000 | status byte (the low byte is all the driver tests: busy bits 0x02 erase / 0x01 write,
 * done bits 0x08 / 0x04). Operations complete at once: the driver's busy-polls (osEPiReadIo loops, no yield on the
 * console either) end at their first read; the console's erase / program times (tens of ms) are not modelled, nothing
 * in the game measures them. Reads of base in erase / program mode also return the status word (the driver polls
 * there without selecting status mode first).
 *
 * The image is kept in console byte order (a plain 131072-byte big-endian FlashRAM dump, the format of a cartridge
 * dump and of most emulators' .fla files): DMAs go byte by byte through the vbe accessors. It starts erased (0xFF)
 * until the host loads a stored image (port_save_load / rtw_save_ptr, save.c). Every completed erase and
 * page program bumps the generation counter; the host persists the image when it changes. */
#include "pc_os.h"
#include "save.h"
#include <emscripten.h>
#include <string.h>

#define FLASH_STATUS_MAGIC 0x11118000u
#define FLASH_ID_TYPE 0x11118001u  /* FLASH_TYPE_MAGIC */
#define FLASH_ID_MAKER 0x00C2001Eu /* FLASH_VERSION_MX_C */

enum { MODE_READ_ARRAY, MODE_STATUS, MODE_ID, MODE_ERASE, MODE_PROGRAM };

static unsigned char image[PORT_FLASH_SIZE];   /* console byte order */
static unsigned char page_buf[PORT_FLASH_PAGE];
static int image_ready, mode, erase_pending;
static unsigned erase_off, erase_len, status;
unsigned port_flash_gen, port_flash_erases, port_flash_programs, port_flash_reads;

static void ready(void) {
    if (!image_ready) { memset(image, 0xFF, sizeof image); image_ready = 1; }
}
unsigned char *port_flash_data(void) { ready(); return image; }

/* the handle the game's osFlashInit filled (OSPiHandle: baseAddress at 0x0C = PHYS_TO_K1(FLASH_START_ADDR)) */
int port_flash_is_handle(const void *handle) {
    return handle && (vbe_ld32((const unsigned char *)handle + 0x0C) & 0x1FFFFFFFu) == 0x08000000u;
}

static void command(uint32_t cmd) {
    unsigned page = cmd & 0xFFFFu;
    switch (cmd >> 24) {
    case 0x3C: mode = MODE_ERASE; erase_off = 0; erase_len = PORT_FLASH_SIZE; erase_pending = 1; break;
    case 0x4B:
        mode = MODE_ERASE;
        erase_off = (page & ~0x7Fu) * PORT_FLASH_PAGE;
        erase_len = 0x4000;
        erase_pending = erase_off < PORT_FLASH_SIZE;
        break;
    case 0x78:
        if (mode == MODE_ERASE && erase_pending) {
            ready();
            memset(image + erase_off, 0xFF, erase_len);
            erase_pending = 0;
            status = 0x08;
            port_flash_erases++;
            port_flash_gen++;
        }
        break;
    case 0xB4: mode = MODE_PROGRAM; break;
    case 0xA5:
        mode = MODE_PROGRAM;
        if (page * PORT_FLASH_PAGE < PORT_FLASH_SIZE) {
            ready();
            unsigned char *d = image + page * PORT_FLASH_PAGE;
            for (unsigned i = 0; i < PORT_FLASH_PAGE; i++) d[i] &= page_buf[i];
            status = 0x04;
            port_flash_programs++;
            port_flash_gen++;
        }
        break;
    case 0xD2: mode = MODE_STATUS; break;
    case 0xE1: mode = MODE_ID; break;
    case 0xF0: mode = MODE_READ_ARRAY; break;
    default: port_log("[save] flash: unknown command %08X\n", cmd); break;
    }
}

/* libultra PI I/O on the flash handle (the game calls them only from osFlash.c); devAddr is a KSEG1 address */
int osEPiWriteIo(void *handle, uint32_t devAddr, uint32_t data) {
    (void)handle;
    if ((devAddr & 0x1FFFFFFFu) == 0x08010000u) command(data);
    else if ((devAddr & 0x1FFFFFFFu) == 0x08000000u && mode == MODE_STATUS) status = data & 0xFF;
    else port_log("[save] flash: write %08X to %08X ignored\n", data, devAddr);
    return 0;
}
int osEPiReadIo(void *handle, uint32_t devAddr, uint32_t *data) {
    (void)handle;
    uint32_t v = FLASH_STATUS_MAGIC | status;
    if (mode == MODE_READ_ARRAY) {
        unsigned off = ((devAddr & 0xFFFFu) << 1) & ~3u;
        ready();
        v = (uint32_t)image[off] << 24 | (uint32_t)image[off + 1] << 16 | (uint32_t)image[off + 2] << 8 | image[off + 3];
    } else if (mode == MODE_ID) {
        v = FLASH_ID_TYPE;
    }
    vbe_st32(data, v);
    return 0;
}
int osEPiLinkHandle(void *handle) { (void)handle; return 0; }

/* PI DMA on the flash handle (pc_pi.c osEPiStartDma): OS_READ = flash -> RDRAM, OS_WRITE = RDRAM -> flash */
int port_flash_dma(void *mbv, int direction) {
    N64IoMesg *mb = mbv;
    unsigned char *dram = mb->dramAddr;
    unsigned n = mb->size;
    if (direction == 0) {
        if (mode == MODE_ID) {
            static const unsigned char id[8] = { 0x11, 0x11, 0x80, 0x01, 0x00, 0xC2, 0x00, 0x1E };
            for (unsigned i = 0; i < n; i++) vbe_st8(dram + i, i < 8 ? id[i] : 0);
            port_log("[save] retrace %u: flash silicon id read (%u array pages read so far)\n", port_vi_count,
                     port_flash_reads);
        } else if (mode == MODE_READ_ARRAY) {
            unsigned off = (mb->devAddr & 0xFFFFu) << 1;
            ready();
            for (unsigned i = 0; i < n; i++) vbe_st8(dram + i, off + i < PORT_FLASH_SIZE ? image[off + i] : 0xFF);
            port_flash_reads++;
        } else {
            uint32_t v = FLASH_STATUS_MAGIC | status;
            for (unsigned i = 0; i < n; i++) vbe_st8(dram + i, (unsigned char)(v >> (24 - 8 * (i & 3))));
        }
    } else {
        if (mode == MODE_PROGRAM)
            for (unsigned i = 0; i < n && i < PORT_FLASH_PAGE; i++) page_buf[i] = (unsigned char)vbe_ld8(dram + i);
        else port_log("[save] flash: write DMA in mode %d ignored\n", mode);
    }
    if (mb->retQueue) pc_send(mb->retQueue, mb, 0);
    return 0;
}
