#!/bin/bash
# port/gfx offline test (S3b): build the renderer + hand-written display lists (no ROM data) in the pokemonsnap-emsdk
# image, serve them on localhost, render in the host's headless Chrome (SwiftShader, muted), and extract
#   OUT/shown.png  the canvas as presented (WebGL2, 640x480)
#   OUT/rdram.png  the frame as RDRAM holds it after the write-back (RGBA16, native 320x240)
#   OUT/log.txt    the module's log with the CHECK lines
#   OUT/page.png   a screenshot of the test page
#   port/gfx/test/run.sh [OUT_DIR]      (default port/build/gfx_test/out)
set -euo pipefail
export PATH=$PATH:/Applications/Docker.app/Contents/Resources/bin
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
B=port/build/gfx_test
OUT=${1:-$ROOT/$B/out}
mkdir -p "$ROOT/$B" "$OUT"
docker run --rm -v "$ROOT":/src -w /src pokemonsnap-emsdk emcc -O2 -std=gnu11 -Wall -Wno-unused-parameter \
  -Wno-unused-function -DF3DEX_GBI_2 -D_LANGUAGE_C -Iultralib/include -Iport/ultra \
  port/gfx/gfx_rsp.c port/gfx/gfx_rdp.c port/gfx/test/gfx_test.c \
  --js-library port/gfx/gfx_gl.js -sMAX_WEBGL_VERSION=2 -sMIN_WEBGL_VERSION=2 \
  -sGLOBAL_BASE=256MB -sINITIAL_MEMORY=384MB -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=1MB \
  -sMODULARIZE=1 -sEXPORT_NAME=createGfxTest -sENVIRONMENT=worker \
  -sEXPORTED_FUNCTIONS=_gfx_test_run,_gfx_test_run2 -sEXPORTED_RUNTIME_METHODS=HEAPU8 -o $B/gfx_test.js
cp "$ROOT/port/gfx/test/index.html" "$ROOT/port/gfx/test/worker.js" "$ROOT/$B/"
PORT=$((8300 + $$ % 600))
python3 -m http.server "$PORT" --bind 127.0.0.1 --directory "$ROOT/$B" >/dev/null 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null || true' EXIT
sleep 1
CH="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
# headless Chrome over the DevTools protocol (cdp.mjs: node's own WebSocket, no packages); muted, SwiftShader
node "$ROOT/port/gfx/test/cdp.mjs" "$CH" "http://127.0.0.1:$PORT/index.html" "$OUT" 90000
