#!/usr/bin/env bash
# Build the game locally on Linux or macOS (Apple Silicon) from your own verified retail ISO.
# Linux counterpart of setup.cmd / setup.ps1 / project/tools/setup_build.py.
#
#   ./build.sh --iso PATH [--sdk PATH] [--jobs N] [--prepare-only]
#
# Steps: verify the ISO and extract default.xex + flash.rpf (read-only), patch
# the menu archive, build ReXGlue SDK v0.10.0 from source if no SDK is given,
# run codegen, apply the verified native edits, compile with clang + ninja and
# stage a runnable build in dist/.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
rexglue_repo="https://github.com/rexglue/rexglue-sdk"
rexglue_tag="v0.10.0"
rexglue_commit="f5337cdc947ff6d4c4196737e2c807a48f2a1fc2"

iso=""
sdk=""
case "$(uname -s)/$(uname -m)" in
  Linux/x86_64) platform=linux; preset=linux-amd64; jobs="$(nproc)" ;;
  Darwin/arm64)
    platform=mac; preset=mac-arm64
    # The generated game code is large: allow about 2 GB of memory per compile job.
    cores="$(sysctl -n hw.ncpu)"; mem_jobs="$(( $(sysctl -n hw.memsize) / 2147483648 ))"
    jobs="$(( cores < mem_jobs ? cores : mem_jobs ))"; ((jobs >= 1)) || jobs=1 ;;
  *) platform="" ;;
esac
prepare_only=0

usage() { sed -n '2,10p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }
die() { echo "ERROR: $*" >&2; echo "Setup stopped. See README.md for prerequisites." >&2; exit 1; }
stage() { echo "STAGE:$*"; }

while (($#)); do
  case "$1" in
    --iso) iso="${2:?--iso needs a path}"; shift ;;
    --iso=*) iso="${1#*=}" ;;
    --sdk) sdk="${2:?--sdk needs a path}"; shift ;;
    --sdk=*) sdk="${1#*=}" ;;
    --jobs|-j) jobs="${2:?--jobs needs a number}"; shift ;;
    --jobs=*) jobs="${1#*=}" ;;
    --prepare-only) prepare_only=1 ;;
    -h|--help) usage; exit 0 ;;
    *) usage >&2; die "unknown argument: $1" ;;
  esac
  shift
done

[[ -n "$platform" ]] || die "build.sh supports Linux x86_64 and macOS on Apple Silicon only"

# Portable stand-ins for GNU tools that macOS lacks (realpath -e, sha256sum).
abspath() { python3 -I -c 'import os, sys; p = os.path.realpath(sys.argv[1]); print(p) if os.path.exists(p) else sys.exit(1)' "$1"; }
sha256() { if command -v sha256sum >/dev/null; then sha256sum; else shasum -a 256; fi; }
[[ "$jobs" =~ ^[0-9]+$ ]] && ((jobs >= 1 && jobs <= 64)) || die "--jobs must be between 1 and 64"
[[ -n "$iso" ]] || { usage >&2; die "select your retail ISO with --iso PATH"; }
iso="$(abspath "$iso")" || die "ISO not found"
[[ "$(printf '%s' "$iso" | tr '[:upper:]' '[:lower:]')" == *.iso ]] || die "select a retail Xbox 360 ISO file"

need() { command -v "$1" >/dev/null || die "missing $1. Install: python cmake ninja clang git"; }
for tool in python3 cmake ninja clang clang++ git; do need "$tool"; done
clang_major="$(clang++ -dumpversion | cut -d. -f1)"
if [[ "$platform" == mac ]]; then
  # Apple's clang uses its own version numbers; Xcode 16 (Apple clang 16) is the oldest tested.
  ((clang_major >= 16)) || die "Xcode 16 or newer is required (found Apple clang $clang_major)"
else
  ((clang_major >= 19)) || die "clang 19 or newer is required (found $clang_major)"
fi

# Import the upstream tools directly; setup_build.py itself is Windows-only.
run_tools() { python3 -I -c "import sys; sys.path.insert(0, sys.argv[1]); $1" "$root/project/tools" "${@:2}"; }

stage "Preparing your local game files"
run_tools "from disc_image import extract; extract(sys.argv[2], sys.argv[3])" "$iso" "$root"
run_tools "from patch_menu import patch; patch(sys.argv[2])" "$root/dist/assets/resource/flash/flash.rpf"

