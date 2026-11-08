#!/usr/bin/env python3
"""Pokémon Snap wasm32 port build (session S2 skeleton). Runs inside the pokemonsnap-emsdk container from the repo
root: port/docker/run.sh port   (= python3 port/tools/port_build.py).

Phases (outputs in port/build/, gitignored; nothing here reads the ROM or the matching build's outputs):
  1. census     every object of the matching link (port/symbols/objects.tsv) + every src/**/*.c on disk, classified
                (port/symbols/asset_tus.tsv, symbols.tsv) into what the port does with it -> port/census.tsv
  2. C -> IR    each TU the port compiles: clang -O0 IR without optnone (port/build/ll/<tag>.ll), incremental
  3. sigs       one canonical signature per function over all game IR (port/tools/sigs.py) -> port/build/sigdb.json
  4. vbe        virtual big-endian rewrite + call-site normalisation (port/tools/vbe.py), then -O1 -> port/build/vobj
  5. PC layer   port/ultra/*.c and port/boot/*.c, native (no vbe) -> port/build/pc
  6. stubs      undefined symbols of the link: RDRAM data symbols -> labels in the RDRAM mirror (n64_rdram), game /
                libultra functions without a port body -> logged stubs with their canonical signature
                (port/tools/gen_stubs.py) -> port/build/gen, port/stubs.tsv
  7. link       emcc -> port/build/web/pokemonsnap.{js,wasm} (undefined symbols are an error)
"""
import collections, concurrent.futures as cf, hashlib, json, os, re, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
OUT = 'port/build'
LL, VOBJ, PCOBJ, GEN, WEB = (f'{OUT}/{d}' for d in ('ll', 'vobj', 'pc', 'gen', 'web'))
SYM = 'port/symbols'
RDRAM = (0x80000000, 0x80800000)

INC = ['-Iport/include', '-Iinclude', '-Isrc', '-Iultralib/include', '-Iultralib/include/ido', '-Iultralib/include/PR',
       '-Iultralib/src', f'-I{OUT}/include', '-I.']
DEFS = ['-DPORT', '-DF3DEX_GBI_2', '-D_LANGUAGE_C', '-DNDEBUG', '-D_FINALROM', '-D_MIPS_SZLONG=32', '-D_MIPS_SZINT=32']
BASE = ['emcc', '-std=gnu89', '-nostdinc', '-fno-builtin', '-fno-strict-aliasing', '-fwrapv', '-funsigned-char',
        '-g0', '-include', 'port/include/port_prefix.h'] + DEFS + INC
# -funsigned-char: IDO's default (configure.py passes no -signed). Sources are compiled in EUC-JP like the matching
# build (asm-processor --output-enc=EUC-JP): see to_ll().
# IDO accepts these; clang >= 15/16 makes them errors by default (same list as the Conker port)
RELAX = ['-Wno-error=int-conversion', '-Wno-error=incompatible-pointer-types',
         '-Wno-error=incompatible-function-pointer-types', '-Wno-error=implicit-int',
         '-Wno-error=implicit-function-declaration', '-Wno-error=return-type', '-Wno-error=return-mismatch',
         '-Wno-error=int-to-pointer-cast', '-Wno-unknown-pragmas', '-Wno-error=pointer-sign']
# -fno-discard-value-names: clang's va_arg lowering names its pointers %argp.cur*, which vbe.py needs (the variadic
# buffer is written by the wasm backend in native layout, so 64-bit va_arg loads must not be half-swapped)
LL_FLAGS = ['-S', '-emit-llvm', '-O0', '-Xclang', '-disable-O0-optnone', '-fno-discard-value-names']
O1_FLAGS = ['-c', '-O1', '-g0', '-Wno-everything']
PC_FLAGS = ['emcc', '-c', '-O2', '-g0', '-std=gnu11', '-Wall', '-Wno-unused-parameter', '-Wno-unused-function',
            '-Iport/ultra', f'-I{OUT}/gen']
# ultralib files that configure.py builds as libultra VERSION_J (kept for the TUs the port compiles)
VERSION_J = {'aisetfreq', 'cartrominit', 'contpfs', 'contramread', 'contramwrite', 'contreaddata', 'crc', 'epirawdma',
             'epirawread', 'epirawwrite', 'gbpakcheckconnector', 'gbpakgetstatus', 'gbpakinit', 'gbpakpower',
             'gbpakreadid', 'gbpakreadwrite', 'gbpaksetbank', 'motor', 'pfschecker', 'pfsfilestate', 'pfsgetstatus',
             'pfsinitpak', 'pimgr', 'pfsselectbank', 'sirawdma', 'vimgr', 'viswapcontext', 'devmgr', 'epiread',
             'epiwrite', 'initialize', 'pirawdma', 'pirawread', 'sirawread'}
