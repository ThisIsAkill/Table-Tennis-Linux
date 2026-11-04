#!/usr/bin/env bash
# Linux and macOS launcher for the Table Tennis recompilation.
#
# Reads tt-recomp.ini next to this script (same keys as the Windows
# settings.ini), lets command-line flags override it, and starts the game with
# the same engine options the Windows launcher passes.
#
#   ./tt-recomp.sh [--iso=PATH] [--resolution=720p|800p|1080p|1440p|4k]
#                  [--framerate=60|120|144|240|0] [--fullscreen=true|false]
#                  [--show_fps=true|false] [--aa=none|fxaa|fxaa_extreme]
#                  [--post=auto|fsr|cas|bilinear] [--aspect=auto|letterbox|fill]
#                  [--achievement_sound=true|false] [--haptics=true|false]
#                  [--button_prompts=auto|xbox|playstation] [--voice_chat=true|false]
#                  [--render_scale=auto|1|2|3] [--check_updates=true|false]
#                  [--auto_update=true|false] [--system_link=true|false]
#                  [--player_name=NAME] [--lan_peers=IP,IP] [--server=HOST[:PORT]]
#                  [--online_redundancy=auto|on|off] [--keyboard=on|off]
#                  [--vsync=auto|on|off] [--brightness=0.8..2.0]
#                  [--texture_filtering=off|2x|4x|8x|16x]
#                  [--motion_swing=off|strong|normal|light] [--save]
#                  [-- extra engine flags...]
#
# System link (SYSTEM LINK in the main menu) plays against other PCs and Steam Decks on the same
# network: player_name is the name the others see (default: this computer's host name, which
# must differ between the machines), lan_peers lists the other machines' IP addresses for
# networks that do not pass broadcasts (some Wi-Fi access points and VPNs). ONLINE in the main
# menu plays the same menus over the internet through the online server in server=.
# online_redundancy sends every online match datagram twice to cover lost packets: auto (only
# while the connection loses packets), on, or off.
#
# keyboard=on plays with the keyboard too (off by default; meant for testing without a
# controller), as player 1 next to the first controller: W A S D or the arrow keys move, I J K L
# are the four spins in the controller's diamond (backspin, leftspin, rightspin, topspin), Q soft
# shot, E focus shot, F full focus, C camera, 1 / 3 the shoulder buttons (PC SETTINGS tabs);
# Space or Enter accepts, Backspace goes back, P or
# Enter pauses, Tab is Back/View. Any keybind_* engine flag overrides a key.
#
# --save writes the resulting settings back to tt-recomp.ini. An engine_flags= line in the
# ini passes further engine options. The in-game OPTIONS > PC SETTINGS page edits the same file.
#
# Updates (installed versions only, i.e. with a .tt-version file): with check_updates=true the
# launcher looks for a newer signed release in the background at most every 6 hours
# (tools/tt-update.sh --check-only) and leaves <dir>/.tt-update.json for the game. When the game
# exits after the player chose to update (<dir>/.tt-update-requested), or before starting when
# auto_update=true and an update is known, it runs the update: the installer window
# (install-table-tennis --update) when a display is available, tools/tt-update.sh otherwise,
# then starts the game again. TT_RELEASE_URL overrides where releases are looked up.
set -euo pipefail
# Arrays that may be empty are expanded as ${a[@]+"${a[@]}"}: the bash 3.2 that macOS ships
# treats a plain "${a[@]}" of an empty array as unset under set -u.

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
config="$here/tt-recomp.ini"

iso_path=""
resolution="1080p"
framerate="60"
fullscreen="false"
show_fps="false"
aa="none"
post="auto"
aspect="auto"
achievement_sound="true"
haptics="true"
button_prompts="auto"
render_scale="auto"
render_scale_auto="1"
render_scale_auto_failed="0"
gamemode="auto"
check_updates="true"
auto_update="false"
engine_flags=""
voice_chat="true"
system_link="true"
player_name=""
lan_peers=""
server=""
online_redundancy="auto"
keyboard="off"
vsync="auto"
brightness="1.0"
texture_filtering="16x"
motion_swing="off"
save=0
extra=()
orig_args=("$@")  # for restarting after an update

