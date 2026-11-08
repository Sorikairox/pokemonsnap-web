"""One wasm signature per game function (docs/port/MEMORY.md, "Calls").

Provenance: copied from the Conker web port's port/tools/sigs.py (the user's own project, no licence file, same
author), unchanged apart from this note and the empty A0_KEEP table. Session S2.

IDO accepted calls through implicit `int f()` declarations and local prototypes that disagree with
the definition; in wasm a direct call whose type differs from the callee's is linked as a trap.
Before the vbe rewrite, vbe_build.py collects every non-static `define` and every direct call site
over all game TUs (scan()), and picks the canonical signature of each function: the definition's,
or for asm-only functions (stubs) the most common call-site signature. normalize() then rewrites
each mismatching call site and `declare` in a TU to the canonical type, converting arguments
(int<->int sext/trunc, ptr<->int, float<->int bitcast, missing -> 0, extra -> dropped) and the
result (void -> 0). This reproduces what the MIPS o32 call would do for integer/pointer arguments;
an f32/int mix is wrong either way (different registers on MIPS too) and is reported.

P341: the definition's zeroext / signext attributes of i8 / i16 parameters and results are part of the canonical
signature (can[f][2] / can[f][3]). wasm passes an i8 / i16 in an i32 and the callee trusts the caller to have
extended it; a rewritten call without the attribute handed over the unmasked int (event_action_rail command 11 ->
world_record_find(u8) got 0x300B for record id 0x0B, so no multiplayer weapon pickup ever reached a player).
Rewritten sites now carry the attributes, sites whose only difference is a missing / different extension are
rewritten too, and a narrow zeroext result widens with zext.
"""
import collections, json, re
from vbe import split_top, take_type

DEFINE = re.compile(r'^define ((?:[\w()]+ )*?)(\S+|\{[^}]*\}) (@[\w.$-]+)\((.*?)\)( [^{]*)?\{?\s*$')
CALL = re.compile(r'^(\s*)(?:(%[\w.$-]+) = )?((?:tail |musttail |notail )?call )(.*?)(@[\w.$-]+)\((.*)\)([^)]*)$')
DECLARE = re.compile(r'^declare (.*?)(@[\w.$-]+)\((.*)\)(.*)$')
PATTR = {'noundef', 'signext', 'zeroext', 'inreg', 'nonnull', 'noalias', 'nocapture', 'readonly', 'returned',
         'writeonly', 'immarg', 'noused'}


def ptypes(params):
    out = []
    for p in split_top(params):
        if not p:
            continue
        if p == '...':
            out.append('...')
            continue
        t, _ = take_type(p)
        out.append(t)
    return out


def site_ret(mid):
    """'noundef i32 ' or 'i32 (i32, ...) ' -> 'i32'"""
    mid = mid.strip()
    if '(' in mid:
        mid = mid[:mid.index('(')].strip()
    toks = [t for t in mid.split() if t not in PATTR and not t.startswith(('align', 'dereferenceable'))]
    return toks[-1] if toks else None


EXT = ('zeroext', 'signext')


def pexts(params):
    """zeroext / signext (or '') of each parameter of a define / declare parameter list"""
    out = []
    for p in split_top(params):
        if not p:
            continue
        if p == '...':
            out.append('')
            continue
        _, rest = take_type(p)
        out.append(next((t for t in rest.split() if t in EXT), ''))
    return out


def rext(prefix):
    """zeroext / signext (or '') among the return attributes in front of the type"""
    return next((t for t in prefix.split() if t in EXT), '')


def site_exts(args):
    out = []
    for a in split_top(args):
        if not a:
            continue
        _, rest = take_type(a)
        out.append(next((t for t in rest.split() if t in EXT), ''))
    return out


def narrow(t):
    return t in ('i8', 'i16')


def site_args(args):
    out = []
    for a in split_top(args):
        if not a:
            continue
        t, rest = take_type(a)
        toks = rest.split()
        # strip attributes (noundef, signext, align N, byval(T)...) -> value is the remainder
        while len(toks) > 1 and (toks[0] in PATTR or toks[0].startswith(('byval', 'sret', 'align', 'dereferenceable'))):
            if toks.pop(0) == 'align':
                toks.pop(0)  # its number
        out.append((t, ' '.join(toks)))
    return out


def parse_define(l):
    """S2 (Pokémon Snap): DEFINE's lazy parameter group stopped at the first ')' inside a parameter attribute
    (`sret(%struct.ldiv_t)`, `byval(%struct.Vec3f)`), which truncated those definitions' signatures and made sigs
    rewrite every call to them. The parameter list is now cut at its balanced closing parenthesis.
    -> (None, prefix, ret, name, params) like DEFINE's groups, or None."""
    m = re.match(r'^define ((?:[\w()]+ )*?)(\S+|\{[^}]*\}) (@[\w.$-]+)\(', l)
    if not m:
        return None
    depth, i = 1, m.end()
    while i < len(l) and depth:
        depth += {'(': 1, ')': -1}.get(l[i], 0)
        i += 1
    if depth:
        return None
    return (None, m[1], m[2], m[3], l[m.end():i - 1])


