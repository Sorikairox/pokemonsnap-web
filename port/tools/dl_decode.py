#!/usr/bin/env python3
"""Decode the renderer's command dump (session S3c): the "[gfx] <addr> <w0> <w1>" lines that port_gfx_set_log_ops(1)
writes (port/web/run.mjs --logops F) -> one readable F3DEX2 / RDP line per command.
   python3 port/tools/dl_decode.py port_work/DIR/log.txt [> out.txt]
Names and field layouts from ultralib/include/PR/gbi.h (F3DEX_GBI_2). Reads only the log (no ROM bytes)."""
import re, sys

OPS = {0x00: 'NOOP', 0x01: 'VTX', 0x02: 'MODIFYVTX', 0x03: 'CULLDL', 0x04: 'BRANCH_Z', 0x05: 'TRI1', 0x06: 'TRI2',
       0x07: 'QUAD', 0x08: 'LINE3D', 0xD5: 'SPECIAL_1', 0xD7: 'TEXTURE', 0xD8: 'POPMTX', 0xD9: 'GEOMETRYMODE',
       0xDA: 'MTX', 0xDB: 'MOVEWORD', 0xDC: 'MOVEMEM', 0xDD: 'LOAD_UCODE', 0xDE: 'DL', 0xDF: 'ENDDL', 0xE0: 'SPNOOP',
       0xE1: 'RDPHALF_1', 0xE2: 'SETOTHERMODE_L', 0xE3: 'SETOTHERMODE_H', 0xE4: 'TEXRECT', 0xE5: 'TEXRECTFLIP',
       0xE6: 'LOADSYNC', 0xE7: 'PIPESYNC', 0xE8: 'TILESYNC', 0xE9: 'FULLSYNC', 0xEA: 'SETKEYGB', 0xEB: 'SETKEYR',
       0xEC: 'SETCONVERT', 0xED: 'SETSCISSOR', 0xEE: 'SETPRIMDEPTH', 0xEF: 'RDPSETOTHERMODE', 0xF0: 'LOADTLUT',
       0xF1: 'RDPHALF_2', 0xF2: 'SETTILESIZE', 0xF3: 'LOADBLOCK', 0xF4: 'LOADTILE', 0xF5: 'SETTILE', 0xF6: 'FILLRECT',
       0xF7: 'SETFILLCOLOR', 0xF8: 'SETFOGCOLOR', 0xF9: 'SETBLENDCOLOR', 0xFA: 'SETPRIMCOLOR', 0xFB: 'SETENVCOLOR',
       0xFC: 'SETCOMBINE', 0xFD: 'SETTIMG', 0xFE: 'SETZIMG', 0xFF: 'SETCIMG'}
CC_A = ['COMBINED', 'TEXEL0', 'TEXEL1', 'PRIMITIVE', 'SHADE', 'ENVIRONMENT', '1', 'NOISE'] + ['0'] * 8
CC_B = ['COMBINED', 'TEXEL0', 'TEXEL1', 'PRIMITIVE', 'SHADE', 'ENVIRONMENT', 'CENTER', 'K4'] + ['0'] * 8
CC_C = ['COMBINED', 'TEXEL0', 'TEXEL1', 'PRIMITIVE', 'SHADE', 'ENVIRONMENT', 'SCALE', 'COMBINED_ALPHA', 'TEXEL0_ALPHA',
        'TEXEL1_ALPHA', 'PRIMITIVE_ALPHA', 'SHADE_ALPHA', 'ENV_ALPHA', 'LOD_FRACTION', 'PRIM_LOD_FRAC', 'K5'] + ['0'] * 16
