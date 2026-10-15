#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
upstream="$root/.deps/rexglue-sdk/thirdparty/FFmpeg"
source_dir="$root/.deps/src/ffmpeg-xma"
build="$root/build/ps5-ffmpeg"
export PS5_PAYLOAD_SDK=${PS5_PAYLOAD_SDK:-/home/skm/Downloads/ps5/Links-Awakening-PS5/.deps/native/ps5-payload-sdk}
export PATH="/home/skm/Downloads/ps5/Links-Awakening-PS5/.deps/host/nasm/bin:$PS5_PAYLOAD_SDK/bin:$PATH"
compiler=/home/skm/ps5sdk/native-app-boilerplate/tooling/prospero-clang18
# This is ReXGlue's custom XMAFRAMES decoder, not stock FFmpeg's XMA decoder.
if [[ ! -f "$source_dir/configure" ]]; then
    mkdir -p "$source_dir"
    git -C "$upstream" archive HEAD | tar -x -C "$source_dir"
    # Upstream checks in a host config header; out-of-tree configure needs it absent.
    rm "$source_dir/config.h"
fi
patch_file="$root/ps5/patches/ffmpeg-readable-fft-tables.patch"
if git -C "$source_dir" apply --check "$patch_file" 2>/dev/null; then
    git -C "$source_dir" apply "$patch_file"
elif ! git -C "$source_dir" apply --reverse --check "$patch_file" 2>/dev/null; then
    echo "FFmpeg FFT table patch does not match the pinned source" >&2
    exit 1
fi
mkdir -p "$build"
cd "$build"
"$source_dir/configure" --prefix="$build/install" \
    --enable-cross-compile --target-os=freebsd --arch=x86_64 \
    --cc="$compiler" --ld="$compiler" --ar=llvm-ar --ranlib=llvm-ranlib --nm=llvm-nm \
    --extra-cflags='-D__PROSPERO__ -fPIC -ffunction-sections -fdata-sections' \
    --extra-ldflags="-L$PS5_PAYLOAD_SDK/target/lib -lSceLibcInternal -lkernel -lpthread -lc" \
    --enable-static --enable-pic --disable-shared --disable-autodetect \
    --disable-everything --disable-programs --disable-doc --disable-debug \
    --disable-network --disable-devices --disable-avdevice --disable-avfilter \
    --disable-avformat --disable-postproc --disable-swresample --disable-swscale \
    --disable-encoders --disable-muxers --disable-hwaccels --disable-symver \
    --enable-x86asm --disable-avx512 --enable-avcodec --enable-avutil \
    --enable-pthreads --enable-decoder=xmaframes,wmapro
make -j"${FORZA_BUILD_JOBS:-4}"
make install
"$PS5_PAYLOAD_SDK/bin/llvm-nm" --defined-only "$build/install/lib/libavcodec.a" > "$build/symbols.txt"
grep -q ' ff_xmaframes_decoder$' "$build/symbols.txt"
printf 'Native custom XMAFRAMES decoder installed: %s\n' "$build/install"
