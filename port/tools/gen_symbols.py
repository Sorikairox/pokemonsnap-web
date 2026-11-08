#!/usr/bin/env python3
"""Freeze the matching build's layout into port/symbols/*.tsv (names, addresses, ROM offsets and sizes only).

Inputs: build/pokemonsnap.map + build/pokemonsnap.elf (port/docker/run.sh build) and splat.yaml. Outputs:
  segments.tsv  segment, type (splat), rom_start, rom_end, vram_start, vram_end, bss_start, bss_end, exclusive_ram_id
                (one row per top-level splat segment; vram of raw-data segments is the linker's running address only)
  objects.tsv   out_section, in_section, object, vram, size, rom ('-' for bss), segment  (map input sections, size > 0)
  symbols.tsv   name, vram, rom ('-' for bss/abs), size, type, bind, section, segment, object  (ELF symtab order)
rom = section load address + (vram - section address). The tool checks the map against the ELF (section addresses,
sizes, *_ROM_START symbols) and that the ROM ranges are contiguous and end at 16 MiB; any mismatch is an error.
"""
import bisect
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import elf32  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
MAP = os.path.join(ROOT, 'build', 'pokemonsnap.map')
ELF = os.path.join(ROOT, 'build', 'pokemonsnap.elf')
YAML = os.path.join(ROOT, 'splat.yaml')
OUT = os.path.join(ROOT, 'port', 'symbols')
ROM_SIZE = 0x1000000
HEX = r'0x([0-9a-f]+)'
OUTSEC = re.compile(rf'^(\.\S+)(?:\s+{HEX}\s+{HEX}(?:\s+load address {HEX})?)?\s*$')
OUTSEC_CONT = re.compile(rf'^\s+{HEX}\s+{HEX}(?:\s+load address {HEX})?\s*$')
INSEC = re.compile(rf'^ (\.\S+|COMMON)(?:\s+{HEX}\s+{HEX}\s+(\S+))?\s*$')
INSEC_CONT = re.compile(rf'^\s+{HEX}\s+{HEX}\s+(\S+)\s*$')


def parse_map(path):
    """{out_section: dict(vma, size, lma, inputs=[(vma, size, in_section, object)])}"""
    lines = open(path, encoding='latin-1').read().split('\n')
    start = lines.index('Linker script and memory map')
    outs, cur, pending_out, pending_in = {}, None, None, None
    for line in lines[start + 1:]:
        if pending_out is not None:
            m = OUTSEC_CONT.match(line)
            if m:
                vma, size, lma = (int(x, 16) if x else None for x in m.groups())
                cur = outs[pending_out] = dict(vma=vma, size=size, lma=vma if lma is None else lma, inputs=[])
            pending_out = None
            continue
        if pending_in is not None:
            m = INSEC_CONT.match(line)
            if m and cur is not None:
                cur['inputs'].append((int(m.group(1), 16), int(m.group(2), 16), pending_in, m.group(3)))
            pending_in = None
            continue
        m = OUTSEC.match(line)
        if m:
            name, vma, size, lma = m.groups()
            if vma is None:
                pending_out = name
            else:
                vma, size = int(vma, 16), int(size, 16)
                cur = outs[name] = dict(vma=vma, size=size, lma=int(lma, 16) if lma else vma, inputs=[])
            continue
        if line.startswith('/DISCARD/') or line.startswith('OUTPUT('):
            cur = None
            continue
        m = INSEC.match(line)
        if m and cur is not None:
            sec, vma, size, obj = m.groups()
            if vma is None:
                pending_in = sec
            else:
                cur['inputs'].append((int(vma, 16), int(size, 16), sec, obj))
    return outs


def splat_segments(path):
    """{name: (type, exclusive_ram_id)} of the top-level splat segments (needs PyYAML: run in the tools image)."""
    import yaml
    out = {}
    for seg in yaml.safe_load(open(path, encoding='utf-8'))['segments']:
        if isinstance(seg, dict):
            name, typ, excl = seg.get('name'), seg.get('type'), seg.get('exclusive_ram_id', '-')
        else:
            name, typ, excl = (seg[2] if len(seg) > 2 else None), (seg[1] if len(seg) > 1 else None), '-'
        if name and typ:
            out[str(name).replace('/', '_')] = (typ, excl)
            out.setdefault(str(name).rsplit('/', 1)[-1], (typ, excl))
    return out


def seg_of(out_section):
    s = out_section[1:]
    return s[:-4] if s.endswith('_bss') else s


