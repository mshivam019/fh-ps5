#!/usr/bin/env sh
# Prepare your own Forza Horizon disc for the PS5 app (Linux or WSL2).
# Usage: sh setup.sh --iso "/path/to/Forza Horizon (USA).iso" [--output DIR/PPSA99690]
set -eu
exec python3 "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/ps5/tools/setup.py" "$@"
