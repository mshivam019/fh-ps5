#!/usr/bin/env bash
# Run the Linux build of Forza Horizon headless (nested KWin, audio muted) with a
# scripted controller, screenshots every SHOT_EVERY seconds and per-thread CPU
# use over the last minute.
# Usage: tools/linux-run.sh TAG SECONDS [extra runtime args]
# Env: FORZA_AUTOPRESS (default: boot videos, title, main menu, Single Player,
#      skip the festival video, then hold the throttle), SHOT_EVERY (15),
#      BUILD (build/linux), SDK (.deps/rexglue-sdk), PRELOAD (library for the game only).
set -u
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
tag=$1; seconds=$2; shift 2
build=$root/${BUILD:-build/linux}; sdk=$root/${SDK:-.deps/rexglue-sdk}
out="$root/build/runs/$tag"; mkdir -p "$out" "$root/build/run-user"
throttle=$(for t in $(seq 90 6 $((seconds - 5))); do printf ',%s:RT:5.5' "$t"; done)
export FORZA_AUTOPRESS=${FORZA_AUTOPRESS:-8:START,10:START,12:START,14:START,30:START,40:A,46:A,50:A,54:A,58:A,62:A,66:A$throttle}
inner="$out/inner.sh"
{
  echo '#!/usr/bin/env bash'
  echo "cd '$build'"
  echo "export LD_LIBRARY_PATH='$build:$sdk/out/linux-amd64' SDL_VIDEODRIVER=wayland"
  [[ -n ${PRELOAD:-} ]] && echo "export LD_PRELOAD='$PRELOAD'"
  echo "export RECOMP_PERF=1 RECOMP_PERF_SCENE=boot FORZA_FRAMEDUMP_DIR='$out' FORZA_SCREENSHOT_TRIGGER='$out/shot.trigger'"
  printf 'exec ./forza --game_data_root=%q --user_data_root=%q --log_file=%q --audio_mute=true' \
    "$root/private/game" "$root/build/run-user" "$out/forza.log"
  printf ' %q' "$@"
  echo
} > "$inner"
chmod +x "$inner"
timeout --kill-after=10 "$seconds" kwin_wayland --virtual --no-lockscreen \
  --socket "forza-$tag" --width 1280 --height 720 -- "$inner" > "$out/stdout.log" 2>&1 &
runner=$!
cpu() {
  local pid; pid=$(pgrep -n -f "$build/forza" || pgrep -n -x forza) || return
  for t in /proc/$pid/task/*; do
    echo "$(basename "$t") $(tr ' ' _ < "$t/comm") $(sed 's/.*) //' "$t/stat" | awk '{print $12+$13}')"
  done
}
for ((s = ${SHOT_EVERY:-15}; s < seconds - 2; s += ${SHOT_EVERY:-15})); do
  sleep "${SHOT_EVERY:-15}"; touch "$out/shot.trigger"
  if (( s >= seconds - 62 && s < seconds - 62 + ${SHOT_EVERY:-15} )); then cpu > "$out/cpu-before"; cpu_at=$SECONDS; fi
done
cpu > "$out/cpu-after"; elapsed=$((SECONDS - ${cpu_at:-0}))
wait "$runner"
find /dev/shm -maxdepth 1 -name 'xenia_memory_*' -user "$(id -u)" -mmin -30 -delete
if [[ -s $out/cpu-before && -s $out/cpu-after ]]; then
  join <(sort "$out/cpu-before") <(sort "$out/cpu-after") |
    awk -v s="$elapsed" '{printf "%s %.0f%%\n", $2, ($5-$3)/s}' | sort -k2 -n -r | head -6
fi
grep "PERF:" "$out/forza.log" | tail -6 | cut -c40-
