#!/usr/bin/env python3
"""Template + fill for the retrowser build (session S9, docs/port/RTW.md "ROM-freedom").

The decomp's own C data (sine / tangent tables, text, Pokemon paths, glyph maps...) is the ROM's data written as C,
so the linked wasm holds runs of ROM bytes (vbe = .n64 word order for words, plain order for byte strings, .v64 for
some halfword tables). The retrowser build ships a **template** instead: every such run in the game objects' data
(port/build/vobj/*, plus the linker's merged string pool) is zeroed, and `pokemonsnap.romfill` lists where it goes
and where it comes from in the player's ROM (offsets only). rtw.c applies it from the ROM before main(), and checks
an FNV-1a hash of the filled bytes against the one recorded here.

  python3 -I port/rtw/romfill.py ROM.z64 LINK_DIR OUT_DIR   (reads LINK_DIR/pokemonsnap.wasm + .map; writes
                                                           OUT_DIR/pokemonsnap.wasm (the template) and
                                                           OUT_DIR/pokemonsnap.romfill, LINK_DIR/romfill_report.tsv)

Search: the ROM in .z64, .n64 and .v64 order, every 16-byte-aligned 32-byte window with >= 8 distinct byte values
indexed; each data section of a game object is slid over byte by byte; a window hit is extended both ways while
the bytes stay equal (inside the section). Runs >= MIN (16) bytes with a non-zero byte are filled. The tool applies
its own table to the template and refuses to write unless the result is the original wasm byte for byte.

Fill table (little-endian u32 words): magic 'SNRF', version 1, count, fnv1a(filled bytes in table order), then per
run: memory address, length, ROM offset (in that order's image), order (0 z64, 1 n64 = p^3, 2 v64 = p^1)."""
import os, re, struct, sys

W, STRIDE, MIN = 32, 16, 16


def leb(b, i, signed=False):
    r = s = 0
    while True:
        x = b[i]; i += 1
        r |= (x & 0x7F) << s; s += 7
        if x < 0x80:
            if signed and x & 0x40:
                r -= 1 << s
            return r, i


def data_segments(w):
    """(memory address, file offset, length) of every active data segment"""
    i, out = 8, []
    while i < len(w):
        sid = w[i]; sz, j = leb(w, i + 1)
        if sid == 11:
            n, k = leb(w, j)
            for _ in range(n):
                flag, k = leb(w, k)
                if flag != 0:
                    sys.exit(f'romfill: data segment flag {flag} not handled')
                if w[k] != 0x41:
                    sys.exit('romfill: data segment offset is not i32.const')
                addr, k = leb(w, k + 1, signed=True)
                if w[k] != 0x0B:
                    sys.exit('romfill: data segment offset expression too long')
                ln, k = leb(w, k + 1)
                out.append((addr & 0xFFFFFFFF, k, ln))
                k += ln
        i = j + sz
    return out


def map_sections(path):
    """input sections of the data output sections (not .bss): [addr, size, object:(section), symbols]. wasm-ld map
    columns: Addr Off Size, then the name indented 1 (output section), 9 (input section) or 17 (symbol) spaces. The
    Off column is stale after wasm-opt (asyncify rewrites the code): addresses are mapped through the data segments."""
    secs, out_sec = [], None
    for l in open(path):
        m = re.match(r'^\s*(\S+)\s+(\S+)\s+(\S+)( +)(\S.*)$', l.rstrip('\n'))
        if not m or m.group(1) == 'Addr':
            continue
        depth, name = len(m.group(4)), m.group(5)
        if depth == 1:
            out_sec = name
            continue
        if m.group(1) == '-' or out_sec in (None, '.bss', '.tbss', 'CODE'):
            continue
        addr, size = int(m.group(1), 16), int(m.group(3), 16)
        if depth == 9 and ':(' in name:
            secs.append([addr, size, name, []])
        elif depth > 9 and secs:
            secs[-1][3].append((addr, size, name))
    return secs


def fnv(data, h=2166136261):
    for x in data:
        h = ((h ^ x) * 16777619) & 0xFFFFFFFF
    return h


