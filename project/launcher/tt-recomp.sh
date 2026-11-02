#!/usr/bin/env bash
# Linux launcher for the Table Tennis recompilation.
#
# Reads tt-recomp.ini next to this script (same keys as the Windows
# settings.ini), lets command-line flags override it, and starts the game with
# the same engine options the Windows launcher passes.
#
#   ./tt-recomp.sh [--iso=PATH] [--resolution=720p|800p|1080p|1440p|4k]
#                  [--framerate=60|120|144|240|0] [--fullscreen=true|false]
#                  [--show_fps=true|false] [--aa=none|fxaa|fxaa_extreme]
#                  [--post=fsr|cas|bilinear] [--aspect=letterbox|fill]
#                  [--achievement_sound=true|false] [--haptics=true|false]
#                  [--render_scale=auto|1|2|3] [--check_updates=true|false]
#                  [--auto_update=true|false] [--system_link=true|false]
#                  [--player_name=NAME] [--lan_peers=IP,IP] [--save] [-- extra engine flags...]
#
# System link (SYSTEM LINK in the main menu) plays against other PCs and Steam Decks on the same
# network: player_name is the name the others see (default: this computer's host name, which
# must differ between the machines), lan_peers lists the other machines' IP addresses for
# networks that do not pass broadcasts (some Wi-Fi access points and VPNs).
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

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
config="$here/tt-recomp.ini"

iso_path=""
resolution="1080p"
framerate="60"
fullscreen="false"
show_fps="false"
aa="none"
post="bilinear"
aspect="letterbox"
achievement_sound="true"
haptics="true"
render_scale="auto"
gamemode="auto"
check_updates="true"
auto_update="false"
engine_flags=""
system_link="true"
player_name=""
lan_peers=""
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
    render_scale) render_scale="$value" ;;
    gamemode) gamemode="$value" ;;
    check_updates) check_updates="$value" ;;
    auto_update) auto_update="$value" ;;
    engine_flags) engine_flags="$value" ;;
    system_link) system_link="$value" ;;
    player_name) player_name="$value" ;;
    lan_peers) lan_peers="$value" ;;
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
    --help|-h) sed -n '2,31p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
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
  1440p) width=2560; height=1440; scale=2 ;;
  4k|2160p) width=3840; height=2160; scale=3 ;;
  *) die "unknown resolution '$resolution' (720p, 800p, 1080p, 1440p, 4k)" ;;
esac
# The 3D view renders at the game's 1280x720 times this factor; auto follows the resolution.
# A higher factor than the screen needs supersamples (sharper, more GPU work).
case "$render_scale" in
  auto) ;;
  1|2|3) scale="$render_scale" ;;
  *) die "render_scale must be auto, 1, 2 or 3" ;;
esac
case "$framerate" in 0|60|120|144|240) ;; *) die "unknown framerate '$framerate' (60, 120, 144, 240, 0)" ;; esac
case "$fullscreen" in true|false) ;; *) die "fullscreen must be true or false" ;; esac
case "$show_fps" in true|false) ;; *) die "show_fps must be true or false" ;; esac
case "$aa" in none|fxaa|fxaa_extreme) ;; *) die "unknown aa '$aa' (none, fxaa, fxaa_extreme)" ;; esac
case "$post" in fsr|cas|bilinear) ;; *) die "unknown post '$post' (fsr, cas, bilinear)" ;; esac
case "$aspect" in letterbox|fill) ;; *) die "unknown aspect '$aspect' (letterbox, fill)" ;; esac
case "$achievement_sound" in true|false) ;; *) die "achievement_sound must be true or false" ;; esac
case "$haptics" in true|false) ;; *) die "haptics must be true or false" ;; esac
case "$gamemode" in auto|off) ;; *) die "gamemode must be auto or off" ;; esac
case "$check_updates" in true|false) ;; *) die "check_updates must be true or false" ;; esac
case "$auto_update" in true|false) ;; *) die "auto_update must be true or false" ;; esac
case "$system_link" in true|false) ;; *) die "system_link must be true or false" ;; esac
[[ "$player_name" =~ ^[A-Za-z0-9_\ -]{0,15}$ ]] || die "player_name: up to 15 letters, digits, spaces, - or _"
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
render_scale=$render_scale
gamemode=$gamemode
check_updates=$check_updates
auto_update=$auto_update
engine_flags=$engine_flags
system_link=$system_link
player_name=$player_name
lan_peers=$lan_peers
EOF
  mv -f "$config.tmp" "$config"
