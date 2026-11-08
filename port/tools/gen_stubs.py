"""Close the wasm link (session S2): called by port_build.py after every object is built.

Collects the defined / undefined symbols of all objects (llvm-nm) and of emscripten's libc, then for every symbol
the link still needs:
  * a data symbol of the matching link in KSEG0 RAM (port/symbols/symbols.tsv)  -> a label inside the RDRAM mirror;
    in fact *every* global RAM data symbol without a C definition gets its label, so the console layout is kept
    byte for byte (gen/rdram_labels.s: `n64_rdram`, 8 MiB, 8 MiB aligned, linked first);
  * a function (a game or libultra function without a port body)                -> a logged stub with the function's
    canonical signature (port/build/sigdb.json, so call sites and stub agree; wasm traps on a type mismatch);
  * a name libc defines                                                         -> left to libc, listed (the game
    must not get host-byte semantics on game memory: each one is checked when it is first used);
  * anything else                                                               -> a zero data stub, listed.
Writes port/build/gen/{rdram_labels.s,stubs.c}, their objects, and port/stubs.tsv (names only).
"""
import collections, glob, json, os, re, subprocess

NM = '/emsdk/upstream/bin/llvm-nm'
RD0, RDSZ = 0x80000000, 0x800000
CT = {'i1': 'int', 'i8': 'int', 'i16': 'int', 'i32': 'int', 'i64': 'long long', 'float': 'float',
      'double': 'double', 'ptr': 'void *', 'void': 'void'}
NARROW = {('i8', 'zeroext'): 'unsigned char', ('i8', 'signext'): 'signed char',
          ('i16', 'zeroext'): 'unsigned short', ('i16', 'signext'): 'short'}


def nm(paths):
    """-> {path: (defined set, undefined set)}"""
    out = {}
    for i in range(0, len(paths), 200):
        chunk = paths[i:i + 200]
        p = subprocess.run([NM, '-P', '-A'] + chunk, capture_output=True, text=True)
        for l in p.stdout.splitlines():
            m = re.match(r'^(.*?)(?:\[[^\]]*\])?: (\S+) (\S)', l)
            if not m:
                continue
            path, name, t = m[1], m[2], m[3]
            d, u = out.setdefault(path, (set(), set()))
            if t in ('U', 'w', 'v'):  # w / v: weak undefined (port/gfx's weak gspL3DEX2_fifoTextStart, S3c)
                u.add(name)
            elif t not in ('t', 'd', 'b', 'r'):  # locals do not resolve other objects' references
                d.add(name)
    return out


def libc_defined():
    lib = subprocess.run(['em-config', 'CACHE'], capture_output=True, text=True).stdout.strip() + \
        '/sysroot/lib/wasm32-emscripten'
    libs = [f'{lib}/{n}' for n in ('libc.a', 'libcompiler_rt.a', 'libdlmalloc.a') if os.path.exists(f'{lib}/{n}')]
    p = subprocess.run([NM, '-j', '--defined-only'] + libs, capture_output=True, text=True)
    return {l.strip() for l in p.stdout.splitlines() if l.strip() and not l.endswith(':')}


def ctype(t, ext=''):
    return NARROW.get((t, ext)) or CT.get(t, 'int')


def stub_c(name, entry):
    if entry:
        r, ps = entry[0], entry[1]
        rx = entry[2] if len(entry) > 2 else ''
        pxs = entry[3] if len(entry) > 3 else []
    else:
        r, ps, rx, pxs = 'void', [], '', []
    var = bool(ps) and ps[-1] == '...'
    fixed = ps[:-1] if var else ps
    args = [f'{ctype(p, pxs[i] if i < len(pxs) else "")} a{i}' for i, p in enumerate(fixed)]
    if var:
        args.append('...')
    sig = f'{ctype(r, rx)} {name}({", ".join(args) if args else "void"})'
    ret = '' if r == 'void' else ' return 0;'
    return sig, f'__attribute__((weak)) {sig} {{ port_stub_hit("{name}");{ret} }}\n'


