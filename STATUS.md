# Forza Horizon PS5: status and plan

Last updated: October 15, 2026. Repo `mshivam019/fh-ps5` (private), branch `main`.
Console title: **PPSA99690**, installed on the M.2 at `/mnt/ext1/etaHEN/games/PPSA99690`.

## Where it stands

Forza Horizon boots on the PS5 and plays: title screen, profile, Single Player,
the opening drive and free roam, with sound, controller and saves. It holds 30 FPS
most of the time. The main open problem is **stutter in busy or new areas**.

| Area | State |
| --- | --- |
| Boot, menus, loading | Works |
| Free roam driving | Works, 30 FPS with dips (see Performance) |
| Audio | Works |
| Controller | Works (DualSense through the SDL bridge, every pad drives player 1) |
| Profile and saves | Works: the profile loads and saves. Progress is only kept after the game's own autosave (saving icon). |
| Shader stutter | Fixed: new shaders compile in the background |
| PlayStation prompts | Built, not yet tested on the console (see below) |
| Icon and background | Done: public-domain NPS photo of Trail Ridge Road (`ps5/sce_sys/CREDITS.md`) |
| Setup for other players (`ps5/tools/setup.py`) | Adapted for Forza, end-to-end test not finished |
| Release packaging | Not started (Naruto scripts copied, not adapted) |

## What was fixed to get here

1. **Black 3D world** (Linux and PS5). Merging the Naruto PS5 patch onto the Forza
   SDK patches dropped one call: vertex buffer residency invalidation for bulk fetch
   constant writes. Found by building the unpatched SDK and bisecting the graphics
   files. Fixed in `src/graphics/vulkan/command_processor.cpp`.
2. **Controller and sign-in did nothing.** The game module defines its own kernel
   overrides (upstream `src/kernel/*`) and forwards to the runtime's versions by name.
   The list of names the app exports (`ps5/module/runtime-lookups.txt`) is now
   generated from the module's own overrides by `build-game-module.py --write-imports`.
3. **Stuck on the loading screen.** Each guest thread got a 16 MiB native stack; the
   65th thread failed with EAGAIN. PS5 now uses 4 MiB (`src/system/xthread.cpp`), and
   thread creation logs and retries a refused priority (`src/core/threading_posix.cpp`).
4. **Shader stutter.** The engine finished every queued pipeline at the end of each
   submission, which froze the frame, then dropped the frame. New cvar
   `vulkan_async_pipeline_wait_at_submission = false` lets pipelines finish on worker
   threads; `vulkan_async_skip_incomplete_frames = false` presents frames anyway. Linux,
   cold caches, 190 s boot-to-drive: stutters 405 → 20, p95 frame time 68.3 → 34.5 ms.
   PS5: pipeline waits on the graphics thread fell from up to 112 ms per 5 s to about 1 ms.

## Performance (main concern)

Console log windows are 5 s; 150 frames is a full 30 FPS. In free roam most windows
are 150 with no frame over 50 ms. Bad windows drop to 114–144 frames.

| Cause in a bad window | Evidence |
| --- | --- |
| The game's CPU code is late with the frame's commands | `command_stall_ms` 130–360 ms per 5 s (normal ~30) |
| The graphics thread waits for the GPU | `fence_ms` 30–96 ms per 5 s (normal ~1) |
| Heavy command processing | ~3,000 draws per frame; the sampled draw cost is 45–60% of the graphics thread |

On Linux (Ryzen laptop) the game's main guest thread is also at ~99% of a core, so
the CPU side is close to its limit at 30 FPS.

### Plan, in order

1. **Readback sync test.** Run with `readback_resolve = "none"` to see whether the
   GPU waits (`fence_ms`) disappear. Cost: car thumbnails in the garage may be noise.
   If they disappear, make the readback asynchronous instead of turning it off.
2. **Profile the guest CPU.** Find what the game thread does in late frames
   (streaming, physics, AI). Options: per-thread CPU time on the console, upstream's
   `cpu_profile` on Linux with the same scenario, hot guest functions.
3. **Cut graphics-thread cost per draw.** Textures (~0.9 s per 5 s) and vertex buffers
   (~0.6 s per 5 s) are the largest sampled stages. Check which Naruto fast paths
   apply to Forza's draw patterns and where the merged code still does per-draw work.
4. **Thread placement.** Keep pipeline workers and helper threads off the cores the
   game's main thread and the graphics thread use; cap pipeline workers (3–4).
5. Re-measure on the console after each step: same route, 5 minutes, compare
   `PS5 frame timing` windows.

## Other open items

- **Test PlayStation prompts on the console.** Built into the current `eboot.bin`
  (not yet deployed): A/B/X/Y and RT prompt textures, and the loading-screen controls
  diagram (PlayStation face buttons, L1/L2/R1/R2 labels, plain home button). More
  prompt textures (LB/RB/LT, Start/Back, d-pad, sticks) still need to be found with
  `tools/texdump` on Linux; START/BACK on the diagram are unchanged.
- **Oversteer report.** Check whether the car pulls with the stick centred, or
  turns hard on small stick movements. If so, log the stick values the game receives
  and fix the mapping in `ps5/app/input.cpp`.
- **Finish the setup test**: `python3 ps5/tools/setup.py --iso private/<disc>.iso
  --output build/setup-test/PPSA99690`. The interrupted run rebuilt part of the SDK's
  Linux libraries in `.deps/rexglue-sdk/out/linux-amd64`; rebuild `build/linux`
  (`cmake --build build/linux -j8`) before the next Linux run.
- Adapt `ps5/tools/package-*.py` and `extract-xiso.py` headers, then README, SETUP,
  BUILD, RELEASING docs as in the Naruto repo.
- Replace the `[Naruto thread]` and `[Naruto GPU init]` debug prints in the SDK patch.

## Testing on the console

The console needs to be on, with nothing else running. Closing Forza loses progress
since the last autosave, so ask before closing it.

```sh
python3 tools/console.py close                  # close Forza only
python3 tools/deploy-console.py --skip-game     # app, module, config, art (game files already there)
python3 tools/console.py boot TAG --seconds 300 # launch and record logs to build/console/
python3 tools/console.py log TAG                # fetch the runtime log
```

Builds:

```sh
python3 ps5/tools/build-title.py --jobs 8       # PS5 runtime and eboot.bin
python3 ps5/tools/build-game-module.py --generated upstream/forza/forza-recomp/generated \
  --toolchain .deps/tools/llvm-mingw-20260922-ucrt-ubuntu-22.04-x86_64/bin \
  --sysroot ../Links-Awakening-PS5/.deps/native/ps5-payload-sdk --sdk .deps/rexglue-sdk \
  --work build/ps5-module --output build/ps5-module/forza.module --jobs 8 --write-imports
tools/linux-run.sh TAG SECONDS [args]            # headless Linux run with screenshots and PERF lines
```

SDK changes are made in `.deps/rexglue-sdk`, mirrored into the scratch repo
`.deps/sdk-merge` (branch `forza-ps5`), committed there, and exported with
`git -C .deps/sdk-merge diff forza-base forza-ps5 > ps5/patches/rexglue-ps5.patch`.
