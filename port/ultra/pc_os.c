/* PC layer: libultra threads, message queues and events on Emscripten fibers (ASYNCIFY); session S3a.
 * Design: docs/port/MEMORY.md "Threads". Written for this port from libultra's documented semantics (the Conker web
 * port's pc_sched.c, the user's own project, was the reference for the fiber model).
 *
 *  - Every OSThread gets a fiber with fixed-address stacks (static slots: their addresses end up in game memory, so
 *    they must not depend on heap history). The host context (main(), or an exported call from the page) is the
 *    "idle" context: it posts the retrace and runs threads until none is runnable.
 *  - Scheduling is libultra's: the highest-priority runnable thread runs; equal priorities are FIFO by the time they
 *    became runnable; a thread runs until it blocks (osRecvMesg on an empty queue, osSendMesg BLOCK on a full one,
 *    osStopThread, osYieldThread, lowering its priority) or makes a higher-priority thread runnable (preemption at
 *    once, as on the console). Threads switch to each other directly (one fiber swap); the host context is entered
 *    only when nothing is runnable.
 *  - Priority 0 (OS_PRIORITY_IDLE) is never run: the game's idle thread lowers itself to 0 and spins `while (1)`.
 *  - No preemption by time: a thread that busy-waits without calling libultra would hang (none found so far). */
#include "pc_os.h"
#include <emscripten.h>
#include <emscripten/fiber.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- fibers */
#define MAX_THREADS 256
#define C_STACK (128 * 1024)
#define A_STACK (64 * 1024)
#define TAG 0x5C000000u

enum { S_FREE, S_STOPPED, S_READY, S_RUNNING, S_WAITING, S_DEAD };

typedef struct PcThread {
    N64Thread *t;
    emscripten_fiber_t fiber;
    void *entry, *arg;
    int state;
    N64MesgQueue *waitq;
    int waitsend;            /* blocked sending to a full queue (else receiving from an empty one) */
    unsigned long long seq;  /* FIFO order among equal priorities */
    unsigned switches;
} PcThread;

static PcThread slots[MAX_THREADS];
static char cstacks[MAX_THREADS][C_STACK] __attribute__((aligned(16)));
static char astacks[MAX_THREADS][A_STACK] __attribute__((aligned(16)));
static emscripten_fiber_t host_fiber;
static char host_astack[A_STACK] __attribute__((aligned(16)));
static int host_init;
static PcThread *cur;
static unsigned long long seq_counter;
static N64Thread *active_list;               /* __osGetActiveQueue: all created threads via tlnext */
static N64Thread thread_tail = {0, -1, 0, 0, 0, 0, 0, 0}; /* libultra's __osThreadTail sentinel */

/* measurements (docs/port/notes/S3a.md): fiber count and switches */
unsigned port_fiber_swaps, port_fiber_live, port_fiber_peak, port_fiber_created;

static PcThread *slot_of(N64Thread *t) {
    if (!t) return cur;
    uint32_t tag = t->thprof;
    if ((tag & 0xFFFF0000u) == TAG && (tag & 0xFFFFu) < MAX_THREADS) {
        PcThread *p = &slots[tag & 0xFFFFu];
        if (p->state != S_FREE && p->t == t) return p;
    }
    return NULL;
}

N64Thread *port_current_thread(void) { return cur ? cur->t : NULL; }

static void set_state(PcThread *p, int s) {
    static const uint16_t os[] = {0, OS_STATE_STOPPED_, OS_STATE_RUNNABLE_, OS_STATE_RUNNING_, OS_STATE_WAITING_,
                                  OS_STATE_STOPPED_};
    p->state = s;
    N64_THREAD_SET_STATE(p->t, os[s]);
}

static void make_ready(PcThread *p) {
    set_state(p, S_READY);
    p->waitq = NULL;
    p->seq = ++seq_counter;
}

static PcThread *pick(void) {
    PcThread *best = NULL;
    for (int i = 0; i < MAX_THREADS; i++) {
        PcThread *p = &slots[i];
        if (p->state != S_READY || p->t->priority <= 0) continue;
        if (!best || p->t->priority > best->t->priority || (p->t->priority == best->t->priority && p->seq < best->seq))
            best = p;
    }
    return best;
}

static void swap_to(emscripten_fiber_t *from, PcThread *to) {
    cur = to;
    if (to) {
        set_state(to, S_RUNNING);
        to->switches++;
    }
    port_fiber_swaps++;
    emscripten_fiber_swap(from, to ? &to->fiber : &host_fiber);
}

