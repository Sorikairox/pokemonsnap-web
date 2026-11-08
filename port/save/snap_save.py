#!/usr/bin/env python3
"""Decode and check a Pokemon Snap FlashRAM image (session S8, docs/port/SAVE.md).

    python3 port/save/snap_save.py IMAGE.fla [--json]

IMAGE is the 131072-byte FlashRAM in console byte order (big-endian words, as on the cartridge; the port's
OUT/flash.fla). The layout is the game's save struct (src/more_funcs/more_funcs.h UnkBigBoy, 0x1F2A4 bytes at flash
offset 0) and its checks (src/more_funcs/5BF20.c func_800BF178_5C018: version string + MD4 over `data`, the game's own
MD4 variant in src/more_funcs/md4.c, reimplemented here). Exit code 0 when the game would accept the save (it then
boots with it instead of a new file), 1 otherwise. Reads only the given file; prints names, numbers, offsets.
"""
import json
import struct
import sys

SIZE = 0x20000
STRUCT_SIZE = 0x1F2A4
DATA_OFF, DATA_LEN = 0x10, 0x1F2A4 - 0x10
PHOTO_SIZE, ALBUM_SIZE = 0x3A0, 0x3E0
VERSION = b'HAL_SNAP_V1.0-1\0'


def rol(v, s):
    v &= 0xFFFFFFFF
    return ((v << s) | (v >> (32 - s))) & 0xFFFFFFFF


def transform(st, block):
    """md4.c MD4_Transform: the 16 words are read as console (big-endian) u32"""
    x = struct.unpack('>16I', block)
    a, b, c, d = st
    F = lambda x_, y, z: (x_ & y) | (~x_ & z)
    G = lambda x_, y, z: (x_ & y) | (x_ & z) | (y & z)
    H = lambda x_, y, z: x_ ^ y ^ z
    def r(f, k_, s, const, a_, b_, c_, d_):
        return rol(a_ + f(b_, c_, d_) + x[k_] + const, s)
    for i in range(0, 16, 4):
        a = r(F, i, 3, 0, a, b, c, d); d = r(F, i + 1, 7, 0, d, a, b, c)
        c = r(F, i + 2, 11, 0, c, d, a, b); b = r(F, i + 3, 19, 0, b, c, d, a)
    for i in range(4):
        a = r(G, i, 3, 0x5A827999, a, b, c, d); d = r(G, i + 4, 5, 0x5A827999, d, a, b, c)
        c = r(G, i + 8, 9, 0x5A827999, c, d, a, b); b = r(G, i + 12, 13, 0x5A827999, b, c, d, a)
    for i in (0, 2, 1, 3):
        a = r(H, i, 3, 0x6ED9EBA1, a, b, c, d); d = r(H, i + 8, 9, 0x6ED9EBA1, d, a, b, c)
        c = r(H, i + 4, 11, 0x6ED9EBA1, c, d, a, b); b = r(H, i + 12, 15, 0x6ED9EBA1, b, c, d, a)
    return [(p + q) & 0xFFFFFFFF for p, q in zip(st, (a, b, c, d))]