def main():
    outs = parse_map(MAP)
    elf = elf32.Elf(ELF)
    esec = {s['name']: s for s in elf.sections if s['name']}
    syms = elf.symbols()
    by_name = {}
    for n, v, *_r in syms:
        by_name.setdefault(n, v)
    errors = []

    alloc = [n for n, s in esec.items() if s['flags'] & 2 and n in outs]  # SHF_ALLOC sections the map lists
    for n in alloc:
        o, s = outs[n], esec[n]
        if (o['vma'], o['size']) != (s['addr'], s['size']):
            errors.append(f'{n}: map {o["vma"]:x}+{o["size"]:x} != elf {s["addr"]:x}+{s["size"]:x}')
        o['nobits'] = s['type'] == 8
    for n in outs:
        if n not in esec and outs[n]['size']:
            errors.append(f'{n}: in the map, not in the ELF')

    # segments: splat order, ROM from <seg>_ROM_START/END (cross-checked against the map's load addresses)
    rows, prev_end, kinds = [], 0, splat_segments(YAML)
    names = sorted((v, n[:-len('_ROM_START')]) for n, v in by_name.items() if n.endswith('_ROM_START'))
    for rs, name in names:
        re_ = by_name[f'{name}_ROM_END']
        typ, excl = kinds.get(name, ('?', '-'))
        o, b = outs.get('.' + name), outs.get(f'.{name}_bss')
        if o and o['size'] and o['lma'] != rs:
            errors.append(f'{name}: ROM_START {rs:x} != map load address {o["lma"]:x}')
        if re_ > rs and rs != prev_end:  # empty placeholder segments (vpk0 *_bss) sit anywhere
            errors.append(f'{name}: ROM starts at {rs:x}, previous segment ends at {prev_end:x}')
        prev_end = max(prev_end, re_)
        vs = by_name.get(f'{name}_VRAM', o['vma'] if o else 0)
        ve = by_name.get(f'{name}_VRAM_END', 0)
        bs, be = (f'{b["vma"]:X}', f'{b["vma"] + b["size"]:X}') if b else ('-', '-')
        rows.append((name, typ, f'{rs:X}', f'{re_:X}', f'{vs:X}', f'{ve:X}', bs, be, excl))
    if prev_end != ROM_SIZE:
        errors.append(f'ROM ends at {prev_end:x}, expected {ROM_SIZE:x}')

    os.makedirs(OUT, exist_ok=True)
    head = '(port/tools/gen_symbols.py from the matching build map/elf; names and offsets only)'
    with open(os.path.join(OUT, 'segments.tsv'), 'w') as f:
        f.write(f'# segment\ttype\trom_start\trom_end\tvram_start\tvram_end\tbss_start\tbss_end\texclusive_ram_id   {head}\n')
        for r in rows:
            f.write('\t'.join(r) + '\n')

    # objects
    index = {}
    with open(os.path.join(OUT, 'objects.tsv'), 'w') as f:
        f.write(f'# out_section\tin_section\tobject\tvram\tsize\trom\tsegment   {head}\n')
        for n, o in outs.items():
            if n not in alloc:
                continue
            ins = sorted((i for i in o['inputs'] if i[1]), key=lambda i: i[0])
            index[n] = ([i[0] for i in ins], ins)
            for vma, size, sec, obj in ins:
                rom = '-' if o['nobits'] else f'{o["lma"] + vma - o["vma"]:X}'
                f.write(f'{n}\t{sec}\t{obj}\t{vma:X}\t{size:X}\t{rom}\t{seg_of(n)}\n')

    # symbols
    counts = {}
    with open(os.path.join(OUT, 'symbols.tsv'), 'w') as f:
        f.write(f'# name\tvram\trom\tsize\ttype\tbind\tsection\tsegment\tobject   {head}\n')
        for name, value, size, typ, bind, sec in syms:
            rom, seg, obj = '-', '-', '-'
            if sec in outs and sec in alloc:
                o = outs[sec]
                seg = seg_of(sec)
                if not o['nobits']:
                    rom = f'{o["lma"] + value - o["vma"]:X}'
                starts, ins = index[sec]
                k = bisect.bisect_right(starts, value) - 1
                if k >= 0 and ins[k][0] <= value < ins[k][0] + ins[k][1]:
                    obj = ins[k][3]
                elif k >= 0 and value == ins[k][0] + ins[k][1]:
                    obj = ins[k][3]  # end labels
            elif sec not in ('abs', 'undef', 'common'):
                seg = '?'
            counts[typ] = counts.get(typ, 0) + 1
            f.write(f'{name}\t{value:X}\t{rom}\t{size:X}\t{typ}\t{bind}\t{sec}\t{seg}\t{obj}\n')

    print(f'{len(rows)} segments, {sum(len(v[1]) for v in index.values())} input sections, {len(syms)} symbols '
          f'({", ".join(f"{k} {v}" for k, v in sorted(counts.items()))}) -> {os.path.relpath(OUT, ROOT)}/',
          file=sys.stderr)
    for e in errors:
        print('ERROR', e, file=sys.stderr)
    sys.exit(1 if errors else 0)


if __name__ == '__main__':
    main()
