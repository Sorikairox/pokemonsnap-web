"""vbe: give a game TU *virtual big-endian* memory semantics (docs/port/MEMORY.md, "Endianness").

Storage model ("word-swapped", the N64 emulator layout): every aligned 32-bit word holds its
big-endian value as a native little-endian i32. Then
    32-bit access at A (A % 4 == 0)   -> native access at A          (pointers, s32, f32: free)
    16-bit access at A (A % 2 == 0)   -> native access at A ^ 2
     8-bit access at A                -> native access at A ^ 3
    64-bit access at A (A % 4 == 0)   -> native i64 at A, halves swapped (rotate by 32)
and every mixed-width aliasing (an s32 read as two s16, a u8 read out of a u32) behaves as on the
N64. ROM bytes become this layout with one bswap32 per word, whatever the data format is.

This pass rewrites the LLVM IR of one TU, compiled at -O0 *before* any optimisation (clang's
little-endian load narrowing/combining then only ever sees the already-rewritten, LE-correct
program):
  * load/store i8, i16 (align >= 2), i64/double: address xor / half swap as above;
  * i16 with align 1, i32/float with align < 4: helper calls (__vbe_ld16u, __vbe_ld32u, ...);
  * llvm.memcpy/memmove/memset unless word-aligned with a constant multiple-of-4 length:
    __vbe_memcpy/__vbe_memmove/__vbe_memset;
  * global definitions: `constant` -> `global`, align >= 4, size padded to a multiple of 4 (so the
    xor permutation never leaves the object), and initialisers that contain 1/2/8-byte scalars get a
    descriptor + a constructor that converts them in place at startup (__vbe_fix).
usage: vbe.py in.ll out.ll [sigdb.json [abs.txt]]  (prints one stats line)

Provenance: the generic pass (type layout, load/store/memcpy/alloca/global rewriting, RCP register calls) is adapted
from the Conker web port's port/tools/vbe.py (the user's own project, Sorikairox/conker-eu-port; no licence file,
same author). Its Conker-only tables (ROM_ABS, KSEG0 pointer sites, debug-build passes, icall) are not carried over;
the Pokémon Snap tables (libc renames, absolute link symbols) are at the end of this file. Session S2.
"""
import collections, os, re, sys

# ---------------------------------------------------------------- type layout (wasm32 datalayout)
SCAL = {'i1': (1, 1), 'i8': (1, 1), 'i16': (2, 2), 'i32': (4, 4), 'i64': (8, 8), 'float': (4, 4),
        'double': (8, 8), 'ptr': (4, 4), 'i128': (16, 16), 'fp128': (16, 16)}


def split_top(s, sep=','):
    out, depth, cur, q = [], 0, [], False
    for ch in s:
        if ch == '"':
            q = not q
        if not q:
            if ch in '([{<':
                depth += 1
            elif ch in ')]}>':
                depth -= 1
        if ch == sep and depth == 0 and not q:
            out.append(''.join(cur)); cur = []
        else:
            cur.append(ch)
    out.append(''.join(cur))
    return [x.strip() for x in out]


def take_type(s):
    """Split a leading LLVM type off s -> (type, rest)."""
    s = s.lstrip()
    if s[0] in '[{<':
        close = {'[': ']', '{': '}', '<': '>'}
        depth = 0
        for i, ch in enumerate(s):
            if ch in '[{<(':
                depth += 1
            elif ch in ']}>)':
                depth -= 1
                if depth == 0:
                    return s[:i + 1], s[i + 1:]
        raise ValueError(s)
    m = re.match(r'(%[\w.$"-]+|i\d+|float|double|ptr|half|fp128|x86_fp80)', s)
    return m[1], s[m.end():]


class Layout:
    def __init__(self, named):
        self.named = named  # name -> body text
        self.memo = {}

    def info(self, t):
        """-> (size, align, leaves) ; leaves = [(off, size, count, stride)] for 1/2/8-byte scalars."""
        if t in self.memo:
            return self.memo[t]
        if t in SCAL:
            sz, al = SCAL[t]
            r = (sz, al, [(0, sz, 1, sz)] if sz in (1, 2, 8) else [])
        elif t.startswith('%'):
            body = self.named.get(t)
            if body is None or body == 'opaque':
                raise ValueError('opaque ' + t)
            r = self.info(body)
        elif t.startswith('['):
            m = re.match(r'\[\s*(\d+)\s+x\s+(.*)\]$', t, re.S)
            n, et = int(m[1]), m[2].strip()
            esz, eal, el = self.info(et)
            leaves = []
            for (o, s, c, st) in el:
                if c == 1:
                    leaves.append((o, s, n, esz))
                elif n * len(el) <= 4096:
                    leaves += [(i * esz + o, s, c, st) for i in range(n)]
                else:
                    raise ValueError('nested array too big ' + t)
            r = (esz * n, eal, leaves)
        elif t.startswith('{') or t.startswith('<{'):
            packed = t.startswith('<')
            inner = t[2:-2] if packed else t[1:-1]
            off, al, leaves = 0, 1, []
            for ft in (split_top(inner) if inner.strip() else []):
                fsz, fal, fl = self.info(ft)
                if not packed:
                    off = (off + fal - 1) // fal * fal
                    al = max(al, fal)
                leaves += [(off + o, s, c, st) for (o, s, c, st) in fl]
                off += fsz
            off = (off + al - 1) // al * al
            r = (off, al, leaves)
        elif t.startswith('<'):  # vector
            m = re.match(r'<\s*(\d+)\s+x\s+(.*)>$', t)
            esz, eal, el = self.info(m[2].strip())
            r = (esz * int(m[1]), esz * int(m[1]), [(o, s, int(m[1]), esz) for (o, s, c, st) in el])
        else:
            raise ValueError('type ' + t)
        self.memo[t] = r
        return r


