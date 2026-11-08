/* PC layer: VI (retrace, framebuffers), SP / DP tasks, AI; session S3a. Design: docs/port/MEMORY.md "Threads".
 *
 * The host drives the retrace: port_vi_retrace() (one NTSC field, 60 Hz) shows the next framebuffer, posts the VI
 * event of osViSetEvent and runs the threads until none is runnable. osSpTaskStart (osSpTaskLoad + osSpTaskStartGo)
 * runs a task synchronously: graphics tasks go to port_gfx_task() (the renderer, port/gfx; a weak default here only
 * counts), audio tasks complete at once (audio HLE: S7). The SP-done event, then for graphics the DP full-sync event,
 * are posted right away; the scheduler thread handles them after it returns from osSpTaskStart, as it would a few
 * hundred microseconds later on the console. Written for this port. */
#include "pc_os.h"
#include <emscripten.h>
#include <emscripten/heap.h>
#include <string.h>

/* ---------------------------------------------------------------- hooks for the renderer (port/gfx) */
/* Run one graphics task (OSTask *, game memory): the renderer's strong definition replaces this one. */
__attribute__((weak)) void port_gfx_task(void *task) { (void)task; }
/* Called at every retrace with the framebuffer being displayed (the pointer the game gave osViSwapBuffer: an RDRAM
 * mirror address, or NULL before the first swap) and the VI state; the renderer's strong definition presents it. */
__attribute__((weak)) void port_vi_present(void *framebuffer, const void *vi_mode, int black) {
    (void)framebuffer; (void)vi_mode; (void)black;
}

/* ---------------------------------------------------------------- VI */
static void *vi_cur, *vi_next;
static const void *vi_mode;
static int vi_black = 1;
static float vi_yscale = 1.0f;
static N64MesgQueue *vi_mq;
static OSMesg_ vi_msg;
static unsigned vi_every = 1, vi_ctr;
unsigned port_vi_swaps;

void osCreateViManager(int pri) { (void)pri; }
void osViSetMode(void *mode) { vi_mode = mode; }
void osViBlack(int on) { vi_black = on != 0; }
void osViSetYScale(float y) { vi_yscale = y; }
void osViSetEvent(void *mq, void *msg, int retraceCount) {
    vi_mq = mq;
    vi_msg = msg;
    vi_every = retraceCount > 0 ? (unsigned)retraceCount : 1;
    vi_ctr = 0;
}
void osViSwapBuffer(void *fb) { vi_next = fb; port_vi_swaps++; }
void *osViGetCurrentFramebuffer(void) { return vi_cur; }
void *osViGetNextFramebuffer(void) { return vi_next; }
void *port_vi_framebuffer(void) { return vi_cur; }

void port_run_threads(void);

/* one retrace (exported: the page calls it once per display frame at 60 Hz; node's main() loops it) */
EMSCRIPTEN_KEEPALIVE void port_vi_retrace(void) {
    port_vi_count++;
    vi_cur = vi_next;
    port_vi_present(vi_cur, vi_mode, vi_black);
    if (vi_mq && ++vi_ctr >= vi_every) {
        vi_ctr = 0;
        pc_send(vi_mq, vi_msg, 0);
    }
    port_run_threads();
}

/* ---------------------------------------------------------------- display-list census (test aid) */
/* Address in a command list -> wasm pointer: wasm addresses (C objects, >= 0x10000000) as is, KSEG0 / physical
 * addresses into the mirror, segmented addresses through the G_MW_SEGMENT table. */
static uint32_t seg_base[16];
static const uint32_t *dl_ptr(uint32_t a) {
    uint32_t heap = (uint32_t)emscripten_get_heap_size();
    for (int depth = 0; depth < 4; depth++) {
        if (a >= 0x10000000u && a < 0x80000000u) return a + 8 <= heap ? (const uint32_t *)(uintptr_t)a : NULL;
        if (a >= 0x80000000u) a &= 0x1FFFFFFFu;
        uint32_t s = (a >> 24) & 0xF;
        if (!s) return a + 8 <= PORT_RDRAM_SIZE ? (const uint32_t *)(n64_rdram + a) : NULL;
        a = seg_base[s] + (a & 0x00FFFFFFu);
    }
    return NULL;
}

