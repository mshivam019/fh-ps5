# Building from source

Players only need the release files and [setup](SETUP.md). This page builds the
app itself.

## App (`eboot.bin`)

The app build reuses native dependencies prepared by the sibling PS5 ports in
the same workspace: `../PS5_Vulkan` (RADV, link recipe, `libc.prx`),
`../Dusklight-PS5` (SDL3 and Mesa sources), `../Links-Awakening-PS5` (prepared
payload SDK) and `~/ps5sdk/native-app-boilerplate` (Clang 18 wrappers).

```sh
bash tools/prepare-sdk.sh             # pinned PC port + ReXGlue SDK, upstream patches, ps5/patches/rexglue-ps5.patch
bash ps5/tools/build-ffmpeg.sh
python3 ps5/tools/build-title.py --jobs 8
python3 ps5/tools/validate-elf-layout.py build/native-title/eboot.elf
```

Host tests (Linux, UBSan/ASan):

```sh
bash ps5/tools/test-memory.sh                 # PS5 memory layer on the host kernel model
bash ps5/tools/test-shared-memory.sh          # residency, batched validity, lock contention counters
bash ps5/tools/test-shared-memory-bounds.sh   # invalidation widening with 16 KiB pages
bash ps5/tools/test-dynamic-vertex-memory.sh  # content-tracked vertex pages
bash ps5/tools/test-wait-primitives.sh        # timer queue and GPU write signal
bash ps5/tools/test-title-waits.sh            # title hooks (render idle, fence wait)
```

The shared-memory tests link the Linux SDK libraries: build them first with
`cmake --build build/linux` (the Linux game build, see `tools/linux-run.sh`).

## Game module

`ps5/tools/setup.py` is the reference: it prepares the sources, builds the code
generator, extracts the disc, generates code and runs
`ps5/tools/build-game-module.py` with the pinned toolchain
(`ps5/module/toolchain.json`). For development:

```sh
python3 ps5/tools/setup.py --iso /path/to/forza.iso --output /tmp/prepared/PPSA99690
```

The app and module share a contract: the runtime symbols the module imports
(`ps5/module/host-imports.txt`) and an ABI identity hashed from that list, the
compile flags (`ps5/module/compile-flags.txt`), the toolchain pins, the module
preparation script and the contents of the SDK headers the module includes
(`ps5/module/abi-headers.txt`). Runtime changes outside those headers keep
players' modules valid; `include/rex/thread/mutex.h` is one of them, so lock
changes require a module rebuild. When any of these change, regenerate the
lists from a module build and rebuild the app:

```sh
python3 ps5/tools/build-game-module.py --write-imports \
  --generated upstream/forza/forza-recomp/generated \
  --toolchain .deps/tools/llvm-mingw-20260922-ucrt-ubuntu-22.04-x86_64/bin \
  --sysroot ../Links-Awakening-PS5/.deps/native/ps5-payload-sdk \
  --sdk .deps/rexglue-sdk \
  --work build/ps5-module --output build/ps5-module/forza.module --jobs 8
```

A module built for a different ABI is refused at startup.

## SDK patch

`tools/prepare-sdk.sh` checks out the pinned SDK into `.deps/rexglue-sdk`,
applies the PC port's SDK patches and then `ps5/patches/rexglue-ps5.patch`.
Edits are made there and mirrored into a scratch repository `.deps/sdk-merge`
(branch `forza-ps5` on top of `forza-base`, the pinned SDK plus the upstream
patches), then exported and checked:

```sh
git -C .deps/sdk-merge diff forza-base > ps5/patches/rexglue-ps5.patch
git -C .deps/rexglue-sdk apply --check -R ps5/patches/rexglue-ps5.patch
```

## Linux run

`tools/linux-run.sh TAG SECONDS` runs the Linux build headless with a scripted
controller, screenshots and per-thread CPU use; `build/runs/TAG/` holds the
output. `tools/console.py` and `tools/deploy-console.py` drive a development
console (`PS5_HOST`).