# ---------------------------------------------------------------- rewriting
LOAD = re.compile(r'^(\s*)(%[\w.$-]+) = load (volatile )?(.*)$')
STORE = re.compile(r'^(\s*)store (volatile )?(.*)$')
MEMI = re.compile(r'^(\s*)(?:tail )?call void @llvm\.(memcpy|memmove|memset)\.[\w.]+\((.*)\)(.*)$')
GLOB = re.compile(r'^(@[\w.$"-]+) = ((?:[a-z_]+ )*?)(global|constant) (.*)$')
ALLOCA = re.compile(r'^(\s*%[\w.$"-]+ = alloca )(.*)$')
ALIGN = re.compile(r'align (\d+)')
# S2: clang's va_arg pointers (needs -fno-discard-value-names): %argp.cur, %argp.cur14.aligned, ...
VA_PTR = re.compile(r'^%argp\.cur\d*(?:\.aligned)?\d*$')

DECLS = {
    '__vbe_ld16u': 'declare i16 @__vbe_ld16u(ptr)', '__vbe_st16u': 'declare void @__vbe_st16u(ptr, i16)',
    '__vbe_ld32u': 'declare i32 @__vbe_ld32u(ptr)', '__vbe_st32u': 'declare void @__vbe_st32u(ptr, i32)',
    '__vbe_memcpy': 'declare void @__vbe_memcpy(ptr, ptr, i32)',
    '__vbe_memmove': 'declare void @__vbe_memmove(ptr, ptr, i32)',
    '__vbe_memset': 'declare void @__vbe_memset(ptr, i32, i32)',
    '__vbe_fix': 'declare void @__vbe_fix(ptr, ptr, i32)',
    'llvm.fshl.i64': 'declare i64 @llvm.fshl.i64(i64, i64, i64)',
}


