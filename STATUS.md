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
