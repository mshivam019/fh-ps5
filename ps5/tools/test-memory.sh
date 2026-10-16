#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
platform="$root/../PS5_PayloadSDK/platform"
build="$root/build/memory-test"
mkdir -p "$build"
for source in src/shm.c src/placement.c tests/host_kernel.c; do
    cc -std=gnu11 -O1 -g -ffunction-sections -fdata-sections -I"$platform/include" -I"$platform/src" \
        -I"$platform/tests" -c "$platform/$source" -o "$build/$(basename "$source").o"
done
clang++ -std=c++23 -O1 -g -mssse3 -fsanitize=undefined -pthread -Wl,--gc-sections \
    -I"$platform/include" -I"$platform/tests" \
    -I"$root/.deps/rexglue-sdk/include" \
    "$root/.deps/rexglue-sdk/src/core/memory_ps5.cpp" \
    "$root/.deps/rexglue-sdk/src/core/memory.cpp" \
    "$root/ps5/tests/memory.cpp" "$build"/*.o -o "$build/test-memory"
"$build/test-memory"