class Pass:
    def __init__(self, lines):
        self.lines = lines
        self.n = 0
        self.need = set()
        self.st = dict(ld8=0, st8=0, ld16=0, st16=0, ld64=0, st64=0, helper=0, mem=0, memkeep=0,
                       glob=0, globfix=0, other=0, alloca4=0, allocapad=0, va64=0)
        named = {}
        for l in lines:
            m = re.match(r'^(%[\w.$"-]+) = type (.*)$', l)
            if m:
                named[m[1]] = m[2].strip()
        self.lay = Layout(named)

    def tmp(self):
        self.n += 1
        return f'%vbe.{self.n}'

    def xaddr(self, ind, p, x, out):
        a, b, c = self.tmp(), self.tmp(), self.tmp()
        out += [f'{ind}{a} = ptrtoint ptr {p} to i32', f'{ind}{b} = xor i32 {a}, {x}',
                f'{ind}{c} = inttoptr i32 {b} to ptr']
        return c

    def rot(self, ind, v, out):
        r = self.tmp()
        out.append(f'{ind}{r} = call i64 @llvm.fshl.i64(i64 {v}, i64 {v}, i64 32)')
        self.need.add('llvm.fshl.i64')
        return r

    def do_load(self, m, out):
        ind, res, vol, rest = m[1], m[2], m[3] or '', m[4]
        parts = split_top(rest)
        ty, ptr = parts[0], parts[1]
        if not ptr.startswith('ptr '):
            return False
        p = ptr[4:].strip()
        tail = ', ' + ', '.join(parts[2:]) if len(parts) > 2 else ''
        al = ALIGN.search(tail)
        al = int(al[1]) if al else SCAL.get(ty, (1, 1))[1]
        if ty in ('i8', 'i1'):
            q = self.xaddr(ind, p, 3, out); self.st['ld8'] += 1
            out.append(f'{ind}{res} = load {vol}{ty}, ptr {q}{tail}')
        elif ty == 'i16' and al >= 2:
            q = self.xaddr(ind, p, 2, out); self.st['ld16'] += 1
            out.append(f'{ind}{res} = load {vol}{ty}, ptr {q}{tail}')
        elif ty == 'i16':
            out.append(f'{ind}{res} = call i16 @__vbe_ld16u(ptr {p})'); self.need.add('__vbe_ld16u')
            self.st['helper'] += 1
        elif ty in ('i64', 'double') and VA_PTR.match(p):
            self.st['va64'] += 1  # S2: a 64-bit va_arg read: the variadic buffer is native (wasm backend), keep it
            return False
        elif ty in ('i64', 'double') and al >= 4:
            v = self.tmp(); self.st['ld64'] += 1
            out.append(f'{ind}{v} = load {vol}i64, ptr {p}{tail}')
            r = self.rot(ind, v, out)
            out.append(f'{ind}{res} = ' + (f'bitcast i64 {r} to double' if ty == 'double' else f'or i64 {r}, 0'))
        elif ty in ('i32', 'float') and al < 4:
            self.need.add('__vbe_ld32u'); self.st['helper'] += 1
            if ty == 'i32':
                out.append(f'{ind}{res} = call i32 @__vbe_ld32u(ptr {p})')
            else:
                v = self.tmp()
                out += [f'{ind}{v} = call i32 @__vbe_ld32u(ptr {p})', f'{ind}{res} = bitcast i32 {v} to float']
        elif ty in ('i32', 'float', 'ptr'):
            return False
        else:
            self.st['other'] += 1
            return False
        return True

    def do_store(self, m, out):
        ind, vol, rest = m[1], m[2] or '', m[3]
        parts = split_top(rest)
        ty, val = take_type(parts[0])
        val = val.strip()
        ptr = parts[1]
        if not ptr.startswith('ptr '):
            return False
        p = ptr[4:].strip()
        tail = ', ' + ', '.join(parts[2:]) if len(parts) > 2 else ''
        al = ALIGN.search(tail)
        al = int(al[1]) if al else SCAL.get(ty, (1, 1))[1]
        if ty in ('i8', 'i1'):
            q = self.xaddr(ind, p, 3, out); self.st['st8'] += 1
            out.append(f'{ind}store {vol}{ty} {val}, ptr {q}{tail}')
        elif ty == 'i16' and al >= 2:
            q = self.xaddr(ind, p, 2, out); self.st['st16'] += 1
            out.append(f'{ind}store {vol}{ty} {val}, ptr {q}{tail}')
        elif ty == 'i16':
            out.append(f'{ind}call void @__vbe_st16u(ptr {p}, i16 {val})'); self.need.add('__vbe_st16u')
            self.st['helper'] += 1
        elif ty in ('i64', 'double') and al >= 4:
            v = val
            if ty == 'double':
                v = self.tmp(); out.append(f'{ind}{v} = bitcast double {val} to i64')
            r = self.rot(ind, v, out); self.st['st64'] += 1
            out.append(f'{ind}store {vol}i64 {r}, ptr {p}{tail}')
        elif ty in ('i32', 'float') and al < 4:
            v = val
            if ty == 'float':
                v = self.tmp(); out.append(f'{ind}{v} = bitcast float {val} to i32')
            out.append(f'{ind}call void @__vbe_st32u(ptr {p}, i32 {v})'); self.need.add('__vbe_st32u')
            self.st['helper'] += 1
        elif ty in ('i32', 'float', 'ptr'):
            return False
        else:
            self.st['other'] += 1
            return False
        return True

    def do_alloca(self, m, out):
        """(Conker P185) stack slots get IDO's layout: every alloca starts on a word (align >= 4; 8-byte types keep clang's
        align 8) and a fixed-size slot is padded to a multiple of 4 bytes. So no two locals share a word: a 32-bit
        access to a 2-aligned `s16 x[2]` (passed as `s32 *`, P178 event_load) or an xor-2 / xor-3 sub-word access
        can no longer reach a neighbouring slot."""
        parts = split_top(m[2])
        ty, rest = parts[0].strip(), [x.strip() for x in parts[1:]]
        al = next((int(x.split()[1]) for x in rest if x.startswith('align ')), 1)
        hascount = any(not x.startswith('align ') and not x.startswith('addrspace') for x in rest)
        try:
            size = self.lay.info(ty)[0]
        except (ValueError, TypeError, AttributeError):
            size = None
        nal = max(al, 4)
        if size is not None and size >= 8 and self.lay.info(ty)[1] >= 8:
            nal = max(nal, 8)
        nty = ty
        if not hascount and size is not None and size % 4:
            nty = f'<{{ {ty}, [{4 - size % 4} x i8] }}>'
            self.st['allocapad'] += 1
        if nal == al and nty == ty:
            return False
        if nal != al:
            self.st['alloca4'] += 1
        rest = [x for x in rest if not x.startswith('align ')] + [f'align {nal}']
        out.append(m[1] + ', '.join([nty] + rest))
        return True

    def do_mem(self, m, out):
        ind, kind, args = m[1], m[2], split_top(m[3])
        def strip_attrs(a):  # "ptr align 4 %x" -> ("%x", 4)
            t = a.split()
            al = int(t[t.index('align') + 1]) if 'align' in t else 1
            return t[-1] if not t[-1].startswith('noundef') else t[-2], al
        d, dal = strip_attrs(args[0])
        ln = args[2].split()[-1]
        if kind == 'memset':
            v = args[1].split()[-1]
            ok = dal >= 4 and ln.lstrip('-').isdigit() and int(ln) % 4 == 0
            if ok:
                self.st['memkeep'] += 1
                return False
            x = self.tmp()
            out.append(f'{ind}{x} = zext i8 {v} to i32')
            out.append(f'{ind}call void @__vbe_memset(ptr {d}, i32 {x}, i32 {ln})')
        else:
            s, sal = strip_attrs(args[1])
            if dal >= 4 and sal >= 4 and ln.isdigit() and int(ln) % 4 == 0:
                self.st['memkeep'] += 1
                return False
            out.append(f'{ind}call void @__vbe_{kind}(ptr {d}, ptr {s}, i32 {ln})')
        self.need.add('__vbe_' + kind); self.st['mem'] += 1
        return True

    def do_global(self, m, out, fixes):
        name, attrs, kw, rest = m[1], m[2], m[3], m[4]
        if name.startswith('@llvm.') or 'external' in attrs.split():
            return False
        ty, rest2 = take_type(rest)
        parts = split_top(rest2)
        init, opts = parts[0], parts[1:]
        try:
            size, _, leaves = self.lay.info(ty)
        except ValueError:
            self.st['other'] += 1
            return False
        opts = [o for o in opts if not o.startswith('align ')]
        meta = [o for o in opts if o.startswith('!')]  # (Conker P342) -g builds: `!dbg !N` attachments go after the align
        opts = [o for o in opts if not o.startswith('!')]
        al = ALIGN.search(rest2)
        al = max(4, int(al[1]) if al else 4)
        pad = (-size) % 4
        if pad:
            ty2 = f'<{{ {ty}, [{pad} x i8] }}>'
            init = 'zeroinitializer' if init == 'zeroinitializer' else f'<{{ {ty} {init}, [{pad} x i8] zeroinitializer }}>'
            ty = ty2
        attrs = attrs.replace('unnamed_addr ', '')  # objects must not be merged/rewritten as constants
        out.append(f'{name} = {attrs}global {ty} {init}' + ''.join(', ' + o for o in opts) + f', align {al}' + ''.join(', ' + o for o in meta))
        self.st['glob'] += 1
        if leaves and init not in ('zeroinitializer', 'undef', 'poison'):
            fixes.append((name, leaves, ty, size))
        return True

    def run(self):
        out, fixes = [], []
        for l in self.lines:
            done = False
            if l.startswith('  '):
                m = LOAD.match(l)
                if m:
                    done = self.do_load(m, out)
                else:
                    m = STORE.match(l)
                    if m:
                        done = self.do_store(m, out)
                    else:
                        m = MEMI.match(l)
                        if m:
                            done = self.do_mem(m, out)
                        else:
                            m = ALLOCA.match(l)
                            if m:
                                done = self.do_alloca(m, out)
            elif l.startswith('@'):
                m = GLOB.match(l)
                if m:
                    done = self.do_global(m, out, fixes)
            if not done:
                out.append(l)
        if fixes:
            self.st['globfix'] = len(fixes)
            calls = []
            for i, (g, leaves, ty, size) in enumerate(fixes):
                # header word 0: our layout size, word 1: LLVM's alloc size (checked at run time)
                words = ', '.join([f'i32 {size}', f'i32 ptrtoint (ptr getelementptr ({ty}, ptr null, i32 1) to i32)']
                                  + [f'i32 {x}' for lf in leaves for x in lf])
                out.append(f'@__vbe_desc.{i} = private constant [{4 * len(leaves) + 2} x i32] [{words}]')
                calls.append(f'  call void @__vbe_fix(ptr {g}, ptr @__vbe_desc.{i}, i32 {len(leaves)})')
            out += ['define internal void @__vbe_ctor() {'] + calls + ['  ret void', '}']
            if any(l.startswith('@llvm.global_ctors') for l in self.lines):
                raise SystemExit('vbe: TU already has llvm.global_ctors (not supported)')
            out.append('@llvm.global_ctors = appending global [1 x { i32, ptr, ptr }] '
                       '[{ i32, ptr, ptr } { i32 101, ptr @__vbe_ctor, ptr null }]')
            self.need.add('__vbe_fix')
        text = '\n'.join(out)
        for k in sorted(self.need):
            if not re.search(r'^declare [^\n]*@' + re.escape(k) + r'\(', text, re.M):
                text += '\n' + DECLS[k]
        return text + '\n'