# libultra directories whose C the port compiles as game code (pure computation on game memory); everything else
# in ultralib (os/, io/, debug/: threads, messages, the RCP, PI/SI/VI/AI) is replaced by the PC layer.
ULTRA_COMPILE = ('ultralib/src/gu/', 'ultralib/src/libc/', 'ultralib/src/audio/', 'ultralib/src/sp/',
                 'ultralib/src/vimodes/')
# game TUs that are libultra replaced by the PC layer
PC_REPLACED = {}  # S8: src/os/osFlash.c (the game's FlashRAM driver) is compiled; the chip is port/save/flashram.c

ACTIONS = {  # category -> (action, what happens to it)
    'game': ('compile', 'game C, compiled through vbe'),
    'game-data': ('compile', 'game C data only (no asset include), compiled through vbe'),
    'game-mixed': ('compile', 'game C with asset includes: compiled with -DPORT, the includes left out'),
    'asset': ('rom', 'asset data only (#include assets/...): not compiled; symbols are RDRAM mirror labels, bytes from the ROM'),
    'ultra-lib': ('compile', 'libultra C (gu/libc/audio/sp/vimodes), compiled through vbe'),
    'ultra-pc': ('pc', 'libultra os/io/debug C: replaced by the PC layer (logged stubs until written)'),
    'ultra-asm': ('pc', 'libultra assembly: replaced by the PC layer (pc_libc.c or logged stubs)'),
    'pc-replaced': ('pc', 'game TU that is a hardware driver: replaced by the PC layer'),
    'entry': ('pc', 'boot assembly (bss clear, jump to game_main): port/boot/main.c'),
    'rsp': ('hle', 'RSP microcode .incbin: not built; the renderer / audio HLE replace it'),
    'asm-data': ('rom', 'unmapped data (asm/data .s): mirror labels, bytes from the ROM'),
    'rom-bin': ('rom', 'binary segment (audio, textures, fx, vpk0): read from the ROM at run time'),
    'unlinked': ('none', 'C file on disk that the matching link does not contain'),
}


def tsv(path):
    rows = []
    for l in open(path):
        if l.startswith('#') or not l.strip():
            continue
        rows.append(l.rstrip('\n').split('\t'))
    return rows


def src_of(obj):
    """build/src/sys/main.c.o -> src/sys/main.c ; build/assets/x.bin.o -> assets/x.bin"""
    p = obj[len('build/'):] if obj.startswith('build/') else obj
    return p[:-2] if p.endswith('.o') else p


def census():
    objs = tsv(f'{SYM}/objects.tsv')
    syms = tsv(f'{SYM}/symbols.tsv')
    assets = {r[0]: r for r in tsv(f'{SYM}/asset_tus.tsv')}
    seg_of, sections = {}, collections.defaultdict(set)
    for out_sec, in_sec, obj, vram, size, rom, seg in objs:
        seg_of.setdefault(obj, seg)
        sections[obj].add(in_sec)
    nfunc = collections.Counter(r[8] for r in syms if r[4] == 'func')
    rows = []
    linked = set()
    for obj in sorted(seg_of):
        src = src_of(obj)
        linked.add(src)
        nf = nfunc.get(obj, 0)
        if src.endswith('.c'):
            if src.startswith('ultralib/'):
                cat = 'ultra-lib' if src.startswith(ULTRA_COMPILE) else 'ultra-pc'
            elif src in PC_REPLACED:
                cat = 'pc-replaced'
            elif src in assets:
                cat = 'game-mixed' if assets[src][4] == 'yes' else 'asset'
            else:
                cat = 'game' if nf else 'game-data'
        elif src.startswith('ultralib/'):
            cat = 'ultra-asm'
        elif src.startswith('src/entry/'):
            cat = 'entry'
        elif src.startswith('src/rsp/'):
            cat = 'rsp'
        elif src.startswith('asm/'):
            cat = 'asm-data'
        else:
            cat = 'rom-bin'
        rows.append([src, seg_of[obj], cat, ACTIONS[cat][0], str(nf), ','.join(sorted(sections[obj]))])
    for dp, dn, fn in os.walk('src'):
        for f in fn:
            p = os.path.join(dp, f)
            if f.endswith('.c') and p not in linked:
                rows.append([p, '-', 'unlinked', 'none', '0', '-'])
    return rows