CC_D = ['COMBINED', 'TEXEL0', 'TEXEL1', 'PRIMITIVE', 'SHADE', 'ENVIRONMENT', '1', '0']
AC_ABD = ['COMBINED', 'TEXEL0', 'TEXEL1', 'PRIMITIVE', 'SHADE', 'ENVIRONMENT', '1', '0']
AC_C = ['LOD_FRACTION', 'TEXEL0', 'TEXEL1', 'PRIMITIVE', 'SHADE', 'ENVIRONMENT', 'PRIM_LOD_FRAC', '0']
BL_P = ['IN', 'MEM', 'BLEND', 'FOG']
BL_A = ['A_IN', 'A_FOG', 'A_SHADE', '0']
BL_B = ['1MA', 'A_MEM', '1', '0']
FMT = ['RGBA', 'YUV', 'CI', 'IA', 'I', '?5', '?6', '?7']
SIZ = ['4b', '8b', '16b', '32b']
GEOM = [(0x1, 'ZBUFFER'), (0x4, 'SHADE'), (0x200, 'CULL_FRONT'), (0x400, 'CULL_BACK'), (0x10000, 'FOG'),
        (0x20000, 'LIGHTING'), (0x40000, 'TEXGEN'), (0x80000, 'TEXGEN_LIN'), (0x100000, 'LOD'),
        (0x200000, 'SMOOTH'), (0x800000, 'CLIPPING')]


def combine(w0, w1):
    c = [((w0 >> 20) & 15, (w1 >> 28) & 15, (w0 >> 15) & 31, (w1 >> 15) & 7, (w0 >> 12) & 7, (w1 >> 12) & 7, (w0 >> 9) & 7, (w1 >> 9) & 7),
         ((w0 >> 5) & 15, (w1 >> 24) & 15, w0 & 31, (w1 >> 6) & 7, (w1 >> 21) & 7, (w1 >> 3) & 7, (w1 >> 18) & 7, w1 & 7)]
    out = []
    for a, b, cc, d, aa, ab, ac, ad in c:
        out.append(f'({CC_A[a]}-{CC_B[b]})*{CC_C[cc]}+{CC_D[d]} | ({AC_ABD[aa]}-{AC_ABD[ab]})*{AC_C[ac]}+{AC_ABD[ad]}')
    return ' ;; '.join(out)


def othermode_l(v):
    s = []
    for c in range(2):
        s.append(f'{BL_P[(v >> (30 - 2 * c)) & 3]}*{BL_A[(v >> (26 - 2 * c)) & 3]}+{BL_P[(v >> (22 - 2 * c)) & 3]}*{BL_B[(v >> (18 - 2 * c)) & 3]}')
    flags = [n for b, n in [(0x8, 'AA_EN'), (0x10, 'Z_CMP'), (0x20, 'Z_UPD'), (0x40, 'IM_RD'), (0x80, 'CLR_ON_CVG'),
                            (0x1000, 'CVG_X_ALPHA'), (0x2000, 'ALPHA_CVG_SEL'), (0x4000, 'FORCE_BL')] if v & b]
    flags.append(f'cvgdst={(v >> 8) & 3} zmode={(v >> 10) & 3} acmp={v & 3} zsrc={"PRIM" if v & 4 else "PIXEL"}')
    return ' / '.join(s) + ' ' + ' '.join(flags)