# ---------------------------------------------------------------- Pokémon Snap specifics (S2)
# Game TUs that define libc names (the decomp's own ultralib libc / gu) would replace emscripten's libc for the PC
# layer too (musl's vfprintf calling the game's vbe strlen / memcpy). Every reference in every game TU is renamed to
# @vbe.<name>. LIBC_CLASH = names that game TUs define and libc.a / libcompiler_rt.a define as well (port_build.py
# checks the list against the objects after each build and fails on a new clash); LIBC_VBE = libc names that the game
# only calls (ultralib asm: bcopy / bzero / bcmp / sqrtf) and that the PC layer defines with vbe semantics
# (port/ultra/pc_libc.c), instead of emscripten's host-byte versions.
LIBC_CLASH = ['acosf', 'asinf', 'atan2f', 'atanf', 'cosf', 'ldiv', 'lldiv', 'memcpy', 'sinf', 'sprintf', 'strchr', 'strlen',
              'tanf']
LIBC_VBE = ['bcopy', 'bzero', 'bcmp', 'sqrtf']
RENAME = re.compile(r'@(' + '|'.join(LIBC_CLASH + LIBC_VBE) + r')\b(?![.\w])')

# Absolute link symbols (port/tools/port_build.py writes the table from port/symbols/symbols.tsv): every symbol of
# the matching link whose value is not a KSEG0 RAM address (segment ROM offsets *_ROM_START / *_ROM_END, sizes
# *_SIZE, the ROM-side symbols of bin / vpk0 segments). The game uses them as numbers (`&beach_code_ROM_START` handed
# to the DMA), so every reference becomes a constant `inttoptr` and the external declaration is dropped.
EXT_DECL = re.compile(r'^@([\w.$-]+) = external (?:dso_local )?global [^\n]*$', re.M)
SYMREF = re.compile(r'@([\w.$-]+)\b(?![.\w])')