stage "Preparing game build support"
# Fixes to the SDK itself (not to generated code) live in project/sdk-patches and are applied
# to the pinned source before it is built. The installed SDK records which set it was built
# with, so changing the patches triggers a rebuild.
sdk_patches=("$root"/project/sdk-patches/*.patch)
# FidelityFX on: FSR 1 and CAS presentation use prebuilt shaders and only need AMD's headers.
# SDK patch 0008 leaves out the ffx-api runtime (temporal FSR 2/3) on Linux, whose shader build
# runs AMD's Windows compiler under Wine and fails with current glslang. Explicit so a reused
# build tree cannot inherit another setting.
sdk_options=(-DREXGLUE_ENABLE_FIDELITYFX=ON)
patch_stamp="$({ cat "${sdk_patches[@]}" 2>/dev/null; echo "${sdk_options[*]}"; } | sha256 | cut -c1-16)"
if [[ -z "$sdk" ]]; then
  sdk="$root/.tools/rexglue-sdk"
  if [[ ! -x "$sdk/bin/rexglue" || "$(cat "$sdk/.tt-sdk-patches" 2>/dev/null)" != "$patch_stamp" ]]; then
    src="$root/.tools/rexglue-src"
    if [[ ! -d "$src/.git" ]]; then
      git clone --recursive --depth 1 --branch "$rexglue_tag" "$rexglue_repo" "$src"
    fi
    [[ "$(git -C "$src" rev-parse HEAD)" == "$rexglue_commit" ]] \
      || die "$src is not ReXGlue $rexglue_tag ($rexglue_commit)"
    git -C "$src" checkout -q -- .
    for p in "${sdk_patches[@]}"; do
      [[ -f "$p" ]] || continue
      git -C "$src" apply "$p" || die "SDK patch does not apply: $p"
      echo "Applied SDK patch $(basename "$p")."
    done
    echo "Building ReXGlue SDK $rexglue_tag from source ($preset-release)."
    (cd "$src" && cmake --preset "$preset" "-DCMAKE_INSTALL_PREFIX=$sdk" "${sdk_options[@]}" \
      && cmake --build --preset "$preset-release" --parallel "$jobs" \
      && cmake --install "out/build/$preset" --config Release) || die "SDK build failed"
    echo "$patch_stamp" > "$sdk/.tt-sdk-patches"
  fi
fi
sdk="$(abspath "$sdk")" || die "SDK path not found"
[[ -x "$sdk/bin/rexglue" && -d "$sdk/lib/cmake/rexglue" ]] \
  || die "SDK path must contain bin/rexglue and lib/cmake/rexglue"
[[ "$("$sdk/bin/rexglue" --version)" == 0.10.0 ]] || die "this project requires ReXGlue SDK v0.10.0"

stage "Generating your local game"
(cd "$root" && "$sdk/bin/rexglue" codegen project/tabletennis_manifest.toml)
run_tools "from patch_native import patch; patch(sys.argv[2])" "$root/project/generated/default"
if ((prepare_only)); then
  echo "Preparation complete. Original-derived files remain local and ignored by Git."
  exit 0
fi

stage "Building your game"
cmake -S "$root/project" -B "$root/build" -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$sdk"
cmake --build "$root/build" --parallel "$jobs"

stage "Finishing installation"
dist="$root/dist"
if [[ "$platform" == mac ]]; then libext=dylib; else libext=so; fi
install -m 755 "$root/build/tabletennis" "$root/build/librexruntime.$libext" "$root/build/librexgpu-xenos.$libext" "$dist/"
install -m 755 "$root/project/launcher/tt-recomp.sh" "$dist/tt-recomp.sh"
install -m 644 "$root/project/assets/tabletennis.ico" "$dist/tabletennis.ico"
mkdir -p "$dist/fonts"
install -m 644 "$root"/project/assets/fonts/*.ttf "$dist/fonts/"
mkdir -p "$dist/licenses"
cp -r "$root/licenses/." "$dist/licenses/"
if [[ -d "$sdk/share/licenses" ]]; then
  mkdir -p "$dist/licenses/SDK"
  cp -r "$sdk/share/licenses/." "$dist/licenses/SDK/"
fi

# Keep existing settings; record this machine's ISO path only in the local build.
settings="$dist/tt-recomp.ini"
if [[ -f "$settings" ]]; then
  { echo "iso_path=$iso"; grep -v '^iso_path=' "$settings" || true; } > "$settings.tmp"
else
  printf 'iso_path=%s\nresolution=1080p\nframerate=60\nfullscreen=false\nshow_fps=false\naa=none\npost=bilinear\n' \
    "$iso" > "$settings.tmp"
fi
mv -f "$settings.tmp" "$settings"

echo "Build complete. Start $dist/tt-recomp.sh. Keep this local build private."