die() { echo "tt-recomp: $*" >&2; exit 1; }

set_option() {
  local key="$1" value="$2"
  case "$key" in
    iso|iso_path) iso_path="$value" ;;
    resolution) resolution="$value" ;;
    framerate) framerate="$value" ;;
    fullscreen) fullscreen="$value" ;;
    show_fps) show_fps="$value" ;;
    aa) aa="$value" ;;
    post) post="$value" ;;
    aspect) aspect="$value" ;;
    achievement_sound) achievement_sound="$value" ;;
    haptics) haptics="$value" ;;
    voice_chat) voice_chat="$value" ;;
    button_prompts) button_prompts="$value" ;;
    render_scale) render_scale="$value" ;;
    render_scale_auto) render_scale_auto="$value" ;;
    render_scale_auto_failed) render_scale_auto_failed="$value" ;;
    gamemode) gamemode="$value" ;;
    check_updates) check_updates="$value" ;;
    auto_update) auto_update="$value" ;;
    engine_flags) engine_flags="$value" ;;
    system_link) system_link="$value" ;;
    player_name) player_name="$value" ;;
    lan_peers) lan_peers="$value" ;;
    server) server="$value" ;;
    online_redundancy) online_redundancy="$value" ;;
    keyboard) keyboard="$value" ;;
    vsync) vsync="$value" ;;
    brightness) brightness="$value" ;;
    texture_filtering) texture_filtering="$value" ;;
    motion_swing) motion_swing="$value" ;;
    *) return 1 ;;
  esac
}

if [[ -f "$config" ]]; then
  while IFS='=' read -r key value || [[ -n "$key" ]]; do
    key="${key//[[:space:]]/}"
    [[ -z "$key" || "$key" == \#* || "$key" == \;* ]] && continue
    value="${value%$'\r'}"
    value="${value#\"}"; value="${value%\"}"
    set_option "$key" "$value" || echo "tt-recomp: ignoring unknown setting '$key' in $config" >&2
  done < "$config"
fi

while (($#)); do
  case "$1" in
    --save) save=1 ;;
    --help|-h) sed -n '2,35p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    --) shift; extra+=("$@"); break ;;
    --*=*)
      key="${1%%=*}"; key="${key#--}"
      set_option "$key" "${1#*=}" || extra+=("$1") ;;
    *) extra+=("$1") ;;
  esac
  shift
done

case "$resolution" in
  720p) width=1280; height=720; scale=1 ;;
  800p) width=1280; height=800; scale=1 ;;
  1080p) width=1920; height=1080; scale=1 ;;
  1440p) width=2560; height=1440; scale=1 ;;
  4k|2160p) width=3840; height=2160; scale=1 ;;
  *) die "unknown resolution '$resolution' (720p, 800p, 1080p, 1440p, 4k)" ;;
esac
# The 3D view renders at the game's 1280x720 times this factor. auto is 1 at every resolution
# (FSR or CAS can upscale): 2 and 3 cost about 3x and 14x the GPU time of 1, far more than their
# pixel count suggests, so they are only used when chosen.
case "$render_scale" in
  auto)
    # The game measures its GPU time in matches and remembers the level this PC can afford
    # (render_scale_auto=); it never goes above what the window can show.
    case "$render_scale_auto" in 1|2|3) scale="$render_scale_auto" ;; *) scale=1 ;; esac
    if (( height <= 800 )); then scale=1; elif (( height <= 1440 && scale > 2 )); then scale=2; fi
    ;;
  1|2|3) scale="$render_scale" ;;
  *) die "render_scale must be auto, 1, 2 or 3" ;;