def abs_syms(text, absmap, st):
    if not absmap:
        return text
    dropped = set()

    def decl(m):
        if m[1] in absmap:
            dropped.add(m[1])
            st['abs_decl'] += 1
            return ''
        return m[0]
    text = EXT_DECL.sub(decl, text)
    if not dropped:
        return text

    def ref(m):
        if m[1] in dropped:
            st['abs_ref'] += 1
            return f'inttoptr (i32 {absmap[m[1]]} to ptr)'
        return m[0]
    return SYMREF.sub(ref, text)


# S3a: console RAM addresses used as pointers (WPO trap 2). Game C hard-codes KSEG0 addresses (`#define VPK_VRAM
# 0x802BC110`, `UNK_STUFF_VRAM 0x80200000` decompressed into and then called, `*(u8*) 0x807FFFF0`), passes them as
# u32 and converts them to pointers later. Game memory is the RDRAM mirror, so every integer -> pointer conversion in
# game code maps KSEG0 0x80000000..0x807FFFFF to the same offset in n64_rdram, and keeps any other value (wasm
# addresses, which never reach 0x80000000; small segment / null values):
#   * constant `inttoptr (i32 C to ptr)` with C in KSEG0 RAM -> `getelementptr (i8, ptr @n64_rdram, i32 C - K0)`;
#   * run-time `%p = inttoptr i32 %x to ptr` -> range check + select, then the inttoptr.
# Runs before the vbe pass (whose own lane-address inttoptrs are left alone). KSEG1 RDRAM (0xA0000000..) is not
# mapped: a cheap test would also catch wasm addresses at 0x20000000.. once the heap grows past 512 MiB.
K0, K0_END = 0x80000000, 0x80800000
K0_CONST = re.compile(r'inttoptr \(i32 (-?\d+) to ptr\)')
K0_RUN = re.compile(r'^(\s*)(%[\w.$-]+) = inttoptr i32 (%[\w.$-]+) to ptr((?:, !dbg !\d+)?)$', re.M)
K0_LOAD = re.compile(r'^(\s*)(%[\w.$-]+) = load (volatile )?ptr, ptr ([%@][\w.$-]+)((?:, align \d+)?(?:, !\w+ !\d+)*)$', re.M)


