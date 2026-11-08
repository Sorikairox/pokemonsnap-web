#!/usr/bin/env python3
"""Freeze the data relocations of the matching build (session S2): port/symbols/relocs.tsv.

The port does not compile the asset TUs and the unmapped asm/data; their bytes are copied from the player's ROM into
the RDRAM mirror at run time (objects that port/census.tsv compiles are left out: wasm-ld relocates those), and every pointer word in them must then be turned into a wasm address (or a function
table index). Which words are pointers is known exactly: the relocations of the matching build's objects
(build/**/*.o, SHT_REL sections of .data / .rodata). This tool lists them, with offsets and names only (no bytes: the
addend stays in the ROM word, read at run time):
    segment  word_vram  target_segment  target_kind  target
target_kind: func (a function symbol), text (a code section + addend), data (a data section / data symbol);
target: the symbol name, or the section (.text/.data/.rodata) of the word's own object for a section-relative one.
Run in the toolchain image after the matching build: port/docker/run.sh relocs (stdlib only).
"""
import collections, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import elf32  # noqa: E402

RTYPE = {2: 'R_MIPS_32', 4: 'R_MIPS_26', 5: 'R_MIPS_HI16', 6: 'R_MIPS_LO16', 7: 'R_MIPS_GPREL16'}


def tsv(p):
    return [l.rstrip('\n').split('\t') for l in open(p) if not l.startswith('#') and l.strip()]


def main():
    objs = tsv('port/symbols/objects.tsv')
    sec_vram, seg_of = {}, {}
    for out_sec, in_sec, obj, vram, size, rom, seg in objs:
        sec_vram[(obj, in_sec)] = int(vram, 16)
        seg_of[obj] = seg
    gsym = {}
    for r in tsv('port/symbols/symbols.tsv'):
        if r[5] == 'global':
            gsym[r[0]] = (r[7], r[4], int(r[1], 16))
    # only objects whose bytes the port takes from the ROM: the TUs it compiles get their pointers from wasm-ld
    compiled = {r[0] for r in tsv('port/census.tsv') if r[3] == 'compile'}
    rows, cnt, odd = [], collections.Counter(), collections.Counter()
    for obj in sorted(seg_of):
        if not os.path.exists(obj) or not obj.endswith('.o'):
            continue
        if obj[len('build/'):-2] in compiled:
            continue
        e = elf32.Elf(obj)
        secs = e.sections
        symtab = next((s for s in secs if s['type'] == 2), None)
        if symtab is None:
            continue
        strs = secs[symtab['link']]
        for rs in secs:
            if rs['type'] != 9:  # SHT_REL
                continue
            tgt = secs[rs['info']]['name']
            if tgt not in ('.data', '.rodata'):
                continue
            base = sec_vram.get((obj, tgt))
            if base is None:
                odd['section not in objects.tsv'] += 1
                continue
            for i in range(rs['size'] // 8):
                off, info = struct.unpack_from('>II', e.b, rs['offset'] + 8 * i)
                typ, si = info & 0xFF, info >> 8
                name_off, value, size, sinfo, other, shndx = struct.unpack_from('>IIIBBH', e.b, symtab['offset'] + 16 * si)
                if typ != 2:
                    odd[RTYPE.get(typ, str(typ))] += 1
                    continue
                stype = sinfo & 15
                name = e._str(strs, name_off)
                if stype == 3 or (shndx not in (0, 0xFFF1, 0xFFF2) and name and sinfo >> 4 == 0):
                    # section symbol, or a local symbol of this object: section-relative (the addend is in the word)
                    tsec = secs[shndx]['name']
                    kind = 'text' if tsec == '.text' else 'data'
                    tseg, target = seg_of[obj], tsec  # a section of the word's own object
                elif name in gsym:
                    tseg, styp, _ = gsym[name]
                    kind = 'func' if styp == 'func' else 'data'
                    target = name
                else:
                    odd['unknown target symbol'] += 1
                    tseg, kind, target = '?', 'data', name or '?'
                rows.append((seg_of[obj], base + off, tseg, kind, target))
                cnt[kind] += 1
    rows.sort(key=lambda r: (r[0], r[1]))
    head = ('# segment\tword_vram\ttarget_segment\ttarget_kind\ttarget   (port/tools/gen_relocs.py: R_MIPS_32 data '
            'relocations of the ROM-loaded objects of the matching build; offsets and names only, the addends stay in '
            'the ROM; target = symbol, or .text/.data/.rodata of the word\'s own object)\n')
    with open('port/symbols/relocs.tsv', 'w') as f:
        f.write(head)
        for seg, v, tseg, kind, target in rows:
            f.write(f'{seg}\t{v:08X}\t{tseg}\t{kind}\t{target}\n')
    print(f'gen_relocs: {len(rows)} R_MIPS_32 data relocations ({dict(cnt)}); skipped: {dict(odd)}')


if __name__ == '__main__':
    main()