esac
case "$framerate" in 0|60|120|144|240) ;; *) die "unknown framerate '$framerate' (60, 120, 144, 240, 0)" ;; esac
case "$fullscreen" in true|false) ;; *) die "fullscreen must be true or false" ;; esac
case "$show_fps" in true|false) ;; *) die "show_fps must be true or false" ;; esac
case "$aa" in none|fxaa|fxaa_extreme) ;; *) die "unknown aa '$aa' (none, fxaa, fxaa_extreme)" ;; esac
case "$post" in auto|fsr|cas|bilinear) ;; *) die "unknown post '$post' (auto, fsr, cas, bilinear)" ;; esac
# auto: the game renders at 720p (times the render scale); FSR upscales it to 1080p and larger
# windows, smaller ones are close enough to native for plain scaling.
present_effect="$post"
if [[ "$post" == auto ]]; then
  if (( height * 10 >= 720 * ${scale:-1} * 14 )); then present_effect=fsr; else present_effect=bilinear; fi
fi
case "$aspect" in auto|letterbox|fill) ;; *) die "unknown aspect '$aspect' (auto, letterbox, fill)" ;; esac
case "$achievement_sound" in true|false) ;; *) die "achievement_sound must be true or false" ;; esac
case "$haptics" in true|false) ;; *) die "haptics must be true or false" ;; esac
case "$voice_chat" in true|false) ;; *) die "voice_chat must be true or false" ;; esac
case "$button_prompts" in auto|xbox|playstation) ;; *) die "button_prompts must be auto, xbox or playstation" ;; esac
case "$gamemode" in auto|off) ;; *) die "gamemode must be auto or off" ;; esac
case "$check_updates" in true|false) ;; *) die "check_updates must be true or false" ;; esac
case "$auto_update" in true|false) ;; *) die "auto_update must be true or false" ;; esac
case "$system_link" in true|false) ;; *) die "system_link must be true or false" ;; esac
case "$keyboard" in on|off) ;; *) die "keyboard must be on or off" ;; esac
case "$vsync" in auto|on|off) ;; *) die "vsync must be auto, on or off" ;; esac
[[ "$brightness" =~ ^[0-9]+(\.[0-9]+)?$ ]] || die "brightness must be a number (1.0 = normal)"
# Anisotropic filtering keeps floor and table textures sharp at shallow angles.
case "$texture_filtering" in
  off) anisotropic=0 ;; 2x) anisotropic=2 ;; 4x) anisotropic=3 ;; 8x) anisotropic=4 ;; 16x) anisotropic=5 ;;
  *) die "texture_filtering must be off, 2x, 4x, 8x or 16x" ;;
esac
# Shots by a flick of a gyro controller; the value is how light a flick may be.
case "$motion_swing" in
  off) motion_args=(--motion_swing=false) ;;
  strong) motion_args=(--motion_swing=true --motion_swing_sensitivity=2) ;;
  normal) motion_args=(--motion_swing=true --motion_swing_sensitivity=3) ;;
  light) motion_args=(--motion_swing=true --motion_swing_sensitivity=4) ;;
  *) die "motion_swing must be off, strong, normal or light" ;;
esac
[[ "$player_name" =~ ^[A-Za-z0-9_\ -]{0,15}$ ]] || die "player_name: up to 15 letters, digits, spaces, - or _"
[[ "$server" =~ ^[A-Za-z0-9.:_-]*$ ]] || die "server must be a host name or address, optionally with :port"
[[ "$lan_peers" =~ ^[0-9.,\ ]*$ ]] || die "lan_peers must be IPv4 addresses separated by commas"

[[ -n "$iso_path" ]] || die "no ISO set. Add iso_path=/path/to/TableTennis.iso to $config or pass --iso=PATH."
[[ -f "$iso_path" ]] || die "ISO not found: $iso_path"

if ((save)); then
  cat > "$config.tmp" <<EOF
