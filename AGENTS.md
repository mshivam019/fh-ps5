# Forza Horizon PS5 workspace

`main` is the release line. Development notes (`STATUS.md`, `PLAN.md`) and the
render-thread optimization work live on branch `optimize-render-thread`; read
them there before performance work. Shared console instructions are in
`../AGENTS.md`. Release steps: `ps5/RELEASING.md`.

The port runs on PPSA99690 (firmware 9.00): free roam and races at 30 FPS with
stutter in busy scenes. The app is runtime-only and loads `game/forza.module`,
built by `ps5/tools/setup.py` from the player's disc. Saves, caches and logs
live under the title on the console; preserve them.

Keep source modifications, build outputs and receipts isolated in this folder
(`build/` is ignored and holds per-candidate receipts). Do not publish game
dumps, derived game code, `forza.module`, executables with embedded game code,
credentials or private artifacts. Repository https://github.com/mshivam019/fh-ps5
is private. Verify console availability and the running title before
deployment; close the target before replacing live files and keep an
`eboot.bin.before-*` backup; raw SHA-256 verify staged, live and backup files.

The app and module are a matched pair: changing any header listed in
`ps5/module/abi-headers.txt` (including `include/rex/thread/mutex.h`) requires
rebuilding and redeploying `forza.module`. Graphics headers are not in the list.

SDK edits live in the ignored `.deps/rexglue-sdk` tree, mirrored into
`.deps/sdk-merge` (branch `forza-ps5`); export with
`git -C .deps/sdk-merge diff forza-base > ps5/patches/rexglue-ps5.patch` and
check with `git -C .deps/rexglue-sdk apply --check -R ps5/patches/rexglue-ps5.patch`
before committing.