static const char *const op_names[256] = {
    [0x00] = "G_NOOP", [0x01] = "G_VTX", [0x02] = "G_MODIFYVTX", [0x03] = "G_CULLDL", [0x04] = "G_BRANCH_Z",
    [0x05] = "G_TRI1", [0x06] = "G_TRI2", [0x07] = "G_QUAD", [0x08] = "G_LINE3D", [0xD3] = "G_SPECIAL_3",
    [0xD4] = "G_SPECIAL_2", [0xD5] = "G_SPECIAL_1", [0xD6] = "G_DMA_IO", [0xD7] = "G_TEXTURE", [0xD8] = "G_POPMTX",
    [0xD9] = "G_GEOMETRYMODE", [0xDA] = "G_MTX", [0xDB] = "G_MOVEWORD", [0xDC] = "G_MOVEMEM", [0xDD] = "G_LOAD_UCODE",
    [0xDE] = "G_DL", [0xDF] = "G_ENDDL", [0xE0] = "G_SPNOOP", [0xE1] = "G_RDPHALF_1", [0xE2] = "G_SETOTHERMODE_L",
    [0xE3] = "G_SETOTHERMODE_H", [0xE4] = "G_TEXRECT", [0xE5] = "G_TEXRECTFLIP", [0xE6] = "G_RDPLOADSYNC",
    [0xE7] = "G_RDPPIPESYNC", [0xE8] = "G_RDPTILESYNC", [0xE9] = "G_RDPFULLSYNC", [0xEA] = "G_SETKEYGB",
    [0xEB] = "G_SETKEYR", [0xEC] = "G_SETCONVERT", [0xED] = "G_SETSCISSOR", [0xEE] = "G_SETPRIMDEPTH",
    [0xEF] = "G_RDPSETOTHERMODE", [0xF0] = "G_LOADTLUT", [0xF1] = "G_RDPHALF_2", [0xF2] = "G_SETTILESIZE",
    [0xF3] = "G_LOADBLOCK", [0xF4] = "G_LOADTILE", [0xF5] = "G_SETTILE", [0xF6] = "G_FILLRECT",
    [0xF7] = "G_SETFILLCOLOR", [0xF8] = "G_SETFOGCOLOR", [0xF9] = "G_SETBLENDCOLOR", [0xFA] = "G_SETPRIMCOLOR",
    [0xFB] = "G_SETENVCOLOR", [0xFC] = "G_SETCOMBINE", [0xFD] = "G_SETTIMG", [0xFE] = "G_SETZIMG",
    [0xFF] = "G_SETCIMG"};

typedef struct DlStats { unsigned ops[256], cmds, bad, depth_max, addr_log, naddr; uint32_t hash; } DlStats;

/* walk a task's display list (F3DEX2 / L3DEX2 encoding), following G_DL calls and branches; FNV-1a over the words */
static void dl_walk(uint32_t start, DlStats *st) {
    uint32_t stack[18];
    int sp = 0;
    const uint32_t *p = dl_ptr(start);
    memset(seg_base, 0, sizeof seg_base);
    st->hash = 2166136261u;
    while (p && st->cmds < 200000) {
        uint32_t w0 = p[0], w1 = p[1], op = w0 >> 24;
        st->ops[op]++;
        st->cmds++;
        st->hash = (st->hash ^ w0) * 16777619u;
        st->hash = (st->hash ^ w1) * 16777619u;
        p += 2;
        if (st->addr_log && st->naddr < 24 && (op == 0xFD || op == 0xFE || op == 0xFF || op == 0x01 || op == 0xDA ||
                                               (op == 0xDB && ((w0 >> 16) & 0xFF) == 0x06)))
            port_log("[dlhist]   %-14s %08X %08X\n", op_names[op], w0, w1), st->naddr++;
        if (op == 0xDB && ((w0 >> 16) & 0xFF) == 0x06) seg_base[((w0 & 0xFFFF) >> 2) & 0xF] = w1;
        else if (op == 0xDE) {
            const uint32_t *t = dl_ptr(w1);
            if (!t) { st->bad++; continue; }
            if (!((w0 >> 16) & 0xFF)) {
                if (sp >= 18) { st->bad++; break; }
                stack[sp++] = (uint32_t)(uintptr_t)p;
                if ((unsigned)sp > st->depth_max) st->depth_max = (unsigned)sp;
            }
            p = t;
        } else if (op == 0xDF) {
            if (!sp) break;
            p = (const uint32_t *)(uintptr_t)stack[--sp];
        }
    }
    if (!p) st->bad++;
}

