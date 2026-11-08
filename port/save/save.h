/* PC layer: the save (session S8, docs/port/SAVE.md). FlashRAM device: flashram.c; host interface: save.c. */
#ifndef PORT_SAVE_H
#define PORT_SAVE_H
#include <stdint.h>

#define PORT_FLASH_SIZE 0x20000u /* 1 Mbit FlashRAM; retrowser save: { kind: flash, size: 131072 } */
#define PORT_FLASH_PAGE 128u

unsigned char *port_flash_data(void);         /* the image, console byte order (erased 0xFF until written) */
int port_flash_is_handle(const void *handle); /* an OSPiHandle of the flash domain (base 0x08000000) */
int port_flash_dma(void *mb, int direction);  /* osEPiStartDma on that handle */
extern unsigned port_flash_gen, port_flash_erases, port_flash_programs, port_flash_reads;

#endif
