/* Force-included (-include) before every game TU of the wasm build (port/tools/port_build.py). Never seen by IDO. */
#ifndef PORT_PREFIX_H
#define PORT_PREFIX_H

/* IDO initialises a local array from another array (`T a[N] = other;`, a copy); clang does not. The `#ifdef PORT`
 * sites in game C declare the array and copy with this (llvm.memcpy: vbe.py keeps word copies, helpers otherwise). */
#define PORT_ARRAY_INIT(dst, src) __builtin_memcpy((dst), (src), sizeof(dst))

/* S3a: a console KSEG0 RAM address as the matching address in the RDRAM mirror, for hard-coded addresses that game C
 * does arithmetic on together with symbol addresses (VPK_VRAM - (uintptr_t) intro_code_VRAM_END). Plain pointer uses
 * of such constants need nothing: vbe.py maps every integer -> pointer conversion of KSEG0 RAM into the mirror. */
extern unsigned char n64_rdram[];
#define PORT_K0(a) ((unsigned int)(unsigned long)n64_rdram + ((unsigned int)(a) - 0x80000000u))

#endif