def scan(text, defs, sites):
    for l in text.split('\n'):
        if l.startswith('define '):
            m = parse_define(l)
            if m and 'internal' not in m[1] and 'private' not in m[1]:
                defs[m[3]] = (m[2], ptypes(m[4]), rext(m[1]), pexts(m[4]))
        elif l.startswith('declare ') and '...' in l:
            m = DECLARE.match(l)
            if m and not m[2].startswith('@llvm.') and ptypes(m[3])[:-1]:
                defs.setdefault(m[2], (site_ret(m[1]), ptypes(m[3])))  # prototyped variadic
        elif 'call ' in l and l.startswith('  '):
            m = CALL.match(l)
            if m and not m[5].startswith('@llvm.'):
                r = site_ret(m[4])
                if r:
                    sites[m[5]][json.dumps([r, [t for t, _ in site_args(m[6])]])] += 1


def canonical(defs, sites):
    can = {k: list(v) for k, v in defs.items()}
    for f, c in sites.items():
        if f not in can:
            r, ps = json.loads(c.most_common(1)[0][0])
            can[f] = [r, ps]
    return can


ZERO = {'ptr': 'null', 'float': '0.0', 'double': '0.0'}


def zero(t):
    return ZERO.get(t, '0' if t.startswith('i') else 'zeroinitializer')


def conv(ind, v, src, dst, tmp, out, ext='sext'):
    if src == dst:
        return v
    if v in ('undef', 'poison'):
        return v
    isint = lambda t: re.match(r'^i\d+$', t)
    t = tmp()
    if isint(src) and isint(dst):
        a, b = int(src[1:]), int(dst[1:])
        out.append(f'{ind}{t} = {ext if a < b else "trunc"} {src} {v} to {dst}')
    elif src == 'ptr' and isint(dst):
        out.append(f'{ind}{t} = ptrtoint ptr {v} to {dst}')
    elif isint(src) and dst == 'ptr':
        out.append(f'{ind}{t} = inttoptr {src} {v} to ptr')
    elif {src, dst} in ({'float', 'i32'}, {'double', 'i64'}):
        out.append(f'{ind}{t} = bitcast {src} {v} to {dst}')
    elif src == 'float' and dst == 'double':
        out.append(f'{ind}{t} = fpext float {v} to double')
    elif src == 'double' and dst == 'float':
        out.append(f'{ind}{t} = fptrunc double {v} to float')
    else:
        return zero(dst)
    return t


# P46 mechanism (port-web): o32 leaves a missing argument in its register. Calls listed here pass
# the CALLER's first parameter (still in a0 in the asm) instead of zero. Add an entry only as a
# TEMPORARY fix, and remove it when the port branch fixes the call in source.
#   P49: the camera_start_transition -> camera_update_eye_base entry is gone (P47 passes arg0 in game_14FF90.c).
A0_KEEP = set()
# S2: direct call sites that pass fewer arguments than the canonical signature takes: (caller, callee, passed,
# taken). vbe.py writes them next to its output (<out>.short); port_build.py lists them in port/build/short_calls.tsv.
SHORT = []
DEFINE_A0 = re.compile(r'^define [^@]*(@[\w.$]+)\((ptr|i32)[^%,)]*(%[\w.]+)?')


def attrs(entry):
    """(return attribute + ' ' or '', [parameter attribute or '']) of a canonical entry; only narrow ints keep one"""
    r, ps = entry[0], entry[1]
    ra = entry[2] if len(entry) > 2 and narrow(r) else ''
    pa = list(entry[3]) if len(entry) > 3 else []
    pa = [(pa[i] if i < len(pa) and narrow(p) else '') for i, p in enumerate(ps)]
    return (ra + ' ' if ra else ''), pa