iso_path=$iso_path
resolution=$resolution
framerate=$framerate
fullscreen=$fullscreen
show_fps=$show_fps
aa=$aa
post=$post
aspect=$aspect
achievement_sound=$achievement_sound
haptics=$haptics
button_prompts=$button_prompts
render_scale=$render_scale
render_scale_auto=$render_scale_auto
render_scale_auto_failed=$render_scale_auto_failed
gamemode=$gamemode
check_updates=$check_updates
auto_update=$auto_update
engine_flags=$engine_flags
system_link=$system_link
player_name=$player_name
lan_peers=$lan_peers
server=$server
online_redundancy=$online_redundancy
keyboard=$keyboard
vsync=$vsync
brightness=$brightness
texture_filtering=$texture_filtering
motion_swing=$motion_swing
EOF
  mv -f "$config.tmp" "$config"
fi

args=(
  "--game_data_root=$iso_path"
  "--resolution_scale=$scale" "--draw_resolution_scale_x=$scale" "--draw_resolution_scale_y=$scale"
  "--window_width=$width" "--window_height=$height" "--fullscreen=$fullscreen"
  "--swap_post_effect=$aa" "--present_effect=$present_effect"
  "--show_fps=$show_fps" "--framerate=$framerate" "--achievement_sound=$achievement_sound" "--haptics=$haptics" "--button_prompts=$button_prompts"
  "--voice_chat=$voice_chat"
  --protect_zero=false --gpu_allow_invalid_fetch_constants=true
  # Letterbox other screen shapes (Steam Deck 16:10) instead of cropping up to 10% of the
  # edges as TV overscan. (SDK patch 0005 keeps the game's TV mode at 1280x720.)
  --present_safe_area_x=100 --present_safe_area_y=100
  "--system_link=$system_link"
  "--display_brightness=$brightness"
  "--anisotropic_override=$anisotropic"
  "${motion_args[@]}"
)
# vsync=on presents in step with the display (no tearing; mailbox or FIFO), off presents at once
# (lowest latency). auto is on for macOS, where MoltenVK's immediate mode tears visibly, and off
# elsewhere; the game paces itself to the framerate setting either way.
if [[ "$vsync" == auto ]]; then
  [[ "$(uname -s)" == Darwin ]] && vsync_on=1 || vsync_on=0
else
  [[ "$vsync" == on ]] && vsync_on=1 || vsync_on=0
fi
((vsync_on)) && args+=(--vulkan_allow_present_mode_immediate=false)
# Keyboard play: the engine's keyboard device joins player 1 (it never takes a controller's slot).
# The keys follow the controller layout of OPTIONS > CONTROLS; the right stick (the same four
# spins as the face buttons) is on the number pad. The automated tests rely on Space, Backspace,
# Return and Shift+arrows, which keep their meaning.
if [[ "$keyboard" == on ]]; then
  args+=(--mnk_mode=true
    "--keybind_a=Space,K" "--keybind_b=Backspace,L" "--keybind_x=J" "--keybind_y=I"
    "--keybind_left_trigger=Q" "--keybind_right_trigger=E"
    "--keybind_left_shoulder=1" "--keybind_right_shoulder=3,F"
    "--keybind_back=Tab,C" "--keybind_start=Return,P"
    "--keybind_lstick_up=W,Up" "--keybind_lstick_down=S,Down"
    "--keybind_lstick_left=A,Left" "--keybind_lstick_right=D,Right" "--keybind_lstick_press="
    "--keybind_rstick_up=Numpad8" "--keybind_rstick_down=Numpad2"
    "--keybind_rstick_left=Numpad4" "--keybind_rstick_right=Numpad6" "--keybind_rstick_press=")
