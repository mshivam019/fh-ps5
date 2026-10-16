# Forza Horizon for PS5

Native PS5 port of the Xbox 360 game **Forza Horizon** (2012), based on
[Forza Horizon 1: ReImagined](https://github.com/Dyotson/RecompiledForzaHorizon)
(the PC recompilation by Dyotson) and the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk),
rendered with Vulkan through RADV. Title ID: **PPSA99690**.

The game's PowerPC code is statically recompiled to native code. You must supply
your own **Forza Horizon** Xbox 360 disc 1 image (USA release). Game files,
generated game code and saves are not distributed with this project.

[Releases](https://github.com/mshivam019/fh-ps5/releases) ·
[Install](ps5/SETUP.md) · [Build from source](ps5/BUILD.md)

## State

- Native homebrew title launched from the PS5 home screen (tested on firmware 9.00).
- Boots to the title screen, profile, free roam and races, with audio, DualSense
  input and saves, at the original 30 FPS.
- **Busy scenes still stutter.** The GPU command thread needs more than one
  frame of CPU in dense scenes (command dispatch, texture and buffer uploads),
  and the game waits on it. The lock stalls that caused most of the earlier
  hitching were removed; what remains is ordinary CPU work. Measurements and
  the next steps are on the development branch (below).
- PlayStation button prompts are built in but not yet verified on a console.
- Setup for players has not completed an end-to-end test yet; see
  [`ps5/SETUP.md`](ps5/SETUP.md).

Development notes, measurements and the ongoing render-thread optimization live
on the [`optimize-render-thread`](https://github.com/mshivam019/fh-ps5/tree/optimize-render-thread)
branch (`STATUS.md`, `PLAN.md`).

## Install

1. Install **PPSA99690.zip** with your native-title launcher or PS5 Upload.
2. On a Linux computer (Windows: WSL2), extract the matching setup ZIP and run
   `sh setup.sh --iso "/path/to/Forza Horizon (USA).iso"`.
3. Copy the `game` folder setup creates into `/mnt/ext1/etaHEN/games/PPSA99690/`.
4. Launch **Forza Horizon** from the home screen.

[`ps5/SETUP.md`](ps5/SETUP.md) has the requirements, layout and controls.

## How it works

The app (`eboot.bin`) contains the PS5 runtime only. Setup builds the game code
from your disc into a private module, `game/forza.module`, which the app loads
at startup and binds to its runtime (`ps5/module/`). Port-specific changes:

- **PS5 runtime** (`ps5/patches/rexglue-ps5.patch` on the pinned SDK plus the
  PC port's SDK patches): sandbox paths, fibers, clocks, guest memory on PS5
  direct memory with 16 KB-page write watches, native display with
  refresh-synchronized frame pacing, background shader compilation, and GPU
  command-processor and memory-synchronization work: the command thread spins
  instead of sleeping on the emulator's global lock, batches validity and
  protection updates per upload, answers protection queries from tracked state,
  and the virtual filesystem has its own lock.
- **Game module** (`ps5/module/`): loader, app/module contract, and
  `title_waits.cpp`, which gives the game's render-idle sleep and GPU fence
  polling real waits.
- **PlayStation button prompts** (`ps5/app/button_prompts.cpp`): Xbox prompt
  textures are recognised by content hash and redrawn as PlayStation glyphs.
- **Diagnostics** (`ps5/app/diagnostic_trace.cpp`, `tools/frame-trace.py`):
  five-second frame, memory-synchronization and lock-holder summaries in the
  runtime log, an optional per-frame stage log (`ps5_long_frame_log`) and a
  bounded binary frame trace (`/app0/trace.enable`).

## Repository layout

```text
ps5/app/       Native app: startup, input, prompts, display, diagnostics
ps5/module/    Game-module loader, the app/module contract and title hooks
ps5/tools/     Setup, module build, app build, host tests and release packaging
ps5/patches/   ReXGlue SDK changes for PS5
ps5/tests/     Host tests for the memory, shared-memory, display and wait code
tools/         SDK preparation, console helpers, Linux run script, texture dump
```

## Credits and licensing

The Forza Horizon 1: ReImagined project by Dyotson and contributors (MIT)
supplies the code-generation configuration, game hooks and SDK patches; ReXGlue
by Tom Clay (derived from Xenia) supplies the recompiler and runtime. Mihawk's
PS5 Vulkan template supplies the native graphics and application foundation.
The dynamic-buffer content tracking follows BrinooTk's PS5X360 (off by default).

Project code is MIT ([LICENSE](LICENSE)). The app also contains BSD, MIT, zlib,
Apache-2.0, LGPL and GPL components; see
[`ps5/THIRD_PARTY_NOTICES.txt`](ps5/THIRD_PARTY_NOTICES.txt). Each release
includes their license texts and a source archive. Artwork credits are in
[`ps5/sce_sys/CREDITS.md`](ps5/sce_sys/CREDITS.md).

This is an unofficial fan project, not affiliated with or endorsed by Playground
Games, Turn 10 Studios, Microsoft or Sony.