def normalize(lines, can, tmp, stats):
    out = []
    cur = (None, None, None)
    curfn = None
    # S2: calls to a function this TU defines itself as internal / private (a `static` that another TU may also
    # have, with another signature: libaudio's _getRate / _getVol in seqplayer.c and csplayer.c) are never rewritten
    local = set()
    for l in lines:
        if l.startswith('define '):
            d = parse_define(l)
            if d and ('internal' in d[1] or 'private' in d[1]):
                local.add(d[3])
    for l in lines:
        if l.startswith('define '):
            m = DEFINE_A0.match(l)
            cur = (m[1], m[2], m[3]) if m else (None, None, None)
            fm = re.match(r'define [^@]*(@[\w.$-]+)\(', l)
            curfn = fm[1] if fm else None
        if l.startswith('declare '):
            m = DECLARE.match(l)
            if m and m[2] in can and not m[2].startswith('@llvm.'):
                r, ps = can[m[2]][:2]
                ra, pa = attrs(can[m[2]])
                if '...' not in ps or ps[:-1]:
                    out.append(f'declare {ra}{r} {m[2]}({", ".join(p + (" " + pa[i] if pa[i] else "") for i, p in enumerate(ps))})')
                    continue
        elif l.startswith('  ') and 'call ' in l:
            m = CALL.match(l)
            if m and m[5] in can and m[5] not in local and not m[5].startswith('@llvm.'):
                r, ps = can[m[5]][:2]
                ra, pa = attrs(can[m[5]])
                rx = 'zext' if ra.strip() == 'zeroext' else 'sext'
                sr = site_ret(m[4])
                sa = site_args(m[6])
                se = site_exts(m[6])
                # P341: a narrow parameter whose extension differs from the definition's must be rewritten as well
                extdiff = any(narrow(p) and pa[i] and (i >= len(se) or se[i] != pa[i]) for i, p in enumerate(ps) if p != '...') \
                    or (narrow(r) and ra and sr != r)
                var = ps and ps[-1] == '...'
                fixed = ps[:-1] if var else ps
                if var and sr and (sr != r or [t for t, _ in sa][:len(fixed)] != fixed or '...' not in m[4]):
                    ind, res, new, args = m[1], m[2], [], []
                    for i, p in enumerate(fixed):
                        args.append(f'{p}{" " + pa[i] if pa[i] else ""} {conv(ind, sa[i][1], sa[i][0], p, tmp, new) if i < len(sa) else zero(p)}')
                    args += [f'{t} {v}' for t, v in sa[len(fixed):]]
                    call = f'{m[3]}{ra}{r} ({", ".join(fixed + ["..."])}) {m[5]}({", ".join(args)})'
                    if res and sr != 'void' and r != 'void':
                        t = tmp(); new.append(f'{ind}{t} = {call}')
                        c = conv(ind, t, r, sr, tmp, new, rx)
                        new.append(f'{ind}{res} = select i1 true, {sr} {c}, {sr} {c}')
                    elif res and sr != 'void':
                        new.append(f'{ind}{call}'); new.append(f'{ind}{res} = select i1 true, {sr} {zero(sr)}, {sr} {zero(sr)}')
                    else:
                        new.append(f'{ind}{tmp()} = {call}' if r != 'void' else f'{ind}{call}')
                    out += new
                    stats['call_fixed_var'] += 1
                    continue
                if not var and sr and (sr != r or [t for t, _ in sa] != ps or extdiff):
                    if not (sr != r or [t for t, _ in sa] != ps):
                        stats['ext_fixed'] += 1
                    ind, res = m[1], m[2]
                    new = []
                    args = []
                    raw = [a for a in split_top(m[6]) if a]
                    for i, p in enumerate(ps):
                        if i < len(sa) and sa[i][0] == p and not narrow(p):
                            args.append(raw[i])  # S2: keeps byval(...) / sret(...): wasm passes a pointer to a copy
                        elif i < len(sa):
                            if {sa[i][0], p} == {'float', 'i32'}:
                                stats['fp_int_arg'] += 1
                            args.append(f'{p}{" " + pa[i] if pa[i] else ""} {conv(ind, sa[i][1], sa[i][0], p, tmp, new)}')
                        elif i == 0 and (cur[0], m[5]) in A0_KEEP and cur[2] and cur[1] == p:
                            args.append(f'{p} {cur[2]}')
                            stats['a0_keep'] += 1
                        else:
                            args.append(f'{p} {zero(p)}')
                    if len(sa) < len(ps):  # S2: short K&R call (the console passed whatever the registers held)
                        SHORT.append((curfn, m[5], len(sa), len(ps)))
                    call = f'{m[3]}{ra}{r} {m[5]}({", ".join(args)})'
                    if res and sr != 'void':
                        if r == 'void':
                            new.append(f'{ind}{call}')
                            new.append(f'{ind}{res} = select i1 true, {sr} {zero(sr)}, {sr} {zero(sr)}')
                        else:
                            t = tmp()
                            new.append(f'{ind}{t} = {call}')
                            c = conv(ind, t, r, sr, tmp, new, rx)
                            new.append(f'{ind}{res} = select i1 true, {sr} {c}, {sr} {c}')
                    elif r != 'void':  # an unnamed non-void call would take an implicit %N slot
                        new.append(f'{ind}{tmp()} = {call}')
                    else:
                        new.append(f'{ind}{call}')
                    out += new
                    stats['call_fixed'] += 1
                    continue
        out.append(l)
    return out