# ---------------------------------------------------------------- incremental keys
_hash = {}


def fhash(p):
    if p not in _hash:
        try:
            _hash[p] = hashlib.sha1(open(p, 'rb').read()).hexdigest()
        except OSError:
            _hash[p] = 'missing'
    return _hash[p]


def key(parts, files):
    h = hashlib.sha1()
    for x in parts:
        h.update(x.encode() + b'\0')
    for f in sorted(set(files)):
        h.update(f.encode() + b'=' + fhash(f).encode() + b'\0')
    return h.hexdigest()


def parse_dep(path):
    try:
        txt = open(path).read().replace('\\\n', ' ')
    except OSError:
        return None
    deps = txt.split(':', 1)[1].split() if ':' in txt else []
    return [os.path.normpath(d) for d in deps]


def jload(p):
    try:
        return json.load(open(p))
    except (OSError, ValueError):
        return None


def tag_of(src):
    return src[:-2].replace('/', '__')


def tu_flags(src):
    stem = os.path.basename(src)[:-2]
    ver = 'VERSION_J' if src.startswith('ultralib/') and stem in VERSION_J else 'VERSION_I'
    return BASE + RELAX + [f'-DBUILD_VERSION={ver}']


def to_ll(src):
    tag = tag_of(src)
    ll, kf, dep = f'{LL}/{tag}.ll', f'{LL}/{tag}.key', f'{LL}/{tag}.d'
    flags = tu_flags(src) + LL_FLAGS
    old = jload(kf)
    if old and os.path.exists(ll) and old['key'] == key([' '.join(flags)], [src] + old['deps']):
        return src, 0, old.get('warn', ''), False
    if os.path.exists(kf):
        os.remove(kf)
    raw = open(src, 'rb').read()
    cin, extra = src, []
    if any(b > 0x7F for b in raw):
        # the matching build feeds IDO the TU in EUC-JP (asm-processor --output-enc=EUC-JP): Japanese character
        # constants ('あ' = 0xA4A2) and strings must have those bytes, so the port compiles an EUC-JP copy of the TU
        # (its own includes are found through -iquote of the original directory; headers are ASCII)
        cin = f'{OUT}/euc/{src}'
        os.makedirs(os.path.dirname(cin), exist_ok=True)
        open(cin, 'wb').write(raw.decode('utf-8').encode('euc_jp'))
        extra = ['-iquote', os.path.dirname(src)]
    p = subprocess.run(flags[:1] + extra + flags[1:] + ['-MD', '-MF', dep, cin, '-o', ll], capture_output=True, text=True)
    if p.returncode:
        return src, 1, errors_only(p.stderr), True
    deps = [d for d in parse_dep(dep) or [] if d not in (src, cin)]
    rom = [d for d in deps if d.startswith(('assets/', 'build/', 'asm/'))]
    if rom:  # ROM-free rule: no TU of the port may include anything extracted from the ROM
        os.remove(ll)
        return src, 1, 'includes ROM-derived files: ' + ' '.join(rom[:5]), True
    warn = summarize_warnings(p.stderr)
    json.dump({'key': key([' '.join(flags)], [src] + deps), 'deps': deps, 'warn': warn}, open(kf, 'w'))
    return src, 0, warn, True


def errors_only(stderr):
    ls = stderr.splitlines()
    keep = [i for i, l in enumerate(ls) if ' error: ' in l or l.startswith('error:')]
    out = []
    for i in keep[:10]:
        out += ls[i:i + 3]
    return '\n'.join(out) or stderr[-3000:]


WARN = re.compile(r'warning: .*\[(-W[^\]]+)\]')


def summarize_warnings(stderr):
    c = collections.Counter(WARN.findall(stderr))
    return ' '.join(f'{k[2:]}={v}' for k, v in c.most_common())


TOOLS = ['port/tools/vbe.py', 'port/tools/sigs.py', 'port/tools/icall.py', 'port/tools/v0.py']