fi
# The project's online matchmaking server, used when the ini sets no server= of its own. It is a
# name (DuckDNS) so the server can move without an update; if the name does not resolve at start
# (DNS or DuckDNS down), the server's current address is used instead.
default_server_name="rockstartt.duckdns.org" default_server_address="2.31.190.147" default_server_port=27360
resolves() {
  if command -v getent >/dev/null 2>&1; then
    timeout 3 getent hosts "$1" >/dev/null 2>&1
  else  # macOS
    dscacheutil -q host -a name "$1" 2>/dev/null | grep -q "address"
  fi
}
if [[ -z "$server" ]]; then
  if resolves "$default_server_name"; then
    server="$default_server_name:$default_server_port"
  else
    server="$default_server_address:$default_server_port"
    echo "tt-recomp: $default_server_name does not resolve; using $server" >&2
  fi
fi
[[ -n "$server" ]] && args+=("--online_server=$server")
case "$online_redundancy" in
  auto|on|off) args+=("--online_redundancy=$online_redundancy") ;;
  *) echo "tt-recomp: online_redundancy must be auto, on or off" >&2 ;;
esac
# Players are told apart by name, and machines of the same kind share a host name (every Steam
# Deck is "steamdeck"), so the default adds three characters of this machine's ID.
if [[ -z "$player_name" ]]; then
  host="$(cat /proc/sys/kernel/hostname 2>/dev/null || hostname -s 2>/dev/null || echo player)"
  id="$(head -c 3 /etc/machine-id 2>/dev/null || true)"
  if [[ -z "$id" ]] && command -v ioreg >/dev/null 2>&1; then  # macOS: the hardware UUID
    id="$(ioreg -rd1 -c IOPlatformExpertDevice | awk -F'"' '/IOPlatformUUID/ { print tolower(substr($4, 1, 3)) }')"
  fi
  player_name="$(printf '%s' "$host" | tr -cd 'A-Za-z0-9_-' | cut -c1-11)${id:+-$id}"
fi
[[ -n "$player_name" ]] && args+=("--user_name=$player_name")
[[ -n "${lan_peers// /}" ]] && args+=("--net_peers=${lan_peers// /}")
# Screens that are not 16:9 (the game follows the window's shape, see project/src/pc_features.h):
# fill widens the 3D view on wider screens with the menus kept 16:9, and shows more at the top
# and bottom on narrower ones (Steam Deck 16:10) with the menus stretched slightly; letterbox keeps
# the original picture with bars; auto fills wider screens and letterboxes narrower ones.
args+=("--picture=$aspect")
# engine_flags= in the ini adds engine options (for example --occlusion_query_enable=false);
# flags after -- on the command line still come last and win.
if [[ -n "$engine_flags" ]]; then
  read -r -a ini_flags <<< "$engine_flags"
  extra=(${ini_flags[@]+"${ini_flags[@]}"} ${extra[@]+"${extra[@]}"})
fi
# The engine rejects a flag given twice, so an extra flag replaces ours.
for flag in ${extra[@]+"${extra[@]}"}; do
  [[ "$flag" == --*=* ]] || continue
  name="${flag%%=*}"
  for i in "${!args[@]}"; do
    [[ "${args[i]%%=*}" == "$name" ]] && unset 'args[i]'
  done
done

cd "$here"