def k0_ptrs(text, st):
    def const(m):
        v = int(m[1]) & 0xFFFFFFFF
        if K0 <= v < K0_END:
            st['k0_const'] += 1
            return f'getelementptr (i8, ptr @n64_rdram, i32 {v - K0})'
        return m[0]
    text = K0_CONST.sub(const, text)
    n = [0]

    def run(m):
        n[0] += 1
        st['k0_run'] += 1
        ind, r, x, dbg = m[1], m[2], m[3], m[4]
        t = f'%k0.{n[0]}'
        return (f'{ind}{t}.a = add i32 {x}, -2147483648{dbg}\n'
                f'{ind}{t}.b = icmp ult i32 {t}.a, 8388608{dbg}\n'
                f'{ind}{t}.c = add i32 {t}.a, ptrtoint (ptr @n64_rdram to i32){dbg}\n'
                f'{ind}{t}.d = select i1 {t}.b, i32 {t}.c, i32 {x}{dbg}\n'
                f'{ind}{r} = inttoptr i32 {t}.d to ptr{dbg}')
    text = K0_RUN.sub(run, text)
    # pointers loaded from game memory: data the relocations do not cover (vpk0-compressed blobs the game
    # decompresses at run time: sprites with KSEG0 pointers inside; unmapped data) hold console addresses. Every
    # `load ptr` that is not a plain local variable (an alloca slot of the -O0 IR) is mapped the same way.
    allocas = set()
    # the TU's own C globals hold wasm addresses (game stores, initialisers): not mapped; mirror labels (external
    # declarations: data from the ROM) are
    cglob = set(re.findall(r'^(@[\w.$-]+) = (?!external)', text, re.M))

    def ld(m):
        ind, r, vol, p, rest = m[1], m[2], m[3] or '', m[4], m[5]
        if p in allocas or p in cglob:
            return m[0]
        n[0] += 1
        st['k0_load'] += 1
        t = f'%k0.{n[0]}'
        dbg = re.search(r', !dbg !\d+$', rest)
        dbg = dbg[0] if dbg else ''
        return (f'{ind}{t}.l = load {vol}ptr, ptr {p}{rest}\n'
                f'{ind}{t}.i = ptrtoint ptr {t}.l to i32{dbg}\n'
                f'{ind}{t}.a = add i32 {t}.i, -2147483648{dbg}\n'
                f'{ind}{t}.b = icmp ult i32 {t}.a, 8388608{dbg}\n'
                f'{ind}{t}.c = add i32 {t}.a, ptrtoint (ptr @n64_rdram to i32){dbg}\n'
                f'{ind}{t}.d = select i1 {t}.b, i32 {t}.c, i32 {t}.i{dbg}\n'
                f'{ind}{r} = inttoptr i32 {t}.d to ptr{dbg}')
    out = []
    for blk in re.split(r'(?=^define )', text, flags=re.M):
        allocas = set(re.findall(r'^\s*(%[\w.$-]+) = alloca ', blk, re.M))
        out.append(K0_LOAD.sub(ld, blk))
    text = ''.join(out)
    if (st['k0_const'] or st['k0_run'] or st['k0_load']) and not re.search(r'^@n64_rdram = ', text, re.M):
        text += '\n@n64_rdram = external global [8388608 x i8]\n'
    return text


# S5: function identity. Data the relocations do not cover (the `bin` blobs inside code segments: world block
# descriptors, Pokemon tables, scene setups) holds functions as console addresses; calls through them work
# (icall.py's __port_fptr_fix), but the game also compares them with its functions (world/block.c: `&drawSkyBox2Cycle
# == gfx->renderFunc`), and a console address is never equal to a wasm function-table index. Every pointer equality
# test whose operands are not both constants maps each non-constant operand through __port_fptr_canon (pc_icall.c):
# a console / mirror address that is the start of a function of the overlay resident there becomes that function's
# table index, anything else is unchanged. Two pointers compare equal exactly when they do on the console.
FEQ = re.compile(r'^(\s*)(%[\w.$-]+) = icmp (eq|ne) ptr ([^,]+), ([^,]+?)((?:, !dbg !\d+)?)$', re.M)


def fptr_eq(text, st):
    n = [0]

    def one(ind, v, out, dbg):
        if not v.startswith('%'):
            return v
        n[0] += 1
        t = f'%fq.{n[0]}'
        out.append(f'{ind}{t} = call ptr @__port_fptr_canon(ptr {v}){dbg}')
        return t

    def sub(m):
        ind, r, op, a, b, dbg = m[1], m[2], m[3], m[4].strip(), m[5].strip(), m[6]
        if a == 'null' or b == 'null' or not (a.startswith('%') or b.startswith('%')):
            return m[0]
        out = []
        a2, b2 = one(ind, a, out, dbg), one(ind, b, out, dbg)
        st['fptr_eq'] += 1
        return '\n'.join(out + [f'{ind}{r} = icmp {op} ptr {a2}, {b2}{dbg}'])
    text = FEQ.sub(sub, text)
    if st['fptr_eq'] and not re.search(r'^declare [^\n]*@__port_fptr_canon\(', text, re.M):
        text += '\ndeclare ptr @__port_fptr_canon(ptr)\n'
    return text


# S5: the console's bss layout of a TU. The decomp declares some objects smaller than the game uses them and keeps
# the console addresses with unreferenced filler symbols after them: player.c `struct DObjDynamicStore
# PlayerMatrixStore; // size 0x38` (4 bytes + VLA; the player's DObj writes 0x34 bytes of matrix data into it) is
# followed by `u8 D_803AE4A5_54E8B5; u16 D_803AE4A6_54E8B6; u8 D_803AE4A8_54E8B8[0x30];`. On the console the overflow
# lands in that filler; in wasm clang placed IsTutorialEnabled and MainCameraBorder* there, the borders became huge and
# every main-camera scissor was empty (the Beach went black once the player moved). Every zero-initialised global
# that the matching build puts in a bss section is therefore laid out at its console offset inside one byte array
# per console section (`@__bss.<tu>.<section>`, 16-aligned at a 16-aligned console address, so vbe's a ^ 3 / a ^ 2
# sub-word addressing matches the mirror's) and the global becomes an alias of its place. Overflows then reach the
# same neighbours as on the console. Initialised data keeps its own globals.
GDEF = re.compile(r'^(@[\w.$-]+) = ((?:(?:internal|private|dso_local|hidden|common|unnamed_addr|local_unnamed_addr) )*)'
                  r'global (.+?)((?:, section "[^"]*")?), align (\d+)$')