static void dl_histogram(const DlStats *st, unsigned task_no) {
    port_log("[dlhist] first graphics task (#%u, retrace %u): %u commands, max call depth %u, %u unresolved, "
             "hash %08X\n", task_no, port_vi_count, st->cmds, st->depth_max, st->bad, st->hash);
    for (int i = 0; i < 256; i++)
        if (st->ops[i]) port_log("[dlhist] %02X %-18s %u\n", i, op_names[i] ? op_names[i] : "?", st->ops[i]);
}

/* ---------------------------------------------------------------- SP / DP */
static N64Task *sp_loaded;
unsigned port_gfx_tasks, port_audio_tasks, port_other_tasks;

void osSpTaskLoad(void *t) { sp_loaded = t; }

void osSpTaskStartGo(void *tv) {
    N64Task *t = tv;
    if (t != sp_loaded) port_log("[sp] osSpTaskStartGo without osSpTaskLoad\n");
    if (t->type == M_GFXTASK_) {
        DlStats st;
        memset(&st, 0, sizeof st);
        st.addr_log = port_gfx_tasks == 0 || port_gfx_tasks == 199; /* address forms of tasks #1 and #200 */
        if (st.addr_log) port_log("[dlhist] task #%u: address operands (image / vertex / matrix / segment)\n", port_gfx_tasks + 1);
        dl_walk((uint32_t)(uintptr_t)t->data_ptr, &st);
        port_gfx_tasks++;
        if (port_gfx_tasks == 1) dl_histogram(&st, port_gfx_tasks);
        port_log("[task] vi %u gfx #%u dl %08X ucode %08X: %u commands, hash %08X\n", port_vi_count, port_gfx_tasks,
                 (uint32_t)(uintptr_t)t->data_ptr, port_wasm_to_n64(t->ucode), st.cmds, st.hash);
        port_gfx_task(t);
        pc_post_event(N64_EVENT_SP);
        pc_post_event(N64_EVENT_DP);
    } else {
        if (t->type == M_AUDTASK_) port_audio_tasks++;
        else { port_other_tasks++; port_log("[sp] task type %u\n", t->type); }
        pc_post_event(N64_EVENT_SP);
    }
}

void osSpTaskYield(void) {}
int osSpTaskYielded(void *t) { (void)t; return 0; } /* tasks finish inside osSpTaskStartGo: never yielded */

/* RDP-only command list (the scheduler's FIFO queue-3 path): nothing to run yet; the full-sync event follows */
int osDpSetNextBuffer(void *buf, long long size) {
    (void)buf; (void)size;
    pc_post_event(N64_EVENT_DP);
    return 0;
}

/* ---------------------------------------------------------------- AI (audio output: S7) */
int osAiSetFrequency(int f) {
    const unsigned clock = 48681812u; /* VI_NTSC_CLOCK */
    if (f <= 0) return 0;
    unsigned dac = (unsigned)((double)clock / f + 0.5);
    if (dac < 132) return -1;
    return (int)(clock / dac);
}
int osAiSetNextBuffer(void *buf, int size) { (void)buf; (void)size; return 0; }