# --- Updates -----------------------------------------------------------------------------------
installed_version=""  # empty: a development build, which never updates
[[ -f "$here/.tt-version" ]] && installed_version="$(head -n 1 "$here/.tt-version" | tr -d '[:space:]')"
updater="$here/tools/tt-update.sh"
update_log="$here/logs/update.log"
# Runs the update. Returns 0 when the (new or current) game should start, 1 when not.
run_update() {
  mkdir -p "$here/logs"
  # Steam injects its overlay into everything started from a game; the update tools are not
  # games, and the 32-bit overlay only fills their log with ld.so errors.
  local LD_PRELOAD=""; export LD_PRELOAD
  if [[ -x "$here/install-table-tennis" && -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]]; then
    "$here/install-table-tennis" --update "$here"
    case $? in
      0|10) return 0 ;;   # updated, or "Play the current version"
      20) ;;              # no window possible: update without one
      *) return 1 ;;      # closed without choosing to play
    esac
  fi
  echo "tt-recomp: updating (log: $update_log)" >&2
  if "$updater" "$here" > "$update_log" 2>&1; then
    echo "tt-recomp: update finished" >&2
  else
    echo "tt-recomp: update failed, starting the current version ($(grep '^@@error' "$update_log" | tail -1 | cut -c9-))" >&2
  fi
  return 0
}
known_update() {  # version from .tt-update.json, if newer than the installed one
  local v
  v="$(grep -o '"version": *"[^"]*"' "$here/.tt-update.json" 2>/dev/null | head -1 | sed 's/.*"\([^"]*\)"$/\1/')"
  [[ -n "$v" && "$v" != "$installed_version" && "$(printf '%s\n%s\n' "$v" "$installed_version" | sort -V | tail -1)" == "$v" ]] \
    && echo "$v"
}
restart_updated() { exec env TT_UPDATE_DONE=1 "$here/tt-recomp.sh" ${orig_args[@]+"${orig_args[@]}"}; }
if [[ -n "$installed_version" && -x "$updater" && -z "${TT_UPDATE_DONE:-}" ]]; then
  if [[ "$auto_update" == true && -n "$(known_update)" ]]; then
    run_update && restart_updated
    exit 0
  fi
  if [[ "$check_updates" == true ]]; then
    stamp="$here/.tt-update-check"
    if [[ -z "$(find "$stamp" -mmin -360 2>/dev/null)" ]]; then
      touch "$stamp"; mkdir -p "$here/logs"
      # In the background with its own 5 s network timeouts; never delays the game.
      limit=(); command -v timeout >/dev/null 2>&1 && limit=(timeout 60)  # macOS has no timeout
      ( ${limit[@]+"${limit[@]}"} "$updater" "$here" --check-only > "$here/logs/update-check.log" 2>&1 & ) 2>/dev/null
    fi
  fi
fi

# Versions before 1.1.1 left their 4.5 GB guest-memory file in /dev/shm after every exit; enough
# of them fill /dev/shm, and then the game and Steam crash (SIGBUS). Remove this user's leftovers
# when no copy of the game is running.
if ! pgrep -x -u "$(id -u)" tabletennis >/dev/null 2>&1; then
  find /dev/shm -maxdepth 1 -type f -name 'xenia_memory_*' -user "$(id -u)" -delete 2>/dev/null || true
fi

# Feral GameMode switches the CPU to its performance profile while the game runs. The GPU
# command thread is single-threaded and CPU-bound on handhelds such as the Steam Deck, whose
# balanced profile kept the cores near 2.4 GHz instead of boosting.
launch=("$here/tabletennis")
if [[ "$gamemode" == auto ]] && command -v gamemoderun >/dev/null 2>&1; then
  launch=(gamemoderun "$here/tabletennis")
fi
# macOS: let MoltenVK use Metal's private API to disable primitive restart (see build.sh). The
# bundled MoltenVK is built for it; without this flag it keeps restart on and warns every frame.
[[ "$(uname -s)" == Darwin ]] && export MVK_CONFIG_USE_METAL_PRIVATE_API=1
# The game runs as a child (not exec) so an update the player asked for can run after it exits.
"${launch[@]}" ${args[@]+"${args[@]}"} ${extra[@]+"${extra[@]}"} &
game=$!
trap 'kill -TERM "$game" 2>/dev/null' TERM INT HUP
while true; do
  status=0; wait "$game" || status=$?
  kill -0 "$game" 2>/dev/null || break   # wait was interrupted by a forwarded signal
done
trap - TERM INT HUP
if [[ -f "$here/.tt-update-requested" ]]; then
  rm -f "$here/.tt-update-requested"
  if [[ -n "$installed_version" && -x "$updater" ]]; then
    run_update && restart_updated
  fi
fi
exit "$status"
