#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
sdk="$root/.deps/rexglue-sdk"
clang++ -std=c++23 -O2 -g -pthread -msse4.1 -fsanitize=undefined -DREX_PLATFORM_LINUX=1 \
 -I"$sdk/thirdparty/simde" -I"$sdk/include" -I"$sdk/thirdparty/fmt/include" -I"$sdk/thirdparty/spdlog/include" \
 -I"$root/upstream/forza/forza-recomp/src" \
 "$root/ps5/tests/title_waits.cpp" "$root/ps5/module/title_waits.cpp" "$sdk/src/system/gpu_write_signal.cpp" \
 -L"$sdk/out/linux-amd64" -Wl,-rpath,"$sdk/out/linux-amd64" -lrexruntime -lfmt \
 -o "$root/build/wait-fixes/title-waits-test"
timeout 15 "$root/build/wait-fixes/title-waits-test"
