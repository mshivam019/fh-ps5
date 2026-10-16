# Forza Horizon PS5: status and plan

This file and `PLAN.md` are kept on the `optimize-render-thread` branch. The
October 16 lock/stutter investigation is recorded chronologically below; the
handoff that used to be in `STUTTER-HANDOFF.md` follows at the end.

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

The high-utilization guest thread spends much of its sampled CPU time polling
graphics progress. Its utilization alone does not establish a simulation
bottleneck. The October 15 console capture contains 125,414 samples, with none
dropped. In 330.9 seconds of high-draw log windows, draw processing takes a
median 2.72 seconds per five-second window, while pipeline waits take 0.7 ms.
The capture does not attribute every individual hitch to a specific operation.

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

### Resident-stream scan comparison candidate

`VulkanCommandProcessor::IssueDraw` now removes already-resident vertex streams
from each shader fetch mask before scanning it. Missing streams still follow the
same fetch validation, residency request, and invalidation paths. No graphics
features, readbacks, saves, or caches are disabled.

- Mask equivalence: 40,028 cases cover indices 0–95 and full/partial invalidation.
- C++ smoke: 1,024 random masks agree. A five-million-draw resident-only
  microbenchmark takes 12.53 ms versus 46.18 ms for the old scan. This measures
  only that small loop, not game frame time.
- Linux 180-second boot-to-drive smoke renders the world and car. Guest p95 is
  34.9 ms versus 35.0 ms in the control. Presentation results vary substantially
  between runs; they do not establish a whole-game stutter improvement.
- Console playtest: the user reports multiple stutters. The first 18 high-draw
  windows average 27.9 FPS, versus 27.88 FPS across 66 baseline windows. Routes
  and sample durations differ, so these are not a controlled A/B comparison.
  The candidate still has 67–166 ms frame gaps with zero shader placeholders
  in the recent windows and 0.7–0.8 ms of pipeline waits per five seconds.
  Several affected windows also contain 150–360 ms of command starvation.
  This change has not established a meaningful whole-game pacing improvement.
  Next investigation: correlate individual frame gaps with guest command
  delivery and streaming, rather than infer their cause from utilization.

Rejected experiments: fence-poll sleeping lowered CPU utilization without
improving pacing. Unchanged shader-constant comparisons provided no throughput
gain and were removed. Neither experiment is included in this candidate.


### Display-completion pacing and scheduler corrections

**Rejected console candidate:** the user reported lag with the completed pacing
build. The pre-experiment executable was restored byte-for-byte from
`eboot.bin.before-display-pacing-20261015-193641`. The attempted rollback launch
returned `launch_sony_error_0x80020008`; the subsequent process query found no
running Forza process. Do not treat this source revision as a proven improvement.

The full lagging runtime log is `build/console/pacing-lag-full-runtime.log`
(console `forza_020.log`). Excluding the first two startup windows, 20 windows
average 29.46 guest swap requests per second, with 57 gaps over 50 ms and a
150 ms maximum. This does not measure distinct displayed images.

The pacing change introduces a feedback bottleneck: UI redraws can continue
queueing copies of the held guest image, but only the pacing thread grants a new
image acquisition. It waits for a serial newer than the last submitted serial,
then waits another vblank before granting the next acquisition. Queue delay
therefore becomes part of the acquisition interval. A 60 Hz discrete-event
counterexample with the UI running before the pacer produces only 20, 15 or
12 distinct images per second with one, two or three queued images, despite
30 Hz guest production. These are model results, not measured console FPS.
The console log has no per-image acquisition/scanout correlation, so the exact
queue depth and displayed cadence during the reported lag remain unmeasured.

The next candidate gives the UI thread sole ownership of paced PS5 painting.
UI ticks without a pending paced request cannot paint or queue held-image
copies. Request deadlines run from the preceding request, not its completion;
there is no extra refresh wait after scanout. Guest-image sequence numbers
travel through the mailbox and are reported after confirmed display progress.
The five-second `PS5 display cadence` log reports distinct and repeated images,
completion-observation gaps, the acquired guest sequence and submitted flip.
These timestamps observe completion after a vblank wait, not hardware scanout
timestamps.

The request-clocked host model maintains one queued image and two-refresh
image intervals with either one or eight UI redraw attempts per refresh.
The display bridge sanitizer regression still passes delayed submission,
scanout, stale frames, queued-ahead frames and API-error handling.
The native executable builds and its five loadable segments match deployment.
The previous executable is retained as
`eboot.bin.before-single-owner-20261015-200258`.

Console smoke `single-owner` (`forza_021.log`) launched and remained running
through the 40-second capture. VideoOut confirmed flip 1; no display-wait timeout
appears in the captured kernel log. The last three cadence windows report
142/149/149 distinct images in 5.02 seconds each, with 0/0/1 repeated images.
Their maximum completion-observation gaps are 83.4/50.1/50.1 ms. The last guest
window reports 150 swap requests in 5.00 seconds, max gap 33.5 ms. Thus the
candidate sustains near-30 Hz presentation rather than the queue-feedback
model's 15–20 Hz, but remaining display gaps and user-driven race/free-roam
acceptance are not resolved by this boot capture.

The PS5 presenter now waits for VideoOut display progress rather than counting
`vkQueuePresentKHR` returns as completed refreshes. This covers direct and
UI-thread presentation. A paced acquisition token prevents independent UI
redraws from taking newer guest images before their scheduled refresh.

The completion bridge accepts the first displayed serial newer than the
captured submission. It does not chase the newest queued serial: that caused
repeated 250 ms pacing-thread timeouts while the UI kept submitting frames.
Those timeouts do not establish a 250 ms screen freeze.

Guest vblank now sleeps toward its guest-clock deadline instead of polling every
millisecond. Detailed per-draw timing is opt-in; the five-second frame-gap and
pipeline-wait logs remain enabled. Graphics, thumbnail readbacks, saves and
shader caches remain enabled and unchanged.

Verification:

- Native display regression passes with AddressSanitizer and UndefinedBehaviorSanitizer:
  delayed submission, delayed scanout, stale frames, queued-ahead frames, API
  errors and output close. The queued-ahead regression failed before its fix.
- Linux scheduler smoke emitted 120 ticks in both modes. Deadline waiting used
  121 wakeups versus 1,892; p95 lateness was 304 us versus 976 us. These are host
  measurements, not console scheduler measurements.
- PS5 runtime and executable build successfully. Deployment verifies all five
  loadable ELF segments and retains the previous executable.
- Earlier console build `pacing-ui` confirmed a completed VideoOut flip, but
  still logged pacing timeouts. Its gameplay log includes a five-second window
  with seven gaps over 50 ms, maximum 99.9 ms, and only 0.8 ms of pipeline waits.
  Later windows delivered 150 swap requests in five seconds. These counters
  measure guest swap requests, not displayed-frame cadence.
- Corrected console smoke `pacing-complete` ran for 30 seconds, confirmed
  VideoOut flip 29, and recorded no display-wait timeout in its kernel capture.
  Guest swap gaps still reached 82–150 ms in the later five-second windows.
  Successful display synchronization does not remove late guest frames.
- The corrected build still requires a user-driven race/free-roam comparison.
  No stutter-elimination or cold-area performance claim is established.

References:

- [Vulkan presentation contract](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueuePresentKHR.html):
  queueing presentation is not evidence that scanout completed.
