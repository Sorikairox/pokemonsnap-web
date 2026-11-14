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
cp port/rtw/cover.png "$D/"   # the cartridge picture (manifest "picture")
# TODO(retrowser host, F3 / F5): `controls.motion` (motion aim) is new in the host's interface: F3 `true`, F5 the
# object { "when": 3 } (motion aim switched by option 3, the Options menu's "Motion Aim" row). A check tool (and host)
# whose schema does not know a form rejects the whole manifest (additionalProperties: false / type boolean). The
# shipped copy is first checked as written; if the tool rejects controls.motion, the object form falls back to `true`
# (an older host: its own ☰ Motion choice, and the module also needs option 3 on), and if that is rejected too the key
# is left out (no motion aim). The source manifest port/rtw/retrowser.json keeps the new form. Remove this step once
# every host in use knows it.
motion_fallback() {
  node -e '
const fs = require("fs"), man = process.argv[1], m = JSON.parse(fs.readFileSync(man, "utf8"));
if (!m.controls || !("motion" in m.controls)) process.exit(1);
if (typeof m.controls.motion === "object") { m.controls.motion = true; console.log("rtw: TODO: the check tool does not know controls.motion { when }: shipped as motion: true"); }
else { delete m.controls.motion; console.log("rtw: TODO: the check tool does not know controls.motion: left out of the shipped manifest"); }
fs.writeFileSync(man, JSON.stringify(m, null, 2) + "\n");' "$D/retrowser.json"
}
for _ in 1 2 3; do
  node "$RTW_MJS" hash "$D" > /dev/null
  if out=$(node "$RTW_MJS" check "$D" 2>&1); then break; fi
  echo "$out" | grep -q "motion" && motion_fallback || { echo "$out"; exit 1; }
done
node "$RTW_MJS" hash "$D"
node "$RTW_MJS" check "$D" --rom "$Z64"
[ -f "$RTW_ROM" ] && node "$RTW_MJS" check "$D" --rom "$RTW_ROM"
python3 -I port/rtw/romscan.py "$Z64" "$D"/pokemonsnap.wasm "$D"/pokemonsnap.js "$D"/pokemonsnap.romfill "$D"/retrowser.json --min 64
