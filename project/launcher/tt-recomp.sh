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
#                  [--post=fsr|cas|bilinear] [--aspect=letterbox|fill] [--save]
#                  [-- extra engine flags...]
#
# --save writes the resulting settings back to tt-recomp.ini. An engine_flags= line in the
# ini passes further engine options.
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
engine_flags=""
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
    aspect) aspect="$value" ;;
    engine_flags) engine_flags="$value" ;;
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
    --help|-h) sed -n '2,16p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
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
case "$framerate" in 0|60|120|144|240) ;; *) die "unknown framerate '$framerate' (60, 120, 144, 240, 0)" ;; esac
case "$fullscreen" in true|false) ;; *) die "fullscreen must be true or false" ;; esac
case "$show_fps" in true|false) ;; *) die "show_fps must be true or false" ;; esac
case "$aa" in none|fxaa|fxaa_extreme) ;; *) die "unknown aa '$aa' (none, fxaa, fxaa_extreme)" ;; esac
case "$post" in fsr|cas|bilinear) ;; *) die "unknown post '$post' (fsr, cas, bilinear)" ;; esac
case "$aspect" in letterbox|fill) ;; *) die "unknown aspect '$aspect' (letterbox, fill)" ;; esac

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
engine_flags=$engine_flags
EOF
  mv -f "$config.tmp" "$config"
fi

args=(
  "--game_data_root=$iso_path"
  "--resolution_scale=$scale" "--draw_resolution_scale_x=$scale" "--draw_resolution_scale_y=$scale"
  "--window_width=$width" "--window_height=$height" "--fullscreen=$fullscreen"
  "--swap_post_effect=$aa" "--present_effect=$post"
  "--show_fps=$show_fps" "--framerate=$framerate"
  --protect_zero=false --gpu_allow_invalid_fetch_constants=true
  # Letterbox other screen shapes (Steam Deck 16:10) instead of cropping up to 10% of the
  # edges as TV overscan. (SDK patch 0005 keeps the game's TV mode at 1280x720.)
  --present_safe_area_x=100 --present_safe_area_y=100
)
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
exec "$here/tabletennis" "${args[@]}" "${extra[@]}"
