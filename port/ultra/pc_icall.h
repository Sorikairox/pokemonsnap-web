/* Safe indirect calls (docs/port/MEMORY.md, "Indirect calls"; port/tools/icall.py). Session S3a.
 * Provenance: the Conker web port's pc_icall.h (the user's own project); Snap additions: the address -> function
 * table (PortFvram, __port_fptr_fix). Shared by port/ultra/pc_icall.c and the generated port/build/gen/icall_tab.c.
 * No libc headers. */
#ifndef PC_ICALL_H
#define PC_ICALL_H

typedef struct { void *fn; unsigned sig; } PortFsig;
typedef struct { void *fn; unsigned vram, segment; } PortFvram;
typedef unsigned long long u64_;

void __port_fsig_reg(const PortFsig *t, int n);   /* per-TU constructors */
void __port_fvram_reg(const PortFvram *t, int n); /* per-TU constructors: console address of each function */
unsigned __port_fsig(void *fp);                   /* 0 = unknown target */
void *__port_fptr_fix(void *fp);                  /* console / mirror address -> function of the resident overlay */
void *__port_fptr_canon(void *p);                 /* S5: pointer equality: a function address -> that function */
void *port_n64_function(unsigned vram);
u64_ __port_icall_slow(void *fp, unsigned tsig, const u64_ *b, int n, unsigned k, unsigned rk);
void port_icall_note(void *fp, unsigned tsig, int n, unsigned k, unsigned rk);
const char *port_fsig_name(unsigned id);
void port_icall_report(void);
extern unsigned port_icall_slow_calls;
extern const unsigned port_sig_vi;                /* sig_id("v:i") (icall_tab.c) */
extern const unsigned port_sig_ii;                /* sig_id("i:i"): a void entry made to return its v0 (S5) */

/* argument conversion: slot i of the site's register file (kind 0 i32, 1 i64, 2 f32, 3 f64) to the
 * target's parameter type; the same rules as port/tools/sigs.py conv() for direct calls */
#define KIND_(k, i) (((k) >> (2 * (i))) & 3)
static inline unsigned lo32_(u64_ v) { return (unsigned)v; }
static inline float f_of_(unsigned u) { union { unsigned u; float f; } x; x.u = u; return x.f; }
static inline unsigned u_of_(float f) { union { unsigned u; float f; } x; x.f = f; return x.u; }
static inline double d_of_(u64_ u) { union { u64_ u; double d; } x; x.u = u; return x.d; }
static inline u64_ u64_of_(double d) { union { u64_ u; double d; } x; x.d = d; return x.u; }

static inline int A_i(const u64_ *b, int n, unsigned k, int i) {
    if (i >= n) return 0;
    return KIND_(k, i) == 3 ? 0 : (int)lo32_(b[i]);           /* i32/i64 trunc, f32 bitcast */
}
static inline long long A_I(const u64_ *b, int n, unsigned k, int i) {
    if (i >= n) return 0;
    switch (KIND_(k, i)) { case 0: case 1: case 3: return (long long)b[i]; }  /* sext, -, bitcast */
    return 0;
}
static inline float A_f(const u64_ *b, int n, unsigned k, int i) {
    if (i >= n) return 0;
    switch (KIND_(k, i)) { case 0: case 2: return f_of_(lo32_(b[i])); case 3: return (float)d_of_(b[i]); }
    return 0;
}
static inline double A_d(const u64_ *b, int n, unsigned k, int i) {
    if (i >= n) return 0;
    switch (KIND_(k, i)) { case 1: case 3: return d_of_(b[i]); case 2: return (double)f_of_(lo32_(b[i])); }
    return 0;
}
/* result conversion: the target's result to the site's kind rk (4 = void), as i64 bits */
static inline u64_ R_i(int r, unsigned rk) {
    switch (rk) { case 0: case 1: return (u64_)(long long)r; case 2: return (unsigned)r; }
    return 0;
}
static inline u64_ R_I(long long r, unsigned rk) {
    switch (rk) { case 0: case 1: case 3: return (u64_)r; }
    return 0;
}
static inline u64_ R_f(float r, unsigned rk) {
    switch (rk) { case 0: case 2: return u_of_(r); case 3: return u64_of_((double)r); }
    return 0;
}
static inline u64_ R_d(double r, unsigned rk) {
    switch (rk) { case 1: case 3: return u64_of_(r); case 2: return u_of_((float)r); }
    return 0;
}

#endif
