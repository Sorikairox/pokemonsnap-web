"""Void functions that leave a callee's result in v0 (session S5; docs/port/MEMORY.md "Calls: v0 of void functions").

On MIPS o32 a `void` function whose every return is reached right after a call of a value-returning function still
hands that value to its caller in v0, and IDO-era game code reads it (Snap: 22 Pokemon spawn wrappers
`void f(...) { Pokemon_Spawn(...); }` called through `GObj* (*init)(...)`; pokemonAdd() stops at the first NULL). In
wasm a void function has no result (the port read 0).

Rule (general, no per-function edits): a void function F is *promoted* to return i32 when
  1. the matching build's IDO object shows v0 at every `jr ra` of F comes only from calls (port/symbols/v0.tsv,
     port/tools/v0_scan.py: ground truth from the console code, not the C),
  2. the -O0 IR of F agrees: the set of callees whose call is the last one before a return (over all paths) equals
     that set (so the C and the console code are the same shape; an IR path with no call at all is accepted when the
     console has none: IDO proved it impossible, e.g. a counted loop rotated to run its body once first), and
  3. every such callee leaves its result in v0: it returns an integer or pointer (i8/i16/i32/ptr), or is itself a
     promoted void function (fixpoint).
A promoted F returns, at each `ret void`, the result of the last call on the path that reached it (phi nodes over the
control flow, as v0 flows on the console). Callers that use F as void are unchanged (the result is dropped); callers
that read a value (direct sites after sigs.py, indirect calls whose site type returns a value) get the console's v0.
Every other void function called for a value still reads 0 and is listed in port/v0_report.tsv (static) and logged at
run time ([icall] void target ... called as ...).
"""
import re

LABEL = re.compile(r'^([\w.$-]+):(?:\s+;.*)?$')
CALLRE = re.compile(r'^\s*(?:(%[\w.$-]+) = )?(?:tail |musttail |notail )?call (.*?)([@%][\w.$-]+)\((.*)$')
DEFRE = re.compile(r'^define ([^@]*?)\bvoid (@[\w.$-]+)\(')
V0_TYPES = ('i32', 'ptr', 'i16', 'i8', 'i1')
PATTR = {'noundef', 'signext', 'zeroext', 'inreg', 'nonnull', 'noalias', 'fastcc', 'ccc', 'cc', 'hidden', 'dso_local'}


def functions(text):
    """-> {name: (define line index, [body lines])} for every define of a module text (lines list)"""
    out = {}
    lines = text if isinstance(text, list) else text.split('\n')
    i = 0
    while i < len(lines):
        l = lines[i]
        if l.startswith('define '):
            m = re.match(r'^define [^@]*(@[\w.$-]+)\(', l)
            j = i + 1
            while j < len(lines) and lines[j] != '}':
                j += 1
            if m:
                out[m[1]] = (i, j)
            i = j
        i += 1
    return out


def call_info(l):
    """a call line -> (result or None, return type, callee '@name' / 'icall', ext attr) or None (not a call / an
    intrinsic)"""
    if 'call ' not in l:
        return None
    m = CALLRE.match(l)
    if not m:
        return None
    res, mid, callee = m[1], m[2], m[3]
    if callee.startswith('@llvm.') or 'asm ' in mid:
        return None
    toks = [t for t in mid.split() if t not in PATTR and not t.startswith(('align', 'dereferenceable'))]
    if '(' in mid:   # variadic / explicit function type: "i32 (ptr, ...) "
        toks = [t for t in mid[:mid.index('(')].split() if t not in PATTR]
    rt = toks[0] if toks else '?'
    ext = 'signext' if ' signext ' in ' ' + mid else ('zeroext' if ' zeroext ' in ' ' + mid else '')
    return res, rt, (callee if callee.startswith('@') else 'icall'), ext


def blocks(body):
    """body lines (between define and }) -> [(label, [lines])]; the first block is 'entry' (label line optional)"""
    out = []
    cur = None
    for l in body:
        m = LABEL.match(l)
        if m:
            cur = (m[1], [])
            out.append(cur)
            continue
        if cur is None:
            cur = ('entry', [])
            out.append(cur)
        cur[1].append(l)
    return out


def successors(lines):
    """terminator successors (one entry per edge) of a block's lines"""
    k = None
    for i, l in enumerate(lines):
        if l.startswith(('  br ', '  switch ', '  indirectbr ')):
            k = i
    if k is None:
        return []
    return re.findall(r'label %([\w.$-]+)', ' '.join(lines[k:]))


def analyse(body):
    """IR side of the rule: for each `ret void`, the set of callees ('@f' / 'icall' / 'entry' when no call on some
    path) whose call is the last one before it. -> list of frozensets"""
    bl = blocks(body)
    names = [b[0] for b in bl]
    preds = {n: [] for n in names}
    last = {}
    for n, ls in bl:
        for t in successors(ls):
            if t in preds:
                preds[t].append(n)
        lc = None
        for l in ls:
            ci = call_info(l)
            if ci:
                lc = ci[2]
        last[n] = lc
    vin = {n: frozenset() for n in names}
    vin[names[0]] = frozenset({'entry'})
    vout = {}

    def out_of(n):
        return frozenset({last[n]}) if last[n] else vin[n]
    changed = True
    while changed:
        changed = False
        for n in names:
            if n == names[0]:
                continue
            new = frozenset().union(*[out_of(p) for p in preds[n]]) if preds[n] else frozenset()
            if new != vin[n]:
                vin[n] = new
                changed = True
    rets = []
    for n, ls in bl:
        cur = vin[n]
        for l in ls:
            ci = call_info(l)
            if ci:
                cur = frozenset({ci[2]})
            if l.strip() == 'ret void' or l.strip().startswith('ret void,'):
                rets.append(cur)
    return rets


