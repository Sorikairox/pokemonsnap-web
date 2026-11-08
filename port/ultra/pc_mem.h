/* PC layer: the N64 memory model of the wasm port (session S2; design in docs/port/PLAN.md section 2 and 3.1).
 *
 * Game TUs see *virtual big-endian* memory (port/tools/vbe.py): every aligned 32-bit word holds its big-endian value
 * as a native i32, halfwords live at a ^ 2, bytes at a ^ 3. PC-layer code is native C: it must go through the
 * accessors below for any byte / halfword of game memory, and never hand game memory to byte-wise libc functions.
 *
 * RDRAM: `n64_rdram` (8 MiB, 8 MiB aligned, linked first: port/build/gen/rdram_labels.s) holds console RAM at the
 * same offsets; every RAM data symbol without a C definition is a label inside it. */
#ifndef PC_MEM_H
#define PC_MEM_H
#include <stddef.h>
#include <stdint.h>

#define PORT_RDRAM_SIZE 0x800000u

extern unsigned char n64_rdram[];

typedef struct PortSegment {
    const char *name, *type;
    uint32_t rom_start, rom_end, vram_start, vram_end, bss_start, bss_end;
} PortSegment;
extern const PortSegment port_segments[];
/* relocations of the ROM-loaded data (port/build/gen/relocs.c from port/symbols/relocs.tsv) */
typedef struct PortRelocTarget { const void *addr; uint32_t vram; uint32_t is_func; } PortRelocTarget;
typedef struct PortReloc { uint32_t word_vram; int32_t target; /* >= 0 port_reloc_targets, -1 mirror, -2 code without port body, -3 absolute */ } PortReloc;
typedef struct PortRelocRange { uint32_t first, count; } PortRelocRange;
extern const PortRelocTarget port_reloc_targets[];
extern const PortReloc port_relocs[];
extern const PortRelocRange port_reloc_ranges[];
extern const unsigned port_segment_count;
/* C data + bss of the compiled TUs of an overlay segment: wasm data section ovl_<segment> (port_build.py puts every
 * writable global of those TUs there), gen/pc_segments.c */
typedef struct PortOvlData { unsigned segment; unsigned char *start, *end; } PortOvlData;
extern const PortOvlData port_ovl_data[];
extern const unsigned port_ovl_data_count;
extern unsigned port_vi_count;

#define VBE8(p) ((unsigned char *)((uintptr_t)(p) ^ 3))
#define VBE16(p) ((unsigned short *)((uintptr_t)(p) ^ 2))
static inline unsigned vbe_ld8(const void *p) { return *VBE8(p); }
static inline void vbe_st8(void *p, unsigned v) { *VBE8(p) = (unsigned char)v; }
static inline unsigned vbe_ld16(const void *p) { return *VBE16(p); }
static inline void vbe_st16(void *p, unsigned v) { *VBE16(p) = (unsigned short)v; }
static inline uint32_t vbe_ld32(const void *p) { return *(const uint32_t *)p; }
static inline void vbe_st32(void *p, uint32_t v) { *(uint32_t *)p = v; }

/* console address (KSEG0 / KSEG1 / physical, < 8 MiB) -> mirror pointer; NULL (and a log line) outside RDRAM */
void *port_n64_to_wasm(uint32_t addr);
/* mirror pointer -> console KSEG0 address (0 if the pointer is not in the mirror) */
uint32_t port_wasm_to_n64(const void *p);

/* ROM: the player's ROM, kept in the vbe layout (one bswap32 per word of the big-endian .z64) */
int port_set_rom(const unsigned char *data, uint32_t size); /* .z64 / .n64 / .v64; exported */
int port_rom_ready(void);
uint32_t port_rom_size(void);
/* copy ROM bytes [rom, rom + n) into game memory at dst (vbe layout; any alignment) */
int port_rom_copy(void *dst, uint32_t rom, uint32_t n);
/* copy a segment of port/symbols/segments.tsv from the ROM into the mirror at its VRAM, clear its bss and apply its
 * relocations (pointer words -> wasm addresses / function-table indices) */
int port_load_segment(const char *name);
const PortSegment *port_find_segment(const char *name);
/* after a copy from the ROM into game memory: relocations / overlay residency / overlay C data reset (pc_mem.c) */
void port_dma_loaded(void *dst, uint32_t rom, uint32_t n);
void port_mem_init(void);
int port_segment_resident(unsigned si);
const char *port_resident_at(uint32_t vram);

void port_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#endif
