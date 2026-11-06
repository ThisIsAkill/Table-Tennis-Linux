#!/usr/bin/env bash
# Copyright (c) 2026 Akhil Moola (ThisIsAkill). MIT License, see LICENSE.
# Unattended test run of the Linux build in a virtual display.
#
#   scripts/autotest.sh NAME SCENARIO [extra engine flags...]
#
# Output goes to logs/runs/NAME/ (frames, inputs.log, engine log, summary.txt).
# SCENARIO is a file in scripts/scenarios/ (or a path). Lines:
#   menu <seconds> <key>   press <key> this long after the main menu is reached
#   every <period> <key>   press <key> every <period> seconds after the main menu
#   hold <seconds> <key>   press <key> this long after the main menu and keep it down to the end
#   stop <seconds>         end the run this long after the main menu is reached
#   timeout <seconds>      hard limit for the whole run (default 300)
# Until the main menu appears, Return is pressed every 4 s to skip the intro.
#
# TT_DIST=DIR runs another build folder (default dist/).
# TT_PAD=1 sends presses to a virtual Xbox 360 pad (scripts/vpad.py, needs write access to
# /dev/uinput) instead of the keyboard: space=A, BackSpace=B, Return=Start, shift+arrows=D-pad.
# TT_DISPLAY=:N picks the virtual display (default :97). TT_PARALLEL=1 allows another game
# instance to run at the same time (scripts/lan-test.sh runs two under one GPU lock).
# The window is 960x540 by default (TT_WINDOW=WxH to change). Under Xvfb a
# 1080p window drops to ~15 FPS, and game time follows frames, so a large
# window makes every run about four times slower.
set -u
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
name="${1:?run name}"; scenario="${2:?scenario}"; shift 2
[[ -f "$scenario" ]] || scenario="$root/scripts/scenarios/$scenario"
[[ -f "$scenario" ]] || { echo "no scenario $scenario"; exit 2; }
out="$root/logs/runs/$name"
window="${TT_WINDOW:-960x540}"
dist="${TT_DIST:-$root/dist}"

# Machine-specific settings live in scripts/autotest.local (not tracked), e.g.
#   TT_GPU_LOCK=/path/to/gpu.lock   take this flock for the whole run (shared GPU)
#   TT_RO_BIND=/mnt/games           mount this path read-only for the game
[[ -f "$root/scripts/autotest.local" ]] && source "$root/scripts/autotest.local"
if [[ -z "${TT_LOCKED:-}" && -n "${TT_GPU_LOCK:-}" ]]; then
  [[ -e "$out" ]] && { echo "$out exists; pick another name"; exit 2; }
  exec env TT_LOCKED=1 flock -w 1800 "$TT_GPU_LOCK" "$0" "$name" "$scenario" "$@"
fi
[[ -z "${TT_LOCKED:-}" && -e "$out" ]] && { echo "$out exists; pick another name"; exit 2; }
mkdir -p "$out"
[[ -z "${TT_PARALLEL:-}" ]] && pgrep -x tabletennis >/dev/null && { echo "tabletennis already running"; exit 1; }

hard=300; stop=""; events=(); repeats=(); holds=(); held=()
while read -r kind a b; do
  case "$kind" in
    menu) events+=("$a $b") ;;
    every) repeats+=("$a $b") ;;
    hold) holds+=("$a $b") ;;
    stop) stop="$a" ;;
    timeout) hard="$a" ;;
  esac
done < <(grep -v '^\s*#' "$scenario" | grep -v '^\s*$')

