/* Indirect calls (docs/port/MEMORY.md "Indirect calls"; port/tools/icall.py, icall_gen.py). Session S3a.
 * Provenance: the signature registry follows the Conker web port's pc_icall.c (the user's own project); the
 * address -> function table is new for Snap.
 *
 *  - Signature registry: a wasm function pointer is its index in the function table; fsig[index] holds the wasm
 *    signature id of the function there (0 = unknown), filled before main by every vbe-compiled TU's constructor.
 *  - Address -> function table: every compiled function with a matching-build address registers {fn, console
 *    address, segment}. A function pointer that is a console address (KSEG0) or an RDRAM-mirror address (computed at
 *    run time, or from data no relocation covered) maps to the function at that address of the overlay resident
 *    there (overlays share VRAM: pc_mem.c tracks which one was loaded last). */
#include "pc_mem.h"
#include "pc_icall.h"
#include <stdlib.h>

#define FSIG_MAX (1 << 16)
static unsigned fsig[FSIG_MAX];
static unsigned fsig_regs, fsig_over, fsig_clash;
unsigned port_icall_slow_calls;

void __port_fsig_reg(const PortFsig *t, int n) {
    for (int i = 0; i < n; i++) {
        uintptr_t idx = (uintptr_t)t[i].fn;
        if (idx >= FSIG_MAX) { fsig_over++; continue; }
        if (fsig[idx] && fsig[idx] != t[i].sig) fsig_clash++;
        fsig[idx] = t[i].sig;
        fsig_regs++;
    }
}

unsigned __port_fsig(void *fp) {
    uintptr_t idx = (uintptr_t)fp;
    return idx < FSIG_MAX ? fsig[idx] : 0;
}

/* ---------------------------------------------------------------- address -> function */
#define FVRAM_MAX 8192
static PortFvram fvram[FVRAM_MAX];
static unsigned nfvram, fvram_sorted, fptr_fixes, fptr_fails;

void __port_fvram_reg(const PortFvram *t, int n) {
    for (int i = 0; i < n && nfvram < FVRAM_MAX; i++) fvram[nfvram++] = t[i];
    fvram_sorted = 0;
}

static int fv_cmp(const void *a, const void *b) {
    const PortFvram *x = a, *y = b;
    if (x->vram != y->vram) return x->vram < y->vram ? -1 : 1;
    return x->segment < y->segment ? -1 : x->segment > y->segment;
}

/* the function compiled for console address `vram` in the overlay resident there (NULL if none) */
void *port_n64_function(uint32_t vram) {
    if (!fvram_sorted) { qsort(fvram, nfvram, sizeof fvram[0], fv_cmp); fvram_sorted = 1; }
    unsigned lo = 0, hi = nfvram;
    while (lo < hi) {
        unsigned m = (lo + hi) / 2;
        if (fvram[m].vram < vram) lo = m + 1; else hi = m;
    }
    void *any = NULL;
    unsigned nany = 0;
    for (unsigned i = lo; i < nfvram && fvram[i].vram == vram; i++) {
        if (port_segment_resident(fvram[i].segment)) return fvram[i].fn;
        any = fvram[i].fn;
        nany++;
    }
    return nany == 1 ? any : NULL; /* not resident (yet): unambiguous only if one overlay has a function there */
}

/* the function at `vram` only if its overlay is resident there (NULL otherwise) */
void *port_n64_function_resident(uint32_t vram) {
    void *f = port_n64_function(vram);
    const PortFvram *e = NULL;
    for (unsigned i = 0; f && i < nfvram && !e; i++)
        if (fvram[i].fn == f && fvram[i].vram == vram) e = &fvram[i];
    return e && port_segment_resident(e->segment) ? f : NULL;
}

void *__port_fptr_fix(void *fp) {
    uintptr_t a = (uintptr_t)fp, b = (uintptr_t)n64_rdram;
    uint32_t vram = 0;
    if (a - b < PORT_RDRAM_SIZE) vram = 0x80000000u + (uint32_t)(a - b);
    else if (a >= 0x80000000u && a < 0x80000000u + PORT_RDRAM_SIZE) vram = (uint32_t)a;
    void *f = vram ? port_n64_function(vram) : NULL;
    if (f) {
        if (fptr_fixes++ < 16)
            port_log("[icall] function pointer %08X -> fn #%u (console %08X, overlay %s)\n", (unsigned)a,
                     (unsigned)(uintptr_t)f, vram, port_resident_at(vram) ? port_resident_at(vram) : "?");
        return f;
    }
    if (fptr_fails++ < 16) port_log("[icall] function pointer %08X: no function there (console %08X)\n", (unsigned)a, vram);
    return fp;
}