def v0_plan(ok, defs, sites, can):
    """S5 (port/tools/v0.py): decide which void functions return the callee result the console leaves in v0;
    mark them in `can` (canonical return i32, and can['__v0__'] = {tag: [names promoted in that TU]}), and write
    port/v0_report.tsv: every void function, how it is called for a value and what it then returns."""
    import v0, sigs
    mips = {}
    for r in tsv(f'{SYM}/v0.tsv'):
        tu, fn, nret, callonly, srcs = r
        mips[(tu, '@' + fn)] = (callonly == '1', set(srcs.split(';')) if srcs else set())
    voids = {}       # (src, name) -> (internal, ir sources, params)
    iret = {}        # (src, name) -> return type of internal functions
    vdefs = {n for n, d in defs.items() if d[0] == 'void'}
    used = collections.Counter()   # void function -> direct call sites whose (non-void) result the caller uses
    for src in ok:
        lines = open(f'{LL}/{tag_of(src)}.ll').read().split('\n')
        fns = v0.functions(lines)
        for name, (i0, i1) in fns.items():
            body = lines[i0 + 1:i1]
            for j, l in enumerate(body):
                ci = v0.call_info(l)
                if ci and ci[0] and ci[1] != 'void' and ci[2] in vdefs:
                    pat = re.compile(re.escape(ci[0]) + r'(?![\w.$-])')
                    if any(pat.search(x) for x in body[j + 1:]):
                        used[ci[2]] += 1
        for name, (i0, i1) in fns.items():
            d = sigs.parse_define(lines[i0])
            if not d:
                continue
            internal = 'internal' in d[1] or 'private' in d[1]
            if internal:
                iret[(src, name)] = d[2]
            if d[2] != 'void':
                continue
            rets = v0.analyse(lines[i0 + 1:i1])
            voids[(src, name)] = (internal, frozenset().union(*rets) if rets else frozenset(), len(rets))

    def why_not(key):
        src, name = key
        internal, irs, nret = voids[key]
        m = mips.get(key)
        if m is None:
            return 'no IDO object row'
        if not m[0]:
            bad = sorted(x for x in m[1] if not x.startswith('call:'))
            return 'console v0 not only a call result (' + ','.join(bad) + ')'
        ms = {('@' + x[5:]) if x.startswith('call:') else x for x in m[1]}
        # a path the C allows but the compiled code proved impossible (a loop that always runs once, IDO rotated it)
        # shows as `entry` in the IR only: v0 there is never read, the port returns 0 on it
        if ms != set(irs) - {'entry'}:
            return f'IR last calls {sorted(irs)} differ from the console {sorted(ms)}'
        return ''
    cand = {k: why_not(k) for k in voids}
    prom = set()

    def valued(x, src):
        if x == 'icall':
            return False
        if (src, x) in iret:
            return iret[(src, x)] in v0.V0_TYPES or (src, x) in prom
        if x in can and can[x][0] in v0.V0_TYPES:
            return True
        return any(k[1] == x and not voids[k][0] for k in prom)
    changed = True
    while changed:
        changed = False
        for k, why in cand.items():
            if why or k in prom:
                continue
            if all(valued(x, k[0]) for x in voids[k][1]):
                prom.add(k)
                changed = True
    per = collections.defaultdict(list)
    for src, name in sorted(prom):
        per[tag_of(src)].append(name)
        if not voids[(src, name)][0] and name in can:
            can[name] = ['i32'] + list(can[name][1:2]) + [''] + list(can[name][3:4])
    can['__v0__'] = {t: sorted(v) for t, v in per.items()}
    can['__v0_ext__'] = sorted(n for s, n in prom if not voids[(s, n)][0])
    rows = []
    for k in sorted(voids):
        src, name = k
        internal, irs, nret = voids[k]
        vsites = 0 if internal else used[name]
        m = mips.get(k)
        con = ';'.join(sorted(m[1])) if m else '-'
        if k in prom:
            res = 'returns v0: ' + ' / '.join(sorted(x.lstrip('@') for x in irs))
        else:
            why = cand[k] or 'a callee does not leave a value in v0 (' + ','.join(sorted(x.lstrip('@') for x in irs if not valued(x, src))) + ')'
            res = 'void (a value read reads 0): ' + why
        rows.append(f'{src}\t{name.lstrip("@")}\t{"static" if internal else "global"}\t{vsites}\t{con}\t{res}\n')
    nval = sum(1 for r in rows if r.split('\t')[3] != '0')
    write_if_changed('port/v0_report.tsv', '# tu\tfunction\tlinkage\tdirect_sites_using_the_value\tconsole_v0_at_return\tport   '
                     '(port_build.py v0_plan, port/tools/v0.py: every void function of the game C; console v0 from '
                     'port/symbols/v0.tsv; promoted functions return that v0, the others read 0 where a caller uses a '
                     'value: direct sites counted here, indirect ones are logged at run time as [icall] void target)\n'
                     + ''.join(rows))
    bad = [r for r in rows if r.split('\t')[3] != '0' and 'returns v0' not in r]
    print(f'v0: {len(voids)} void functions, {len(prom)} return the v0 of their last call; {nval} with direct value '
          f'sites ({len(bad)} of them read 0: port/v0_report.tsv)')
    return len(prom), len(voids)
