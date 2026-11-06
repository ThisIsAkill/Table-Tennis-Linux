#!/usr/bin/env bash
# Copyright (c) 2026 Akhil Moola (ThisIsAkill). MIT License, see LICENSE.
# Stand-in for scripts/install/tt-install.sh while developing the installer UI. It speaks the
# backend's stdout protocol (@@step / @@progress / @@log / @@warn / @@error / @@done) with short
# sleeps and touches nothing but the --prefix directory.
#
#   fake-backend.sh --iso PATH --prefix DIR --steam yes|no --desktop yes|no [--cache DIR]
#   fake-backend.sh --verify PATH          answers "@@verify ok ..." (or fail for names with "bad")
#
# Environment: FAKE_SPEED=<seconds per tick> (default 0.4), FAKE_FAIL=<step id> makes that step
# fail with an @@error, FAKE_SKIP_TOOLS=1 emits the image step instead of tools (a machine with
# podman), FAKE_STEAM_RUNNING=1 emits the Steam restart warning.
set -u
speed="${FAKE_SPEED:-0.4}"
fail_at="${FAKE_FAIL:-}"
iso="" prefix="" steam="yes" desktop="no" cache=""

if [[ "${1:-}" == "--verify" ]]; then
  echo "checking ${2:-}"
  sleep "${FAKE_VERIFY_DELAY:-0}"
  case "${2:-}" in
    *bad*|*wrong*|*other*) echo "@@verify fail Unsupported game executable. Expected the retail USA/Europe release (Title ID 545407DF)."; exit 1 ;;
    *) echo "@@verify ok Rockstar Games presents Table Tennis, USA / Europe retail release (Title ID 545407DF). The image is unmodified and complete."; exit 0 ;;
  esac
fi

while [[ $# -gt 0 ]]; do
  case "$1" in
    --iso) iso="$2"; shift 2 ;;
    --prefix) prefix="$2"; shift 2 ;;
    --steam) steam="$2"; shift 2 ;;
    --desktop) desktop="$2"; shift 2 ;;
    --cache) cache="$2"; shift 2 ;;
    *) echo "@@error Unknown option $1"; exit 2 ;;
  esac
done
[[ -n "$iso" && -n "$prefix" ]] || { echo "@@error --iso and --prefix are required"; exit 2; }

trap 'echo "@@log Stopping..."; sleep 0.5; echo "@@error Cancelled"; exit 130' INT TERM

tick() { sleep "$speed"; }
step() {  # id title progress_from progress_to log lines...
  local id="$1" title="$2" from="$3" to="$4"; shift 4
  echo "@@step $id $title"
  if [[ "$fail_at" == "$id" ]]; then
    tick; echo "@@log Something is off here"; tick
    case "$id" in
      tools) echo "@@error Could not download the build tools: the network is unreachable. Check your connection and try again." ;;
      build) echo "@@error The compiler ran out of memory while building the game. Close other programs and try again, or build with fewer parallel jobs." ;;
      *) echo "@@error Step $id failed (simulated)." ;;
    esac
    exit 1
  fi
  local n=$# i=0
  for line in "$@"; do
    echo "@@log $line"
    i=$((i + 1))
    echo "@@progress $((from + (to - from) * i / n))"
    tick
  done
}

echo "Table Tennis setup (stand-in backend)"
echo "iso=$iso prefix=$prefix steam=$steam desktop=$desktop cache=${cache:-default}"
step check "Checking your system" 0 2 "Opening $iso read-only" "default.xex matches the retail USA/Europe release" "x86-64-v3 CPU, 14 GB free in the cache folder"
if [[ -z "${FAKE_SKIP_TOOLS:-}" ]]; then
  step tools "Getting build tools" 2 12 "podman is not installed; using an Ubuntu 24.04 rootfs without admin rights" "Downloading ubuntu-base (28 MB)" "Unpacking" "Installing clang, cmake, ninja, python3 inside the rootfs"
  echo "@@warn The build tools take about 1.2 GB; they stay in ${cache:-~/.cache/tt-recomp} for next time."
else
  step image "Preparing the build container" 2 12 "Creating the container image" "Mounting the work directory"
fi
step extract "Reading your game disc" 12 15 "Extracting default.xex (hash verified)" "Extracting assets/resource/flash/flash.rpf" "Applying the menu patch"
step sdk "Building the ReXGlue SDK" 15 45 "Cloning rexglue-sdk v0.10.0" "Applying 5 project patches" "[1/312] Compiling base/..." "[120/312] Compiling cpu/backend/..." "[312/312] Linking librexruntime.so" "Installing the SDK"
step codegen "Generating the game code" 45 58 "Running rexglue codegen" "4,812 functions" "Applying 17 native edits"
step build "Compiling the game" 58 90 "[1/96] Compiling generated/default/ppc_recomp.0.cpp" "[24/96] Compiling generated/default/ppc_recomp.23.cpp" "[48/96] Compiling generated/default/ppc_recomp.47.cpp" "[72/96] Compiling src/achievement_ui.cpp" "[96/96] Linking tabletennis"
step install "Installing" 90 96 "Copying the game to $prefix" "Writing tt-recomp.ini (1280x800, letterbox)"
mkdir -p "$prefix" 2>/dev/null && printf '#!/usr/bin/env bash\necho "launcher stand-in"\n' > "$prefix/tt-recomp.sh" && chmod +x "$prefix/tt-recomp.sh"
if [[ "$steam" == "yes" || "$desktop" == "yes" ]]; then
  lines=()
  [[ "$steam" == "yes" ]] && lines+=("Adding a non-Steam shortcut with the Table Tennis artwork")
  [[ "$desktop" == "yes" ]] && lines+=("Writing table-tennis.desktop")
  step integrate "Adding shortcuts" 96 100 "${lines[@]}"
  if [[ "$steam" == "yes" && -n "${FAKE_STEAM_RUNNING:-}" ]]; then
    echo "@@warn Steam was running while the shortcut was added; restart Steam to see Table Tennis in your library."
  fi
fi
echo "@@progress 100"
echo "@@done $prefix"
exit 0
