#include "common.h"
#include "ld_addrs.h"

#define UNK_STUFF_VRAM 0x80200000

#define UNK_STUFF_SP_IMEM_OK *(u8*) (UNK_STUFF_VRAM - 0x10)
#define UNK_STUFF_SP_DMEM_OK *(u8*) (UNK_STUFF_VRAM - 0x0C)
#define UNK_STUFF_MINUS_8 *(u8*) (UNK_STUFF_VRAM - 0x08)

void app_level_CheckIllegalCopy(void) { /* was func_80364360_504770 */
    dmaReadVPK0((u32*) unk_segment_AA18E0_vpk0_ROM_START, UNK_STUFF_VRAM);
    UNK_STUFF_SP_IMEM_OK = gSPImemOkay;
    UNK_STUFF_SP_DMEM_OK = gSPDmemOkay;
#ifdef PORT
    // wasm port: the decompressed routine is MIPS code outside the decomp (18 instructions, read at port session
    // S3a), which the port cannot call. It sets the result byte to 0 when both boot-time RSP memory checks passed,
    // else to 0xFF; the same here.
    UNK_STUFF_MINUS_8 = (UNK_STUFF_SP_IMEM_OK != 0 && UNK_STUFF_SP_DMEM_OK != 0) ? 0 : 0xFF;
#else
    ((void (*)(void)) UNK_STUFF_VRAM)();
#endif
    if (UNK_STUFF_MINUS_8 != 0) {
        setPlayerFlag(PFID_ILLEGAL_COPY, true);
    }
}
