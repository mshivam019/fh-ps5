# Installing Forza Horizon

Use `PPSA99690.zip` and the newer `Forza-PS5-v0.1.0-setup-v1.3.0.zip`
from the private v0.1.0 release. The setup update changes desktop preparation only;
the app and its module interface are unchanged. This repository remains private.

You need your own **USA Xbox 360 disc 1, base version (4D5309C9); TU4 and DLC are unsupported**. The app ZIP contains no game.
No M.2 drive, PS5 Upload, or wired network is required by these scripts.
Your PS5 still needs a working native folder-title loader and title registration.

## Start with an ISO

Extract the setup ZIP and allow at least 20 GB of free space. Initial setup
needs internet access to download pinned sources and tools.

**Windows x64:** install [64-bit Python 3.11+](https://www.python.org/downloads/windows/)
and [Git for Windows](https://git-scm.com/downloads/win), then double-click
`Setup-Windows.cmd`. Choose your ISO and output location. No Linux, WSL2,
Visual Studio, CMake or manually installed compiler is needed. Python downloads
and verifies the native Windows code generator and compiler automatically.
The tools also need Microsoft’s [Visual C++ Redistributable x64](https://aka.ms/vc14/vc_redist.x64.exe),
which many PCs already have. Setup checks for it and gives the download link if missing.
This is the small runtime package, not Visual Studio.
Use a short setup path such as `C:\PS5-Setup` to avoid Windows path-length limits.

**Linux x86-64:** install Python 3.11+, Git, CMake, Ninja and Clang (Ubuntu:
`sudo apt install python3 git cmake ninja-build clang`), then run `sh setup.sh`.
The existing Linux build remains supported. Native macOS ISO compilation is not
implemented; macOS can still reuse and validate an already prepared game folder.

A native file picker is used when available, with a terminal fallback. Setup
extracts the disc in Python, checks the supported executables, compiles the
private game module and verifies the finished copy. It never contacts your console.
Python handles setup, but native compilation is still necessary; a pure Python
replacement for the C++ compiler is not used.

For a **complete app-and-game folder**, select the app ZIP on the command line:

```sh
sh setup.sh --iso "/path/to/game.iso" --app "/path/to/PPSA99690.zip" --output "/path/to/Ready/PPSA99690"
```

On Windows use `Setup-Windows.cmd` instead of `sh setup.sh` with the same arguments
(or `py -3 ps5/tools/setup.py`).

Without `--app`, the result contains only `game/`: merge it into the matching
installed app. `--game /path/to/extracted-disc` can replace `--iso`; it still builds
the module. Use the separate reuse route below if your module is already built.

## Already have a prepared game folder?

Run `Prepare-Game-Files-Windows.cmd` on Windows, or:

```sh
sh prepare-game-files.sh
```

Select the existing `game` folder (or its `PPSA99690` parent) and an output location.
This requires only Python 3.9+ on Windows, Linux or macOS, with no compiler or WSL.
It checks the disc executables and module, then makes a new checked copy. Repeated
runs choose a new numbered folder. It does not compile an ISO or repair a bad module.

To combine prepared game files and the app ZIP without recompiling:

```sh
python3 ps5/tools/prepare-files.py --title PPSA99690 --app "/path/to/PPSA99690.zip" --game "/path/to/game" --output "/path/to/Ready"
sh validate.sh --src "/path/to/Ready/PPSA99690" --require-complete
```

## Copy using any method

The ready folder has this layout (app files appear only when `--app` was used):

```text
Ready/
  MANIFEST-PPSA99690.json
  COPY-PPSA99690-TO-PS5.txt
  PPSA99690/
    eboot.bin
    sce_sys/
    game/                  disc contents and forza.module
```

Keep the manifest on your computer beside the title folder. Copy the title folder
or merge its contents into the installed title using your preferred file manager,
USB drive or FTP over Wi-Fi/Ethernet. **Close the game before updating its files.**
Do not nest `PPSA99690/PPSA99690` or `game/game`. Preserve existing `user/`, saves,
caches and `forza.toml`. Do not delete the old title to install an update.

Choose internal, M.2 or USB storage according to what your loader supports; USB
can also just be used to carry files to the console. Copying alone does not register
a new title. Use your existing native-title loader's registration procedure.

Optional FTP transfer is a separate step:

```sh
sh transfer.sh --src "/path/to/Ready/PPSA99690" --console YOUR_PS5_IP --remote-dir "/your/installed/path/PPSA99690" --closed
```

Windows users can run `Transfer-Files-Windows.cmd` with those same arguments.
FTP is optional, requires a running console FTP server, and checks transferred
sizes rather than remote SHA-256 hashes. The script preserves existing settings
and personal files; `--closed` asserts that you have closed the title. It does not
launch, close or register titles. Run `sh transfer.sh --help` for login/port options.

## Validate again whenever needed

Run `Validate-Files-Windows.cmd`, or:

```sh
sh validate.sh --src "/path/to/Ready/PPSA99690"
```

Add `--require-complete` only for app-and-game folders. Add `--report /path/to/new-report.json`
for a machine-readable result. Keep the original manifest: regenerating one from
a damaged copy would lose the reference. You can validate a retrieved console copy
against it using `--manifest /path/to/MANIFEST-PPSA99690.json`.

Checks cover file sizes, SHA-256, title layout, supported guest executables and the
module's expected exports/interface. They do not prove gameplay or validate every
game-content file against a retail reference. Save files and extra personal files
are reported separately. A mismatched module needs the setup matching the app.
Never share the generated module or your disc contents.

For compiler failures, retain `build/setup/setup.log`. For controls and runtime
settings see [SETUP.md](ps5/SETUP.md). Developer checks are documented in
[the helper tests](ps5/tools/tests/README.md).

## Validation of setup v1.3.0

The Windows code generator and compiler are exercised under Wine on Linux with
real private game inputs. This is not a physical Windows-machine test. File picker
execution on native Windows/macOS and a fresh console installation remain untested.

The existing full Forza app/game folder passed verification: 2,432 files, 7,383,740,126 bytes. After preserving the upstream dot-product overflow fix, Windows-generated code matches the patched Linux build across 86,832 guest function bodies.
The app executable and module ABI are unchanged. Generated files are private and are not included in downloads.

All 40 automated tests pass. Native Windows ISO extraction, code generation, compilation and folder packaging passed under Wine. The resulting full folder verified 2,432 files (7,383,735,350 bytes). This is build/packaging evidence, not a fresh console gameplay test.
