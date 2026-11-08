/* PC layer: native views of the libultra structures the OS replacement touches (session S3a).
 *
 * The PC layer is compiled natively (no vbe.py), but these structures live in game memory, which is virtual
 * big-endian (pc_mem.h): 32-bit fields are native words and can be read directly; 16-bit and 8-bit fields sit at
 * a ^ 2 / a ^ 3 and must go through vbe_ld16 / vbe_st16 / vbe_ld8 / vbe_st8. The layouts below are the libultra
 * ones (ultralib/include/PR/os_thread.h, os_message.h, os_pi.h, sptask.h, os_cont.h); pointers are 32-bit in
 * wasm32 as on the N64, so the offsets match (checked by the static asserts). Written for this port. */
#ifndef PC_OS_H
#define PC_OS_H
#include "pc_mem.h"

typedef void *OSMesg_;

/* OSThread (0x1B0 bytes on the N64; the PC layer uses the first 0x20) */
typedef struct N64Thread {
    struct N64Thread *next;    /* 0x00 */
    int32_t priority;          /* 0x04 */
    struct N64Thread **queue;  /* 0x08 */
    struct N64Thread *tlnext;  /* 0x0C: all-threads list (error.c walks it from __osGetActiveQueue) */
    uint32_t state_flags;      /* 0x10: u16 state (BE offset 0x10 = high half), u16 flags (low half) */
    int32_t id;                /* 0x14 */
    int32_t fp;                /* 0x18 */
    uint32_t thprof;           /* 0x1C: unused by libultra games; the PC layer keeps its slot tag here */
} N64Thread;
#define N64_THREAD_STATE(t) ((t)->state_flags >> 16)
#define N64_THREAD_SET_STATE(t, s) ((t)->state_flags = ((uint32_t)(s) << 16) | ((t)->state_flags & 0xFFFFu))
enum { OS_STATE_STOPPED_ = 1, OS_STATE_RUNNABLE_ = 2, OS_STATE_RUNNING_ = 4, OS_STATE_WAITING_ = 8 };

typedef struct N64MesgQueue {
    N64Thread *mtqueue;   /* 0x00 */
    N64Thread *fullqueue; /* 0x04 */
    int32_t validCount;   /* 0x08 */
    int32_t first;        /* 0x0C */
    int32_t msgCount;     /* 0x10 */
    OSMesg_ *msg;         /* 0x14 */
} N64MesgQueue;

/* OSIoMesg: hdr {u16 type; u8 pri; u8 status; OSMesgQueue *retQueue}, dramAddr, devAddr, size, piHandle */
typedef struct N64IoMesg {
    uint32_t hdr_word;        /* 0x00: type / pri / status (sub-word, vbe accessors) */
    N64MesgQueue *retQueue;   /* 0x04 */
    void *dramAddr;           /* 0x08 */
    uint32_t devAddr;         /* 0x0C */
    uint32_t size;            /* 0x10 */
    void *piHandle;           /* 0x14 */
} N64IoMesg;

/* OSTask_t (sptask.h): every field is a 32-bit word */
typedef struct N64Task {
    uint32_t type, flags;
    void *ucode_boot;     uint32_t ucode_boot_size;
    void *ucode;          uint32_t ucode_size;
    void *ucode_data;     uint32_t ucode_data_size;
    void *dram_stack;     uint32_t dram_stack_size;
    void *output_buff;    void *output_buff_size;
    void *data_ptr;       uint32_t data_size;
    void *yield_data_ptr; uint32_t yield_data_size;
} N64Task;
enum { M_GFXTASK_ = 1, M_AUDTASK_ = 2 };

_Static_assert(sizeof(N64Thread) == 0x20, "OSThread head");
_Static_assert(sizeof(N64MesgQueue) == 0x18, "OSMesgQueue");
_Static_assert(sizeof(N64IoMesg) == 0x18, "OSIoMesg");
_Static_assert(sizeof(N64Task) == 0x40, "OSTask_t");

/* OS_EVENT_* (os_message.h) */
enum { N64_EVENT_SP = 4, N64_EVENT_SI = 5, N64_EVENT_AI = 6, N64_EVENT_VI = 7, N64_EVENT_PI = 8, N64_EVENT_DP = 9,
       N64_EVENT_COUNT = 23 };

/* pc_os.c: messages and events usable from any PC-layer code (thread or host context) */
int pc_send(N64MesgQueue *mq, OSMesg_ msg, int block);
void pc_post_event(int event);

/* pc_icall.c: a game function pointer (table index, or a console / mirror address of a function) called with
 * one i32 argument and no result, converting like the trampolines (fiber entry points) */
void port_call_vi(void *fp, int a0);
#endif