fi

args=(
  "--game_data_root=$iso_path"
  "--resolution_scale=$scale" "--draw_resolution_scale_x=$scale" "--draw_resolution_scale_y=$scale"
  "--window_width=$width" "--window_height=$height" "--fullscreen=$fullscreen"
  "--swap_post_effect=$aa" "--present_effect=$post"
  "--show_fps=$show_fps" "--framerate=$framerate" "--achievement_sound=$achievement_sound" "--haptics=$haptics"
  --protect_zero=false --gpu_allow_invalid_fetch_constants=true
  # Letterbox other screen shapes (Steam Deck 16:10) instead of cropping up to 10% of the
  # edges as TV overscan. (SDK patch 0005 keeps the game's TV mode at 1280x720.)
  --present_safe_area_x=100 --present_safe_area_y=100
  "--system_link=$system_link"
)
# Players are told apart by name, and machines of the same kind share a host name (every Steam
# Deck is "steamdeck"), so the default adds three characters of this machine's ID.
if [[ -z "$player_name" ]]; then
  host="$(cat /proc/sys/kernel/hostname 2>/dev/null || echo player)"
  id="$(head -c 3 /etc/machine-id 2>/dev/null || true)"
  player_name="$(printf '%s' "$host" | tr -cd 'A-Za-z0-9_-' | cut -c1-11)${id:+-$id}"
fi
[[ -n "$player_name" ]] && args+=("--user_name=$player_name")
[[ -n "${lan_peers// /}" ]] && args+=("--net_peers=${lan_peers// /}")
# aspect=fill renders the 3D view for the screen's shape and stretches the picture to fill it.
# The game keeps its horizontal view, so screens narrower than 16:9 (Steam Deck 16:10) show
# more at the top and bottom; 2D menus and HUD are stretched slightly. Wider screens would
# lose some of the top and bottom.
if [[ "$aspect" == fill ]]; then
  ratio="$(awk -v w="$width" -v h="$height" 'BEGIN { printf "%.4f", w / h }')"
  args+=("--display_aspect=$ratio" --present_letterbox=false)
fi
# engine_flags= in the ini adds engine options (for example --occlusion_query_enable=false);
# flags after -- on the command line still come last and win.
if [[ -n "$engine_flags" ]]; then
  read -r -a ini_flags <<< "$engine_flags"
  extra=("${ini_flags[@]}" "${extra[@]}")
fi
# The engine rejects a flag given twice, so an extra flag replaces ours.
for flag in "${extra[@]}"; do
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
restart_updated() { exec env TT_UPDATE_DONE=1 "$here/tt-recomp.sh" "${orig_args[@]}"; }
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
      ( timeout 60 "$updater" "$here" --check-only > "$here/logs/update-check.log" 2>&1 & ) 2>/dev/null
    fi
  fi
fi

# Feral GameMode switches the CPU to its performance profile while the game runs. The GPU
# command thread is single-threaded and CPU-bound on handhelds such as the Steam Deck, whose
# balanced profile kept the cores near 2.4 GHz instead of boosting.
launch=("$here/tabletennis")
if [[ "$gamemode" == auto ]] && command -v gamemoderun >/dev/null 2>&1; then
  launch=(gamemoderun "$here/tabletennis")
fi
# The game runs as a child (not exec) so an update the player asked for can run after it exits.
"${launch[@]}" "${args[@]}" "${extra[@]}" &
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