FVRAM = f'{OUT}/fvram'


def tu_meta(rows):
    """S3a: per compiled TU, (a) its functions' console addresses for the address -> function table
    (port/build/fvram/<tag>.txt: 'name vram segment-index', from port/symbols/symbols.tsv) and (b) the wasm data
    section of its writable globals: ovl_<segment> for overlay TUs (reset on reload, pc_mem.c), none for main."""
    segs = [r[0] for r in tsv(f'{SYM}/segments.tsv') if r[2] not in ('-', '')]  # = port_segments order
    sidx = {s: i for i, s in enumerate(segs)}
    funcs = collections.defaultdict(list)
    bss = collections.defaultdict(list)
    for r in tsv(f'{SYM}/symbols.tsv'):
        name, vram, rom, size, typ, bind, sec, seg, obj = r
        if typ == 'func' and seg in sidx and obj.startswith('build/'):
            funcs[src_of(obj)].append(f'{name} {vram} {sidx[seg]}\n')
        # S5: the TU's console bss layout (vbe.py bss_layout keeps it: objects that overflow into the next one on the
        # console, like player.c's PlayerMatrixStore, must overflow into the same neighbours here)
        if typ in ('data', 'none') and 'bss' in sec and obj.startswith('build/') and re.match(r'^[0-9A-Fa-f]+$', size):
            bss[src_of(obj)].append(f'{name} {vram} {size} {sec}\n')
    os.makedirs(FVRAM, exist_ok=True)
    meta = {}
    for src, seg, cat, action, nf, secs in rows:
        if action != 'compile':
            continue
        fv = f'{FVRAM}/{tag_of(src)}.txt'
        write_if_changed(fv, ''.join(sorted(funcs.get(src, []))))
        write_if_changed(f'{FVRAM}/{tag_of(src)}.bss', ''.join(sorted(bss.get(src, []))))
        sec = f'ovl_{seg}' if seg in sidx and seg != 'main' and re.match(r'^[A-Za-z_]\w*$', seg) else '-'
        meta[src] = (fv, sec)
    return meta


def build_obj(src, sig_hash, abs_hash, meta):
    tag = tag_of(src)
    ll, vll, obj, kf = f'{LL}/{tag}.ll', f'{LL}/{tag}.v.ll', f'{VOBJ}/{tag}.o', f'{VOBJ}/{tag}.key'
    fv, sec = meta[src]
    k = key([(jload(f'{LL}/{tag}.key') or {}).get('key', 'none'), sig_hash, abs_hash, ' '.join(O1_FLAGS), sec],
            TOOLS + [fv, fv[:-4] + '.bss'])
    old = jload(kf)
    if old and old['key'] == k and os.path.exists(obj):
        return src, 0, old['stats'], False
    if os.path.exists(kf):
        os.remove(kf)
    p = subprocess.run([sys.executable, 'port/tools/vbe.py', ll, vll, f'{OUT}/sigdb.json', f'{OUT}/abs_syms.txt',
                        fv, sec], capture_output=True, text=True,
                       env=dict(os.environ, PYTHONDONTWRITEBYTECODE='1'))   # S5: parallel vbe.py runs raced on .pyc
    if p.returncode:
        return src, 2, p.stderr[-3000:], True
    stats = p.stdout.strip().split(' ', 1)[1] if ' ' in p.stdout.strip() else ''
    q = subprocess.run(['emcc'] + O1_FLAGS + [vll, '-o', obj], capture_output=True, text=True)
    if q.returncode:
        return src, 3, q.stderr[-3000:], True
    json.dump({'key': k, 'stats': stats}, open(kf, 'w'))
    return src, 0, stats, True


