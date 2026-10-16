#!/usr/bin/env bash
# Fetch the pinned Forza Horizon recompilation into upstream/forza, check out the
# pinned ReXGlue SDK into .deps/rexglue-sdk with upstream's SDK patches
# (patches/rexglue-sdk), then apply this port's PS5 runtime patch on top.
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
forza_rev=$(python3 -c "import json;print(json.load(open('$root/sources.json'))['forza']['revision'])")
forza="$root/upstream/forza"
sdk="$root/.deps/rexglue-sdk"
if [[ ! -d $forza/.git ]]; then
  git clone https://github.com/Dyotson/RecompiledForzaHorizon "$forza"
fi
git -C "$forza" checkout --quiet --detach "$forza_rev"
patch="$root/ps5/patches/rexglue-ps5.patch"
if [[ -d $sdk/.git ]] && git -C "$sdk" apply --reverse --check "$patch" 2>/dev/null; then
  echo "SDK already prepared"
  exit 0
fi
if [[ -d $sdk/.git ]]; then
  git -C "$sdk" checkout --quiet -- .
  git -C "$sdk" clean -fdq
fi
REXSDK_SRC="$sdk" bash "$forza/tools/build_rexglue_sdk.sh" --source-only
git -C "$sdk" apply "$patch"
echo "SDK prepared: $(git -C "$sdk" rev-parse --short HEAD) + upstream patches + PS5 patch"
