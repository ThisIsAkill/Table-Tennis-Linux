#!/usr/bin/env bash
# Drives the installer through every screen against fake-backend.sh on a virtual display and
# captures each one. Needs Xvfb, xdotool and ImageMagick's import. Nothing real is touched:
# HOME is pointed at a scratch folder, the disc images are sparse files, and the fake backend
# only writes a stand-in launcher under that scratch HOME.
#
#   tools/installer/screenshots.sh OUT_DIR [BINARY]
set -euo pipefail
out="${1:?output directory}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bin="${2:-$here/bin/install-table-tennis}"
[[ -x "$bin" ]] || { echo "build the installer first ($bin missing)"; exit 1; }
mkdir -p "$out"
work="$out/work"
rm -rf "${work:?}"
mkdir -p "$work/home/Downloads" "$work/home/Games" "$work/sdcard"
truncate -s 7340032000 "$work/sdcard/TableTennis.iso"
truncate -s 7340032000 "$work/home/Downloads/Table Tennis (USA, Europe).iso"
truncate -s 4200000000 "$work/home/Downloads/some-other-game.iso"

export HOME="$work/home"
export XDG_STATE_HOME="$work/home/.local/state"
export TT_INSTALLER_FAKE_MOUNTS="$work/sdcard|primary|sd"
export SDL_VIDEODRIVER=x11 SDL_RENDER_DRIVER=software

start_xvfb() {  # display geometry
  if ! DISPLAY="$1" xdpyinfo >/dev/null 2>&1; then
    Xvfb "$1" -screen 0 "$2" -nolisten tcp >/dev/null 2>&1 &
    echo $! > "$work/xvfb-${1#:}.pid"
    for _ in $(seq 20); do DISPLAY="$1" xdpyinfo >/dev/null 2>&1 && break; sleep 0.25; done
  fi
}
shot() { DISPLAY="$display" import -window root "$out/$1.png"; echo "  $1.png"; }
click() { DISPLAY="$display" xdotool mousemove "$1" "$2" click 1; }
key() {
  local id; id="$(DISPLAY="$display" xdotool search --name 'Table Tennis Setup' | head -1)"
  DISPLAY="$display" xdotool windowfocus --sync "$id" key "$@"
}
app_pid=""
launch() {  # extra args...
  "$bin" --backend "$here/fake-backend.sh" "$@" >"$work/app-$display.log" 2>&1 &
  app_pid=$!
  sleep 2.5
}
stop() { [[ -n "$app_pid" ]] && kill "$app_pid" 2>/dev/null && wait "$app_pid" 2>/dev/null || true; app_pid=""; }
trap 'stop; for p in "$work"/xvfb-*.pid; do [[ -f "$p" ]] && kill "$(cat "$p")" 2>/dev/null || true; done' EXIT

# --- 1280x800: the Steam Deck's desktop ---------------------------------------------------------
display=:95
start_xvfb "$display" 1280x800x24
export DISPLAY="$display"
echo "1280x800:"
FAKE_VERIFY_DELAY=4 FAKE_SPEED=1.0 FAKE_STEAM_RUNNING=1 launch
shot 01-welcome-found-isos
click 1194 185                # USE IT on the first card
sleep 1.5; shot 02-checking
sleep 4;   shot 03-check-ok
sleep 3;   shot 04-options     # the check continues by itself
click 340 254; sleep 0.8; shot 04b-options-sd-card   # the SD card as install location
click 340 176; sleep 0.5      # back to internal storage
click 1109 697                # INSTALL
sleep 3;  click 562 298        # SHOW DETAILS
sleep 11; shot 05-installing-details
sleep 1; key Escape; sleep 0.6; shot 06-cancel-confirm; key Escape
for _ in $(seq 60); do grep -q '@@done' "$XDG_STATE_HOME/tt-recomp-installer/install.log" 2>/dev/null && break; sleep 1; done
sleep 2; shot 07-done
stop

FAKE_FAIL=build FAKE_SPEED=0.2 launch --iso "$work/sdcard/TableTennis.iso"
sleep 3                       # check result, then the automatic continue
click 1109 697                # INSTALL
sleep 12; shot 08-error
key Right; sleep 0.3; key Return; sleep 0.8   # focus moves from TRY AGAIN to SHOW DETAILS
shot 08b-error-details
stop

# A wrong file: the check screen's failure wording.
FAKE_VERIFY_DELAY=1 launch --iso "$work/home/Downloads/some-other-game.iso"
sleep 1; shot 09-check-failed
stop

# --- 1920x1080 ---------------------------------------------------------------------------------
display=:94
start_xvfb "$display" 1920x1080x24
export DISPLAY="$display"
echo "1920x1080:"
FAKE_SPEED=1.0 launch --size 1920x1080
shot 10-welcome-1080p
stop
echo "done: $out"