def write_if_changed(path, text):
    try:
        if open(path).read() == text:
            return False
    except OSError:
        pass
    open(path, 'w').write(text)
    return True


def abs_table():
    """absolute link symbols (not KSEG0 RAM): name -> value. Globals only, never a function."""
    out = {}
    for r in tsv(f'{SYM}/symbols.tsv'):
        name, vram, rom, size, typ, bind = r[:6]
        if bind != 'global' or typ == 'func':
            continue
        v = int(vram, 16)
        if not RDRAM[0] <= v < RDRAM[1]:
            out[name] = v
    return out


def ld_addrs_header():
    """port/build/include/ld_addrs.h: the extern declarations of the segment link symbols (the matching build gets
    them from splat in build/include; the port writes its own from the frozen symbol table, names only)."""
    pat = re.compile(r'^\w+_(ROM_START|ROM_END|VRAM|VRAM_END|(TEXT|DATA|RODATA|BSS)_(START|END|SIZE))$')
    names = sorted({r[0] for r in tsv(f'{SYM}/symbols.tsv') if r[5] == 'global' and pat.match(r[0])})
    txt = ('/* GENERATED by port/tools/port_build.py from port/symbols/symbols.tsv - do not edit. */\n'
           '#ifndef _HEADER_SYMBOLS_H_\n#define _HEADER_SYMBOLS_H_\n\n#include "common.h"\n\n' +
           ''.join(f'extern Addr {n};\n' for n in names) + '\n#endif\n')
    os.makedirs(f'{OUT}/include', exist_ok=True)
    write_if_changed(f'{OUT}/include/ld_addrs.h', txt)
    return len(names)


def run(cmd, **kw):
    p = subprocess.run(cmd, capture_output=True, text=True, **kw)
    return p.returncode, p.stdout, p.stderr


