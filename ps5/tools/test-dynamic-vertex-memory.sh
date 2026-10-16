#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
sdk="$root/.deps/rexglue-sdk"
build="$root/build/dynamic-vertices"
mkdir -p "$build"
clang++ -std=c++23 -O2 -g -fsanitize=undefined -DREX_PLATFORM_LINUX=1 \
    -I"$sdk/include" -I"$sdk/thirdparty/fmt/include" \
    -I"$sdk/thirdparty/spdlog/include" -I"$sdk/thirdparty/xxhash" -I"$sdk/thirdparty" \
    "$root/ps5/tests/dynamic_vertex_memory.cpp" "$sdk/src/graphics/shared_memory.cpp" \
    -L"$sdk/out/linux-amd64" -Wl,-rpath,"$sdk/out/linux-amd64" \
    -lrexruntime -lfmt -lxxhash -o "$build/dynamic-vertex-ubsan"
"$build/dynamic-vertex-ubsan"
