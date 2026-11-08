#!/bin/bash
# port/audio offline test (S7): build the aspMain HLE with hle_test.c (command lists written by the test) in the
# pokemonsnap-emsdk image and run it in node there with the player's ROM (./pokemonsnap.z64, read at run time only:
# ADPCM banks and the microcode's resample table). Prints CHECK lines; exit code 0 when all pass.
#   port/audio/test/run.sh [ROM]
set -euo pipefail
export PATH=$PATH:/Applications/Docker.app/Contents/Resources/bin
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
ROM=${1:-pokemonsnap.z64}
B=port/build/audio_test
mkdir -p "$ROOT/$B"
docker run --rm -v "$ROOT":/src -w /src pokemonsnap-emsdk bash -c "
  emcc -O2 -std=gnu11 -Wall -Wno-unused-parameter -Iport/ultra port/audio/audio_hle.c port/audio/test/hle_test.c \
    -sINITIAL_MEMORY=64MB -sNODERAWFS=1 -sENVIRONMENT=node -o $B/hle_test.js &&
  node $B/hle_test.js '$ROM'"