/* The current thread can no longer run (it blocked, stopped, yielded or was preempted): run the next one, or return
 * to the host context. Returns when the thread is resumed. */
static void dispatch(void) {
    PcThread *me = cur, *next = pick();
    if (next == me) { set_state(me, S_RUNNING); return; }
    swap_to(&me->fiber, next);
}

/* libultra preemption: called after making a thread runnable from the current one */
static void maybe_preempt(void) {
    if (!cur) return;
    PcThread *n = pick();
    if (n && n->t->priority > cur->t->priority) {
        make_ready(cur);
        dispatch();
    }
}

static void fiber_entry(void *arg) {
    PcThread *p = arg;
    port_call_vi(p->entry, (int)(uintptr_t)p->arg);
    /* the entry returned: libultra's __osCleanupThread destroys the thread */
    port_log("[os] thread %d returned from its entry: destroyed\n", (int)p->t->id);
    extern void osDestroyThread(void *t);
    osDestroyThread(NULL);
}

/* debug (S3c): every live thread with its entry (console address), priority, state, the queue it blocks on (console
 * address) and how often it ran since the previous dump. Exported; node: --threads F. */
EMSCRIPTEN_KEEPALIVE void port_thread_dump(void) {
    static const char *const sn[] = {"free", "stopped", "ready", "running", "waiting", "dead"};
    static unsigned last[MAX_THREADS];
    unsigned port_function_vram(void *fn);
    port_log("[threads] retrace %u\n", port_vi_count);
    for (int i = 0; i < MAX_THREADS; i++) {
        PcThread *p = &slots[i];
        if (p->state == S_FREE || p->state == S_DEAD) continue;
        port_log("[threads] slot %3d id %3d pri %3d %-8s entry %08X arg %08X wait %s %08X ran %u\n", i, (int)p->t->id,
                 (int)p->t->priority, sn[p->state], port_function_vram(p->entry), port_wasm_to_n64(p->arg),
                 p->waitq ? (p->waitsend ? "send" : "recv") : "-", p->waitq ? port_wasm_to_n64(p->waitq) : 0,
                 p->switches - last[i]);
        last[i] = p->switches;
    }
}

/* host side: run threads until none is runnable */
void port_run_threads(void) {
    if (!host_init) { emscripten_fiber_init_from_current_context(&host_fiber, host_astack, A_STACK); host_init = 1; }
    if (cur) { port_log("[os] port_run_threads called from a thread\n"); return; }
    PcThread *p;
    while ((p = pick())) swap_to(&host_fiber, p);
    cur = NULL;
}

/* ---------------------------------------------------------------- threads */
static void list_unlink(N64Thread *t) {
    for (N64Thread **pp = &active_list; *pp && *pp != &thread_tail; pp = &(*pp)->tlnext)
        if (*pp == t) { *pp = t->tlnext; return; }
}

void osCreateThread(void *tv, int id, void *entry, void *arg, void *sp, int pri) {
    N64Thread *t = tv;
    (void)sp; /* the console stack is unused: the fiber has its own C stack */
    PcThread *p = slot_of(t);
    if (p && p == cur) { port_log("[os] osCreateThread on the running thread %d\n", id); return; }
    if (!p)
        for (int i = 0; i < MAX_THREADS && !p; i++)
            if (slots[i].state == S_FREE || slots[i].state == S_DEAD) p = &slots[i];
    if (!p) { port_log("[os] out of thread slots (%d)\n", MAX_THREADS); abort(); }
    if (p->state == S_FREE || p->state == S_DEAD) {
        port_fiber_live++;
        if (port_fiber_live > port_fiber_peak) port_fiber_peak = port_fiber_live;
    } else {
        list_unlink(t);
    }
    port_fiber_created++;
    unsigned idx = (unsigned)(p - slots);
    memset(p, 0, sizeof *p);
    p->t = t;
    p->entry = entry;
    p->arg = arg;
    t->next = NULL;
    t->priority = pri;
    t->queue = NULL;
    t->id = id;
    t->fp = 0;
    t->thprof = TAG | idx;
    t->state_flags = 0;
    set_state(p, S_STOPPED);
    t->tlnext = active_list ? active_list : &thread_tail;
    active_list = t;
    emscripten_fiber_init(&p->fiber, fiber_entry, p, cstacks[idx], C_STACK, astacks[idx], A_STACK);
}

