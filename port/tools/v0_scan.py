"""What register v0 holds when each compiled function of the matching build returns (session S5).

On MIPS o32 a function's result is left in v0. A C function declared `void` whose last action is a call to a
value-returning function still leaves that callee's result in v0, and IDO-era game code reads it: Snap's Pokemon
spawn wrappers are `void f(...) { Pokemon_Spawn(...); }`, called through `GObj* (*init)(...)`, and pokemonAdd() stops
spawning at the first NULL. In wasm a void function returns nothing (the port read 0). This tool records, from the
matching build's IDO objects (ground truth, not the C), where v0 comes from at every `jr ra` of every function, so
the port build (port/tools/v0.py) can give exactly those void functions the result the console leaves in v0.

Method: `mips-linux-gnu-objdump -dr` of each compiled TU's object (port/census.tsv action compile); per function a
forward data-flow over instructions (branch delay slots and branch-likely handled) of the set of v0 definitions:
`entry` (the caller's v0: garbage), `set` (an instruction writes v0), `call:NAME` (a jal / tail `j` to NAME; R_MIPS_26
relocations name the callee, local calls are resolved by objdump's <symbol>), `icall` (jalr), and `jr` (a jump
through a register other than ra: successors unknown, the function is not analysed further).

Output port/symbols/v0.tsv: tu, function, number of returns, `callonly` (1 if every return's v0 is a call result),
the union of sources over the returns. Names only (no ROM bytes). Run in the toolchain image:
    port/docker/run.sh v0
"""
import collections, os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OBJDUMP = os.environ.get('OBJDUMP', 'mips-linux-gnu-objdump')

FUNC = re.compile(r'^([0-9a-f]+) <([^>]+)>:$')
INSN = re.compile(r'^\s+([0-9a-f]+):\t(\S+)(?:\t(.*))?$')
RELOC = re.compile(r'^\s+([0-9a-f]+): (R_MIPS_\w+)\s+(\S+)$')
TARGET = re.compile(r'\b([0-9a-f]+) <([^>+]+)(?:\+0x[0-9a-f]+)?>$')

BRANCH = {'b', 'beq', 'bne', 'beqz', 'bnez', 'blez', 'bgtz', 'bltz', 'bgez', 'bc1t', 'bc1f',
          'beql', 'bnel', 'beqzl', 'bnezl', 'blezl', 'bgtzl', 'bltzl', 'bgezl', 'bc1tl', 'bc1fl'}
LIKELY = {o for o in BRANCH if o.endswith('l') and o not in ('bal',)}
CALLS = {'jal', 'bal', 'bltzal', 'bgezal'}
NODEST = {'sb', 'sh', 'sw', 'swl', 'swr', 'sd', 'sc', 'swc1', 'sdc1', 'swc2', 'mtc1', 'mthi', 'mtlo', 'ctc1', 'dmtc1',
          'mult', 'multu', 'div', 'divu', 'dmult', 'dmultu', 'ddiv', 'ddivu', 'jr', 'jalr', 'teq', 'tne', 'tge', 'tgeu',
          'tlt', 'tltu', 'teqi', 'tnei', 'cache', 'sync', 'nop', 'break', 'syscall', 'eret', 'tlbp', 'tlbr', 'tlbwi',
          'tlbwr', 'c.eq.s', 'c.lt.s', 'c.le.s', 'c.eq.d', 'c.lt.d', 'c.le.d', 'c.un.s', 'c.olt.s', 'c.ole.s', 'c.ult.s',
          'c.ule.s', 'c.f.s', 'c.seq.s', 'c.ngl.s', 'c.nge.s', 'c.ngt.s', 'c.ngle.s', 'c.sf.s', 'c.ueq.s', 'c.un.d',
          'c.olt.d', 'c.ole.d', 'c.ult.d', 'c.ule.d', 'c.f.d', 'c.seq.d', 'c.ngl.d', 'c.nge.d', 'c.ngt.d', 'c.ngle.d',
          'c.sf.d', 'c.ueq.d'}


def writes_v0(op, args):
    if op in NODEST or op in BRANCH or op in CALLS or op in ('j',):
        return False
    return (args or '').split(',')[0].strip() == 'v0'