def promote(lines, fname, promoted, tmpn):
    """rewrite function `fname` (a void define in `lines`) to return i32 = v0 at each return. `promoted`: names of
    every promoted function (callees returning void that are promoted are called as i32). Returns new lines."""
    fs = functions(lines)
    i0, i1 = fs[fname]
    head = lines[i0]
    m = DEFRE.match(head)
    assert m and m[2] == fname, head
    head = head[:m.start(0)] + f'define {m[1]}i32 {fname}(' + head[m.end(0):]
    body = lines[i0 + 1:i1]
    bl = blocks(body)
    names = [b[0] for b in bl]
    preds = {n: [] for n in names}
    for n, ls in bl:
        for t in successors(ls):
            if t in preds:
                preds[t].append(n)
    n_ = [tmpn]

    def tmp():
        n_[0] += 1
        return f'%v0.{n_[0]}'
    newb = []
    vout = {}
    for bi, (n, ls) in enumerate(bl):
        out = []
        cur = '0' if bi == 0 else f'%v0.in.{n}'
        for l in ls:
            ci = call_info(l)
            if ci:
                res, rt, callee, ext = ci
                if rt == 'void' and callee in promoted:
                    t = tmp()
                    l = re.sub(r'call (.*?)void ' + re.escape(callee) + r'\(', lambda mm: f'call {mm[1]}i32 {callee}(', l, 1)
                    l = re.sub(r'^(\s*)', lambda mm: f'{mm[1]}{t} = ', l, 1)
                    out.append(l)
                    cur = t
                    continue
                out.append(l)
                if res and rt in V0_TYPES:
                    if rt == 'i32':
                        cur = res
                    elif rt == 'ptr':
                        t = tmp()
                        out.append(f'  {t} = ptrtoint ptr {res} to i32')
                        cur = t
                    else:
                        t = tmp()
                        out.append(f'  {t} = {"sext" if ext == "signext" else "zext"} {rt} {res} to i32')
                        cur = t
                else:
                    cur = '0'   # not a v0 value (void, float, i64): garbage on the console; the rule excludes it
                continue
            if l.strip() == 'ret void':
                out.append(l.replace('ret void', f'ret i32 {cur}'))
                continue
            out.append(l)
        vout[n] = cur
        newb.append([n, out])
    # unreachable blocks (no predecessor): v0 is garbage there, 0
    for bi, (n, out) in enumerate(newb):
        if bi and not preds[n]:
            pat = re.compile(re.escape(f'%v0.in.{n}') + r'(?![\w.$-])')
            out[:] = [pat.sub('0', l) for l in out]
            for k in vout:
                if vout[k] == f'%v0.in.{n}':
                    vout[k] = '0'
    # phi nodes: v0 at each block entry (non-entry blocks with predecessors)
    for bi, (n, out) in enumerate(newb):
        if bi == 0 or not preds[n]:
            continue
        pat = re.compile(re.escape(f'%v0.in.{n}') + r'(?![\w.$-])')
        uses = any(pat.search(l) for l in out) or any(vout[s] == f'%v0.in.{n}' for s in names)
        if not uses:
            continue
        k = 0
        while k < len(out) and re.match(r'^\s*%[\w.$-]+ = phi ', out[k]):
            k += 1
        inc = ', '.join(f'[ {vout[p]}, %{p} ]' for p in preds[n])
        out.insert(k, f'  %v0.in.{n} = phi i32 {inc}')
    # a phi may name another block's phi that was replaced by 0 (unreachable block): fine, vout holds '0' then
    body2 = []
    for bi, (n, out) in enumerate(newb):
        lab = [l for l in body if LABEL.match(l) and LABEL.match(l)[1] == n]
        if lab:
            body2.append(lab[0])
        body2 += out
    return lines[:i0] + [head] + body2 + lines[i1:], n_[0]


def rewrite_internal_sites(lines, internal):
    """`call void @f(` -> `%v0.dN = call i32 @f(` for promoted functions defined in this TU (sigs.py leaves calls to
    a TU's own internal functions alone)"""
    out, n = [], 0
    for l in lines:
        ci = call_info(l) if 'call ' in l else None
        if ci and ci[1] == 'void' and ci[2] in internal and not ci[0]:
            n += 1
            l = re.sub(r'call (.*?)void ' + re.escape(ci[2]) + r'\(', lambda mm: f'call {mm[1]}i32 {ci[2]}(', l, 1)
            l = re.sub(r'^(\s*)', lambda mm: f'{mm[1]}%v0.d{n} = ', l, 1)
        out.append(l)
    return out