def main():
    for d in (LL, VOBJ, PCOBJ, GEN, WEB):
        os.makedirs(d, exist_ok=True)
    rows = census()
    n_ld = ld_addrs_header()
    absmap = abs_table()
    write_if_changed(f'{OUT}/abs_syms.txt', ''.join(f'{k} {v:X}\n' for k, v in sorted(absmap.items())))
    compile_srcs = [r[0] for r in rows if r[3] == 'compile']
    status = {}
    with cf.ThreadPoolExecutor(os.cpu_count() or 4) as pool:
        nll = 0
        ok = []
        fails = {}
        warns = {}
        for src, rc, msg, rebuilt in pool.map(to_ll, compile_srcs):
            nll += rebuilt
            if rc:
                fails[src] = ('C->IR', msg)
            else:
                ok.append(src)
                warns[src] = msg
        # phase 3: canonical signatures over every game TU's IR
        import sigs
        defs, sites = {}, collections.defaultdict(collections.Counter)
        for src in ok:
            sigs.scan(open(f'{LL}/{tag_of(src)}.ll').read(), defs, sites)
        can = sigs.canonical(defs, sites)
        # S5: void functions that leave a callee's result in v0 return it (port/tools/v0.py)
        nprom, nvoid = v0_plan(ok, defs, sites, can)
        write_if_changed(f'{OUT}/sigdb.json', json.dumps(can, sort_keys=True))
        mism = sorted(f for f, c in sites.items() if len(c) > 1 or
                      (f in defs and json.dumps(list(defs[f])[:2]) not in c))
        _hash.pop(f'{OUT}/sigdb.json', None); _hash.pop(f'{OUT}/abs_syms.txt', None)
        sig_hash, abs_hash = fhash(f'{OUT}/sigdb.json'), fhash(f'{OUT}/abs_syms.txt')
        nobj = 0
        tot = collections.Counter()
        objs = []
        meta = tu_meta(rows)
        _hash.clear()  # the fvram files were just (re)written
        for src, rc, msg, rebuilt in pool.map(lambda s: build_obj(s, sig_hash, abs_hash, meta), ok):
            nobj += rebuilt
            if rc:
                fails[src] = ({2: 'vbe', 3: 'IR->obj'}[rc], msg)
                continue
            objs.append(f'{VOBJ}/{tag_of(src)}.o')
            status[src] = msg
            for kv in msg.split():
                k, v = kv.split('=')
                tot[k] += int(v)
    print(f'port_build: {len(compile_srcs)} TUs to compile, {nll} IR and {nobj} objects rebuilt, '
          f'{len(fails)} failed; ld_addrs.h {n_ld} names; {len(absmap)} absolute symbols')
    for src, (stage, msg) in sorted(fails.items()):
        print(f'--- FAILED {src} ({stage})\n{msg}')
    # short K&R calls (sigs.SHORT, written by vbe.py next to each .v.ll)
    short = []
    for src in ok:
        try:
            short += [src + '\t' + l for l in open(f'{LL}/{tag_of(src)}.v.ll.short').read().splitlines()]
        except OSError:
            pass
    write_if_changed(f'{OUT}/short_calls.tsv', '# tu\tcaller\tcallee\tpassed\ttaken   (direct calls passing fewer '
                     'words than the canonical signature; the missing ones are 0 in wasm)\n' + ''.join(s + '\n' for s in sorted(short)))
    write_if_changed(f'{OUT}/sig_mismatch.txt', ''.join(f'{f}\t{json.dumps(defs.get(f))}\t{dict(sites[f])}\n' for f in mism))
    write_if_changed(f'{OUT}/vbe_report.txt', 'transforms: ' + ' '.join(f'{k}={v}' for k, v in tot.most_common()) + '\n')
    print(f'sigs: {len(can)} functions, {len(mism)} with mismatching call sites; {len(short)} short calls; '
          f'vbe: ' + ' '.join(f'{k}={v}' for k, v in tot.most_common()))

    # phase 5: PC layer (native)
    pc_srcs = sorted(f'port/ultra/{f}' for f in os.listdir('port/ultra') if f.endswith('.c')) + \
        sorted(f'port/boot/{f}' for f in os.listdir('port/boot') if f.endswith('.c')) + \
        sorted(f'port/gfx/{f}' for f in os.listdir('port/gfx') if f.endswith('.c')) + \
        sorted(f'port/audio/{f}' for f in os.listdir('port/audio') if f.endswith('.c')) + \
        sorted(f'port/save/{f}' for f in os.listdir('port/save') if f.endswith('.c'))   # S3c: renderer, S7: audio, S8: save
    ovl_secs = sorted({meta[s][1] for s, msg in status.items() if meta[s][1] != '-' and 'ovlglob=' in msg})
    segtab_c = gen_segments(ovl_secs)
    import icall_gen
    nsig = icall_gen.generate(f'{GEN}/icall_tab.c', LL)
    print(f'icall: {tot["icall"]} indirect call sites -> trampolines ({tot["icall_skip"]} left), {nsig} target '
          f'signatures; {tot["ovlglob"]} overlay globals in {len(ovl_secs)} ovl_* sections')
    pc_objs, pc_fail = [], False

    def pc_one(src):
        obj = f'{PCOBJ}/{src.replace("/", "__")[:-2]}.o'
        rc, so, se = run(PC_FLAGS + [src, '-o', obj])
        return src, obj, rc, se
    with cf.ThreadPoolExecutor(os.cpu_count() or 4) as pool:
        for src, obj, rc, se in pool.map(pc_one, pc_srcs + [segtab_c, f'{GEN}/icall_tab.c']):
            if rc:
                print(f'--- FAILED (PC layer) {src}\n{se[-3000:]}'); pc_fail = True
            else:
                if se.strip():
                    print(se[-2000:])
                pc_objs.append(obj)
    if pc_fail:
        write_census(rows, status, fails)
        sys.exit(1)

    # phase 6: stubs and mirror labels
    write_census(rows, status, fails)
    if fails:
        sys.exit(1)
    import gen_stubs
    stub_objs, stub_rep = gen_stubs.generate(objs, pc_objs, can, OUT)

    # phase 7: link
    js = f'{WEB}/pokemonsnap.js'
    link = ['emcc', f'{GEN}/rdram_labels.o'] + objs + pc_objs + [o for o in stub_objs if not o.endswith('rdram_labels.o')] + [
        '-o', js, '-O1', '-sGLOBAL_BASE=256MB', '-sINITIAL_MEMORY=384MB', '-sALLOW_MEMORY_GROWTH=1',
        '-sSTACK_SIZE=1MB', '-sASYNCIFY', '-sASYNCIFY_STACK_SIZE=65536', '-sMODULARIZE=1',
        '-sEXPORT_NAME=createPokemonSnap', '-sINVOKE_RUN=0', '-sEXIT_RUNTIME=0', '-sENVIRONMENT=worker,node',
        '-sERROR_ON_UNDEFINED_SYMBOLS=1', '-sEXPORTED_FUNCTIONS=_main,_malloc,_free,_port_set_rom,_port_stub_report,_port_vi_retrace,_port_set_pad,'
        '_port_flash_image,_port_flash_generation,_port_gfx_log_stats,_port_gfx_set_log_ops,_port_rb_record,_port_rb_journal,'
        '_port_rb_journal_size,_port_rb_load,_port_rb_point_count,'
        '_rtw_audio_rate,_rtw_audio_drain,_rtw_audio_buffer,_port_audio_capture,_port_audio_capture_ptr,'   # S7: audio
        '_port_audio_capture_frames,_port_audio_pcm_hash,'
        '_port_save_load,_port_save_poll,_rtw_save_ptr,_rtw_save_size,_rtw_save_generation',   # S8: save
        '-sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAP32,callMain', '--profiling-funcs',
        '--js-library', 'port/gfx/gfx_gl.js', '-sMAX_WEBGL_VERSION=2', '-sMIN_WEBGL_VERSION=2',   # S3c: renderer
        '-Wl,--error-limit=0', f'-Wl,-Map={OUT}/pokemonsnap.map']
    rc, so, se = run(link)
    open(f'{OUT}/link_log.txt', 'w').write(se)
    if rc:
        print('--- LINK FAILED (port/build/link_log.txt)\n' + se[-6000:])
        sys.exit(1)
    nwarn = se.count('warning')
    print(f'link OK: {js} ({os.path.getsize(js[:-3] + ".wasm")} bytes wasm), {nwarn} link warnings '
          f'(port/build/link_log.txt); {stub_rep}')


