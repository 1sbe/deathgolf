#!/bin/sh
# Builds src/core.cpp to web/wasm/core.{js,wasm}. Requires emsdk (em++) on PATH.
set -e
cd "$(dirname "$0")"
em++ src/core.cpp -O2 -std=c++17 -o web/wasm/core.js \
  -sMODULARIZE=1 -sEXPORT_NAME=createCore -sALLOW_MEMORY_GROWTH=1 \
  -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPF32 -sENVIRONMENT=web,node --no-entry