def snap_md4(data):
    """md4.c MD4_Finalize(input, length): 64-byte blocks, then the game's padding of the rest (bit counter kept as
    8 little-endian bytes, appended as bytes)"""
    st = [0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476]
    counter = 0
    n = len(data)
    rem = (n // 64) * 64 if n >= 64 else 0
    for o in range(0, rem, 64):
        counter += 0x200
        st = transform(st, data[o:o + 64])
    bits = (n - rem) * 8
    counter += bits
    cnt = (counter & 0xFFFFFFFFFFFFFFFF).to_bytes(8, 'little')
    off = bits >> 3
    buf = bytearray(64)
    tail = data[rem:rem + off + 1]          # the game copies one byte past the end (masked below)
    buf[:len(tail)] = tail
    sh = 7 - (bits & 7)
    buf[off] = (buf[off] | (1 << sh)) & ~((1 << sh) - 1) & 0xFF
    if off < 0x38:
        buf[0x38:] = cnt
        st = transform(st, bytes(buf))
    else:
        st = transform(st, bytes(buf))
        buf = bytearray(0x38) + cnt
        st = transform(st, bytes(buf))
    return st


def photo_used(img, off):
    return struct.unpack_from('>i', img, off + 4)[0] != -1


def decode(img):
    s32 = lambda o: struct.unpack_from('>i', img, o)[0]
    u32 = lambda o: struct.unpack_from('>I', img, o)[0]
    d = {}
    stored = [u32(0), u32(4), u32(8), u32(12)]
    data = img[DATA_OFF:DATA_OFF + DATA_LEN]
    calc = snap_md4(data)
    d['md4_stored'] = ' '.join(f'{v:08X}' for v in stored)
    d['md4_computed'] = ' '.join(f'{v:08X}' for v in calc)
    d['version'] = img[0x10:0x20].rstrip(b'\0').decode('ascii', 'replace')
    d['version_ok'] = img[0x10:0x20] == VERSION
    d['md4_ok'] = stored == calc
    d['offsets'] = {'unk_6C': hex(s32(0x20)), 'photos (unk_180)': hex(s32(0x24)), 'unk_FBA0': hex(s32(0x28)),
                    'album': hex(s32(0x2C))}
    d['offsets_ok'] = [s32(0x20), s32(0x24), s32(0x28), s32(0x2C)] == [0x6C, 0x180, 0xFBA0, 0x10A20]
    d['saved_time_lo'] = hex(u32(0x30))
    d['hi_scores (unk_34)'] = [s32(0x34 + 4 * i) for i in range(7)]
    d['unk_50 (courses open?)'] = s32(0x50)
    raw = img[0x54:0x64].split(b'\0')[0]
    try:
        name = raw.decode('euc_jp')
    except UnicodeDecodeError:
        name = raw.hex()
    d['player_name'] = name
    w64, w68 = u32(0x64), u32(0x68)
    d['word_64'] = f'{w64:08X}'
    d['word_68'] = f'{w68:08X}'
    names64 = {15: 'hasApple', 14: 'hasPesterBall', 13: 'hasFlute', 12: 'unk_64_12', 11: 'unk_64_11',
               10: 'hasDashEngine', 9: 'unk_64_09', 8: 'unk_64_08', 7: 'hasFinishedTutorial', 6: 'unk_64_06',
               5: 'unk_64_05', 4: 'unk_64_04 (illegal copy)', 3: 'unk_64_03', 2: 'zoomSwitch', 1: 'invertedY',
               0: 'unk_64_00'}
    d['flags_set'] = [n for b, n in names64.items() if w64 >> b & 1] + \
        [f'unk_68_{b}' for b in range(27, 32) if w68 >> b & 1]
    d['unk_64_29'] = w64 >> 29
    d['unk_64_22 (default 0x4C)'] = hex(w64 >> 22 & 0x7F)
    d['unk_64_16 (default 0x24)'] = hex(w64 >> 16 & 0x3F)
    d['unk_6C (per-Pokemon, nonzero)'] = {i: s32(0x6C + 4 * i) for i in range(69) if s32(0x6C + 4 * i)}
    d['report photos (unk_180) used'] = [i for i in range(69) if photo_used(img, 0x180 + i * PHOTO_SIZE)]
    d['unk_FBA0 used'] = [i for i in range(4) if photo_used(img, 0xFBA0 + i * PHOTO_SIZE)]
    d['album photos used'] = [i for i in range(60) if photo_used(img, 0x10A20 + i * ALBUM_SIZE)]
    for i in d['report photos (unk_180) used']:
        o = 0x180 + i * PHOTO_SIZE
        w0 = u32(o)
        pk = [(u32(o + 0x20 + 0x18 * k) >> 19) for k in range(12)]
        d.setdefault('report photo details', {})[i] = {
            'levelID': (w0 >> 25) if (w0 >> 31) == 0 else (w0 >> 25) - 128, 'word0': f'{w0:08X}',
            'pokemonIDs': [p for p in pk if p and p != 0x1FFF]}
    d['tail_after_struct'] = 'erased (FF)' if set(img[STRUCT_SIZE:]) <= {0xFF} else 'mixed'
    return d


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(args) != 1:
        print(__doc__)
        return 2
    img = open(args[0], 'rb').read()
    if len(img) != SIZE:
        print(f'{args[0]}: {len(img)} bytes, expected {SIZE}')
        return 1
    if set(img) == {0xFF}:
        print(f'{args[0]}: blank (erased) flash')
        return 1
    d = decode(img)
    if '--json' in sys.argv:
        print(json.dumps(d, indent=1, ensure_ascii=False))
    else:
        for k, v in d.items():
            print(f'{k}: {v}')
    ok = d['version_ok'] and d['md4_ok']
    print('the game accepts this save' if ok else 'the game would reject this save (new file)')
    if not d['version_ok']:
        for name, sw in (('32-bit word-swapped (little-endian words)', lambda b: b[::-1]),
                         ('16-bit byte-swapped', lambda b: b[1::-1] + b[3:1:-1])):
            hdr = b''.join(sw(img[o:o + 4]) for o in range(0x10, 0x20, 4))
            if hdr == VERSION:
                print(f'note: this is a {name} image (some emulators store FlashRAM so); the console format is '
                      'big-endian bytes')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
