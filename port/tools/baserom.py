#!/usr/bin/env python3
"""Write the big-endian (.z64) baserom the upstream build expects, from the player's ROM in any byte order.

usage: baserom.py SRC DST SHA1   (SRC is only read; DST must be a gitignored path, e.g. ./pokemonsnap.z64)
"""
import hashlib
import sys


def to_z64(b: bytes) -> bytes:
    magic = b[:4]
    if magic == b'\x80\x37\x12\x40':
        return b
    if magic == b'\x37\x80\x40\x12':  # .v64/.n64 byte-swapped halfwords
        out = bytearray(len(b))
        out[0::2] = b[1::2]
        out[1::2] = b[0::2]
        return bytes(out)
    if magic == b'\x40\x12\x37\x80':  # little-endian words
        out = bytearray(len(b))
        for i in range(4):
            out[i::4] = b[3 - i::4]
        return bytes(out)
    sys.exit(f'unknown ROM header {magic.hex()}')


def main():
    src, dst, want = sys.argv[1:4]
    z = to_z64(open(src, 'rb').read())
    got = hashlib.sha1(z).hexdigest()
    if got != want:
        sys.exit(f'SHA-1 {got} != {want}: not the US ROM')
    open(dst, 'wb').write(z)
    print(f'{dst}: {len(z)} bytes, SHA-1 {got} OK')


if __name__ == '__main__':
    main()
