#!/usr/bin/env python3
"""Decode and check a Pokemon Snap FlashRAM image (session S8, docs/port/SAVE.md).

    python3 port/save/snap_save.py IMAGE.fla [--json]
    python3 port/save/snap_save.py --write OUT.fla (--base IN.fla | --new NAME) [--courses N] [--items LIST]
                                   [--flag NAME=0|1 ...]        (session S10a: craft a progress save)

IMAGE is the 131072-byte FlashRAM in console byte order (big-endian words, as on the cartridge; the port's
OUT/flash.fla). The layout is the game's save struct (src/more_funcs/more_funcs.h UnkBigBoy, 0x1F2A4 bytes at flash
offset 0) and its checks (src/more_funcs/5BF20.c func_800BF178_5C018: version string + MD4 over `data`, the game's own
MD4 variant in src/more_funcs/md4.c, reimplemented here). Exit code 0 when the game would accept the save (it then
boots with it instead of a new file), 1 otherwise. Reads only the given file; prints names, numbers, offsets.

--write sets progress fields the way the game's own setters store them and recomputes the MD4 as func_800BF244_5C0E4
does before writing (func_800BF10C_5BFAC: MD4_Finalize over `data`), then checks the result with the decoder above:
  --base IN    start from a save the game wrote (keeps name, photos, scores); or
  --new NAME   start from the game's new file (func_800C042C_5D2CC: all zero, every photo slot unk_04 = -1, unk_64_05,
               unk_64_22 = 0x4C, unk_64_16 = 0x24, the self offsets, version string) with NAME (ASCII -> full-width
               EUC-JP, as the name entry writes it) and PFID_16 (unk_68_31, set by menu_new_game A5CC50.c after the
               name entry). A save the game wrote (--base) is the better start: it holds what a real first trip sets
               (e.g. PFID_7 = unk_64_08: the lab menu with the PKMN Album, oaks_lab 8A8A50.c)
  --courses N  courses open beyond the Beach, 0..6 (unk_64_29, func_800BFC70_5CB10: the lab's course list
               D_80195CEC_95B50C[N]; course order Beach 0, Tunnel 1, Volcano 2, River 3, Cave 4, Valley 5, Rainbow 6,
               include/enums.h). unk_50 (func_800C02A0_5D140, the course Oak announces) is set to the same N so the lab
               shows no "new course" sequence.
  --items LIST comma list of apple, pester, flute, dash (hasApple, hasPesterBall, hasFlute, hasDashEngine)
  --flag NAME=V  any single-bit flag of word 0x64 / 0x68 by the names printed by the decoder
Crafted images are personal data: write them under port_work/ only.
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


FLAG_BITS = {  # word 0x64 bit -> name, as the decoder prints them (console bit numbering, MSB-first bitfields)
    15: 'hasApple', 14: 'hasPesterBall', 13: 'hasFlute', 12: 'unk_64_12', 11: 'unk_64_11', 10: 'hasDashEngine',
    9: 'unk_64_09', 8: 'unk_64_08', 7: 'hasFinishedTutorial', 6: 'unk_64_06', 5: 'unk_64_05', 4: 'unk_64_04',
    3: 'unk_64_03', 2: 'zoomSwitch', 1: 'invertedY', 0: 'unk_64_00'}
ITEMS = {'apple': 'hasApple', 'pester': 'hasPesterBall', 'flute': 'hasFlute', 'dash': 'hasDashEngine'}


def new_image(name):
    """func_800C042C_5D2CC (new file) + a name + PFID_16; the rest of the flash stays erased"""
    img = bytearray(b'\xFF' * SIZE)
    img[:STRUCT_SIZE] = bytes(STRUCT_SIZE)
    for i in range(69 + 4):                       # unk_180[69] and unk_FBA0[4] are contiguous
        struct.pack_into('>i', img, 0x180 + i * PHOTO_SIZE + 4, -1)
    for i in range(60):
        struct.pack_into('>i', img, 0x10A20 + i * ALBUM_SIZE + 4, -1)
    struct.pack_into('>4i', img, 0x20, 0x6C, 0x180, 0xFBA0, 0x10A20)
    w64 = (0x4C << 22) | (0x24 << 16) | (1 << 5)   # unk_64_22, unk_64_16, unk_64_05
    struct.pack_into('>I', img, 0x64, w64)
    struct.pack_into('>I', img, 0x68, 1 << 31)      # unk_68_31 = PFID_16 (name entered)
    img[0x10:0x20] = VERSION
    full = b''.join(bytes([0xA3, 0xC1 + ord(c) - ord('A')]) if 'A' <= c <= 'Z' else
                    bytes([0xA3, 0xE1 + ord(c) - ord('a')]) if 'a' <= c <= 'z' else
                    bytes([0xA3, 0xB0 + ord(c) - ord('0')]) if '0' <= c <= '9' else b'\xA1\xA1' for c in name[:7])
    img[0x54:0x64] = full.ljust(16, b'\0')
    return img


def set_flag(img, name, val):
    for b, n in FLAG_BITS.items():
        if n.split(' ')[0] == name:
            w = struct.unpack_from('>I', img, 0x64)[0]
            struct.pack_into('>I', img, 0x64, (w | 1 << b) if val else (w & ~(1 << b)))
            return
    if name.startswith('unk_68_') and 27 <= int(name[7:]) <= 31:
        b = int(name[7:])
        w = struct.unpack_from('>I', img, 0x68)[0]
        struct.pack_into('>I', img, 0x68, (w | 1 << b) if val else (w & ~(1 << b)))
        return
    raise SystemExit(f'unknown flag {name}')


def write_main(argv):
    opt = lambda n: argv[argv.index(n) + 1] if n in argv else None
    out = opt('--write')
    if opt('--base'):
        img = bytearray(open(opt('--base'), 'rb').read())
        if len(img) != SIZE or not decode(img)['md4_ok']:
            raise SystemExit('base image is not a save the game accepts')
    elif opt('--new'):
        img = new_image(opt('--new'))
    else:
        raise SystemExit('--write needs --base IN or --new NAME')
    if opt('--courses') is not None:
        n = int(opt('--courses'))
        if not 0 <= n <= 6:
            raise SystemExit('--courses is 0..6')
        w = struct.unpack_from('>I', img, 0x64)[0]
        struct.pack_into('>I', img, 0x64, (w & 0x1FFFFFFF) | (n << 29))   # unk_64_29:3, the top bits
        struct.pack_into('>i', img, 0x50, n)                                # unk_50
    for it in filter(None, (opt('--items') or '').split(',')):
        set_flag(img, ITEMS[it], 1)
    for i, a in enumerate(argv):
        if a == '--flag':
            k, v = argv[i + 1].split('=')
            set_flag(img, k, int(v))
    data = bytes(img[DATA_OFF:DATA_OFF + DATA_LEN])
    struct.pack_into('>4I', img, 0, *snap_md4(data))
    d = decode(img)
    assert d['version_ok'] and d['md4_ok']
    open(out, 'wb').write(img)
    print(f'wrote {out}: courses {d["unk_64_29"]}, unk_50 {d["unk_50 (courses open?)"]}, flags {d["flags_set"]}, '
          f'name {d["player_name"]}, MD4 {d["md4_computed"]}')
    return 0


def main():
    if '--write' in sys.argv:
        return write_main(sys.argv[1:])
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