- [Android frame-pacing guidance](https://developer.android.com/games/sdk/frame-pacing):
  synchronize presentation with display timing and avoid queue stuffing.
- [Upstream Forza performance findings](https://github.com/Dyotson/RecompiledForzaHorizon/blob/main/docs/perf.md):
  distinguish guest and presentation cadence; preserve texture invalidation,
  sampler lifetimes and required readbacks when optimizing graphics work.

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

## Bounded frame diagnostics

The optimized native build retains local DWARF information in
`build/native-title/link/llvm-pie.elf`. The console executable remains packaged
without those debug sections. Keep the matching ELF when collecting addresses.

Create `/app0/trace.enable` before launch to enable the diagnostic capture.
Without the marker, tracing stays disabled. The capture writes
`/app0/logs/frame-trace.bin` on a background thread with 100 ms drain intervals.
The reusable queue holds 4,096 records. Producers try a short queue lock and
drop rather than wait on contention or saturation; the writer copies bounded
batches and releases the lock before file I/O. Capture stops at ten minutes or
64 MiB, not after a lifetime event count. Every two seconds, a binary health
record reports cumulative dropped events. Kernel messages report write errors.
A new diagnostic launch replaces the previous binary capture, so download it
first. Removing the marker disables capture on the next launch.

Events cover GPU swap arrival, mailbox publication and acquisition, paced UI
requests, UI paint boundaries, native flip submission and observed completion.
Spans over two milliseconds cover command starvation, blocking submission-fence
checks, swap submission, swap processing and host-file reads with paths and
requested byte counts. Seventeen exclusive per-frame wall-time categories cover
remaining worker time, residual command dispatch, empty-ring/register/fence
waits, swap work, submission, primitives, textures, render targets, pipelines,
bindings, vertex buffers, constants, resolves/readbacks, shader translation and
samplers. Nested categories are subtracted from their parent. Ordered cycle
reads, calibrated against the steady clock at startup, avoid a system-clock call
at every draw-stage boundary. Wall time still includes OS descheduling.

Thread CPU time is sampled once per command frame. The console probe selected
`CLOCK_THREAD_CPUTIME_ID`; `getrusage(RUSAGE_THREAD)` is a fallback. An unsupported
clock is reported as unavailable, never as zero CPU usage. This CPU total covers
the whole frame interval, not CPU time for each individual stage. Residual
dispatch includes command decoding and uninstrumented work; neither it nor
whole-frame wall time alone proves CPU saturation.

Download the binary over FTP and decode locally:

```sh
python3 tools/frame-trace.py build/console/frame-trace.bin \
  --after-seconds RACE_START_SECONDS --json build/console/frame-trace-report.json
```

The report correlates distinct-image gaps over 45 ms with publication,
acquisition, native submission and completion observations. Overlapping slow
reads/waits are evidence of temporal overlap, not proof of causality.
The decoder ignores an incomplete final record when reading a live download.

Local verification: `ps5/tests/trace_queue.cpp` covers empty/saturated queues,
FIFO wraparound beyond 65,536 records, concurrent integrity and exact drop
accounting under AddressSanitizer and UndefinedBehaviorSanitizer.
`ps5/tests/trace_decoder.py` checks exclusive totals versus contiguous spans,
race cutoffs, incomplete frame categories, health counters, partial records,
17-category metadata, CPU totals outside the wall budget and unsupported CPU
clocks. An actual writer smoke preserved 160,000 unique events with zero drops.
A later calibrated-clock smoke separated 3 ms dispatch, 5 ms textures, 10 ms
fence sleep and 2 ms constants into exclusive categories: wall totals were
20.10–20.23 ms and thread CPU totals 9.97–9.99 ms on the host. These are helper
smoke measurements, not console gameplay measurements.

The dispatch-stage executable preserves the previous executable as
`eboot.bin.before-dispatch-stage-diagnostics-20261015-205323`. Matching local
symbols are `build/console/dispatch-stage-symbols.elf`. Saves, graphics settings,
thumbnail readbacks and shader caches are unchanged.

### First diagnostic findings

The preserved recent trace (`build/console/diagnostic-recent-trace.bin`) contains
59,983 events over 202.565 seconds. Its decoded report is
`build/console/diagnostic-recent-report.json`; runtime evidence is
`build/console/diagnostic-recent-runtime.log` (`forza_022.log`).

- An early 183.508 ms distinct-image gap overlaps a 156.061 ms command-starvation
  span. Publication to acquisition takes 0.926 ms and acquisition to native flip
  takes 0.689 ms. The long delay is upstream of presentation.
- Later distinct-image gaps of 100.104/83.455/83.431 ms follow consecutive
  publication intervals of 100.704/86.374/83.746 ms. Those images were already
  late when published; their UI request-to-acquire times are below 0.8 ms.
- The longest recorded file read is 9.040 ms; the longest recorded blocking
  fence-check span is 9.016 ms. Neither alone establishes the cause of those
  later 83–100 ms gaps. Decompression, guest simulation, CPU command processing,
  register waits and descheduling are not yet separated by this first capture.
- A concrete mailbox race repeats a displayed image: the request snapshots
  publication 2031, but the UI acquires image 2032. Acknowledging 2031 permits
  another request for image 2032, followed by a 66.728 ms distinct-image gap.

The correction uses the actual acquired image sequence for completion
accounting and the same sequence for publication. Monotonic comparisons also
handle acquisition ahead of the publisher's notification. The pacer waits for
publication rather than waiting an extra vblank while no image is ready.
A local replay covers the observed stale snapshot, skipped publications and
acquisition-ahead-of-notification boundaries.

The narrowed diagnostic capture also records GPU `WAIT_REG_MEM` spans over
two milliseconds, with register/address and wait flags. This distinguishes a
command-stream synchronization wait from the empty-command-ring wait.
The previous diagnostic executable is backed up as
`eboot.bin.before-publication-accounting-20261015-202230`.

Corrected console capture `publication-accounting` remained running through
the 40-second smoke. Its subsequent binary download contains 21,623 events over
54.395 seconds, with **zero repeated-image completions** across 1,912 confirmed
presentations. The prior capture exposed the repeated-image race. The corrected
capture also contains 2,536 GPU-register-wait spans, confirming that the new
marker is active; these waits include normal guest-vblank synchronization and
are not inherently a bottleneck.

This boot capture is not a controlled race/free-roam comparison. It still
contains a 200.196 ms distinct-image gap during the captured startup sequence.
The long game-side delivery stalls remain under investigation. Do not claim all
stutters are fixed. Next acceptance needs a user-driven session with this build,
then correlation of long publication gaps with command starvation, register
waits, host reads and blocking submission waits.

### Cadence-transition regression and active race capture

The user reported that gameplay continued behind a black screen with the
publication-accounting build. `build/console/black-screen-trace.bin` confirms
presentation stopped at 82.877 seconds while image publication continued to
138.440 seconds. The old refresh-count gate required two refreshes after a
60-to-30 FPS transition, while its replacement publication wait no longer
incremented the refresh count. This prevented all subsequent paint requests.

The PS5 path now admits every new image subject only to the request deadline;
the non-PS5 refresh-count policy is unchanged. A local state replay reproduces
the old loop making 1,000 iterations without a paint request and checks liveness
across both cadence transitions and steady cadences.

The corrected build is installed; the prior executable is preserved as
`eboot.bin.before-cadence-transition-20261015-202610`. The 100-second console
capture remained running. At the user's race-start report, a subsequent binary
snapshot spans 108.444 seconds and contains 3,267 confirmed display completions
with zero repeated images. This demonstrates continued presentation past the
previous stop point, not yet a completed race acceptance test.

`build/console/race-start-marker.json` records the user-reported race boundary.
Exclude earlier boot/title samples from gameplay comparisons. The current race
capture requires the user's playtest; do not restart the title during it.

### Race-only diagnostic snapshot

`build/console/race-live-trace.bin` reached the fixed 65,536-event bound at
204.477 seconds. The user-reported race boundary is 108.444 seconds, leaving
96.033 seconds of race-only data. The bounded binary capture has stopped;
runtime logging and the game continue. No restart or build replacement occurred
during this race. The decoded report is `build/console/race-live-report.json`.

The race section has 2,820 confirmed completions, zero repeated images, and
113 distinct-image gaps above 45 ms. Its distinct-completion rate is about
29.36 Hz. A 100.103 ms display gap follows a 100.169 ms publication gap; other
83.428/83.416 ms gaps follow 84.252/83.752 ms publication intervals. The frame
delivery path remains late before the UI can present those images.

There are no host-read spans above two milliseconds in this race section.
Fourteen recorded blocking fence spans reach at most 8.495 ms; thirteen
command-starvation spans reach at most 16.614 ms. Neither class explains the
largest gaps by itself. GPU-register-wait spans occur 2,772 times, with median
14.047 ms, p95 17.767 ms and maximum 19.932 ms. Their common address is
`0x1fca4006`, flags `0x13`, wait `0x100`; normal guest-vblank waits are included.
Do not remove these synchronization waits based only on their aggregate time.

The remaining unclassified time includes CPU command processing, guest
scheduling and waits shorter than the span threshold. The next diagnostic
slice must attribute time between command processing and guest synchronization,
not repeat file-cache changes or claim that all stutters are fixed.

### Sustained attribution race procedure

1. Keep the same race, car, graphics settings and shader cache for comparison.
2. Launch the diagnostic build with `trace.enable` present. Tell the operator
   when the race starts so a capture-relative cutoff can be saved.
3. Race normally and report hitches. The operator must not restart the title
   during the race. Download the binary before the next diagnostic launch.
4. Decode with `--after-seconds` from the saved race marker. Inspect the latest
   dropped-event count and each frame's `complete` flag before attribution.
   Incomplete frame totals must not be interpreted as a complete timing budget.
5. Compare publication/delivery gaps with exclusive command-frame categories.
   Dispatch time excludes instrumented nested register, fence and swap work,
   but still includes OS descheduling and uninstrumented operations. Temporal
   overlap and wall-time totals do not establish CPU saturation.

The user started the sustained diagnostic race at an observed trace elapsed
104.054 seconds. `build/console/sustained-race-start-marker.json` records the
boundary; boot/title events before it are excluded from race analysis. At the
boundary, the capture contains 54,273 events and 3,093 complete seven-category
frame snapshots. The latest health record reports two dropped events in the
startup segment. The game and diagnostic settings remain unchanged during
the race.

Live console verification passed the previous lifetime limit: the snapshot at
176.192 seconds contains 92,572 events. After the 104.054-second race cutoff,
the report includes 38,299 events and zero repeated-image completions.
The latest health record reports three dropped events total versus two at the
race-start boundary. Reported frame examples below have all seven categories.
Evidence: `build/console/sustained-race-live-trace.bin` and
`build/console/sustained-race-live-report.json`.

One 83.323 ms command-frame interval contains 73.344 ms of dispatch, 5.068 ms
of register waits, 0.644 ms of empty-ring waits, 0.009 ms of blocking fence
checks, 3.860 ms of swap work and 0.390 ms of swap submission. It precedes
an 83.406 ms distinct-image gap; the image's publication-to-acquisition delay
is 0.800 ms. Another 67.759 ms interval has 63.389 ms of dispatch.

The largest remaining sampled race hitches are dominated by dispatch wall time,
not the measured nested register/fence waits or empty-ring waits. This narrows
the next diagnostic slice to operations inside dispatch versus OS descheduling.
It does not yet identify a particular texture, draw or resource operation,
and does not establish CPU saturation. Keep the current race build unchanged
until the user completes the comparison run.

### Dispatch-stage capture verification

The previous completed race is preserved in
`build/console/sustained-race-finished-trace.bin` and its decoded report.
It spans 300.919 seconds with 158,912 events, six dropped events and zero
repeated-image completions in the race-only section.

The dispatch-stage console smoke remained running through its 45-second
capture. The subsequent snapshot spans 57.035 seconds, has 49,926 events with
zero drops/repeated completions, and contains 1,760 complete 17-category
snapshots. The kernel CPU-clock probe succeeds with method 1 (thread CPU clock).
This confirms the instrumentation works; startup timing is not gameplay evidence.

The user started the next same-race comparison at an observed elapsed
113.893 seconds. `build/console/dispatch-race-start-marker.json` records the
cutoff. At this observation, the latest health record reports one dropped
startup event. Exclude earlier boot/title samples and do not replace or
restart this build during the race.

### Paused: measured dispatch hotspots

Work is paused at the user's request. Do not restart, redeploy or begin the
next optimization until the user resumes. The installed dispatch-stage
diagnostic build and running game were left unchanged. Its bounded capture
ends automatically after ten minutes or 64 MiB; download it before any future
diagnostic launch replaces it.

The saved race-only evidence is
`build/console/dispatch-race-live-trace.bin` and
`build/console/dispatch-race-live-report.json`. Exclude samples before the
113.893-second boundary in `build/console/dispatch-race-start-marker.json`.
The snapshot covers 93.814 seconds of race data, contains 79,291 race-only
events and zero repeated-image completions. Its cumulative health counter is
one dropped event, unchanged from the startup boundary.

Command frame 16694 takes 102.637 ms and precedes a 100.110 ms distinct-image
gap. Its exclusive wall-time breakdown is:

| Category | Time |
| --- | ---: |
| Primitive processing | 29.776 ms |
| Vertex-buffer preparation | 23.854 ms |
| Empty command ring | 16.659 ms |
| Resolves/readbacks | 8.299 ms |
| Textures | 7.675 ms |
| Residual dispatch | 7.320 ms |
| Swap work and submission | 2.822 ms |
| Bindings | 1.783 ms |
| Constants | 1.262 ms |
| Render targets | 0.951 ms |
| Samplers | 0.816 ms |
| Pipelines | 0.620 ms |
| Shader translation | 0.488 ms |
| Register waits | 0.295 ms |
| Blocking fence checks | 0.012 ms |
| Other worker time | 0.004 ms |

The command thread consumes 49.819 ms of CPU time in that interval, exceeding
the 33.3 ms budget for 30 FPS. This is not solely presentation delay or a
sleeping worker. CPU time is measured for the whole frame, not each category.
Other long frames repeatedly show 21–32 ms of primitive processing and
11–27 ms of vertex-buffer preparation. Texture and resolve costs vary.

**Next investigation:** inspect primitive conversion and vertex-buffer
residency/upload paths, including shared memory preparation and repeated
work. Identify the actual repeated or unnecessary operation before changing
it. Do not infer a particular implementation fault from stage totals alone.
No optimization for these hotspots has been applied. Compare any targeted
candidate against the same race with graphics, saves, readbacks and caches
unchanged. The existing diagnostics can perturb timing; verify an eventual
performance improvement with tracing disabled as well.

Source checkpoint before this note: `52c494d` on `main`. Matching console
symbols are `build/console/dispatch-stage-symbols.elf`. The preceding
executable is retained on the console as
`eboot.bin.before-dispatch-stage-diagnostics-20261015-205323`.


### October 15 consolidated residency fix (resumed)

User resumed stutter work, authorized restarting Forza, and requested exactly
one final manual playtest. Do not ask for additional diagnostic races. The
prior pause is superseded. Existing saves/configuration/cache are preserved.

The completed prior trace is preserved as
`build/console/stutter-resume-trace.bin`; race cutoff remains 113.893 seconds.
It spans 394.749 seconds total, with 227 race-only display gaps over 45 ms,
120 complete long command-frame snapshots, and 13 cumulative dropped events.
The previous measured stage attribution is unchanged; it does not prove every
hitch has one cause.

Changes in the consolidated candidate:

- Populate the shared-memory request cache when all requested pages are already
  resident. Previously, after an unrelated invalidation advanced the epoch,
  these successful requests repeatedly rescanned their pages instead of becoming
  cache hits. Publish invalidation epochs after clearing validity, so new epochs
  cannot acknowledge old page-valid bits.
- Batch pending vertex streams through the existing RequestRanges merger before
  a draw. Overlapping/adjacent ranges share validation/upload/barrier work. Mark
  their residency only after the batch succeeds; preserve fetch and memory
  invalidation. Scratch range storage is reused rather than cleared per draw.
- Fast-path a single already-backed PS5 protection page without allocating
  transaction bookkeeping or repeatedly searching the backing map. Kernel
  failure leaves tracked permissions unchanged. Multi-page and allocation paths
  retain the original rollback handling.

Validation: native PS5 runtime/link/sign/import-layout checks pass. The real
SharedMemory request implementation passes host UBSan checks for residency,
invalidation, invalid input, overlapping ranges, upload failure/retry and
invalidation during upload. The PS5 memory host-kernel regression passes,
including the new single-page protection failure/retry case. An isolated
2-million-request resident-cache benchmark improved from 27.273 ms to 8.914 ms
(without sanitizer); this is not a whole-frame or console FPS result.

Local Linux runs are rendering smoke tests, not a controlled performance A/B:
the control overlapped compilation, and shader cache warmth differs. Do not
claim a quantified gameplay improvement from them. Final deployment uses tracing
disabled; retain the binary capture and marker backup. No graphics settings,
readbacks, shader caches, saves, game module or artwork changes are intended.

Evidence and deployment receipt: `build/stutter-fix/`. Source edits are mirrored
in `.deps/sdk-merge` and exported in `ps5/patches/rexglue-ps5.patch`; patch reverse
application against the working SDK is checked. No release or push requested.
Gameplay stutter acceptance remains the user's single final test.

Installed and raw-SHA verified consolidated executable:
`d6f4e1e9aceb5ca9ddc17e0deac15211e1c706453d818301852196ba74d7838f`.
Previous executable is preserved as
`eboot.bin.before-residency-fix-20261015-211331`, with its raw hash verified.
`trace.enable` was renamed to `trace.enable.disabled-20261015-211331`.
The 45-second console smoke stayed running and read the existing profile.
Its last startup window still contains guest gaps up to 83.4 ms, so this smoke
is not proof of eliminated gameplay stutter. Do not describe these local
optimizations as a verified complete cure. Leave the running build for the
user's one final gameplay test. Linux smoke screenshot confirms rendered car,
road and landscape: `build/stutter-fix/candidate-world.png`.

User completed the single final race test. Downloaded `build/console/residency-race-finished-runtime.log` (`forza_027.log`). Post-smoke summary in `build/stutter-fix/final-race-summary.json`: 54 windows, 153.0 guest gaps over 50 ms, maximum 134.6 ms. No exact race start/end marker; includes possible menus/results. Stutter remains; do not claim the consolidated changes solved it or establish a controlled A/B gain. Detailed per-operation tracing was disabled, so this run cannot identify the remaining operation. No restart or further user test requested.

Follow-up investigation and latest recording: `build/stutter-investigation/FINDINGS.md`. Actual shared-memory host reproduction exposes 1 MiB invalidation widening with PS5 pages versus 256 KiB with desktop pages. Local bounded candidate tested under UBSan; not applied or deployed. This is a candidate source of upload amplification, not proven race-hitch causality. Latest native recording is in Downloads as `ps5-native-1792081630-951ceba056.mp4`; original retained.


### October 15 bounded-invalidation deployment authorized

User explicitly requested deploying the bounded-invalidation candidate. Applied
its 256 KiB cap on optional widening (all actual writes and GPU-written neighbour
constraints remain covered). Native build/link/sign/import-layout checks pass;
the applied production source passes the PS5-page UBSan bounds/upload regression.
Reproducible test: `ps5/tools/test-shared-memory-bounds.sh`.

Installed executable SHA-256:
`7903b4e82b0ea6b2d72223c6fd9760d1dc3565ca38780caa9b34d3779d7026ee`.
Installed and previous executable both verified by raw SHA-256. Rollback:
`eboot.bin.before-bounded-invalidation-20261015-212621` (previous hash
`d6f4e1e9aceb5ca9ddc17e0deac15211e1c706453d818301852196ba74d7838f`).
Saves/settings/shader cache unchanged; detailed tracing remains disabled.
Receipt: `build/stutter-investigation/deployment.json`. Source is mirrored and
exported into the SDK patch; reverse-application check passed. No push/release.

An absent trace marker was incorrectly treated as present by FTP SIZE, after
the executable replacement. Recovery checked the complete FTX2 directory list,
confirmed marker absence, verified both binaries and resumed launch without
redeploying. Local deployment scripts now use explicit listing membership.
Gameplay improvement is unproven; do not claim eliminated stutter.

Bounded candidate launch smoke completed: still running after 45 seconds; runtime `forza_028.log` preserved as `build/console/bounded-invalidation-runtime.log`. Startup windows still include frame gaps; this launch check is not a race-performance result. Game left running.

Bounded candidate follow-up log: `build/console/bounded-latest-runtime.log` (`forza_028.log`), summary `build/stutter-investigation/latest-log-summary.json`. Post-smoke 105.21 seconds: average 29.04 swap requests/s, 77 gaps above 50 ms, maximum 184.0 ms. Stutters persist. Race/menu boundaries unknown; not a controlled A/B. No restart or deployment during log check.


## October 16 — PS5X360 memory optimizations adapted locally

User requested adapting both relevant ideas from BrinooTk/PS5X360 at
`ad628f65ed460416c45103f4867053c5086eab1d`. Implemented 4 KiB GPU upload/validity
units with unchanged 16 KiB PS5 memory protection, plus content-checked dynamic
vertex uploads after three actual host-page invalidations. Non-vertex requests,
range watches and GPU-written data permanently pin their whole host page to
normal tracking. Speculative invalidation widening does not increase heat.
Dynamic units remain invalid and are checked on each draw; both fetch-residency
shortcuts check dynamic eligibility and actual validity. Hashes are committed
only after successful upload and only if their invalidation generation still
matches. Failed uploads retry. Cache reset clears eligibility/hash generations;
pins remain conservative. Unlike upstream's periodic heat aging, this candidate
keeps dynamic pages eligible until an exact invalidation, reset or pin.

`shared_memory_dynamic_vertices=false` safely returns requested ranges to normal
tracking (validated). Default is true. Lightweight cumulative `Vertex memory:`
logs report upload/protection sizes, transitions, hashed/skipped/uploaded bytes.
GPU data and adjacent textures/indices cannot silently lose their protection.

Native PS5 compilation, ELF layout validation and signing passed. Candidate:
`build/ps5x360-adaptation/eboot.bin`, SHA-256
`d23455ededf6e81266aee24be76cc7522914fb20507cfca7825f25f8e97c5835`.
Linux Vulkan build passed. UBSan tests cover changed/unchanged real contents,
failed upload retries, batched/overlapping ranges, whole-host-page pinning,
GPU-written neighbours, registered watches, exact invalidation during upload,
cache reset and disabled-feature fallback. Bounds tests confirm a 16 KiB callback
invalidates four units and a following 4 KiB request uploads just 4096 bytes.
Existing residency/failure tests passed; repeated resident-request fast path
retained. Test scripts: `ps5/tools/test-{dynamic-vertex-memory,shared-memory,shared-memory-bounds}.sh`.

Source mirrored into `.deps/sdk-merge` and exported to
`ps5/patches/rexglue-ps5.patch`; reverse-application check passes. Prior files and
patch backed up under `build/ps5x360-adaptation/`. No commit, push, release or
console deployment in this step. Installed console candidate remains the prior
bounded-invalidation build. Console saves/config/cache untouched. This is a
prepared candidate, not evidence that PS5 stutters are fixed.

Automated Linux smoke completed (160 seconds), with menu, intro and actual
scripted driving screenshots inspected (HUD 123 mph at 144.50 seconds). Last
memory counters: 304 dynamic transitions, 822,439,936 bytes hashed and
799,432,704 redundant upload bytes skipped. This demonstrates the mechanism
executing, not a controlled performance improvement or PS5 gameplay proof.
Final renderer-reinitialization hash reset was subsequently built on both
platforms and regression-tested with UBSan; the gameplay smoke was not repeated
for that lifecycle-only adjustment. Final receipt: `build/ps5x360-adaptation/candidate.json`.

## October 16 — PS5X360 candidate deployed

User reported the console connected, making it available for deployment. Verified
FTP and FTX2 live; no other game active. Installed frozen candidate
`d23455ededf6e81266aee24be76cc7522914fb20507cfca7825f25f8e97c5835`.
Staged, installed and rollback executable raw SHA-256 checks passed. Previous
`7903b4e82b0ea6b2d72223c6fd9760d1dc3565ca38780caa9b34d3779d7026ee` is
retained on console as `eboot.bin.before-ps5x360-memory-20261016-101507`.
Saves/config/shader caches untouched; detailed trace marker absent.
Receipt: `build/ps5x360-adaptation/deployment.json`. Launch requested; startup
verification underway. Manual gameplay/performance acceptance pending.

Startup verification completed: Forza still running after 45 seconds. Fresh
`forza_029.log` confirms `upload-unit=4096 host-page=16384 dynamic=true`,
72 dynamic transitions and 34,570,240 redundant upload bytes skipped. Startup
had frame gaps (up to 116.6 ms); this does not establish race performance.
Left Forza running for the user's single final test. Logs saved under
`build/console/ps5x360-memory-*`; deployment receipt includes startup evidence.

User reported two perceived stutters. Read-only follow-up fetched forza_029.log
as `build/console/ps5x360-user-stutters-runtime.log`. Guest/display gaps both
reach 150.1 ms; recent worst five-second window has 131 swap requests (26.2/s).
Pipeline waits stay about 0.7–0.8 ms total per five-second window. Dynamic
upload avoidance is active, but stutters persist. Detailed summary is
`build/ps5x360-adaptation/user-stutters-summary.json`; remaining cause is not
isolated. No restart, configuration change or deployment during this check.

## October 16 — both PS5X360 memory optimizations withdrawn

User explicitly requested undoing the 4 KiB GPU upload units and dynamic vertex
content checks. Restored the exact previous executable, raw SHA-256 verified:
`7903b4e82b0ea6b2d72223c6fd9760d1dc3565ca38780caa9b34d3779d7026ee`.
This retains the earlier bounded-invalidation changes that preceded this trial.
Experimental binary retained as `eboot.bin.withdrawn-ps5x360-20261016-102128`.
Saves/config/cache untouched. Rollback receipt: `build/ps5x360-adaptation/rollback.json`.

Restored all three affected SDK source files from pre-adaptation backups in both
SDK trees, restored the prior exported patch and bounds-test expectations, and
archived the dynamic-only test/script. Reverse-application check passes. Restored
raw executable also replaces `build/native-title/eboot.bin`. Candidate receipt
marked withdrawn and its deployment script now refuses reuse. Forza relaunch
requested; no additional user test is required for this rollback.

Rollback startup passed: still running after 30 seconds; fresh forza_030.log lacks the withdrawn memory counters. Exact source/patch/binary backup checks passed, Linux runtime rebuilt and copied into its run directory. Forza left running.

## October 16 — three wait/scheduling candidates installed

User explicitly requested trying the three newer fixes from the Pinyon Shift
research. Added a 200 us delay only at the verified base-game render-idle
SleepEx call (LR 0x8259FBEC); a lazy, condition-variable timer queue with
interruptible deadlines; and a GPU-progress sequence/wakeup with a requested
1 ms maximum wait after 20 us of unchanged guest fence progress. Original guest
idle/fence functions always execute, retaining APC and timeout/kick behavior.
GPU signals are wake hints after command-processor packet effects, not fabricated
fence completions. The two withdrawn PS5X360 memory changes remain absent.

Production timer/wakeup sanitizer tests passed (earlier deadlines, cancellation,
self-disarm, 1600 concurrent publications, shutdown and wake races). Hook tests
passed for exact-call filtering, disabled controls, progress wake and original
results. Native runtime, private module and signed executable builds passed;
module hooks and original functions are present. Exported SDK patch reverse check
passed. These checks do not establish race performance.

Installed matched executable `b8c46b26297ee1f1c1dea245e4f04b2e36c835d366b84ae54f4ec5ce5fdb19ed`
and private module `faea58901d178f09a53d6c3b713496578a0cb124480363155547ce9158334ebe`.
Staged/live/backup raw SHA256 verification passed for both. Previous pair retained
on console with `.before-wait-fixes-20261016-103933` suffix and locally under
`build/wait-fixes/before-*`. Saves, configuration and shader caches preserved.
Receipts and checks: `build/wait-fixes/`. Startup verification underway; gameplay
acceptance pending. Never publish the private module or generated game code.

Startup check completed: running after 45 seconds, fresh forza_031.log confirms
render-idle delay and GPU fence wake (first wait signalled). Startup frame gaps
remain, including 166.8 ms; no claim of stutter elimination or improved race
performance. Existing missing-file warnings also occurred in the previous build.
Forza left running for the user’s final gameplay comparison.

User reports remaining stutters with all three wait fixes. Captured live forza_031
as build/console/wait-fixes-user-stutters-runtime.log; summary in
build/wait-fixes/user-stutters.json. Latest roughly 60 seconds include 166.7 ms
guest frame gaps, while pipeline waits total about 0.7–0.9 ms per five seconds.
The wait fixes have not eliminated stalls. No causal before/after conclusion;
no restart or changes made during this read-only follow-up.

## October 16 — work stopped at explicit user request

User requested documenting everything and stopping. Full handoff saved in
STUTTER-HANDOFF.md: installed matched pair, active/withdrawn changes, tests,
remaining stutters, uncertainty, evidence and rollback paths. No console action
performed for this handoff; Forza left running. No further work until resumed.

## October 16 — command-thread blocking isolated; sync-batch candidate prepared

User resumed ("check and do something"); the earlier stop request is superseded.
No console change has been made in this step: the deployment attempt aborted at
its first process query because the console was disconnected, before any FTP
transfer. Installed executable remains the wait-fixes build
`b8c46b26297ee1f1c1dea245e4f04b2e36c835d366b84ae54f4ec5ce5fdb19ed` with module
`faea58901d178f09a53d6c3b713496578a0cb124480363155547ce9158334ebe`.

Re-analysis of the saved dispatch-stage traces (`dispatch-race-live-trace.bin`,
`stutter-resume-trace.bin`, race-only sections, 2,734 and 8,318 command frames):

| Frame wall time | Frames | Command-thread CPU / non-waiting wall | Blocked (wall − waits − CPU) |
| --- | ---: | ---: | ---: |
| under 40 ms | 2,640 / 8,198 | 0.84–0.86 | 2–3 ms |
| 40–60 ms | 44 / 58 | 0.63 | 17 ms |
| 60–80 ms | 34 / 40 | 0.58 | 25 ms |
| 80 ms and over | 16 / 22 | 0.58 | 32 ms |

The blocked time is not general preemption: dispatch, bindings, constants,
samplers, targets and pipelines take 1.2–1.4x their normal-frame medians in long
frames, while primitives take 29x, vertex buffers 12x, resolves 10x and textures
4x. Blocked time correlates 0.98 with the sum of those four shared-memory stages
and 0.45 with the other CPU stages. Those four stages are the paths that take the
global critical region and change host page protection: RequestRanges uploads,
MakeRangeValid (one acquisition for validity bits plus one per physical heap for
protection, per upload chunk) and RangeWrittenByGpu. Guest write-watch faults hold
the same mutex through a kernel protection query and an mprotect. This is the
mechanism hypothesis; the per-frame counters below are what will confirm or
refute it from normal play, without a diagnostic race.

Candidate `build/sync-batch/eboot.bin`, SHA-256
`b7c084f35836fbe804661a3c458751118c13cca8d645f5b82a3e0c9005e3b435`
(module unchanged; no ABI or import change):

- `SharedMemory::MakeRangesValid`: all chunks of one upload pool page are marked
  valid and protected in a single acquisition, then copied. Pages are still
  protected before they are copied. Chunk flushes happen whenever a pool page is
  filled, because the next request closes and flushes that page (non-coherent
  memory). The upload pool gained a `page_size()` accessor for this.
- PS5 `QueryProtect` answers from the tracked arena view access (the state the
  kernel accepted) without a kernel call; pages of unbacked units report no
  access, as the kernel does. Removes one syscall per write-watch fault from
  under the lock. Non-arena addresses still query the kernel.
- Always-on five-second line `PS5 memory sync:` next to `PS5 frame timing`:
  contended global-lock waits and wait time on the shared-memory paths, upload
  ranges and megabytes, Protect calls, mprotect syscalls and time, protection
  queries and kernel fallbacks, write-watch faults and handler time. TSC cycles
  scaled by each window's measured rate.

Verification: PS5 runtime, title link, layout check and signing passed. Linux
build passed; 160 s headless smoke rendered the intro and scripted driving
(car, road, landscape, HUD at 147 s; `build/sync-batch/shot_0147.48.png`), with
PERF around 30 fps. Host tests: `test-memory.sh` (tracked queries cross-checked
against `/proc/self/maps`, unbacked units, repeated-access no-syscall, kernel
fallback counter), `test-shared-memory.sh` (batched validity exactness, gaps stay
invalid, single-page re-upload, degenerate ranges, contended-lock counter),
`test-shared-memory-bounds.sh`, `test-title-waits.sh` pass. `test-wait-primitives.sh`
aborted once with a harness `std::future_error` and passed on three reruns; it
does not compile the changed code. Source mirrored to `.deps/sdk-merge`, exported
to `ps5/patches/rexglue-ps5.patch` (86 files), reverse-application check passed.
Previous patch: `build/sync-batch/before-rexglue-ps5.patch`.

Deployment: `build/sync-batch/deploy.py` replaces only `eboot.bin` after checking
the live hash equals the wait-fixes build, keeps the previous file as
`eboot.bin.before-sync-batch-<stamp>` and verifies staged, live and rollback
hashes. Run it when the console is connected, then
`python3 tools/console.py boot sync-batch --seconds 60` and read the
`PS5 memory sync` lines. Nothing here is gameplay proof; no stutter claim.

### sync-batch deployed and started (console reconnected)

Installed `eboot.bin` SHA-256 `b7c084f35836fbe804661a3c458751118c13cca8d645f5b82a3e0c9005e3b435`;
staged, live and rollback hashes verified. Previous executable retained as
`eboot.bin.before-sync-batch-20261016-112009` (b8c46b26…). Module unchanged.
Forza was not running at deployment (no progress at risk). Launched; still
running after 60 s; `forza_032.log` saved as `build/console/sync-batch-runtime.log`,
startup window counters in `build/sync-batch/startup-counters.json`.

First five-second windows at the title/attract sequence (not gameplay):
lock_waits 2,300–8,100 per window with 32–233 ms of command-thread lock wait;
4,500–5,200 mprotect syscalls costing 210–245 ms (about 45 us each);
1,230–1,505 write-watch faults costing 85–122 ms (about 85 us each, including the
handler's unprotect); 85–115 MB uploaded in 2,300–3,100 ranges. Windows with
4–9 guest gaps over 50 ms had 143–233 ms of lock wait; the window with none had
32 ms. No kernel-side protection queries remain (query_syscalls=0).
This supports the blocking mechanism; gameplay windows are still needed and
come from the user's normal play. No stutter-fix claim.

Likely next reduction, not yet built: a lock-free upload-range scan in
RequestRanges (the valid-page fast path already reads the bits without the lock
for the same reason), and merging protection changes across already-watched
pages so a request costs one mprotect per heap instead of one per range.

### Play-session counters and lock-relief candidate

`forza_032.log` (`build/console/sync-batch-play-runtime.log`), 102 five-second
windows of the user's play with the sync-batch build (user reports stutter
continued, also when not saving):

| | Clean windows (77) | Windows with gaps (25) |
| --- | ---: | ---: |
| Command-thread lock wait | 2.1 ms | 127.9 ms |
| Contended acquisitions | 218 | 5,793 |
| Upload MB | 251.5 | 129.0 |
| Upload ranges | 1,400 | 2,803 |
| mprotect syscalls | 3,450 | 4,678 |
| Write-watch faults | 1,665 | 1,352 |

Correlation with gap count: lock wait 0.74, upload MB -0.48, faults -0.35.
The command thread's stalls are waits for the global critical region; upload
volume and faults do not distinguish bad windows. Guest heap operations
(BaseHeap Alloc/AllocRange/Decommit/Release/Protect and the PhysicalHeap
wrappers) hold that region on PS5 for host-page reconciliation, issuing ~45 us
mprotect calls inside it; a save overwrite also deletes the previous save
directory inside it (DeleteContent), which matches the reported save lag.

Candidate `build/lock-relief/eboot.bin`, SHA-256
`389783fd873c200b1a976640528c27d6b98a8def20839a04dae1635f370ca7c7` (module
unchanged): freed host pages stay read/write on all heaps (Windows-host
behaviour; zero page unaffected), RequestRanges picks upload pages from atomic
reads without the region, DeleteContent holds the region only for the
open-package check, and the memory log gains heap_syncs/heap_sync_ms. PS5 and
Linux builds pass; memory, shared-memory and bounds host tests pass; patch
exported (86 files) and reverse-checked. Linux smoke in progress; the heap
change is PS5-only (4 KiB guest vs 16 KiB host pages) and is not exercised on
Linux. Not deployed yet; `build/lock-relief/deploy.py` expects the installed
b7c084f3… build. No stutter-fix claim until the user plays with it.

### lock-relief installed; holder attribution candidate

lock-relief `389783fd…` installed 11:34 (user authorized closing Forza);
rollback `eboot.bin.before-lock-relief-20261016-113427` (b7c084f3…). Running
after 60 s. Play windows (`build/console/lock-relief-play-runtime.log`,
`forza_033.log`): gap windows still wait ~147 ms per 5 s on the lock; new
`heap_sync_ms` is only 0.4–2.2 ms per 5 s, so guest heap host-page
reconciliation is NOT a significant holder and the freed-page change is not a
fix (kept: harmless, matches Windows host behaviour). The user reports stutter
continues and save lag continues. The save window had 8.2 ms lock wait and an
82 ms gap: save lag is not lock contention. Saves run synchronously on guest
thread t101826 (the thread that registered modules at boot; 38 content reads).

Candidate `build/holder-trace/eboot.bin` adds, on top of lock-relief:
- `global_critical_region::Acquire/AcquireDirect` record the caller's
  `std::source_location`; the command thread's contended waits are attributed
  to the last recorded site and logged every 5 s as
  `PS5 lock holders behind waits: file:line=Nx/ms ...` (top 6). The direct
  `mutex().lock()` in `ObjectTable::LookupObject` records too.
- `Save content mount:` gains `close_delete_ms` and `create_open_ms`;
  `Content write:` and `Content close:` lines report save file writes/closes
  over 1 ms with the thread id.
Patch exported (87 files), reverse-check passed. Deploy with
`build/holder-trace/deploy.py` (expects installed 389783fd…) when Forza is closed.

### holder attribution results and spin-vfs build (installed)

holder-trace executable `9fc0467b…` initially refused to start ("Game module was
built for a different app version"): `include/rex/thread/mutex.h` is an ABI
header, so the source_location change altered the identity. User chose fix
forward over rollback: module rebuilt (`748d87b54510e0fa7a65fc512e61de93a715190fa603d3699f067c569c244bdc`,
backup `game/forza.module.before-holder-trace-20261016-114620`, previous
faea5890…) and installed; game then ran. Receipts: `build/holder-trace/`.

Attribution during the user's play (`build/console/holder-trace-play-runtime.log`,
17 gap windows vs 9 clean): waits per window behind handle lookups
(`object_table.cpp:280`) 66 ms vs 32; `cache.cpp:475` (texture
MakeUpToDateAndWatch, i.e. an unrecorded acquirer after it) 23 vs 9;
`entry.cpp:54` (VFS `Entry::GetChild`, 28–58 holds of ~0.85 ms each) 10 vs 0;
`xmemory.cpp:2120` 9 vs 5; `shared_memory.cpp:716` (invalidation) 9 vs 4.
Save mount steps measured 2.3 ms and 0.6 ms; no Content write/close over 1 ms
was logged yet, so the save lag is still unattributed.

spin-vfs `5f9db050ac0060c3e43f2bbf8c61340939222fd538f40654bbdecd32cbc8654f`
installed 12:0x after the user closed the game; rollback
`eboot.bin.before-spin-vfs-<stamp>` (9fc0467b…); module unchanged (no ABI header
touched). Changes: command-thread shared-memory acquisitions spin up to ~250 us
before blocking (`lock_spins`/`lock_sleeps` counters); the virtual filesystem
uses its own recursive mutex (`src/filesystem/tree_mutex.h`) instead of the
global critical region; dispatch loop records its site. Startup comparison over
the same nine windows: 11 frames over 50 ms vs 49 and 49 for the two previous
builds; lock wait 6–33 ms per window vs up to 242; all contended acquisitions
resolved by spinning, zero sleeps. Startup is not gameplay; the user's play
decides. Receipts: `build/spin-vfs/`; patch exported (95 files), reverse-check passed.

### spin-vfs play results and long-frame logging candidate

With spin-vfs installed (user playing): the one stutter window reported
(console 08:01:21, 6 frames over 50 ms, max 200 ms) had 32.7 ms lock wait and
zero lock sleeps, but 148 MB uploaded, 6,259 mprotect syscalls (282 ms) and
1,527 write faults (122 ms) in five seconds. The 13 windows after it had 150
swaps and no gap each, 5 ms lock wait. The remaining stall is therefore not the
lock; the render thread's own work in streaming bursts (protection syscalls at
~45 us each, copies, texture loads) is the next suspect, and needs per-frame
attribution again. Save mount steps are 1.9 ms and 0.6 ms; no content write or
close over 1 ms has been logged, so save lag is still unattributed.

Candidate `build/longframe/eboot.bin` (module unchanged): the app's frame
stage accounting is always on and every command frame over 45 ms is written to
the runtime log as `PS5 long frame:` with total wall, thread CPU, the memory
sync counter deltas for that frame (lock waits/sleeps, uploads, mprotects,
faults) and the exclusive stage categories over 0.2 ms. Cost: one ordered
cycle read per stage boundary on the command thread (the earlier
dispatch-stage diagnostic builds carried the same). Kernel `NtCreateFile`
logs opens over 2 ms as `Slow file open:` (save files, streaming). Patch
exported (95 files), reverse-check passed. Not yet deployed.

### longframe build installed; first per-frame attribution without lock noise

longframe `09112d633b4734a024c1823819fa89a22a7f980e47eac443c7944d41c8f1e922`
installed (Forza was not running); rollback `eboot.bin.before-longframe-<stamp>`
(spin-vfs 5f9db050…). `PS5 long frame:` lines work (bug: counter deltas wrap
when the five-second log resets the counters; fixed in the next build).

Play session (`build/console/longframe-play-runtime.log`): 67 long frames.
In the stutter burst (08:10:09–11) render-thread CPU per long frame is 24–51 ms
(dispatch 6–9, textures 3–9, resolves 2–4, primitives+vertex buffers 2–8,
protection syscalls 2–8 ms per frame) while 10–160 ms per frame is spent
waiting for the game to submit commands (empty ring). Lock wait is under 1 ms
per frame with zero sleeps. Faults 12–36 per frame. Interpretation: the game
waits on the previous frame's GPU fence while the command thread needs more
than a frame of CPU for busy scenes, so the two serialize; cutting render-thread
CPU (protection syscalls, texture/upload work, dispatch) is the lever.

Save: mount steps 2.3/0.6 ms; no file open over 2 ms, no content write/close
over 1 ms. Six long frames 3 s after the save were the game being late (render
CPU 7–12 ms, empty ring 31–66 ms). `kDeferredOverlappedDelayMillis` is 100 ms:
each deferred overlapped completion (two per save) sleeps 100 ms before
completing. savetrace build adds completion queue/run latency logging,
`Profile settings write` timing (`XamUserWriteProfileSettings` writes one host
file per title setting synchronously) and the counter fix. Not yet deployed.

### savetrace installed

`02f23bdd7c6fa928493ce7fd5051a7c8e96a82da09268b6f2e2dc48aa1b2777f` installed
(game was closed); rollback `eboot.bin.before-savetrace-<stamp>` (longframe
09112d63…); module unchanged. Running after 70 s; long-frame lines present
(startup frames: CPU ~26 ms, empty ring 13–21 ms, register wait 5–13 ms,
protection 2.6–3.2 ms, 15–21 faults). User gave standing permission to close
Forza for deployments. Awaiting a play/save session for the deferred-completion
and profile-settings timings.

### savetrace session: save path measured; coarse-watch candidate

savetrace play (`build/console/savetrace-play-runtime.log`): one save at
08:17:46. Two deferred overlapped completions: run 100.2 ms and 100.1 ms (the
fixed `kDeferredOverlappedDelayMillis = 100`), the second queued 67.4 ms behind
the first; mount steps 1.8/0.6 ms; no profile-settings write, slow open, or
content write/close logged. Fifteen long frames 3–5 s after the save had render
CPU 7–25 ms and empty ring 31–141 ms (game late, light scene). 144 long frames
total: median CPU 26.9 ms, empty ring 22.3, register wait 8.4, dispatch 6.9,
textures 4.5, resolves 2.2, protection 2.9 ms (up to 8.1 with 183 mprotects),
faults 16. Worst frames (150–167 ms) combine 39–48 ms render CPU with 98–124 ms
empty ring: a long render frame leaves the game waiting on the GPU fence, then
its catch-up makes the next frames late.

Candidate coarse-watch (eboot + forza.toml): new cvar `shared_memory_page_log2`
(0 = host page) set to 16 on PS5 so neighbouring small ranges share one 64 KiB
protection change and one validity page; new cvar `deferred_overlapped_delay_ms`
(default 100) set to 16. Nothing outside the shared-memory files depends on the
page size (command processor derives from it). Patch exported (97 files),
reverse-check passed. Live forza.toml matched git HEAD (449d374e…); rollback
copies in `build/coarse-watch/before-*`. Build in progress.

### Dynamic vertex content tracking reinstated (user instruction)

User asked to use the Xenia/PS5X360 approaches to move faster. PS5X360 v0.5.6
lists exactly: fast-path for valid GPU memory (present), mutexes that spin
before blocking (done today for the command thread) and content comparison for
repeatedly rewritten vertex data (the change withdrawn on October 16 before the
lock problem was found). Xenia uses the same write-watch design. Reinstated the
content-check part only, on 16 KiB pages (no 4 KiB upload units), merged onto
today's code: pages that take three write faults become dynamic, stay invalid
and unprotected, and each draw compares an XXH3 hash of their contents against
the last upload; texture/index/watch/GPU-written users pin pages to normal
tracking; `shared_memory_dynamic_vertices=false` now also stops pages from
becoming dynamic (kill switch); duplicate pages within one batch are hashed
once. Header changed (ABI): module rebuilt as a matched pair.

Tests: `test-dynamic-vertex-memory.sh` (adapted to 16 KiB pages, with a
changed-content upload check), `test-shared-memory.sh`, and
`test-shared-memory-bounds.sh` (dynamic tracking disabled for its widening
checks) pass under UBSan. Linux compile passed; Linux smoke in progress. Patch
exported, reverse-check passed. Rollback pair: eboot 1f763494… (coarse-watch),
module 748d87b5…. Deploy via `build/dynamic-vertices/deploy.py`.

### dynamic-vertices installed

`3e02fa081573cabc2d33d4745d8587ea009701b737e38f4c8696e4b14891d9bc` installed
(Forza closed under the user's standing permission); rollback
`eboot.bin.before-dynamic-vertices-<stamp>` (coarse-watch 1f763494…); module
unchanged (identical rebuild; the game module includes no graphics headers).
Linux smoke: 160 s, driving frame correct at 147 s. Startup: 204 dynamic
pages; about 635 MB per 5 s of unchanged dynamic data skipped (hashed instead),
57 MB per 5 s uploaded. Title-screen protection syscalls and faults did not
fall (streaming of new data dominates there); startup long frames 29 vs 35/50
for the two previous builds (same loading pattern). Hashing happens per draw,
not per frame; a per-frame hash memo is a possible follow-up if the
`vertex_buffers` stage grows. Gameplay verdict pending from the user's play.

### dynamic-vertices play result: no gain; switched off in config

User played (`build/console/dynamic-vertices-play-runtime.log`, 60 windows, 17
with gaps, 86 gaps) and reports stutters. Busy long frames (render CPU >= 20 ms)
compared with the three previous sessions: mprotects 79 vs 66–78, faults 19 vs
17–21, render CPU 29.9 vs 27.3 ms, vertex_buffers stage 3.7 vs 1.6 ms (per-draw
hashing), uploads 1.5 MB vs 1.4–1.7. 28.9 GB "skipped" were re-requests the
in-sync shortcut had already avoided before. Protection syscalls therefore come
from newly streamed data, not rewritten buffers; the content check does not
address them and costs about 2 ms per busy frame. PS5X360 0.5.7 likewise made
these speed-ups off by default. Set `shared_memory_dynamic_vertices = false`
in forza.toml (code retained); worst frames remain 35–53 ms render CPU plus
75–140 ms waiting for the game. Remaining levers: command dispatch (~7 ms),
textures (5–9 ms), protection syscalls for streamed data (3–7 ms), and the
game's fence-wait serialization; all need a render-thread CPU profile next.


---

## Archived handoff (formerly STUTTER-HANDOFF.md, superseded by the sections above)

# Forza stutter work — resumed 2026-10-16; sync-batch build installed and running

The user resumed ("check and do something") and then reported the console was
disconnected and asked to wait. The console then reconnected and the sync-batch
executable b7c084f35836fbe804661a3c458751118c13cca8d645f5b82a3e0c9005e3b435 was
installed (module unchanged); the "Current installed state" executable hash below
is superseded, rollback `eboot.bin.before-sync-batch-20261016-112009`.
See STATUS.md "command-thread blocking isolated" for the analysis and the
candidate `build/sync-batch/eboot.bin` (b7c084f35836fbe804661a3c458751118c13cca8d645f5b82a3e0c9005e3b435).
Next step when connected: `python3 build/sync-batch/deploy.py`, then
`python3 tools/console.py boot sync-batch --seconds 60`, then read the
`PS5 memory sync` log lines during the user's normal play.

## Current installed state

Title PPSA99690 on PS5 192.168.50.6, /mnt/ext1/etaHEN/games/PPSA99690.
Executable SHA256: b8c46b26297ee1f1c1dea245e4f04b2e36c835d366b84ae54f4ec5ce5fdb19ed
Private module SHA256: faea58901d178f09a53d6c3b713496578a0cb124480363155547ce9158334ebe
Both must be updated/restored as a matched pair because runtime imports/ABI changed.
Never publish the private module or generated game code.

Active changes:
- Render-idle SleepEx call at LR 0x8259FBEC gets a targeted 200 us sleep;
  original guest function still executes. Control: forza_render_idle_sleep_us (0 disables).
- Timer queue is lazy and uses condition-variable waits; earlier queued deadlines
  interrupt its sleep. Original cancellation and recurring-timer behavior retained.
- Guest fence predicate sub_829F04A8 waits for GPU progress after 20 us unchanged
  progress, with a requested 1 ms timeout; original predicate/timeout/kick logic
  always executes. Control: forza_block_on_gpu_fence. Wake signals are hints,
  not fabricated GPU completion. Scheduler delays may exceed requested timeout.

The withdrawn PS5X360 4 KiB upload-unit and dynamic vertex hashing changes remain
reverted. Earlier bounded-invalidation work remains present. Do not reintroduce
withdrawn changes without a new user instruction.

## Verification and result

Native runtime, private module and signed executable builds passed. SDK patch
reverse-application check passed. Timer/GPU wake sanitizer tests cover earlier
deadlines, cancellation, self-disarm, 1600 concurrent publications, shutdown,
pre-wait signals and racing/broadcast wakeups. Hook checks cover call filtering,
disabled controls, bounded requested wait and preservation of original results.
Staged/live/rollback raw SHA256 checks passed for both installed files.
Forza remained running after 45 seconds. Fresh forza_031.log confirmed render-idle
hook and GPU fence wakeups. No claim of gameplay success follows from build/startup.

User reports a few stutters remain. Latest captured 60.10 seconds: 28.97 swaps/s,
44 frame gaps over 50 ms, largest gap 166.7 ms; slowest five-second window 24.70
swaps/s. Pipeline waits total about 0.7–0.9 ms per five-second window, too small
to explain the large gaps. These counters do not isolate the remaining cause.
There is no controlled equivalent-scene before/after comparison establishing
benefit or regression. The three changes are not accepted as a stutter fix.
Missing-file warnings (including AMB_Red_Rock.fsb) also occurred before this build;
no causal relationship to stalls has been established.

User is tired of repeated tests and requested only one final manual race.
Do not ask for another repetitive diagnostic race by default. Research and local
checks should precede any future proposed console changes when work resumes.

## Evidence and rollback

- STATUS.md contains chronological work/results and earlier candidate history.
- build/wait-fixes/candidate.json, deployment.json, deployment.log, startup.log.
- build/wait-fixes/user-stutters.json and tests/build logs in that directory.
- build/console/wait-fixes-user-stutters-runtime.log (forza_031.log).
- build/console/wait-fixes-before-runtime.log, wait-fixes-runtime.log and klog.
- build/wait-fixes/before-eboot.bin and before-forza.module: previous matched pair.
- build/wait-fixes/before/: source/ABI/patch backups, backed-up-files.json.
- build/ps5x360-adaptation/rollback.json: earlier user-requested memory rollback.
- ../updates-20261016/internet-stutter-research/: pinned upstream research/findings.

Previous executable SHA256: 7903b4e82b0ea6b2d72223c6fd9760d1dc3565ca38780caa9b34d3779d7026ee
Previous module SHA256: 7ce1604a0df368856ab3b87f7d468cb332ee9c2047d1b5b236f94ede34c47d17
Console rollback paths:
- /mnt/ext1/etaHEN/games/PPSA99690/eboot.bin.before-wait-fixes-20261016-103933
- /mnt/ext1/etaHEN/games/PPSA99690/game/forza.module.before-wait-fixes-20261016-103933

Restore only after closing Forza and verifying the matched pair, if later requested.
Saves, config, game assets and shader caches were preserved throughout deployment.
Current work is local/uncommitted; preserve unrelated pre-existing changes.
Implementation: ps5/module/title_waits.cpp, ps5/tools/build-game-module.py,
ps5/module ABI/import lists, SDK timer_queue.cpp, gpu_write_signal.{h,cpp},
command_processor.cpp and system/CMakeLists.txt. SDK changes mirrored into
.deps/sdk-merge and exported in ps5/patches/rexglue-ps5.patch. Focused tests:
ps5/tests/{wait_primitives,title_waits}.cpp and matching ps5/tools/test-*.sh.

Stopped. No additional work is pending authorization within this active turn.

## October 16 — v0.1.0 released; setup verified end to end; catalog record staged

- `main` squashed to one commit (b656062), tagged v0.1.0; stable release at
  https://github.com/mshivam019/fh-ps5/releases/tag/v0.1.0 with PPSA99690.zip
  (e39554fc…), setup ZIP, dependency sources tar + manifest, SHA256SUMS.
  Console runs the release executable bf7a02bc… and config.
- Released setup ZIP run in a clean folder on disk (`../setup-e2e-test`) on the
  user's disc: 9 min 47 s, 2,401 files, 6.9 GB; its forza.module
  (714ce59c…, 120,079,096 bytes) differs byte-wise from the development build
  (748d87b5…) but the release app accepted it and ran (forza_043.log). It is now
  installed; previous module backed up as `forza.module.before-setup-e2e-20261016-130818`.
  A first attempt in the 7.5 GB /tmp RAM disk failed with ENOSPC during disc
  extraction: setup needs about 20 GB on disk (documented in SETUP.md).
- Catalog: record apps/PPSA99690.json committed on fork branch
  `add-forza-horizon` (a9adbb1, from upstream main), `catalog check` passes;
  `verify` fails only because the repository is private. **User instruction:
  do not make fh-ps5 public until they say so; no PR until then.**
