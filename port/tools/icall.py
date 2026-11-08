"""Safe indirect calls (docs/port/MEMORY.md, "Indirect calls"). Session S3a.

Provenance: adapted from the Conker web port's port/tools/icall.py (the user's own project, no licence file, same
author). Changes for Snap: no pass-through (asm-stub) trampolines (Snap has no game stubs); definitions are parsed
with sigs.parse_define (balanced parentheses); a function-pointer fix-up for pointers that are console / mirror
addresses (the address -> function table, pc_icall.c); per-TU registration of each function's console address.

wasm `call_indirect` traps when the caller's function-pointer type differs from the target's definition. IDO-era
code does this all the time (handler tables of mixed `void f(void)` / `void f(s32)` / `f32 f(s32)` entries called
through one pointer type). On MIPS o32 such a call just works: arguments sit in a0-a3 / f12 / f14, the result in v0/f0.

Approach: site-typed trampolines with a runtime signature check.
  * Every indirect call in a game TU, `call R %fp(T0 a0, ...)`, becomes a direct call to a trampoline
    `@__icall.<hash>(ptr %fp, T0 a0, ...)` that has exactly the site's type. Function pointers stay real table
    entries (identity, comparisons and the PC layer are unaffected).
  * A pointer that is not a table index (>= 1 MiB: a console KSEG0 address or an RDRAM-mirror address of a
    function, computed at run time or read from data the relocations did not cover) goes through
    `__port_fptr_fix`, which maps it to the function of the overlay resident at that address.
  * The trampoline asks `__port_fsig(fp)` for the target's wasm signature id (filled at startup by per-TU
    constructors that register every function the TU defines).
  * Same signature (or unknown target): plain call_indirect, the fast path.
  * Different signature: the arguments go into an i64 "register file" with a kind mask, and the generated
    `__port_icall_slow` (port/tools/icall_gen.py) calls the target with its own type, converting each argument and
    the result the way port/tools/sigs.py does for direct calls (int<->int sext/trunc, float<->int bitcast,
    missing -> 0, extra -> dropped, void result -> 0).
Signature ids are a CRC of the *wasm-level* signature ("i:if" = i32(i32,f32)), which is what call_indirect checks.
"""
import re, zlib, hashlib
from sigs import site_ret, site_args, ptypes, parse_define

ICALL = re.compile(r'^(\s*)(?:(%[\w.$-]+) = )?((?:tail |musttail |notail )?call )(.*?)(%[\w.$-]+)\((.*)\)([^)]*)$')
WT = {'i1': 'i', 'i8': 'i', 'i16': 'i', 'i32': 'i', 'ptr': 'i', 'i64': 'I', 'float': 'f', 'double': 'd'}
KIND = {'i': 0, 'I': 1, 'f': 2, 'd': 3}
MAXARGS = 16
FIX_LIMIT = 1 << 20  # function-table indices are below this; anything above is an address to translate


def wsig(ret, params):
    """LLVM types -> wasm-level signature string, or None if not representable."""
    r = 'v' if ret == 'void' else WT.get(ret)
    ps = [WT.get(p) for p in params]
    if r is None or None in ps:
        return None
    return r + ':' + ''.join(ps)


def sig_id(ws):
    return (zlib.crc32(ws.encode()) & 0x7FFFFFFF) | 1


def tramp_name(ret, params):
    return '@__icall.' + hashlib.md5((ret + '(' + ','.join(params) + ')').encode()).hexdigest()[:12]


def rewrite(lines, stats):
    """Rewrite indirect call sites. Returns (lines, {trampoline name: (ret, params)})."""
    out, need = [], {}
    for l in lines:
        if l.startswith('  ') and 'call ' in l and '@' not in l.split('call ', 1)[1].split('(', 1)[0]:
            m = ICALL.match(l)
            if m and '...' not in m[4] and 'asm ' not in m[4]:
                ret = site_ret(m[4])
                sa = site_args(m[6])
                params = [t for t, _ in sa]
                if ret and len(params) <= MAXARGS and wsig(ret, params):
                    name = tramp_name(ret, params)
                    need[name] = (ret, params)
                    args = ', '.join([f'ptr {m[5]}'] + [f'{t} {v}' for t, v in sa])
                    pre = m[3].replace('musttail ', 'tail ')
                    res = f'{m[2]} = ' if m[2] else ''
                    out.append(f'{m[1]}{res}{pre}{ret} {name}({args}){m[7]}')
                    stats['icall'] += 1
                    continue
                stats['icall_skip'] += 1
        out.append(l)
    return out, need