void osStartThread(void *tv) {
    PcThread *p = slot_of(tv);
    if (!p || p == cur) return;
    if (p->state == S_STOPPED || p->state == S_WAITING) {
        make_ready(p);
        maybe_preempt();
    }
}

void osStopThread(void *tv) {
    PcThread *p = slot_of(tv);
    if (!p) return;
    set_state(p, S_STOPPED);
    p->waitq = NULL;
    if (p == cur) dispatch();
}

void osDestroyThread(void *tv) {
    PcThread *p = slot_of(tv);
    if (!p) return;
    list_unlink(p->t);
    p->t->thprof = 0;
    port_fiber_live--;
    if (p == cur) {
        p->state = S_DEAD; /* the fiber is left now and never resumed; its slot is reused by a later create */
        PcThread *next = pick();
        swap_to(&p->fiber, next);
        port_log("[os] destroyed thread resumed\n");
        abort();
    }
    memset(p, 0, sizeof *p);
}

void osYieldThread(void) {
    if (!cur) return;
    make_ready(cur);
    dispatch();
}

void osSetThreadPri(void *tv, int pri) {
    PcThread *p = slot_of(tv);
    if (!p) return;
    if (p->t->priority == pri) return;
    p->t->priority = pri;
    if (p == cur) {
        PcThread *n = pick();
        if (pri <= 0 || (n && n->t->priority > pri)) { make_ready(cur); dispatch(); }
    } else if (p->state == S_READY) {
        maybe_preempt();
    }
}

int osGetThreadPri(void *tv) {
    PcThread *p = slot_of(tv);
    return p ? p->t->priority : 0;
}

void *__osGetActiveQueue(void) { return active_list ? active_list : &thread_tail; }
void *__osGetCurrFaultedThread(void) { return NULL; }

/* ---------------------------------------------------------------- message queues */
void osCreateMesgQueue(void *mqv, void *msg, int count) {
    N64MesgQueue *mq = mqv;
    mq->mtqueue = mq->fullqueue = &thread_tail;
    mq->validCount = 0;
    mq->first = 0;
    mq->msgCount = count;
    mq->msg = msg;
}

/* wake the best thread blocked on mq (sending or receiving) */
static PcThread *wake_one(N64MesgQueue *mq, int sender) {
    PcThread *best = NULL;
    for (int i = 0; i < MAX_THREADS; i++) {
        PcThread *p = &slots[i];
        if (p->state != S_WAITING || p->waitq != mq || p->waitsend != sender) continue;
        if (!best || p->t->priority > best->t->priority || (p->t->priority == best->t->priority && p->seq < best->seq))
            best = p;
    }
    if (best) make_ready(best);
    return best;
}

static void block_on(N64MesgQueue *mq, int sender) {
    cur->waitq = mq;
    cur->waitsend = sender;
    cur->seq = ++seq_counter;
    set_state(cur, S_WAITING);
    dispatch();
}

int pc_send(N64MesgQueue *mq, OSMesg_ msg, int block) {
    while (mq->validCount >= mq->msgCount) {
        if (!block || !cur) return -1;
        block_on(mq, 1);
    }
    int last = (mq->first + mq->validCount) % mq->msgCount;
    mq->msg[last] = msg;
    mq->validCount++;
    if (wake_one(mq, 0)) maybe_preempt();
    return 0;
}

int osSendMesg(void *mq, void *msg, int flag) { return pc_send(mq, msg, flag); }

int osJamMesg(void *mqv, void *msg, int flag) {
    N64MesgQueue *mq = mqv;
    while (mq->validCount >= mq->msgCount) {
        if (!flag || !cur) return -1;
        block_on(mq, 1);
    }
    mq->first = (mq->first + mq->msgCount - 1) % mq->msgCount;
    mq->msg[mq->first] = msg;
    mq->validCount++;
    if (wake_one(mq, 0)) maybe_preempt();
    return 0;
}

int osRecvMesg(void *mqv, void *msgp, int flag) {
    N64MesgQueue *mq = mqv;
    while (mq->validCount == 0) {
        if (!flag) return -1;
        if (!cur) { port_log("[os] blocking osRecvMesg outside a thread\n"); return -1; }
        block_on(mq, 0);
    }
    if (msgp) *(OSMesg_ *)msgp = mq->msg[mq->first];
    mq->first = (mq->first + 1) % mq->msgCount;
    mq->validCount--;
    if (wake_one(mq, 1)) maybe_preempt();
    return 0;
}

