/* PC layer: RDRAM mirror, ROM loader and the helpers that vbe.py-rewritten game IR calls (session S2).
 * Written for this port (the vbe helper semantics follow the Conker web port's pc_mem.c, the user's own project). */
#include "pc_mem.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void port_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* ---------------------------------------------------------------- addresses */
void *port_n64_to_wasm(uint32_t addr) {
    uint32_t phys;
    if (addr >= 0x80000000u && addr < 0xC0000000u) phys = addr & 0x1FFFFFFFu; /* KSEG0 / KSEG1 */
    else phys = addr;
    if (phys < PORT_RDRAM_SIZE) return n64_rdram + phys;
    port_log("[mem] address 0x%08X is not in RDRAM\n", addr);
    return NULL;
}

uint32_t port_wasm_to_n64(const void *p) {
    uintptr_t a = (uintptr_t)p, b = (uintptr_t)n64_rdram;
    return (a >= b && a < b + PORT_RDRAM_SIZE) ? 0x80000000u + (uint32_t)(a - b) : 0;
}

/* ---------------------------------------------------------------- ROM */
static uint32_t *rom_words; /* the ROM in vbe layout: word i = big-endian word i of the .z64 as a native u32 */
static uint32_t rom_size;

int port_rom_ready(void) { return rom_words != NULL; }
uint32_t port_rom_size(void) { return rom_size; }

/* Takes a .z64 (big-endian), .n64 (little-endian words) or .v64 (byte-swapped halfwords) image; the copy is
 * normalised into the vbe layout. The caller keeps ownership of `data`. Returns 0 on success. */
int port_set_rom(const unsigned char *data, uint32_t size) {
    if (size < 0x1000 || (size & 3)) { port_log("[rom] bad size %u\n", size); return -1; }
    uint32_t magic = (uint32_t)data[0] << 24 | data[1] << 16 | data[2] << 8 | data[3];
    int order; /* 0 = z64, 1 = n64, 2 = v64 */
    if (magic == 0x80371240u) order = 0;
    else if (magic == 0x40123780u) order = 1;
    else if (magic == 0x37804012u) order = 2;
    else { port_log("[rom] unknown header 0x%08X\n", magic); return -1; }
    free(rom_words);
    rom_words = malloc(size);
    if (!rom_words) return -1;
    for (uint32_t i = 0; i < size / 4; i++) {
        const unsigned char *b = data + 4 * i;
        uint32_t be;
        if (order == 0) be = (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3];
        else if (order == 1) be = (uint32_t)b[3] << 24 | b[2] << 16 | b[1] << 8 | b[0];
        else be = (uint32_t)b[1] << 24 | b[0] << 16 | b[3] << 8 | b[2];
        rom_words[i] = be;
    }
    rom_size = size;
    char title[21];
    for (int i = 0; i < 20; i++) title[i] = (char)vbe_ld8((unsigned char *)rom_words + 0x20 + i);
    title[20] = 0;
    port_log("[rom] %u bytes (%s order), title \"%s\"\n", size, order == 0 ? "z64" : order == 1 ? "n64" : "v64", title);
    return 0;
}

int port_rom_copy(void *dst, uint32_t rom, uint32_t n) {
    if (!rom_words || rom > rom_size || n > rom_size - rom) {
        port_log("[rom] copy 0x%X+0x%X outside the ROM\n", rom, n);
        return -1;
    }
    unsigned char *d = dst;
    const unsigned char *s = (const unsigned char *)rom_words + rom;
    uint32_t i = 0;
    if ((((uintptr_t)d) & 3) == (rom & 3)) {
        for (; i < n && (((uintptr_t)(d + i)) & 3); i++) vbe_st8(d + i, vbe_ld8(s + i));
        for (; i + 4 <= n; i += 4) *(uint32_t *)(d + i) = *(const uint32_t *)(s + i);
    }
    for (; i < n; i++) vbe_st8(d + i, vbe_ld8(s + i));
    return 0;
}

const PortSegment *port_find_segment(const char *name) {
    for (unsigned i = 0; i < port_segment_count; i++)
        if (!strcmp(port_segments[i].name, name)) return &port_segments[i];
    return NULL;
}

/* Rewrite the pointer words of segment `si` (just copied from the ROM, so each word holds the console address). */
static void apply_relocs(unsigned si) {
    const PortRelocRange *r = &port_reloc_ranges[si];
    unsigned nm = 0, nc = 0, nf = 0, bad = 0, nocode = 0, nabs = 0;
    for (unsigned i = r->first; i < r->first + r->count; i++) {
        const PortReloc *rl = &port_relocs[i];
        uint32_t *p = port_n64_to_wasm(rl->word_vram), w = *p;
        if (rl->target == -3) { nabs++; continue; }   /* absolute symbol outside RAM (segment address): keep */
        if (rl->target == -2) { nocode++; continue; } /* code without a port body: keep the console address */
        if (rl->target < 0) {
            uint32_t phys = (w >= 0x80000000u && w < 0xC0000000u) ? (w & 0x1FFFFFFFu) : 0xFFFFFFFFu;
            if (phys >= PORT_RDRAM_SIZE) { if (bad++ < 3) port_log("[mem]   reloc @%08X: %08X not in RDRAM\n", rl->word_vram, w); continue; }
            *p = (uint32_t)(uintptr_t)(n64_rdram + phys);
            nm++;
            continue;
        }
        const PortRelocTarget *t = &port_reloc_targets[rl->target];
        if (t->is_func) {
            if (w != t->vram && bad++ < 3) port_log("[mem]   reloc @%08X: %08X is not the start of its function\n", rl->word_vram, w);
            *p = (uint32_t)(uintptr_t)t->addr;
            nf++;
        } else {
            *p = (uint32_t)((uintptr_t)t->addr + (w - t->vram));
            nc++;
        }
    }
    if (r->count)
        port_log("[mem]   %u relocations: %u mirror, %u C data, %u functions, %u absolute, %u code without port body, "
                 "%u bad\n", r->count, nm, nc, nf, nabs, nocode, bad);
}

