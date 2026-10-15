# Forza Horizon for PS5: plan

Native PS5 port of **Forza Horizon** (Xbox 360, 2012, title `4D5309C9`), based on
[Forza Horizon 1: ReImagined](https://github.com/Dyotson/RecompiledForzaHorizon)
(Dyotson, MIT; `main` at `c97e172` on October 15, 2026), which statically
recompiles the game with the ReXGlue SDK, the same toolchain as the Naruto PS5
port. Proposed title ID: **PPSA99690** (free in the homebrew catalog and on the
console as of October 15, 2026).

Why Forza Horizon 1 and not 2: the only Forza Horizon 2 PC port ("Azimuth" by
Almost There Games on itch.io, October 2026) is a closed-source launcher built on
the **Xbox One** version (x86-64 with Xbox system calls shimmed to Windows and
DirectX 12). There is no source, and the ReXGlue toolchain this port reuses
does not apply to it. No Forza Horizon 2 Xbox 360 recompilation exists.

## Starting point

Upstream (PC, Linux and Windows):

- Plays the whole single-player game: free roam, festival events, races, audio,
  saves. Title Update 4 (Horizon Rally, 1000 Club) and all 23 DLC packages are
  built from the player's own files.
- Pins ReXGlue **0.10.0.24 nightly `bd833a2`** plus 34 SDK patches
  (`patches/rexglue-sdk/`, mostly shared with RecompiledHaloReach). Two of them
  overlap work already done for Naruto on PS5: threaded Vulkan submission (0038)
  and `clear_memory_page_state` off by default (0039).
- Game hooks in `forza-recomp/src/hooks` (frame time, frame rate up to 144),
  online play through a self-hostable Forza Live server.
- Measured on a Ryzen 7 7800X3D with an RX 9060 XT: 60 FPS holds in free roam and
  in a race with 7 AI cars; above 60, races are bound by the GPU emulation.
- Input: Games on Demand package or **disc 1 ISO** of the base game, about 7.6 GB
  extracted; optional TU4 package (update `4000D145`).

From the Naruto PS5 port (`../Naruto-Rise-PS5`), reusable:

- PS5 ReXGlue runtime patch (`ps5/patches/rexglue-ps5.patch`, against SDK
  `c94f5eb`): sandbox paths, fibers, clocks, guest memory on direct memory with
  16 KB-page write watches, native display, asynchronous submission, frame
  pacing, the texture-load and resolve-readback hooks, command-processor
  optimizations.
- App/module split: runtime-only `eboot.bin`, `ps5/module/` loader with the
  import contract and ABI identity, `setup.py` (ISO extraction, hash checks,
  codegen, pinned llvm-mingw and payload SDK headers), release packaging and the
  catalog record template.
- PlayStation prompt replacement by texture hash (`button_prompts.cpp`) and the
  texture-dump workflow for finding prompts.
- Linux profiling harness (`tools/host-harness`) and console tooling
  (`evidence/deploy-*.py`, `boot-with-logs.py`, `fetch-runtime.py`).

## Phases

### 0. Inputs (needs you)

- Your Forza Horizon base game, disc 1 ISO or Games on Demand package, into
  `private/`. Title Update 4 if you want Horizon Rally and the 1000 Club.
- Record SHA-256 of `default.xex` and the TU, check the title ID and version.

### 1. Linux baseline

- Clone upstream at a pinned commit into `upstream/`; build its SDK overlay
  (`tools/build_rexglue_sdk.sh`) and the game (`tools/build.sh`) as documented.
- Run it headless with a scripted pad (adapt the Naruto harness or upstream's
  `tools/perf_scenario.sh`): menu, opening drive, free roam, a race.
- Measure at the original **30 FPS**: GPU command-thread CPU time per frame and
  draws per frame in free roam and in a race. These numbers decide whether the
  PS5's slower Zen 2 cores can hold 30.

### 2. SDK port

- Rebase the Naruto PS5 runtime patch from `c94f5eb` onto `bd833a2` plus the
  Forza patch set. Keep upstream's versions where both did the same thing
  (threaded submission, page-state default); carry the PS5-only parts.
- Export as `ps5/patches/rexglue-ps5.patch` on top of the upstream patches; keep
  `tools/export-ps5-patch.py` and the fresh-checkout apply check.

### 3. PS5 app and module

- App: copy the Naruto `ps5/` structure (startup, SDL input bridge, display,
  module loader, prompts, save-picture readback if Forza needs it, config).
- Module: generated `default.xex` code plus upstream's game hooks
  (`forza-recomp/src/hooks`, kernel/platform shims that are not PC-specific);
  regenerate `host-imports.txt` and `abi-headers.txt`.
- Leave out PC-only parts: keyboard/mouse, launcher, Windows packaging.

### 4. First console boot

- Install as PPSA99690 with a dev-built module. Goals in order: boot to the
  title screen, main menu, opening drive, free roam, a race, save and reload.
- Lock to 30 FPS (the 360's rate) with frame pacing on; measure with the frame
  timing log and a console recording, as for Naruto.

### 5. Performance

- If below 30: profile on the harness and with the PS5 stage counters; apply the
  Naruto command-processor work and upstream's render-pass/barrier patch (0036);
  look at EDRAM resolve cost (4x MSAA in three tiles per frame).
- 60 FPS is out of scope unless 30 holds with clear headroom.

### 6. Console polish

- PlayStation prompts via texture dumps (menus, HUD, races).
- Rumble (likely heavier use than Naruto: engine and surface feedback).
- Saves, Title Update 4 and DLC through setup.
- Online free roam against the Forza Live server: later and optional; needs PS5
  networking checks and the server's protocol.

### 7. Release

- Setup for Linux/WSL first (ISO or Games on Demand input, optional TU4 and DLC),
  release ZIPs, dependency sources, catalog record.
- Consider a Windows/GUI setup: several replies to the "what next" tweet were
  about setup being too hard.

## Risks

- **Performance.** Naruto's village needs most of a PS5 core on the GPU
  command thread at 30 FPS. Forza's races draw more (MSAA, several cars,
  post-processing). Phase 1 measurements decide feasibility before the PS5 work.
- **SDK jump.** About ten months of SDK changes between `c94f5eb` and `bd833a2`;
  the PS5 patch will need real rework, not a clean rebase.
- **Moving upstream.** Upstream commits daily; pin a commit and update deliberately.
- **Legal.** As with Naruto: no game data, generated code or game-derived
  executables in the repository or releases.

## Progress

- **October 15, 2026: phase 2 started before the game files arrived.**
  - `tools/prepare-sdk.sh` clones upstream at the pinned commit
    (`sources.json`), checks out SDK `bd833a2` with upstream's 34 patches into
    `.deps/rexglue-sdk`, and applies `ps5/patches/rexglue-ps5.patch`.
  - The SDK drift is small: `bd833a2` is 24 commits after Naruto's `c94f5eb`.
    The Naruto PS5 patch was carried over by cherry-picking it (as a commit on
    top of `c94f5eb` plus Naruto's upstream patches) onto `bd833a2` plus the
    Forza patches. Five files conflicted, all in the GPU command processor and
    shared memory: upstream's threaded submission and fetch-constant handling
    replace Naruto's asynchronous submission and fetch-register helper, which
    were removed; Naruto's request cache and page verifier are kept.
  - The merged SDK compiles for Linux (`rexruntime`, `rexgpu-xenos`). The PS5
    build is not set up yet.

## Immediate next steps

1. You: put the Forza Horizon base-game ISO (and TU4 if available) in `private/`.
2. Pin upstream, build the Linux version, run phase 1 measurements.
3. Decide go/no-go on 30 FPS feasibility from those numbers.