def trampoline(name, ret, params):
    n = len(params)
    ws = wsig(ret, params)
    sid = sig_id(ws)
    kinds = 0
    for i, p in enumerate(params):
        kinds |= KIND[WT[p]] << (2 * i)
    rkind = 4 if ret == 'void' else KIND[WT[ret]]
    formal = ', '.join(['ptr %fp0'] + [f'{t} %a{i}' for i, t in enumerate(params)])
    actual = ', '.join(f'{t} %a{i}' for i, t in enumerate(params))
    L = [f'define linkonce_odr hidden {ret} {name}({formal}) {{',
         'entry:',
         f'  %buf = alloca [{max(n, 1)} x i64], align 8',
         '  %fi = ptrtoint ptr %fp0 to i32',
         f'  %small = icmp ult i32 %fi, {FIX_LIMIT}',
         '  br i1 %small, label %go, label %fix',
         'fix:',
         '  %fp1 = call ptr @__port_fptr_fix(ptr %fp0)',
         '  br label %go',
         'go:',
         '  %fp = phi ptr [ %fp0, %entry ], [ %fp1, %fix ]',
         '  %s = call i32 @__port_fsig(ptr %fp)',
         f'  %m = icmp eq i32 %s, {sid}',
         '  %z = icmp eq i32 %s, 0',
         '  %f = or i1 %m, %z',
         '  br i1 %f, label %fast, label %slow',
         'fast:']
    if ret == 'void':
        L += [f'  call void %fp({actual})', '  ret void']
    else:
        L += [f'  %r = call {ret} %fp({actual})', f'  ret {ret} %r']
    L.append('slow:')
    for i, t in enumerate(params):
        a, c = f'%a{i}', f'%c{i}'
        if t == 'i64':
            c = a
        elif t == 'i1':
            L.append(f'  {c} = zext i1 {a} to i64')
        elif t in ('i8', 'i16', 'i32'):
            L.append(f'  {c} = sext {t} {a} to i64')
        elif t == 'ptr':
            L.append(f'  {c} = ptrtoint ptr {a} to i64')
        elif t == 'float':
            L += [f'  %b{i} = bitcast float {a} to i32', f'  {c} = zext i32 %b{i} to i64']
        elif t == 'double':
            L.append(f'  {c} = bitcast double {a} to i64')
        L += [f'  %p{i} = getelementptr inbounds [{max(n, 1)} x i64], ptr %buf, i32 0, i32 {i}',
              f'  store i64 {c}, ptr %p{i}, align 8']
    L.append(f'  %rv = call i64 @__port_icall_slow(ptr %fp, i32 %s, ptr %buf, i32 {n}, i32 {kinds}, i32 {rkind})')
    if ret == 'void':
        L.append('  ret void')
    elif ret == 'i64':
        L.append('  ret i64 %rv')
    elif ret == 'double':
        L += ['  %rd = bitcast i64 %rv to double', '  ret double %rd']
    elif ret == 'float':
        L += ['  %ri = trunc i64 %rv to i32', '  %rf = bitcast i32 %ri to float', '  ret float %rf']
    elif ret == 'ptr':
        L += ['  %ri = trunc i64 %rv to i32', '  %rp = inttoptr i32 %ri to ptr', '  ret ptr %rp']
    else:
        L += [f'  %rt = trunc i64 %rv to {ret}', f'  ret {ret} %rt']
    L.append('}')
    return L


def registrations(lines):
    """Every function this TU defines -> [(name, wasm sig)] (variadic / aggregate ones skipped)."""
    out = []
    for l in lines:
        if l.startswith('define '):
            d = parse_define(l)
            if not d or d[3].startswith(('@__icall.', '@__vbe', '@llvm.')):
                continue
            ps = ptypes(d[4])
            if '...' in ps:
                continue
            ws = wsig(d[2], ps)
            if ws:
                out.append((d[3], ws))
    return out


def epilogue(text, need, regs, vrams):
    """IR appended after the vbe pass: trampolines, the signature registration table, the console-address table
    ({fn, vram, segment} for every function of this TU whose matching-build address is known: `vrams` maps
    '@name' -> (vram, segment index)) and their constructor."""
    L = []
    for name in sorted(need):
        L += trampoline(name, *need[name])
    if need:
        L += ['declare i32 @__port_fsig(ptr)', 'declare i64 @__port_icall_slow(ptr, i32, ptr, i32, i32, i32)',
              'declare ptr @__port_fptr_fix(ptr)']
    ctor = None
    if regs:
        ents = ', '.join(f'{{ ptr, i32 }} {{ ptr {n}, i32 {sig_id(w)} }}' for n, w in regs)
        va = [(n, vrams[n]) for n, _ in regs if n in vrams]
        L += [f'@__icall_reg.tab = private constant [{len(regs)} x {{ ptr, i32 }}] [{ents}]',
              'declare void @__port_fsig_reg(ptr, i32)']
        if va:
            vents = ', '.join(f'{{ ptr, i32, i32 }} {{ ptr {n}, i32 {v}, i32 {s} }}' for n, (v, s) in va)
            L += [f'@__icall_vram.tab = private constant [{len(va)} x {{ ptr, i32, i32 }}] [{vents}]',
                  'declare void @__port_fvram_reg(ptr, i32)']
        L += ['define internal void @__icall_ctor() {',
              f'  call void @__port_fsig_reg(ptr @__icall_reg.tab, i32 {len(regs)})']
        if va:
            L.append(f'  call void @__port_fvram_reg(ptr @__icall_vram.tab, i32 {len(va)})')
        L += ['  ret void', '}']
        ctor = '{ i32, ptr, ptr } { i32 101, ptr @__icall_ctor, ptr null }'
    if ctor:
        old = re.search(r'^@llvm\.global_ctors = appending global \[(\d+) x \{ i32, ptr, ptr \}\] \[(.*)\]$', text, re.M)
        if old:
            text = text[:old.start()] + (f'@llvm.global_ctors = appending global [{int(old[1]) + 1} x {{ i32, ptr, ptr }}] '
                                         f'[{old[2]}, {ctor}]') + text[old.end():]
        else:
            L.append(f'@llvm.global_ctors = appending global [1 x {{ i32, ptr, ptr }}] [{ctor}]')
    return text + '\n'.join(L) + '\n'