def gen_segments(ovl_secs=()):
    """port/build/gen/pc_segments.c: the segment table (name, ROM range, VRAM, bss) from port/symbols/segments.tsv,
    and (S3a) the wasm data sections ovl_<segment> holding the overlay TUs' writable C globals"""
    rows = [r for r in tsv(f'{SYM}/segments.tsv') if r[2] not in ('-', '')]
    h = lambda x: '0' if x in ('-', '') else '0x' + x
    body = ''.join(f'    {{"{r[0]}", "{r[1]}", {h(r[2])}, {h(r[3])}, {h(r[4])}, {h(r[5])}, {h(r[6])}, {h(r[7])}}},\n'
                   for r in rows)
    idx = {r[0]: i for i, r in enumerate(rows)}
    ovl = [(idx[s[4:]], s) for s in ovl_secs]
    txt = ('/* GENERATED by port/tools/port_build.py from port/symbols/segments.tsv - do not edit. Offsets only. */\n'
           '#include "pc_mem.h"\nconst PortSegment port_segments[] = {\n' + body + '};\n'
           'const unsigned port_segment_count = sizeof(port_segments) / sizeof(port_segments[0]);\n' +
           ''.join(f'extern unsigned char __start_{s}[] __attribute__((weak)), __stop_{s}[] __attribute__((weak));\n' for _, s in ovl) +
           'const PortOvlData port_ovl_data[] = {\n' +
           ''.join(f'    {{{i}, __start_{s}, __stop_{s}}},\n' for i, s in ovl) + '    {0, 0, 0}};\n'
           f'const unsigned port_ovl_data_count = {len(ovl)};\n')
    p = f'{GEN}/pc_segments.c'
    write_if_changed(p, txt)
    return p


def write_census(rows, status, fails):
    cnt = collections.Counter()
    out = []
    for src, seg, cat, action, nf, secs in sorted(rows, key=lambda r: (r[2], r[0])):
        if action == 'compile':
            res = 'FAIL:' + fails[src][0] if src in fails else ('ok' if src in status else 'not-built')
        else:
            res = '-'
        cnt[(cat, res)] += 1
        out.append('\t'.join([src, seg, cat, action, nf, res]))
    head = ('# tu\tsegment\tcategory\taction\tfunctions\tresult   (port/tools/port_build.py census: every object of the '
            'matching link + every src/**/*.c on disk; categories in port_build.py ACTIONS; names only)\n')
    write_if_changed('port/census.tsv', head + ''.join(l + '\n' for l in out))
    summ = ''.join(f'{c:12} {r:14} {n:5}  {ACTIONS[c][1]}\n' for (c, r), n in sorted(cnt.items()))
    write_if_changed(f'{OUT}/census_summary.txt', summ)
    print('census (port/census.tsv):\n' + summ, end='')


if __name__ == '__main__':
    main()
