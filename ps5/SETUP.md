# Installing Forza Horizon on PS5

The app (`PPSA99690.zip`) contains no game. You prepare the game on a computer
from your own disc image with the setup package from the same release, then
copy the result into the installed app.

**Status:** setup has been adapted for Forza but has not yet completed an
end-to-end test from a clean machine. Report problems with `build/setup/setup.log`.

## Requirements

- A homebrew-capable PS5 with native folder-title support (tested on firmware 9.00).
- Your own **Forza Horizon** Xbox 360 **disc 1** image (`.iso`), USA release
  (title 4D5309C9, base version). Setup checks the executables' hashes and
  refuses other versions. Title Update 4 and DLC are not supported yet.
- Linux x86-64 with Python 3.11+, Git, CMake, Ninja and Clang, plus about 20 GB
  of free space. On Windows, use WSL2 with Ubuntu.

On Ubuntu 24.04 (also in WSL2):

```sh
sudo apt install python3 git cmake ninja-build clang
```

## Install

1. Install `PPSA99690.zip` with your native-title launcher or PS5 Upload.
2. On your computer, extract `Forza-PS5-<version>-setup-linux.zip` and run, from
   its folder:

   ```sh
   sh setup.sh --iso "/path/to/Forza Horizon (USA).iso"
   ```

   Setup downloads pinned public sources and tools (the PC port, the ReXGlue
   SDK, a portable LLVM, the PS5 payload SDK headers), extracts your disc,
   generates the game code and compiles it. It takes a while and prints the
   output folder (default `../Forza-PS5-prepared/PPSA99690`; choose another
   with `--output`). `--game DIR` uses already extracted disc files.
3. Close the app on the PS5. Copy the output's `game` folder into
   `/mnt/ext1/etaHEN/games/PPSA99690/`, next to `eboot.bin`.
4. Launch **Forza Horizon** from the home screen.

The installed layout:

```text
PPSA99690/
  eboot.bin
  forza.toml          settings (replaced by app updates)
  sce_sys/  sce_module/  licenses/
  game/               from setup: your disc files and forza.module
  user/               created by the app: saves, shader caches and logs
  logs/               runtime logs (forza_NNN.log)
```

The `game` folder contains your game data and code generated from it. Do not
share it. Keep the app and the game folder from the same release: after an app
update, run the matching setup again and replace `game/forza.module` (the app
refuses a module built for a different version). Keep `user/` when updating.

If the app closes right after launch, `game/forza.module` is missing or from a
different release; the kernel log says which.

## Controls

| DualSense | Xbox 360 |
| --- | --- |
| Cross / Circle / Square / Triangle | A / B / X / Y |
| L1 / R1, L2 / R2 | LB / RB, LT / RT |
| Options | Start |
| Touchpad press | Back |
| L3 / R3, D-pad, sticks | as on the 360 |

Every connected DualSense drives player 1.

## Settings

`forza.toml` in the app folder. Notable keys:

- `readback_resolve = "some"`: car thumbnails for the garage.
- `present_frame_pacing = true`: frames timed to the display refresh.
- `deferred_overlapped_delay_ms = 16`: emulated latency of asynchronous
  operations such as save mounts (was 100).
- `shared_memory_dynamic_vertices = false`: content comparison for rewritten
  vertex data; measured without benefit on this game, kept available.
- `ps5_long_frame_log = false`: per-frame diagnostics in the runtime log.
