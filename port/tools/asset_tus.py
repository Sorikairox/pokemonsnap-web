#!/usr/bin/env python3
"""List the translation units that #include ROM-extracted assets (the port must compile their code only).

Writes port/symbols/asset_tus.tsv: tu, segment, asset_includes, kinds (gfx/vtx/... counts), code (yes = the TU also
defines functions, so the port compiles it without the asset data; no = data only, the port does not compile it and
its symbols become labels in the RDRAM mirror). Paths and counts only, no ROM bytes. The segment column comes from
port/symbols/objects.tsv (gen_symbols.py) when it exists.
"""
import collections
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
OUT = os.path.join(ROOT, 'port', 'symbols', 'asset_tus.tsv')
INC = re.compile(r'^\s*#\s*include\s+"(assets/[^"]+)"', re.M)
# a top-level function definition: "type name(args) {" (or "{" on the next line), not a prototype or initialiser
FUNC = re.compile(r'^[A-Za-z_][\w \t\*]*?\b(\w+)\s*\(([^;{}=]*)\)\s*\{', re.M)


def kind(path):
    base = os.path.basename(path)
    m = re.search(r'\.([a-z0-9]+)\.inc\.c$', base)
    return m.group(1) if m else base.rsplit('.', 1)[-1]


def strip_comments(s):
    s = re.sub(r'/\*.*?\*/', ' ', s, flags=re.S)
    return re.sub(r'//[^\n]*', '', s)


def segments():
    p = os.path.join(ROOT, 'port', 'symbols', 'objects.tsv')
    seg = {}
    if os.path.exists(p):
        for line in open(p):
            if line.startswith('#'):
                continue
            f = line.rstrip('\n').split('\t')
            obj, s = f[2], f[6]
            m = re.match(r'build/(src/.+)\.o$', obj)
            if m:
                seg.setdefault(m.group(1), s)
    return seg


def main():
    seg = segments()
    rows = []
    for dp, dn, fn in os.walk(os.path.join(ROOT, 'src')):
        dn.sort()
        for f in sorted(fn):
            if not f.endswith('.c'):
                continue
            p = os.path.join(dp, f)
            text = open(p, encoding='utf-8', errors='replace').read()
            incs = INC.findall(text)
            if not incs:
                continue
            rel = os.path.relpath(p, ROOT)
            body = strip_comments(INC.sub('', text))
            funcs = [m.group(1) for m in FUNC.finditer(body) if m.group(1) not in ('if', 'for', 'while', 'switch')]
            kinds = collections.Counter(kind(i) for i in incs)
            rows.append((rel, seg.get(rel, '-'), len(incs),
                         ','.join(f'{k}:{n}' for k, n in sorted(kinds.items())), 'yes' if funcs else 'no', len(funcs)))
    rows.sort()
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, 'w') as o:
        o.write('# tu\tsegment\tasset_includes\tkinds\tcode\tfunctions   (port/tools/asset_tus.py; paths only)\n')
        for r in rows:
            o.write('\t'.join(map(str, r)) + '\n')
    n_code = sum(1 for r in rows if r[4] == 'yes')
    print(f'{OUT}: {len(rows)} TUs include assets ({sum(r[2] for r in rows)} includes); '
          f'{n_code} also define functions, {len(rows) - n_code} are data only', file=sys.stderr)


if __name__ == '__main__':
    main()
