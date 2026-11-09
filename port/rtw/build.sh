#!/bin/bash
# The retrowser build (session S9, docs/port/RTW.md): port/build/rtw/dist/{retrowser.json, pokemonsnap.js,
# pokemonsnap.wasm (template), pokemonsnap.romfill}. Runs on the host (Docker for the build, node + python3 here).
#   port/rtw/build.sh [--no-build]
# Needs ./pokemonsnap.z64 (port/docker/run.sh rom PATH; the fill table is made from it, it holds offsets only) and
# the retrowser check tool: RTW_MJS (default ~/Code/retrowser/dist/tools/rtw.mjs, the local copy of
# https://retrowser.app/tools/rtw.mjs). Ends with `rtw.mjs check --rom` against the .z64 and, if present, the
# player's own file (RTW_ROM, default "./Pokemon Snap (USA).n64"), and port/rtw/romscan.py (runs >= 64 bytes).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
RTW_MJS=${RTW_MJS:-$HOME/Code/retrowser/dist/tools/rtw.mjs}
RTW_ROM=${RTW_ROM:-$ROOT/Pokemon Snap (USA).n64}
Z64=$ROOT/pokemonsnap.z64
[ -f "$Z64" ] || { echo "rtw: no $Z64 (port/docker/run.sh rom PATH)"; exit 1; }
[ -f "$RTW_MJS" ] || { echo "rtw: no check tool at $RTW_MJS (set RTW_MJS)"; exit 1; }
if [ "${1:-}" != "--no-build" ]; then
  export PATH=$PATH:/Applications/Docker.app/Contents/Resources/bin
  docker run --rm -v "$ROOT":/src -w /src "${SNAP_PORT_IMAGE:-pokemonsnap-emsdk}" python3 port/tools/port_build.py --rtw
fi
L=port/build/rtw/link D=port/build/rtw/dist
rm -rf "$D"; mkdir -p "$D"
python3 -I port/rtw/romfill.py "$Z64" "$L" "$D"
cp "$L/pokemonsnap.js" "$D/"
cp port/rtw/retrowser.json "$D/"
# TODO(retrowser host, F3/F4): `controls.motion` (motion aim) and `controls.extra[].place` (the Shutter's "corner")
# are new keys of the host's interface. A check tool (and host) whose schema does not know them yet rejects the whole
# manifest (additionalProperties: false), so they are left out of the shipped copy until RTW_MJS knows them; the
# source manifest port/rtw/retrowser.json keeps them. Remove this step once every host in use has the keys.
node -e '
const fs = require("fs"), [tool, man] = process.argv.slice(1), src = fs.readFileSync(tool, "utf8"), m = JSON.parse(fs.readFileSync(man, "utf8"));
const knows = (k) => new RegExp(`"${k}"\\s*:\\s*\\{`).test(src), gone = [];
if (m.controls && "motion" in m.controls && !knows("motion")) { delete m.controls.motion; gone.push("controls.motion"); }
for (const x of m.controls?.extra ?? []) if ("place" in x && !knows("place")) { delete x.place; gone.push(`controls.extra bit ${x.bit} place`); }
if (gone.length) {
  console.log(`rtw: TODO: the check tool does not know ${gone.join(", ")} yet: left out of the shipped manifest`);
  fs.writeFileSync(man, JSON.stringify(m, null, 2) + "\n");
}' "$RTW_MJS" "$D/retrowser.json"
node "$RTW_MJS" hash "$D"
node "$RTW_MJS" check "$D" --rom "$Z64"
[ -f "$RTW_ROM" ] && node "$RTW_MJS" check "$D" --rom "$RTW_ROM"
python3 -I port/rtw/romscan.py "$Z64" "$D"/pokemonsnap.wasm "$D"/pokemonsnap.js "$D"/pokemonsnap.romfill "$D"/retrowser.json --min 64