def generate(game_objs, pc_objs, can, out):
    gen = f'{out}/gen'
    syms = {}
    for l in open('port/symbols/symbols.tsv'):
        if l.startswith('#'):
            continue
        r = l.rstrip('\n').split('\t')
        name, vram, rom, size, typ, bind, sec, seg, obj = r
        if bind == 'global' or name not in syms:
            syms[name] = (int(vram, 16), int(size, 16), typ, bind, seg)
    table = nm(game_objs + pc_objs)
    defined, refs = set(), collections.defaultdict(list)
    for path, (d, u) in table.items():
        defined |= d
        for n in u:
            refs[n].append(path)
    libc = libc_defined()
    # a game TU must never define a libc name (it would replace emscripten's libc for the PC layer as well):
    # vbe.py LIBC_CLASH renames them to vbe.<name>; a new one fails the build here
    clash = sorted({n for o in game_objs for n in table.get(o, (set(), set()))[0]} & libc)
    if clash:
        raise SystemExit('gen_stubs: game objects define libc names (add them to vbe.py LIBC_CLASH): ' + ' '.join(clash))
    # how the game IR refers to each name (function call vs data object)
    as_data = set()
    for vll in glob.glob(f'{out}/ll/*.v.ll'):
        for m in re.finditer(r'^@([\w.$-]+) = external (?:dso_local )?global', open(vll).read(), re.M):
            as_data.add(m[1])
    linker = {'__indirect_function_table', '__stack_pointer', '__memory_base', '__table_base', '__heap_base',
              '__data_end', '__dso_handle', 'n64_rdram',  # + what gen/relocs.c defines:
              'port_reloc_targets', 'port_relocs', 'port_reloc_ranges'}
    # __start_/__stop_<section>: defined by wasm-ld for the ovl_<segment> data sections (S3a, port_build.py)
    # emscripten_*, gl* (WebGL2) and port_gfx_gl_create (port/gfx/gfx_gl.js): JS library imports, never stubs (S3c)
    js_lib = lambda n: n.startswith('emscripten_') or n == 'port_gfx_gl_create' or re.match(r'gl[A-Z]', n)
    need = sorted(n for n in refs if n not in defined and n not in linker and not n.startswith(('__start_', '__stop_'))
                  and not js_lib(n))
    labels = {}   # name -> (vram, size)
    for name, (v, sz, typ, bind, seg) in syms.items():
        if bind == 'global' and typ in ('data', 'none') and RD0 <= v < RD0 + RDSZ and name not in defined \
                and name not in libc:
            labels[name] = (v, sz)
    rows, stubs, dstubs = [], [], []
    cnt = collections.Counter()
    for n in need:
        who = sorted({os.path.basename(p)[:-2].replace('__', '/') for p in refs[n]})
        first = who[0] + (f' (+{len(who) - 1})' if len(who) > 1 else '')
        s = syms.get(n)
        if n in labels:
            kind, sig = 'mirror-label', f'0x{labels[n][0]:08X} size 0x{labels[n][1]:X}'
        elif s and s[2] == 'func' and n in as_data and RD0 <= s[0] < RD0 + RDSZ:
            labels[n] = (s[0], 0)
            kind, sig = 'mirror-label-of-function', f'0x{s[0]:08X} (function address taken as data)'
        elif n in libc:
            kind, sig = 'libc', 'emscripten libc (host byte order: check before use on game memory)'
        elif n in as_data:
            kind, sig = 'data-stub', 'zero-filled 256 bytes'
            dstubs.append(n)
        else:
            kind = 'func-stub' if s and s[2] == 'func' else 'func-stub-unknown'
            sig, body = stub_c(n, can.get('@' + n))
            stubs.append(body)
        cnt[kind] += 1
        rows.append((kind, n, sig, first))
    os.makedirs(gen, exist_ok=True)
    # mirror labels (sorted by address; several names may share an address: overlays share VRAM)
    by = collections.defaultdict(list)
    for n, (v, sz) in labels.items():
        by[v - RD0].append((n, min(sz, RDSZ - (v - RD0))))
    lines = ['# GENERATED by port/tools/gen_stubs.py - do not edit. N64 RAM symbols as labels in the RDRAM mirror.',
             '\t.section\t.bss.n64_rdram,"",@', '\t.globl\tn64_rdram', '\t.p2align\t23', 'n64_rdram:']
    cur = 0
    for off in sorted(by):
        if off > cur:
            lines.append(f'\t.skip\t{off - cur}')
            cur = off
        for n, sz in sorted(by[off]):
            lines += [f'\t.globl\t{n}', f'{n}:', f'\t.size\t{n}, {sz}']
    lines += [f'\t.skip\t{RDSZ - cur}', f'\t.size\tn64_rdram, {RDSZ}', '']
    open(f'{gen}/rdram_labels.s', 'w').write('\n'.join(lines))
    hdr = ('/* GENERATED by port/tools/gen_stubs.py - do not edit. Logged stubs for functions without a port body. */\n'
           'void port_stub_hit(const char *name);\n\n')
    data = ''.join(f'__attribute__((weak, aligned(8))) unsigned char {n}[256];\n' for n in dstubs)
    open(f'{gen}/stubs.c', 'w').write(hdr + ''.join(stubs) + data)
    reloc_src, reloc_rep = gen_relocs_c(gen, defined, set(labels), can, syms)
    objs = []
    for src, cmd in ((f'{gen}/rdram_labels.s', ['emcc', '-c']),
                     (reloc_src, ['emcc', '-c', '-O1', '-g0', '-Iport/ultra', '-Wno-incompatible-library-redeclaration',
                                  '-Wno-builtin-requires-header', '-Wno-incompatible-pointer-types']),
                     (f'{gen}/stubs.c', ['emcc', '-c', '-O1', '-g0', '-Wno-incompatible-library-redeclaration',
                                         '-Wno-builtin-requires-header'])):
        obj = src.rsplit('.', 1)[0] + '.o'
        p = subprocess.run(cmd + [src, '-o', obj], capture_output=True, text=True)
        if p.returncode:
            raise SystemExit(f'gen_stubs: {src} failed\n{p.stderr[-3000:]}')
        objs.append(obj)
    head = ('# kind\tname\tsignature / address\treferenced by   (port/tools/gen_stubs.py: every symbol the wasm link '
            'needs that no port object defines; names only)\n')
    txt = head + ''.join('\t'.join(r) + '\n' for r in sorted(rows))
    try:
        old = open('port/stubs.tsv').read()
    except OSError:
        old = None
    if old != txt:
        open('port/stubs.tsv', 'w').write(txt)
    rep = (f'{len(labels)} mirror labels; {reloc_rep}; needed by the link: ' +
           ', '.join(f'{k} {v}' for k, v in sorted(cnt.items())) + ' (port/stubs.tsv)')
    return objs, rep


