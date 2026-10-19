# Releasing

Keep the title ID **PPSA99690** for every release. Raise `contentVersion` in
`ps5/sce_sys/param.json` for each new release and commit it before tagging. Tag
releases `vMAJOR.MINOR.PATCH`.

A release has four assets, all built from the tagged commit:

| Asset | Built with |
| --- | --- |
| `PPSA99690.zip` | `python3 ps5/tools/package-title.py --output release/PPSA99690.zip` |
| `Forza-PS5-vX.Y.Z-setup-linux.zip` | `python3 ps5/tools/package-setup.py --output release/Forza-PS5-vX.Y.Z-setup-linux.zip` |
| `Forza-PS5-vX.Y.Z-dependency-sources.tar.gz` (+ `.manifest.json`) | `python3 ps5/tools/package-sources.py --output release/Forza-PS5-vX.Y.Z-dependency-sources.tar.gz` |
| `SHA256SUMS` | `(cd release && sha256sum -- * > SHA256SUMS)` |

Before publishing:

1. Build the app (`ps5/tools/build-title.py`), install it on a console with a
   game folder prepared by the release's own setup ZIP (extracted to a clean
   directory), and play free roam, a race, saving and loading.
2. The app ZIP contains `PPSA99690/` with `eboot.bin`, `sce_sys/param.json`,
   metadata, the default `forza.toml` and license texts only: no `game/`,
   no `user/`, no generated code. `package-title.py` checks this.

Never replace an asset of a published release; publish a new version instead.
Never publish `forza.module`, generated code or disc contents.

## Homebrew catalog

Follow the [catalog submission guide](https://github.com/blackbearreloaded/ps5-homebrew-catalog/blob/main/docs/submitting.md).
The repository must be public with its MIT license detected. The record, in a
fork of the catalog as `apps/PPSA99690.json`:

```json
{
  "titleid": "PPSA99690",
  "name": "Forza Horizon for PS5",
  "kind": "game",
  "description": "Native PS5 port of Forza Horizon. Requires game files prepared from your own Xbox 360 disc.",
  "license": "MIT",
  "author": "mshivam019",
  "version": "vX.Y.Z",
  "source_repo": "https://github.com/mshivam019/fh-ps5",
  "artifact_url": "https://github.com/mshivam019/fh-ps5/releases/download/vX.Y.Z/PPSA99690.zip",
  "sha256": "<sha256 of PPSA99690.zip>",
  "icon_url": "https://raw.githubusercontent.com/mshivam019/fh-ps5/vX.Y.Z/ps5/sce_sys/icon0.png"
}
```

Check it with `python3 -m catalog check` and `python3 -m catalog verify PPSA99690`
in the catalog checkout, then open the pull request from the repository owner's
account, changing only that record.

## Desktop-only setup updates

When the runtime, generated-code inputs and module ABI are unchanged, publish
a new versioned setup ZIP and its own `.sha256` as additive assets on the matching
app release. Record the setup source commit in the release notes. Never replace
existing assets or change the app tag/contentVersion for a desktop-only update.
Run the helper tests, verify the source ZIP inventory and read back published
assets. State which real game inputs were checked. Keep this repository and
its releases private; publishing a catalog entry requires a separate decision.