display="${TT_DISPLAY:-:97}"
Xvfb "$display" -screen 0 "${window}x24" >/dev/null 2>&1 &
xpid=$!
trap 'kill $xpid 2>/dev/null' EXIT
sleep 2
export DISPLAY="$display"
rm -f "$dist"/logs/*.log 2>/dev/null
# The engine backs guest memory with a 4.5 GB file in /dev/shm and leaves it behind when it
# is killed, so remember what exists now and delete whatever this run adds.
shm_before=$(ls /dev/shm/xenia_memory_* 2>/dev/null)
start=$(date +%s)
# TT_RENDERDOC=1 launches under renderdoccmd; press F12 in the scenario to capture a frame
# (captures land in logs/runs/NAME/capture_frameN.rdc).
wrap=()
[[ -n "${TT_RENDERDOC:-}" ]] && wrap=(renderdoccmd capture --opt-hook-children -c "$out/capture" -w)
ro=()
[[ -n "${TT_RO_BIND:-}" ]] && ro=(--ro-bind "$TT_RO_BIND" "$TT_RO_BIND")
timeout "$hard" bwrap --bind / / --dev-bind /dev /dev "${ro[@]}" --die-with-parent \
  "${wrap[@]}" "$dist/tt-recomp.sh" -- "--mnk_mode=$([[ -n "${TT_PAD:-}" ]] && echo false || echo true)" \
  "--window_width=${window%x*}" "--window_height=${window#*x}" "$@" > "$out/game.log" 2>&1 &
gpid=$!

pad_fifo=""
if [[ -n "${TT_PAD:-}" ]]; then
  pad_fifo="$out/vpad.fifo"; mkfifo "$pad_fifo"
  python3 -I "$root/scripts/vpad.py" "$pad_fifo" > "$out/vpad.log" 2>&1 &
  padpid=$!
  exec 7<>"$pad_fifo"
  sleep 1
fi
press() {
  if [[ -n "$pad_fifo" ]]; then
    local b
    case "$1" in
      space) b=A ;; BackSpace) b=B ;; Return) b=START ;; Escape|Tab) b=BACK ;;
      shift+Up) b=UP ;; shift+Down) b=DOWN ;; shift+Left) b=LEFT ;; shift+Right) b=RIGHT ;;
      *) b="$1" ;;
    esac
    echo "$b" >&7
    echo "[$(( $(date +%s) - start )) s] pad $b" >> "$out/inputs.log"
    return
  fi
  local wid
  wid=$(xdotool search --onlyvisible --name '.' 2>/dev/null | head -1)
  [[ -n "$wid" ]] && xdotool windowfocus --sync "$wid" 2>/dev/null
  xdotool keydown --delay 0 $1; sleep 0.15; xdotool keyup $1
  echo "[$(( $(date +%s) - start )) s] key $1" >> "$out/inputs.log"
}

ei=0; last=""; kept=0; menu=""; next_skip=0; declare -A rnext
while kill -0 $gpid 2>/dev/null; do
  now=$(( $(date +%s) - start ))
  if [[ -z "$menu" ]]; then
    if grep -qs "Main menu reached" "$dist"/logs/*.log; then
      menu=$now; echo "[$now s] main menu reached" >> "$out/inputs.log"
    elif (( now >= next_skip )) && xdotool search --onlyvisible --name '.' >/dev/null 2>&1; then
      press Return; next_skip=$((now + 4))
    fi
  else
    # Events that fall due in the same pass are pressed 0.7 s apart: the menus drop presses
    # that arrive back to back.
    pressed=0
    while (( ei < ${#events[@]} )) && (( menu + ${events[ei]%% *} <= now )); do
      (( pressed )) && sleep 0.7
      press "${events[ei]#* }"; ei=$((ei + 1)); pressed=1
    done
    for hi in "${!holds[@]}"; do
      (( menu + ${holds[hi]%% *} <= now )) || continue
      key="${holds[hi]#* }"; unset 'holds[hi]'
      xdotool keydown --delay 0 "$key"; held+=("$key")
      echo "[$now s] hold $key" >> "$out/inputs.log"
    done
    for ri in "${!repeats[@]}"; do
      period="${repeats[ri]%% *}"
      (( (now - menu) / period > ${rnext[ri]:-0} )) || continue
      rnext[ri]=$(( (now - menu) / period )); press "${repeats[ri]#* }"
    done
    if [[ -n "$stop" ]] && (( now >= menu + stop )); then
      import -window root "$out/final.png" 2>/dev/null
      kill $gpid 2>/dev/null; break
    fi
  fi
  f="$out/f_$(printf %04d $now).png"
  import -window root "$f" 2>/dev/null
  h=$(md5sum < "$f" 2>/dev/null | cut -c1-32)
  if [[ "$h" == "$last" ]]; then rm -f "$f"; else last="$h"; kept=$((kept + 1)); fi
  sleep 2
done
for key in "${held[@]}"; do xdotool keyup "$key" 2>/dev/null; done
wait $gpid 2>/dev/null; code=$?
if [[ -n "$pad_fifo" ]]; then echo QUIT >&7; exec 7>&-; wait "$padpid" 2>/dev/null; rm -f "$pad_fifo"; fi
sleep 1
for f in /dev/shm/xenia_memory_*; do
  [[ -e "$f" ]] || continue
  grep -qxF -- "$f" <<< "$shm_before" || command rm -f -- "$f"
done
command mv -f "$dist"/logs/*.log "$out/" 2>/dev/null && rmdir "$dist/logs" 2>/dev/null
log=$(ls "$out"/tabletennis_*.log 2>/dev/null | head -1)
{
  echo "run: $name  scenario: $(basename "$scenario")  flags: $*  window: $window  build: $dist"
  echo "exit: $code  elapsed: $(( $(date +%s) - start )) s  main menu at: ${menu:-never} s  frames kept: $kept"
  if [[ -n "$log" ]]; then
    fps=$(grep -o 'Current FPS: [0-9.]*' "$log" | awk '{s+=$3; n++} END {if (n) printf "%.1f", s/n}')
    echo "average FPS: ${fps:-n/a}"
    echo "warnings/errors (top):"
    grep -E '\[(warning|error)\]' "$log" | cut -c27- | sed -E 's/\[t[0-9]+\] //; s/[0-9]{2}:[0-9]{2}:[0-9.]+//' \
      | sort | uniq -c | sort -rn | head -8
  fi
} > "$out/summary.txt"
cat "$out/summary.txt"
