/* PC layer: the libc entry points that game code calls but libultra implements in assembly (bcopy.s, bzero.s,
 * bcmp.s, gu/sqrtf.s). vbe.py renames every game reference to @vbe.<name>, so emscripten's libc (host byte order)
 * never serves game memory and the PC layer keeps the real libc. Written for this port from libultra's documented
 * semantics (os_libc.h prototypes); no ultralib code. Session S2. */
#include "pc_mem.h"
#include <math.h>

void __vbe_memmove(unsigned char *d, const unsigned char *s, unsigned n);
void __vbe_memset(unsigned char *d, unsigned c, unsigned n);

void port_bcopy(const void *src, void *dst, int n) __asm__("vbe.bcopy");
void port_bcopy(const void *src, void *dst, int n) {
    if (n > 0) __vbe_memmove(dst, src, (unsigned)n);
}

void port_bzero(void *p, int n) __asm__("vbe.bzero");
void port_bzero(void *p, int n) {
    if (n > 0) __vbe_memset(p, 0, (unsigned)n);
}

int port_bcmp(const void *a, const void *b, int n) __asm__("vbe.bcmp");
int port_bcmp(const void *a, const void *b, int n) {
    for (int i = 0; i < n; i++)
        if (vbe_ld8((const unsigned char *)a + i) != vbe_ld8((const unsigned char *)b + i)) return 1;
    return 0;
}

/* R4300 sqrt.s: IEEE single-precision square root */
float port_sqrtf(float x) __asm__("vbe.sqrtf");
float port_sqrtf(float x) { return __builtin_sqrtf(x); }