def gen_relocs_c(gen, defined, labels, can, syms):
    """gen/relocs.c: the relocation list of port/symbols/relocs.tsv resolved against this link. Per pointer word of a
    ROM-loaded segment: the word's console address and the target: -3 = an absolute symbol outside RAM (kept), -2 = code
    without a port body (kept, counted), -1 = a mirror address (the target lies in the RDRAM
    mirror), else an index into port_reloc_targets = {&symbol, symbol's console address, is_function} for targets
    that are C objects / functions of the port (the word then becomes their wasm address / table index + offset).
    The symbol name fixes the overlay, so shared-VRAM overlays resolve to the right code."""
    rows = [l.rstrip('\n').split('\t') for l in open('port/symbols/relocs.tsv') if not l.startswith('#')]
    segs = [r[0] for r in (l.split('\t') for l in open('port/symbols/segments.tsv') if not l.startswith('#'))
            if r[2] not in ('-', '')]  # the same rows as port_segments (port_build.py gen_segments)
    tgt_idx, targets = {}, []
    by_seg = collections.defaultdict(list)
    cnt = collections.Counter()
    for seg, wv, tseg, kind, target in rows:
        w = int(wv, 16)
        if w & 3:
            cnt['misaligned'] += 1
            continue
        idx = -1
        tv = syms.get(target, (None,))[0] if not target.startswith('.') else None
        if tv is not None and not RD0 <= tv < RD0 + RDSZ:
            cnt['absolute'] += 1  # a symbol outside RAM (segment address D_0E000000, ...): the word keeps its value
            idx = -3
        elif not target.startswith('.') and target in defined and target not in labels:
            if target not in tgt_idx:
                tgt_idx[target] = len(targets)
                targets.append((target, kind == 'func' or (syms.get(target, (0, 0, ''))[2] == 'func')))
            idx = tgt_idx[target]
            cnt['c_' + ('func' if targets[idx][1] else 'data')] += 1
        elif kind in ('func', 'text'):
            cnt['code_unresolved'] += 1  # code with no port body (libultra asm, PC layer): left as is, logged
            idx = -2
        else:
            cnt['mirror'] += 1
        by_seg[seg].append((w, idx))
    decl, tab = [], []
    for name, isf in targets:
        if isf:
            sig, _ = stub_c(name, can.get('@' + name))
            decl.append(sig + ';')
            tab.append(f'    {{(const void *)&{name}, 0x{syms[name][0]:08X}u, 1}},')
        else:
            decl.append(f'extern char {name}[];')
            tab.append(f'    {{(const void *){name}, 0x{syms[name][0]:08X}u, 0}},')
    words, segtab = [], []
    for seg in segs:
        lst = sorted(by_seg.get(seg, []))  # S3a: by address (pc_mem.c relocates the part of a segment a DMA copied)
        segtab.append(f'    {{{len(words)}u, {len(lst)}u}}, /* {seg} */')
        words += [f'{{0x{w:08X}u, {i}}}' for w, i in lst]
    txt = ('/* GENERATED by port/tools/gen_stubs.py from port/symbols/relocs.tsv - do not edit. Offsets and names only. */\n'
           '#include "pc_mem.h"\n' + '\n'.join(decl) + '\n\n'
           'const PortRelocTarget port_reloc_targets[] = {\n' + '\n'.join(tab) + '\n    {0, 0, 0}};\n'
           'const PortReloc port_relocs[] = {\n' + ',\n'.join('    ' + x for x in words) + '\n};\n'
           '/* per segment of port/symbols/segments.tsv (same order as port_segments): first, count */\n'
           'const PortRelocRange port_reloc_ranges[] = {\n' + '\n'.join(segtab) + '\n};\n')
    p = f'{gen}/relocs.c'
    open(p, 'w').write(txt)
    return p, f'{len(rows)} relocations (' + ', '.join(f'{k} {v}' for k, v in sorted(cnt.items())) + ')'