def main():
    rom_path = sys.argv[1]
    z = open(rom_path, 'rb').read()
    if z[:4] != b'\x80\x37\x12\x40':
        sys.exit('romfill: give the .z64 (big-endian) ROM')
    n = bytearray(len(z)); n[0::4], n[1::4], n[2::4], n[3::4] = z[3::4], z[2::4], z[1::4], z[0::4]
    v = bytearray(len(z)); v[0::2], v[1::2] = z[1::2], z[0::2]
    imgs = (z, bytes(n), bytes(v))
    idx = {}
    for o_i, img in enumerate(imgs):
        for o in range(0, len(img) - W + 1, STRIDE):
            win = img[o:o + W]
            if len(set(win)) >= 8:
                idx.setdefault(win, (o_i, o))
    src, dst = sys.argv[2], sys.argv[3]
    w = open(f'{src}/pokemonsnap.wasm', 'rb').read()
    segs = data_segments(w)

    def file_off(addr, size):
        for a, f, ln in segs:
            if a <= addr and addr + size <= a + ln:
                return f + addr - a
        return None
    secs = map_sections(f'{src}/pokemonsnap.map')
    # one search region per object and run of adjacent input sections (string literals are small sections of
    # their own, and a run of ROM text crosses them)
    regs = []
    for addr, size, name, syms in sorted(secs, key=lambda x: x[0]):
        obj = name.split(':(')[0]
        if not (obj.startswith('port/build/vobj/') or obj == '<internal>'):
            continue
        if regs and regs[-1][2] == obj and addr <= regs[-1][0] + regs[-1][1] + 8 and file_off(regs[-1][0], addr + size - regs[-1][0]) is not None:
            r = regs[-1]
            r[1] = max(r[1], addr + size - r[0]); r[3] += syms
        else:
            regs.append([addr, size, obj, list(syms)])
    runs, rep = [], []
    for addr, size, obj, syms in regs:
        fo = file_off(addr, size)
        if fo is None or size < MIN:
            continue
        b = w[fo:fo + size]
        p = 0
        while p + W <= size:
            h = idx.get(b[p:p + W])
            if not h:
                p += 1
                continue
            oi, ro = h
            img = imgs[oi]
            s0, r0 = p, ro
            while s0 > 0 and r0 > 0 and b[s0 - 1] == img[r0 - 1]:
                s0 -= 1; r0 -= 1
            e = p + W
            while e < size and ro + (e - p) < len(img) and b[e] == img[ro + (e - p)]:
                e += 1
            while s0 < e and b[s0] == 0:   # no zero edges (nothing to fill there)
                s0 += 1; r0 += 1
            while e > s0 and b[e - 1] == 0:
                e -= 1
            if e - s0 >= MIN:
                if runs and runs[-1][0] + runs[-1][1] > addr + s0:   # overlap with the previous run: cut
                    cut = runs[-1][0] + runs[-1][1] - (addr + s0)
                    s0 += cut; r0 += cut
                if e - s0 >= MIN:
                    runs.append((addr + s0, e - s0, r0, oi))
                    sym = next((sn for sa, ss, sn in syms if sa <= addr + s0 < sa + max(ss, 1)), '-')
                    rep.append(f'{addr + s0:08x}\t{e - s0}\t{("z64", "n64", "v64")[oi]}\t{r0:06x}\t{obj}\t{sym}')
            p = max(e, p + 1)
    # template + table
    t = bytearray(w)
    filled = bytearray()
    for a, ln, ro, oi in runs:
        fo = file_off(a, ln)
        filled += w[fo:fo + ln]
        t[fo:fo + ln] = bytes(ln)
    h = fnv(filled)
    tab = struct.pack('<4sIII', b'SNRF', 1, len(runs), h) + b''.join(struct.pack('<IIII', *r) for r in runs)
    # self-check: the table applied to the template from the ROM gives the original
    chk = bytearray(t)
    for a, ln, ro, oi in runs:
        fo = file_off(a, ln)
        chk[fo:fo + ln] = imgs[oi][ro:ro + ln]
    if bytes(chk) != w:
        sys.exit('romfill: the fill does not restore the original wasm: nothing written')
    os.makedirs(dst, exist_ok=True)
    open(f'{dst}/pokemonsnap.wasm', 'wb').write(bytes(t))
    open(f'{dst}/pokemonsnap.romfill', 'wb').write(tab)
    open(f'{src}/romfill_report.tsv', 'w').write('# address\tlength\torder\trom offset\tobject\tsymbol   (port/rtw/romfill.py; '
                                                'names and offsets only)\n' + '\n'.join(rep) + '\n')
    by = {}
    for r in rep:
        o = r.split('\t')[4]
        by[o] = by.get(o, 0) + int(r.split('\t')[1])
    print(f'romfill: {len(runs)} runs, {len(filled)} bytes zeroed in the template ({sum(1 for x in filled if x)} non-zero); '
          f'fnv {h:08x}; table {len(tab)} bytes; top objects: ' +
          ', '.join(f'{o.split("/")[-1]} {nb}' for o, nb in sorted(by.items(), key=lambda x: -x[1])[:6]))


if __name__ == '__main__':
    main()