int port_load_segment(const char *name) {
    const PortSegment *s = port_find_segment(name);
    if (!s) { port_log("[mem] no segment %s\n", name); return -1; }
    if (s->vram_start < 0x80000000u) { port_log("[mem] segment %s has no RAM address\n", name); return -1; }
    unsigned char *dst = port_n64_to_wasm(s->vram_start);
    if (!dst || port_rom_copy(dst, s->rom_start, s->rom_end - s->rom_start)) return -1;
    if (s->bss_start && s->bss_end > s->bss_start) memset(port_n64_to_wasm(s->bss_start), 0, s->bss_end - s->bss_start);
    apply_relocs((unsigned)(s - port_segments));
    port_log("[mem] %s: ROM 0x%X..0x%X -> 0x%08X (bss 0x%X bytes)\n", name, s->rom_start, s->rom_end, s->vram_start,
             s->bss_end > s->bss_start ? s->bss_end - s->bss_start : 0);
    return 0;
}

/* ---------------------------------------------------------------- helpers called by vbe.py-rewritten IR */
unsigned short __vbe_ld16u(const unsigned char *p) { return (unsigned short)(vbe_ld8(p) << 8 | vbe_ld8(p + 1)); }
void __vbe_st16u(unsigned char *p, unsigned short v) { vbe_st8(p, v >> 8); vbe_st8(p + 1, v); }
unsigned __vbe_ld32u(const unsigned char *p) {
    return vbe_ld8(p) << 24 | vbe_ld8(p + 1) << 16 | vbe_ld8(p + 2) << 8 | vbe_ld8(p + 3);
}
void __vbe_st32u(unsigned char *p, unsigned v) {
    vbe_st8(p, v >> 24); vbe_st8(p + 1, v >> 16); vbe_st8(p + 2, v >> 8); vbe_st8(p + 3, v);
}

void __vbe_memcpy(unsigned char *d, const unsigned char *s, unsigned n) {
    unsigned i = 0;
    if ((((uintptr_t)d) & 3) == (((uintptr_t)s) & 3)) {
        for (; i < n && (((uintptr_t)(d + i)) & 3); i++) vbe_st8(d + i, vbe_ld8(s + i));
        for (; i + 4 <= n; i += 4) *(uint32_t *)(d + i) = *(const uint32_t *)(s + i);
    }
    for (; i < n; i++) vbe_st8(d + i, vbe_ld8(s + i));
}
void __vbe_memmove(unsigned char *d, const unsigned char *s, unsigned n) {
    if (d <= s || d >= s + n) { __vbe_memcpy(d, s, n); return; }
    while (n--) vbe_st8(d + n, vbe_ld8(s + n));
}
void __vbe_memset(unsigned char *d, unsigned c, unsigned n) {
    unsigned i = 0;
    for (; i < n && (((uintptr_t)(d + i)) & 3); i++) vbe_st8(d + i, c);
    for (; i + 4 <= n; i += 4) *(uint32_t *)(d + i) = (c & 0xFFu) * 0x01010101u;
    for (; i < n; i++) vbe_st8(d + i, c);
}

/* Convert the 1/2/8-byte scalars of a game global's initialiser (laid out little-endian by clang) into the vbe
 * layout, once at startup. desc = {vbe.py size, LLVM alloc size, n x {offset, size, count, stride}}. */
void __vbe_fix(unsigned char *g, const unsigned *desc, unsigned n) {
    unsigned end = 0, mysize = desc[0], llsize = desc[1];
    desc += 2;
    if (mysize > ((llsize + 3) & ~3u)) {
        port_log("[vbe] layout mismatch for global at %p: vbe.py %u bytes, LLVM %u; not converted\n", (void *)g, mysize, llsize);
        return;
    }
    for (unsigned i = 0; i < n; i++) {
        const unsigned *d = desc + 4 * i;
        unsigned e = d[0] + (d[2] - 1) * d[3] + d[1];
        if (e > end) end = e;
    }
    end = (end + 3) & ~3u;
    unsigned char *tmp = malloc(end);
    memcpy(tmp, g, end);
    for (unsigned pass = 0; pass < 2; pass++)
        for (unsigned i = 0; i < n; i++) {
            const unsigned *d = desc + 4 * i;
            for (unsigned k = 0; k < d[2]; k++) {
                unsigned o = d[0] + k * d[3], sz = d[1];
                if (pass == 0) { memset(g + (o & ~3u), 0, ((o + sz + 3) & ~3u) - (o & ~3u)); continue; }
                if (sz == 1) vbe_st8(g + o, tmp[o]);
                else if (sz == 2) vbe_st16(g + o, (unsigned)(tmp[o] | tmp[o + 1] << 8));
                else {
                    uint64_t v;
                    memcpy(&v, tmp + o, 8);
                    *(uint32_t *)(g + o) = (uint32_t)(v >> 32);
                    *(uint32_t *)(g + o + 4) = (uint32_t)v;
                }
            }
        }
    free(tmp);
}
