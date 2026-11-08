#ifndef _CONSTANTS_H
#define _CONSTANTS_H

#include "ultra64.h"

#ifdef PORT /* wasm port: the console address inside the RDRAM mirror (port/include/port_prefix.h) */
#define VPK_VRAM PORT_K0(0x802B5000)
#else
#define VPK_VRAM 0x802B5000 // TODO SHIFT hard-coded VRAM
#endif

#endif
