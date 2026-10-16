#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
sdk="$root/.deps/rexglue-sdk"
mkdir -p "$root/build/wait-fixes"
clang++ -std=c++23 -O2 -g -pthread -fsanitize=address,undefined -DREX_PLATFORM_LINUX=1 \
 -I"$sdk/include" -I"$sdk/thirdparty/fmt/include" -I"$sdk/thirdparty/disruptorplus/include" \
 "$root/ps5/tests/wait_primitives.cpp" "$sdk/src/system/gpu_write_signal.cpp" \
 -o "$root/build/wait-fixes/wait-primitives"
timeout 15 "$root/build/wait-fixes/wait-primitives"
