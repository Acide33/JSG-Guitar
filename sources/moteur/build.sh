#!/bin/sh
# Compile le moteur audio en WebAssembly (wasi-sdk 25)
W=${WASI_SDK:-/home/claude/tools/wasi-sdk-25.0-x86_64-linux}
"$W/bin/clang++" --target=wasm32-wasi --sysroot="$W/share/wasi-sysroot" -O3 -std=c++20 -fno-exceptions -fno-rtti \
  -mexec-model=reactor -msimd128 -ffast-math -fno-finite-math-only -Wall -Wextra -Wno-unused-parameter \
  -Wl,--export-dynamic -Wl,--strip-all -Wl,--initial-memory=16777216 -Wl,--max-memory=134217728 \
  api.cpp -o jsg_engine.wasm
