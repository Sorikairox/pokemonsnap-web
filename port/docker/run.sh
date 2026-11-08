#!/bin/bash
# Matching-build helper. Everything runs in the pokemonsnap-tools container; nothing is installed on the host.
#   port/docker/run.sh image            build the toolchain image (port/docker/Dockerfile)
#   port/docker/run.sh rom PATH         write the .z64 (big-endian) baserom ./pokemonsnap.z64 (gitignored) from the
#                                       player's ROM (.z64/.n64/.v64; SHA-1 checked; the source is only read)
#   port/docker/run.sh build            configure.py (splat extraction) + ninja + checksum (build/pokemonsnap.ok)
#   port/docker/run.sh symbols          port/symbols/*.tsv from build/pokemonsnap.map + .elf, and the asset-TU list
#   port/docker/run.sh sh [cmd...]      shell (or a command) in the toolchain container
set -euo pipefail
export PATH=$PATH:/Applications/Docker.app/Contents/Resources/bin
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
IMG=${SNAP_TOOLS_IMAGE:-pokemonsnap-tools}
SHA1=edc7c49cc568c045fe48be0d18011c30f393cbaf
run() { docker run --rm -v "$ROOT":/src -w /src "$IMG" "$@"; }
# configure.py --setup fetches x86_64 tools on linux/aarch64; the image carries the arm builds in /opt: copied (as
# directories, which the upstream .gitignore matches; symlinks it would not) into tools/ where configure.py expects them
link_tools() { run bash -c 'for t in ido5.3 ido7.1 asm_proc; do rm -rf tools/$t; cp -a /opt/$t tools/$t; done'; }
case ${1:-} in
image)
  ctx="$ROOT/port_work/docker-ctx"; rm -rf "$ctx"; mkdir -p "$ctx"
  cp "$ROOT/port/docker/Dockerfile" "$ROOT/pyproject.toml" "$ROOT/uv.lock" "$ROOT/.python-version" "$ctx/"
  docker build -t "$IMG" "$ctx";;
rom)
  src=${2:?usage: run.sh rom PATH}
  python3 "$ROOT/port/tools/baserom.py" "$src" "$ROOT/pokemonsnap.z64" "$SHA1";;
build)
  link_tools
  run bash -c 'python3 configure.py && ninja';;
symbols)
  run python3 port/tools/gen_symbols.py
  run python3 port/tools/asset_tus.py;;
sh) shift; if [ $# -eq 0 ]; then docker run --rm -it -v "$ROOT":/src -w /src "$IMG" bash; else run "$@"; fi;;
*) sed -n '2,8p' "$0"; exit 2;;
esac