/* S5: canonical form of a pointer for an equality test (vbe.py fptr_eq): a console or mirror address that is the
 * start of a function of the overlay resident there -> that function (its table index); anything else unchanged.
 * A bitmap of every compiled function's console start address keeps the common case (data pointers) cheap. */
static unsigned char fstart_bits[PORT_RDRAM_SIZE / 4 / 8];
static unsigned fstart_built, fptr_canons;

static void fstart_build(void) {
    for (unsigned i = 0; i < nfvram; i++) {
        uint32_t o = fvram[i].vram - 0x80000000u;
        if (o < PORT_RDRAM_SIZE) fstart_bits[o >> 5] |= (unsigned char)(1u << ((o >> 2) & 7));
    }
    fstart_built = nfvram;
}

void *__port_fptr_canon(void *p) {
    uintptr_t a = (uintptr_t)p, b = (uintptr_t)n64_rdram;
    uint32_t o;
    if (a - b < PORT_RDRAM_SIZE) o = (uint32_t)(a - b);
    else if (a >= 0x80000000u && a < 0x80000000u + PORT_RDRAM_SIZE) o = (uint32_t)(a - 0x80000000u);
    else return p;
    if (fstart_built != nfvram) fstart_build();
    if ((o & 3) || !(fstart_bits[o >> 5] & (1u << ((o >> 2) & 7)))) return p;
    void *f = port_n64_function_resident(0x80000000u + o);
    if (!f) return p;
    if (fptr_canons++ < 8)
        port_log("[icall] pointer compare: %08X is fn #%u (console %08X)\n", (unsigned)a, (unsigned)(uintptr_t)f,
                 0x80000000u + o);
    return f;
}

/* ---------------------------------------------------------------- calls from the PC layer */
void port_call_vi(void *fp, int a0) {
    if ((uintptr_t)fp >= (1u << 20)) fp = __port_fptr_fix(fp);
    unsigned s = __port_fsig(fp);
    if (s == 0 || s == port_sig_vi) {
        ((void (*)(int))fp)(a0);
        return;
    }
    u64_ b[1] = {(u64_)(long long)a0};
    __port_icall_slow(fp, s, b, 1, 0, 4);
}

static const char *site_str(int n, unsigned k, unsigned rk, char *buf) {
    static const char L[] = "iIfd";
    char *p = buf;
    *p++ = rk == 4 ? 'v' : L[rk];
    *p++ = ':';
    for (int i = 0; i < n && i < 16; i++) *p++ = L[(k >> (2 * i)) & 3];
    *p = 0;
    return buf;
}

unsigned port_function_vram(void *fn);
/* A void function called through a value-returning pointer reads 0 here, v0 on the N64 (Conker P373): logged once
 * per target so each can be audited. */
#define VOID_SEEN_MAX 256
static void *void_seen[VOID_SEEN_MAX];
static unsigned void_nseen;
void port_icall_note(void *fp, unsigned tsig, int n, unsigned k, unsigned rk) {
    char b[24];
    if (rk != 4 && port_fsig_name(tsig)[0] == 'v') {
        unsigned i = 0;
        for (; i < void_nseen && void_seen[i] != fp; i++) {}
        if (i == void_nseen && void_nseen < VOID_SEEN_MAX) {
            void_seen[void_nseen++] = fp;
            port_log("[icall] void target fn #%u (console %08X, %s) called as %s: reads 0 here, v0 on the N64 (audit)\n",
                     (unsigned)(uintptr_t)fp, port_function_vram(fp), port_fsig_name(tsig), site_str(n, k, rk, b));
        }
    }
    if (port_icall_slow_calls++ < 16)
        port_log("[icall] signature mismatch: fn #%u is %s, called as %s (converted)\n", (unsigned)(uintptr_t)fp,
                 port_fsig_name(tsig), site_str(n, k, rk, b));
}

void port_icall_report(void) {
    port_log("[icall] %u functions registered (%u beyond the table cap, %u re-registered with another signature), "
             "%u console addresses; %u mismatched calls converted, %u void targets called as values; %u function "
             "pointers mapped from addresses, %u not found; %u pointer compares saw a function address\n", fsig_regs,
             fsig_over, fsig_clash, nfvram, port_icall_slow_calls, void_nseen, fptr_fixes, fptr_fails, fptr_canons);
}

/* function (table index) -> its console address, 0 if unknown (debug: port_thread_dump, S3c) */
unsigned port_function_vram(void *fn) {
    for (unsigned i = 0; i < nfvram; i++)
        if (fvram[i].fn == fn) return fvram[i].vram;
    return 0;
}