def zero_init(t):
    """`T init` of a global definition: True if the initialiser is all zeros"""
    if '@' in t:
        return False
    t = re.sub(r'\[\d+ x ', '[', t)
    t = re.sub(r'\bi\d+\b', '', t)
    t = re.sub(r'%[\w.$-]+', '', t)
    return not re.search(r'(?<![\w.])-?\d*[1-9]\d*(?:\.\d+)?(?:e[+-]?\d+)?\b', t.replace('0.000000e+00', '0'))


def bss_layout(text, rows, tag, st):
    lines = text.split('\n')
    where = {}
    for i, l in enumerate(lines):
        if l.startswith('@') and ' global ' in l:
            m = GDEF.match(l)
            if m:
                where[m[1]] = (i, m)
    groups = collections.defaultdict(list)
    for name, vram, size, sec in rows:
        w = where.get('@' + name)
        if not w:
            continue
        i, m = w
        if 'external' in m[2] or not zero_init(m[3]):
            continue
        groups[sec].append((int(vram, 16), int(size, 16), '@' + name, i, m))
    out_defs = []
    for sec, mem in sorted(groups.items()):
        base = min(v for v, *_ in mem) & ~15
        end = max(v + s for v, s, *_ in mem)
        n = ((end - base + 3) & ~3) + 16
        blob = f'@__bss.{tag}.{re.sub(r"[^A-Za-z0-9_]", "_", sec)}'
        secattr = mem[0][4][4]
        out_defs.append(f'{blob} = internal global [{n} x i8] zeroinitializer{secattr}, align 16')
        for v, s, name, i, m in mem:
            link = [t for t in m[2].split() if t in ('internal', 'private', 'dso_local', 'hidden')]
            ty = m[3][:m[3].rfind(' ')].strip() if not m[3].rstrip().endswith(('}>', '}', ']', '>')) else None
            if ty is None:   # a padded aggregate initialiser <{ i8 0, [3 x i8] zeroinitializer }>: type up to its value
                d, k = 0, 0
                for k, c in enumerate(m[3]):
                    d += c in '<{[('
                    d -= c in '>}])'
                    if d == 0 and c in '>}]' and k:
                        break
                ty = m[3][:k + 1]
            lines[i] = f'{name} = {" ".join(link) + " " if link else ""}alias {ty}, getelementptr (i8, ptr {blob}, i32 {v - base})'
            st['bss_layout'] = st.get('bss_layout', 0) + 1
    return '\n'.join(lines + out_defs) + '\n'


# RCP / PI / SI register accesses through a constant KSEG1 address (rcp.h IO_READ / IO_WRITE in game C) are not
# memory on the PC. Constant
# 32-bit loads/stores in 0xA4000000..0xA4FFFFFF become calls to the PC register model
# (port/ultra/pc_io.c __port_io_read / __port_io_write). Non-constant addresses are left alone.
IO_LD = re.compile(r'^(\s*)(%[\w.$-]+) = load (?:volatile )?i32, ptr inttoptr \(i32 (-?\d+) to ptr\)(?:, align \d+)?((?:, !dbg !\d+)?)$', re.M)
IO_ST = re.compile(r'^(\s*)store (?:volatile )?i32 ([^,]+), ptr inttoptr \(i32 (-?\d+) to ptr\)(?:, align \d+)?((?:, !dbg !\d+)?)$', re.M)


def rcp_io(text, st):
    def isio(n):
        return 0xA4000000 <= (int(n) & 0xFFFFFFFF) <= 0xA4FFFFFF
    def ld(m):
        if not isio(m[3]):
            return m[0]
        st['rcp_io'] += 1
        return f'{m[1]}{m[2]} = call i32 @__port_io_read(i32 {m[3]}){m[4]}'
    def stv(m):
        if not isio(m[3]):
            return m[0]
        st['rcp_io'] += 1
        return f'{m[1]}call void @__port_io_write(i32 {m[3]}, i32 {m[2]}){m[4]}'
    n = st['rcp_io']
    text = IO_ST.sub(stv, IO_LD.sub(ld, text))
    # (Conker P42) IO_READ with a computed address, i.e. PHYS_TO_K1(x) = `or i32 x, 0xA0000000` -> inttoptr -> volatile
    # load (Conker: IO_READ(osRomBase | 0xB0000D24), the cart word). The load becomes __port_io_read(x);
    # the register model answers the cart domain 0xB0000000.. with the big-endian ROM word.
    k1 = set(re.findall(r'^\s*(%[\w.$-]+) = or i32 %[\w.$-]+, -1610612736(?:, !dbg !\d+)?$', text, re.M))
    if k1:
        ptr = {}
        for m in re.finditer(r'^\s*(%[\w.$-]+) = inttoptr i32 (%[\w.$-]+) to ptr(?:, !dbg !\d+)?$', text, re.M):
            if m[2] in k1:
                ptr[m[1]] = m[2]
        def dld(m):
            if m[3] not in ptr:
                return m[0]
            st['rcp_io_dyn'] += 1
            return f'{m[1]}{m[2]} = call i32 @__port_io_read(i32 {ptr[m[3]]}){m[4]}'
        text = re.sub(r'^(\s*)(%[\w.$-]+) = load volatile i32, ptr (%[\w.$-]+)(?:, align \d+)?((?:, !dbg !\d+)?)$', dld, text, flags=re.M)
        if st['rcp_io_dyn']:
            n = -1
    if st['rcp_io'] != n:
        if not re.search(r'^declare [^\n]*@__port_io_read\(', text, re.M):
            text += '\ndeclare i32 @__port_io_read(i32)\n'
        if not re.search(r'^declare [^\n]*@__port_io_write\(', text, re.M):
            text += '\ndeclare void @__port_io_write(i32, i32)\n'
    return text