def decode(w0, w1):
    op = w0 >> 24
    n = OPS.get(op, f'?{op:02X}')
    if op == 0x01:
        return f'{n} n={(w0 >> 12) & 0xFF} v0={((w0 >> 1) & 0x7F) - ((w0 >> 12) & 0xFF)} @{w1:08X}'
    if op in (0x05, 0x06, 0x07):
        t = [((w0 >> 16) & 0xFF) // 2, ((w0 >> 8) & 0xFF) // 2, (w0 & 0xFF) // 2]
        if op != 0x05:
            t += [((w1 >> 16) & 0xFF) // 2, ((w1 >> 8) & 0xFF) // 2, (w1 & 0xFF) // 2]
        return f'{n} {t}'
    if op == 0xD9:
        clr = ~(w0 | 0xFF000000) & 0xFFFFFF
        return f'{n} clear[{" ".join(nm for b, nm in GEOM if clr & b)}] set[{" ".join(nm for b, nm in GEOM if w1 & b)}]'
    if op == 0xDA:
        p = (w0 & 0xFF) ^ 1
        return f'{n} {"PROJ" if p & 4 else "MV"} {"LOAD" if p & 2 else "MUL"} {"PUSH" if p & 1 else ""} @{w1:08X}'
    if op == 0xDB:
        return f'{n} idx={(w0 >> 16) & 0xFF:02X} off={w0 & 0xFFFF:04X} val={w1:08X}'
    if op == 0xDC:
        return f'{n} idx={w0 & 0xFF} ofs={((w0 >> 8) & 0xFF) * 8} len={((w0 >> 19) & 0x1F) * 8 + 8} @{w1:08X}'
    if op == 0xD7:
        return f'{n} on={(w0 >> 1) & 0x7F} tile={(w0 >> 8) & 7} lvl={(w0 >> 11) & 7} s={w1 >> 16:04X} t={w1 & 0xFFFF:04X}'
    if op in (0xE2, 0xE3):
        ln = (w0 & 0xFF) + 1
        sft = 32 - ((w0 >> 8) & 0xFF) - ln
        return f'{n} shift={sft} len={ln} val={w1:08X}' + (f' -> {othermode_l(w1)}' if op == 0xE2 and sft <= 3 and ln >= 29 else '')
    if op == 0xEF:
        return f'{n} H={w0 & 0xFFFFFF:06X} cyc={(w0 >> 20) & 3} L={othermode_l(w1)}'
    if op == 0xFC:
        return f'{n} {combine(w0 & 0xFFFFFF, w1)}'
    if op in (0xFD, 0xFF):
        return f'{n} {FMT[(w0 >> 21) & 7]} {SIZ[(w0 >> 19) & 3]} w={(w0 & 0xFFF) + 1} @{w1:08X}'
    if op == 0xF5:
        return (f'{n} tile={(w1 >> 24) & 7} {FMT[(w0 >> 21) & 7]} {SIZ[(w0 >> 19) & 3]} line={(w0 >> 9) & 0x1FF} '
                f'tmem={w0 & 0x1FF} pal={(w1 >> 20) & 15} cmt={(w1 >> 18) & 3} maskt={(w1 >> 14) & 15} '
                f'shiftt={(w1 >> 10) & 15} cms={(w1 >> 8) & 3} masks={(w1 >> 4) & 15} shifts={w1 & 15}')
    if op in (0xF2, 0xF3, 0xF4, 0xF0, 0xED, 0xF6, 0xE4, 0xE5):
        a = ((w0 >> 12) & 0xFFF) / 4, (w0 & 0xFFF) / 4
        b = ((w1 >> 12) & 0xFFF) / 4, (w1 & 0xFFF) / 4
        extra = f' tile={(w1 >> 24) & 7}' if op not in (0xED, 0xF6) else ''
        if op == 0xF3:
            return f'{n} tile={(w1 >> 24) & 7} uls={(w0 >> 12) & 0xFFF} ult={w0 & 0xFFF} lrs={(w1 >> 12) & 0xFFF} dxt={w1 & 0xFFF}'
        return f'{n} {a} {b}{extra}'
    if op in (0xF7, 0xF8, 0xF9, 0xFA, 0xFB):
        return f'{n} {w1:08X}' + (f' lod={w0 & 0xFF}' if op == 0xFA else '')
    return f'{n} {w0:08X} {w1:08X}'


def main():
    for line in open(sys.argv[1], errors='replace'):
        m = re.match(r'\[gfx\] (0x[0-9a-f]+) ([0-9A-F]{8}) ([0-9A-F]{8})', line)
        if not m:
            if line.startswith('[gfx]') or line.startswith('[task]'):
                print(line.rstrip())
            continue
        w0, w1 = int(m[2], 16), int(m[3], 16)
        print(f'{m[1]}  {w0:08X} {w1:08X}  {decode(w0, w1)}')


if __name__ == '__main__':
    main()