/* ---------------------------------------------------------------- events */
static struct { N64MesgQueue *mq; OSMesg_ msg; } events[N64_EVENT_COUNT];

void osSetEventMesg(int e, void *mq, void *msg) {
    if (e < 0 || e >= N64_EVENT_COUNT) return;
    events[e].mq = mq;
    events[e].msg = msg;
}

void pc_post_event(int e) {
    if (e >= 0 && e < N64_EVENT_COUNT && events[e].mq) pc_send(events[e].mq, events[e].msg, 0);
}

/* ---------------------------------------------------------------- interrupts, CPU, caches */
static uint32_t int_mask = 0x003FFF01u; /* OS_IM_ALL */
int osSetIntMask(int m) { int o = (int)int_mask; int_mask = (uint32_t)m; return o; }
void __osSetWatchLo(int v) { (void)v; }
void osInvalICache(void *p, int n) { (void)p; (void)n; }
/* The data cache does not exist here, but the renderer keeps colour images on the GPU (port/gfx, RENDERER.md): an
 * invalidate means the CPU is about to read [p, p+n) (GPU-drawn pixels are written into RDRAM first), a write-back
 * means the CPU wrote there (images in range are re-uploaded before their next use). KSEG0 addresses map to the
 * mirror. */
void port_gfx_inval_dcache(const void *p, uint32_t len);
void port_gfx_writeback_dcache(const void *p, uint32_t len);
static const void *cache_ptr(const void *p) {
    uintptr_t a = (uintptr_t)p;
    return a >= 0x80000000u ? (const void *)(n64_rdram + (a & (PORT_RDRAM_SIZE - 1))) : p;
}
void osInvalDCache(void *p, int n) { if (n > 0) port_gfx_inval_dcache(cache_ptr(p), (uint32_t)n); }
void osWritebackDCache(void *p, int n) { if (n > 0) port_gfx_writeback_dcache(cache_ptr(p), (uint32_t)n); }
void osWritebackDCacheAll(void) { port_gfx_writeback_dcache(0, 0); }

/* Pointers inside the RDRAM mirror give their N64 physical address (0..8 MiB), as on the console: the audio library
 * and the RCP command lists keep these as numbers. Any other pointer (a C object in wasm) is returned unchanged; every
 * consumer maps a value below 8 MiB back into the mirror and takes larger values as wasm addresses. */
int osVirtualToPhysical(void *p) {
    uintptr_t x = (uintptr_t)p, b = (uintptr_t)n64_rdram;
    if (x - b < PORT_RDRAM_SIZE) return (int)(x - b);
    if (x >= 0x80000000u && x < 0x80000000u + PORT_RDRAM_SIZE) return (int)(x & 0x1FFFFFFFu);
    return (int)x;
}

/* boot globals of the console's low memory (os_system.h): osTvType 0x80000300, osRomBase 0x80000308, osResetType
 * 0x8000030C, osMemSize 0x80000318 */
void osInitialize(void) {
    vbe_st32(n64_rdram + 0x300, 1);           /* OS_TV_NTSC */
    vbe_st32(n64_rdram + 0x308, 0xB0000000u);
    vbe_st32(n64_rdram + 0x30C, 0);           /* cold reset */
    vbe_st32(n64_rdram + 0x318, PORT_RDRAM_SIZE);
}

/* ---------------------------------------------------------------- time: a deterministic clock
 * The CPU counter runs at 46.875 MHz: 781250 counts per NTSC retrace. Here it is the retrace count times that, plus
 * a small step per read (so polling loops make progress), never past the next retrace. No wall time. */
#define COUNTS_PER_RETRACE 781250u
unsigned port_vi_count;
static unsigned count_step, count_vi;
static unsigned long long time_base;
static unsigned long long clock_now(void) {
    if (count_vi != port_vi_count) { count_vi = port_vi_count; count_step = 0; }
    if (count_step < COUNTS_PER_RETRACE - 64) count_step += 64;
    return (unsigned long long)port_vi_count * COUNTS_PER_RETRACE + count_step;
}
int osGetCount(void) { return (int)(uint32_t)clock_now(); }
long long osGetTime(void) { return (long long)(clock_now() - time_base); }
void osSetTime(long long t) { time_base = clock_now() - (unsigned long long)t; }
