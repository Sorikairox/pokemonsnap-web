#!/usr/bin/env python3
"""ROM-freedom scan of the retrowser build, finer than rtw.mjs check --rom (which reports whole 4 KiB blocks):
every 32-byte-aligned 32-byte window of the ROM (z64 and n64 = the vbe layout of game C data, and v64), at least
12 distinct byte values, is looked up at every offset of each build file; hits are merged into runs and named by the
matching build's symbol table (port/symbols/symbols.tsv, ROM offsets). Prints runs >= MIN bytes. Local only: reads
the player's ROM, writes nothing.
  python3 -I port/rtw/romscan.py ROM.z64 FILE... [--min 64]"""
import sys, bisect

args = sys.argv[1:]
mn = 64
if '--min' in args:
    i = args.index('--min'); mn = int(args[i + 1]); del args[i:i + 2]
rom_path, files = args[0], args[1:]
z = open(rom_path, 'rb').read()
if z[:4] != b'\x80\x37\x12\x40':
    sys.exit('give the .z64 (big-endian) ROM')
n = bytearray(len(z)); n[0::4], n[1::4], n[2::4], n[3::4] = z[3::4], z[2::4], z[1::4], z[0::4]
v = bytearray(len(z)); v[0::2], v[1::2] = z[1::2], z[0::2]
W = 32
idx = {}
for name, img in (('z64', z), ('n64', bytes(n)), ('v64', bytes(v))):
    for o in range(0, len(img) - W + 1, W):
        w = img[o:o + W]
        if len(set(w)) >= 12:
            idx.setdefault(w, (name, o))
syms = []
try:
    for l in open('port/symbols/symbols.tsv'):
        if l.startswith('#'): continue
        r = l.rstrip('\n').split('\t')
        try: syms.append((int(r[2], 16), int(r[3], 16), r[0], r[8]))
        except ValueError: pass
except OSError:
    pass
syms.sort()
starts = [s[0] for s in syms]
def name_of(rom):
    i = bisect.bisect_right(starts, rom) - 1
    best = None
    while i >= 0 and rom - starts[i] < 0x10000:
        a, sz, nm, obj = syms[i]
        if a <= rom < a + max(sz, 1) and not nm.startswith('.') and not nm.endswith('_START'):
            best = f'{nm} ({obj.replace("build/", "")})'; break
        i -= 1
    return best or '?'
total = 0
for f in files:
    b = open(f, 'rb').read()
    hits = []
    for o in range(len(b) - W + 1):
        h = idx.get(b[o:o + W])
        if h: hits.append((o, h))
    runs = []
    for o, (nm, ro) in hits:
        if runs and runs[-1][3] == nm and o - runs[-1][1] == ro - runs[-1][2] and o <= runs[-1][1] + runs[-1][4]:
            runs[-1][4] = o + W - runs[-1][1]
        else:
            runs.append([f, o, ro, nm, W])
    big = [r for r in runs if r[4] >= mn]
    total += sum(r[4] for r in big)
    print(f'{f}: {len(b)} bytes, {len(hits)} window hits, {len(big)} runs >= {mn} bytes ({sum(r[4] for r in big)} bytes)')
    for _, o, ro, nm, ln in big:
        print(f'  file 0x{o:07x}  rom 0x{ro:06x} ({nm})  {ln:6d} bytes  {name_of(ro)}')
print(f'total {total} bytes in runs >= {mn}')