def main():
    src, dst = sys.argv[1], sys.argv[2]
    sigdb = sys.argv[3] if len(sys.argv) > 3 else None
    absf = sys.argv[4] if len(sys.argv) > 4 else None
    absmap = {}
    if absf:
        for l in open(absf):
            n, v = l.split()
            absmap[n] = int(v, 16)
    st0 = collections.Counter()
    text = open(src).read()
    if sigdb:   # S5: void functions returning their last callee's v0 (port/tools/v0.py; plan from port_build.py)
        import json, v0
        db = json.load(open(sigdb))
        mine = db.get('__v0__', {}).get(os.path.basename(src)[:-3], [])
        allp = set(db.get('__v0_ext__', [])) | set(mine)
        lines = text.split('\n')
        n = 0
        for f in mine:
            lines, n = v0.promote(lines, f, allp, n)
            st0['v0_promoted'] += 1
        internal = {f for f in mine if f not in db.get('__v0_ext__', [])}
        if internal:
            lines = v0.rewrite_internal_sites(lines, internal)
        text = '\n'.join(lines)
    text = RENAME.sub(r'@vbe.\1', text)
    text = fptr_eq(k0_ptrs(rcp_io(abs_syms(text, absmap, st0), st0), st0), st0)
    p = Pass(text.split('\n'))
    p.st.update(st0)
    if sigdb:  # canonical signatures (port/tools/sigs.py, collected by port_build.py before this pass)
        import json, sigs
        st = collections.Counter()
        can = json.load(open(sigdb))
        for k in LIBC_CLASH + LIBC_VBE:  # sigdb is collected before the rename
            if '@' + k in can:
                can['@vbe.' + k] = can['@' + k]
        p.lines = sigs.normalize(p.lines, can, p.tmp, st)
        p.st.update(st)
    # S3a: indirect calls -> site-typed trampolines with a signature check (port/tools/icall.py)
    import icall
    st2 = collections.Counter()
    p.lines, need = icall.rewrite(p.lines, st2)
    regs = icall.registrations(p.lines)
    p.st.update(st2)
    vrams = {}
    fvf = sys.argv[5] if len(sys.argv) > 5 else '-'
    if fvf != '-':  # 'name vram segment-index' per function of this TU in the matching build (port_build.py)
        for l in open(fvf):
            n, v, s = l.split()
            vrams['@' + n] = (int(v, 16), int(s))
    text = icall.epilogue(p.run(), need, regs, vrams)
    # S5: console bss layout (bss_layout below)
    if fvf != '-' and os.path.exists(fvf[:-4] + '.bss'):
        text = bss_layout(text, [l.split() for l in open(fvf[:-4] + '.bss') if l.strip()],
                          os.path.basename(src)[:-3], p.st)
    # S3a: writable globals of an overlay TU go to the wasm data section ovl_<segment>, so that the PC layer can reset
    # them when the game loads the overlay again (pc_mem.c port_dma_loaded; __start_/__stop_ symbols)
    sec = sys.argv[6] if len(sys.argv) > 6 else '-'
    if sec != '-':
        def put(m):
            p.st['ovlglob'] = p.st.get('ovlglob', 0) + 1
            body = m[2]
            am = re.search(r', align \d+$', body)
            return (f'{m[1]}{body[:am.start()]}, section "{sec}"{body[am.start():]}' if am
                    else f'{m[1]}{body}, section "{sec}"')
        text = re.sub(r'^(@(?!llvm\.|__icall|__vbe)[\w.$-]+ = (?:(?:internal|private|dso_local|hidden|unnamed_addr|'
                      r'local_unnamed_addr|common|weak|linkonce_odr|weak_odr) )*global )((?!.*\bsection\b).*)$',
                      put, text, flags=re.M)
    open(dst, 'w').write(text)
    open(dst + '.isig', 'w').write(''.join(sorted({w + '\n' for _, w in regs})))
    if sigdb:
        import sigs
        open(dst + '.short', 'w').write(''.join(f'{a}\t{b}\t{c}\t{d}\n' for a, b, c, d in sigs.SHORT))
    print(src, ' '.join(f'{k}={v}' for k, v in p.st.items() if v))


if __name__ == '__main__':
    main()
