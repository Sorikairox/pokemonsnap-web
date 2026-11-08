/* Force-included (-include) before every game TU of the wasm build (port/tools/port_build.py). Never seen by IDO. */
#ifndef PORT_PREFIX_H
#define PORT_PREFIX_H

/* IDO initialises a local array from another array (`T a[N] = other;`, a copy); clang does not. The `#ifdef PORT`
 * sites in game C declare the array and copy with this (llvm.memcpy: vbe.py keeps word copies, helpers otherwise). */
#define PORT_ARRAY_INIT(dst, src) __builtin_memcpy((dst), (src), sizeof(dst))

#endif
