#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
build=${FORZA_PS5_BUILD:-$root/build/ps5-runtime}
sdk=${PS5_PAYLOAD_SDK:-/home/skm/Downloads/ps5/Links-Awakening-PS5/.deps/native/ps5-payload-sdk}
export PS5_PAYLOAD_SDK=$sdk

cmake -S "$root/ps5/runtime" -B "$build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$root/ps5/cmake/native.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DPS5_PAYLOAD_SDK="$sdk" "$@"
cmake --build "$build" --parallel "${FORZA_BUILD_JOBS:-6}" \
    --target rexruntime rexgpu-xenos
