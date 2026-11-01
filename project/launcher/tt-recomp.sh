#!/usr/bin/env bash
# Linux launcher for the Table Tennis recompilation.
#
# Reads tt-recomp.ini next to this script (same keys as the Windows
# settings.ini), lets command-line flags override it, and starts the game with
# the same engine options the Windows launcher passes.
#
#   ./tt-recomp.sh [--iso=PATH] [--resolution=720p|1080p|1440p|4k]
#                  [--framerate=60|120|144|240|0] [--fullscreen=true|false]
#                  [--show_fps=true|false] [--aa=none|fxaa|fxaa_extreme]
#                  [--post=fsr|cas|bilinear] [--save] [-- extra engine flags...]
#
# --save writes the resulting settings back to tt-recomp.ini.
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
save=0
extra=()

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
    --help|-h) sed -n '2,14p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
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
  1080p) width=1920; height=1080; scale=1 ;;
  1440p) width=2560; height=1440; scale=2 ;;
  4k|2160p) width=3840; height=2160; scale=3 ;;
  *) die "unknown resolution '$resolution' (720p, 1080p, 1440p, 4k)" ;;
esac
case "$framerate" in 0|60|120|144|240) ;; *) die "unknown framerate '$framerate' (60, 120, 144, 240, 0)" ;; esac
case "$fullscreen" in true|false) ;; *) die "fullscreen must be true or false" ;; esac
case "$show_fps" in true|false) ;; *) die "show_fps must be true or false" ;; esac
case "$aa" in none|fxaa|fxaa_extreme) ;; *) die "unknown aa '$aa' (none, fxaa, fxaa_extreme)" ;; esac
case "$post" in fsr|cas|bilinear) ;; *) die "unknown post '$post' (fsr, cas, bilinear)" ;; esac

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
EOF
  mv -f "$config.tmp" "$config"
fi

cd "$here"
exec "$here/tabletennis" \
  "--game_data_root=$iso_path" \
  "--resolution_scale=$scale" "--draw_resolution_scale_x=$scale" "--draw_resolution_scale_y=$scale" \
  "--window_width=$width" "--window_height=$height" "--fullscreen=$fullscreen" \
  "--swap_post_effect=$aa" "--present_effect=$post" \
  "--show_fps=$show_fps" "--framerate=$framerate" \
  --protect_zero=false --gpu_allow_invalid_fetch_constants=true \
  "${extra[@]}"