def parse(text):
    """objdump text -> [(name, [(addr, op, args, reloc_symbol_or_None)])]"""
    funcs, cur = [], None
    for l in text.split('\n'):
        m = FUNC.match(l)
        if m:
            cur = (m[2], [])
            funcs.append(cur)
            continue
        if cur is None:
            continue
        m = RELOC.match(l)
        if m and cur[1] and int(m[1], 16) == cur[1][-1][0]:
            a, op, args, _ = cur[1][-1]
            cur[1][-1] = (a, op, args, (m[2], m[3]))
            continue
        m = INSN.match(l)
        if m:
            cur[1].append((int(m[1], 16), m[2], m[3] or '', None))
    return funcs


def callee(op, args, rel):
    if rel and rel[0] == 'R_MIPS_26' and not rel[1].startswith('.'):
        return rel[1]
    m = TARGET.search(args)
    return m[2] if m else '?'


def analyse(insns):
    """-> (list of return source sets, flag 'jr' if a register jump made the analysis incomplete)"""
    n = len(insns)
    idx = {a: i for i, (a, *_) in enumerate(insns)}
    state = [None] * (n + 2)
    rets = []
    state[0] = frozenset({'entry'})
    work = [0]
    incomplete = False

    def xfer(i, s):
        if i >= n:
            return s
        a, op, args, rel = insns[i]
        return frozenset({'set'}) if writes_v0(op, args) else s

    def push(i, s):
        if i >= n:
            return
        old = state[i]
        new = s if old is None else old | s
        if new != old:
            state[i] = new
            work.append(i)
    while work:
        i = work.pop()
        s = state[i]
        a, op, args, rel = insns[i]
        if op in BRANCH:
            m = TARGET.search(args)
            tgt = idx.get(int(m[1], 16)) if m else None
            sd = xfer(i + 1, s)
            if tgt is not None:
                push(tgt, sd)
            else:
                incomplete = True
            if op != 'b':
                push(i + 2, s if op in LIKELY else sd)
        elif op in CALLS:
            push(i + 2, frozenset({'call:' + callee(op, args, rel)}))
        elif op == 'jalr':
            push(i + 2, frozenset({'icall'}))
        elif op == 'j':
            if rel and rel[0] == 'R_MIPS_26' and not rel[1].startswith('.'):
                rets.append(frozenset({'call:' + rel[1]}))      # tail call
            else:
                m = TARGET.search(args)
                tgt = idx.get(int(m[1], 16)) if m else None
                if tgt is None:
                    incomplete = True
                else:
                    push(tgt, xfer(i + 1, s))
        elif op == 'jr':
            if args.strip() == 'ra':
                rets.append(xfer(i + 1, s))
            else:
                incomplete = True   # jump table: successors unknown
        else:
            push(i + 1, xfer(i, s))
    return rets, incomplete


def main():
    out = os.path.join(ROOT, 'port/symbols/v0.tsv')
    rows = []
    for l in open(os.path.join(ROOT, 'port/census.tsv')):
        if l.startswith('#'):
            continue
        tu, seg, cat, action = l.rstrip('\n').split('\t')[:4]
        if action != 'compile':
            continue
        obj = os.path.join(ROOT, 'build', tu + '.o')
        if not os.path.exists(obj):
            print(f'v0_scan: no object for {tu}', file=sys.stderr)
            sys.exit(1)
        text = subprocess.run([OBJDUMP, '-dr', '--no-show-raw-insn', obj], capture_output=True, text=True,
                              check=True).stdout
        for name, insns in parse(text):
            if not insns:
                continue
            rets, inc = analyse(insns)
            srcs = set().union(*rets) if rets else set()
            if inc:
                srcs.add('jr')
            callonly = int(bool(rets) and not inc and all(s and all(x.startswith('call:') for x in s) for s in rets))
            rows.append(f'{tu}\t{name}\t{len(rets)}\t{callonly}\t{";".join(sorted(srcs))}\n')
    rows.sort()
    with open(out, 'w') as f:
        f.write('# tu\tfunction\treturns\tcallonly\tv0_sources   (port/tools/v0_scan.py: where v0 comes from at each '
                '`jr ra` of the matching build\'s IDO objects; entry = caller\'s v0, set = written by an instruction, '
                'call:F = result of F, icall = indirect call, jr = jump table, not analysed; names only)\n')
        f.writelines(rows)
    n1 = sum(r.split('\t')[3] == '1' for r in rows)
    print(f'v0_scan: {len(rows)} functions, {n1} return only call results in v0 -> {os.path.relpath(out, ROOT)}')


if __name__ == '__main__':
    main()
