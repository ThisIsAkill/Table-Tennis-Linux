#!/usr/bin/env bash
# Build stage of scripts/install/tt-install.sh. Runs inside the build environment (container,
# rootless Ubuntu base system, or the host) and is not meant to be started by hand.
#
# Environment:
#   TT_SRC    project source tree (read only)        TT_ISO    the retail ISO (read only)
#   TT_CACHE  persistent build cache (writable)       TT_STAGE  staging folder for the install
#   TT_JOBS   parallel compile jobs                   TT_KIND   cache flavour (ubuntu2404 | host | fast)
#   TT_PREBUILT_SDK  use this installed SDK instead of building one (fast path)
#   TT_SDK_ONLY=1    stop after building the SDK (release builds; no ISO needed)
#   TT_GENERATED_OPT optimization flag for the generated game code only (e.g. -O2; default: -O3)
#
# Prints the installer's @@ progress lines (see tt-install.sh); Ninja's "[n/total]" lines are
# turned into @@progress by tt-install.sh.
set -euo pipefail

sdk_only="${TT_SDK_ONLY:-0}"; prebuilt="${TT_PREBUILT_SDK:-}"
src="${TT_SRC:?}"; iso="${TT_ISO:-}"; cache="${TT_CACHE:?}"; stage="${TT_STAGE:?}"
jobs="${TT_JOBS:?}"; kind="${TT_KIND:?}"
rexglue_repo="https://github.com/rexglue/rexglue-sdk"
rexglue_tag="v0.10.0"
rexglue_commit="f5337cdc947ff6d4c4196737e2c807a48f2a1fc2"
# AVX2 (x86-64-v3) like the Steam Deck build; the recompiled game needs AVX2 anyway.
flags="-march=x86-64-v3"

helper=(python3 -I -B "$src/scripts/install/tt_install.py")
work="$cache/work"
sdk_src="$cache/rexglue-src"
sdk="$cache/sdk-$kind"
sdk_build="$cache/sdk-build-$kind"
game_build="$cache/game-build-$kind"
mkdir -p "$work" "$stage"
# Git refuses repositories owned by another uid (docker, user namespaces) without this.
export GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=safe.directory GIT_CONFIG_VALUE_0='*'

step() { echo "@@step $1 $2"; }
log() { echo "@@log $*"; }
fail() { echo "@@error $*"; exit 1; }

if [[ "$sdk_only" != 1 ]]; then
  step extract "Reading your game disc"
  "${helper[@]}" extract "$iso" "$src/project/tools" "$work/private_work/default.xex" \
    "$stage/assets/resource/flash/flash.rpf" || fail "Your ISO could not be read or is not the supported USA/Europe retail release (Title ID 545407DF)."
fi

if [[ -n "$prebuilt" ]]; then
step sdk "Getting the engine"
sdk="$prebuilt"
[[ -x "$sdk/bin/rexglue" && "$("$sdk/bin/rexglue" --version)" == 0.10.0 ]] || fail "The prebuilt SDK is damaged. Delete the cache folder's fast/ directory and run the installer again."
log "Using the prebuilt ReXGlue SDK 0.10.0 (patch set $(cat "$sdk/.tt-sdk-patchset" 2>/dev/null))."
else
step sdk "Building the ReXGlue SDK"
# Fixes to the SDK live in project/sdk-patches; a changed set (or toolchain) rebuilds the SDK.
patches=("$src"/project/sdk-patches/*.patch)
sdk_options=(-DREXGLUE_ENABLE_FIDELITYFX=OFF -DCMAKE_CXX_STANDARD=23)
stamp="$({ cat "${patches[@]}"; echo "${sdk_options[*]} $flags $kind"; clang++ --version | head -1; } \
  | sha256sum | cut -c1-16)"
if [[ -x "$sdk/bin/rexglue" && "$(cat "$sdk/.tt-sdk-stamp" 2>/dev/null)" == "$stamp" ]]; then
  log "SDK already built (cached)."
else
  if [[ ! -d "$sdk_src/.git" || "$(git -C "$sdk_src" rev-parse HEAD 2>/dev/null)" != "$rexglue_commit" ]]; then
    echo "@@substep sdk-download"
    log "Downloading ReXGlue SDK $rexglue_tag source."
    tmp="$cache/rexglue-src.download"
    [[ -e "$tmp" ]] && rm -rf "${cache:?}/rexglue-src.download"
    git clone --quiet --depth 1 --branch "$rexglue_tag" "$rexglue_repo" "$tmp" \
      || fail "Downloading the ReXGlue SDK failed. Check your internet connection and try again."
    [[ "$(git -C "$tmp" rev-parse HEAD)" == "$rexglue_commit" ]] \
      || fail "The downloaded ReXGlue SDK is not the expected $rexglue_tag release."
    [[ -e "$sdk_src" ]] && rm -rf "${cache:?}/rexglue-src"
    mv "$tmp" "$sdk_src"
  fi
  echo "@@substep sdk-download"
  # Submodules: resumable, shallow where the server allows it.
  total="$(git -C "$sdk_src" config --file .gitmodules --get-regexp '\.path$' | wc -l)"
  n=0
  while read -r _ path; do
    n=$((n + 1))
    if [[ ! -e "$sdk_src/$path/.git" ]]; then
      git -C "$sdk_src" submodule update --init --quiet --depth 1 -- "$path" 2>/dev/null \
        || git -C "$sdk_src" submodule update --init --quiet -- "$path" \
        || fail "Downloading SDK component $path failed. Check your internet connection and try again."
    fi
    echo "[$n/$total] submodule $path"
  done < <(git -C "$sdk_src" config --file .gitmodules --get-regexp '\.path$')
  git -C "$sdk_src" submodule update --init --recursive --quiet \
    || fail "Downloading SDK components failed. Check your internet connection and try again."
  git -C "$sdk_src" checkout -q -- .
  git -C "$sdk_src" clean -fdq
  for p in "${patches[@]}"; do
    git -C "$sdk_src" apply "$p" || fail "SDK patch $(basename "$p") does not apply."
    log "Applied SDK patch $(basename "$p")."
  done
  echo "@@substep sdk-build"
  cmake -S "$sdk_src" -B "$sdk_build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_C_FLAGS="$flags" -DCMAKE_CXX_FLAGS="$flags" "${sdk_options[@]}" \
    -DCMAKE_INSTALL_PREFIX="$sdk" -DCMAKE_FIND_PACKAGE_NO_PACKAGE_REGISTRY=ON \
    || fail "Configuring the SDK build failed."
  cmake --build "$sdk_build" --parallel "$jobs" || fail "Building the SDK failed (see the log)."
  rm -f "$sdk/.tt-sdk-stamp"
  cmake --install "$sdk_build" >/dev/null || fail "Installing the SDK into the build cache failed."
  echo "$stamp" > "$sdk/.tt-sdk-stamp"
fi
[[ "$("$sdk/bin/rexglue" --version)" == 0.10.0 ]] || fail "The cached SDK is not ReXGlue 0.10.0."
fi
if [[ "$sdk_only" == 1 ]]; then
  log "SDK ready: $sdk"
  exit 0
fi

step codegen "Generating the game code"
# A private copy of the project: code generation writes next to the manifest, and the source
# tree stays untouched (it may be read only).
"${helper[@]}" sync-tree "$src/project" "$work/project" generated installer
gen="$work/project/generated"
codegen_stamp="$({ sha256sum "$work/private_work/default.xex" | cut -c1-64
  cat "$work/project/tabletennis_manifest.toml" "$work/project/overrides.toml" \
    "$work/project/tools/native_edits.json" "$work/project/tools/patch_native.py"
  cat "$sdk/.tt-sdk-stamp" "$sdk/.tt-sdk-patchset" 2>/dev/null || true; } | sha256sum | cut -c1-16)"
if [[ -d "$gen/default" && "$(cat "$work/codegen.stamp" 2>/dev/null)" == "$codegen_stamp" ]]; then
  log "Generated code is up to date (cached)."
else
  rm -f "$work/codegen.stamp"
  [[ -e "$work/generated.prev" ]] && rm -rf "${work:?}/generated.prev"
  [[ -d "$gen" ]] && mv "$gen" "$work/generated.prev"
  (cd "$work/project" && "$sdk/bin/rexglue" codegen tabletennis_manifest.toml) \
    || fail "Code generation failed."
  python3 -I -B -c 'import sys; sys.path.insert(0, sys.argv[1]); from patch_native import patch; patch(sys.argv[2])' \
    "$work/project/tools" "$gen/default" || fail "The verified native edits did not apply."
  if [[ -d "$work/generated.prev" ]]; then
    "${helper[@]}" keep-mtimes "$gen" "$work/generated.prev"
    rm -rf "${work:?}/generated.prev"
  fi
  echo "$codegen_stamp" > "$work/codegen.stamp"
fi

step build "Compiling the game"
# The binaries are stripped when packaged, so the generated code is compiled without debug
# line tables (about 7% faster; debug info never changes the generated machine code).
cmake -S "$work/project" -B "$game_build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=clang++ "-DCMAKE_PREFIX_PATH=$sdk" -DCMAKE_FIND_PACKAGE_NO_PACKAGE_REGISTRY=ON \
  -DREXGLUE_RECOMP_DEBUG_INFO=none "-DTT_GENERATED_OPT=${TT_GENERATED_OPT:-}" \
  >/dev/null || fail "Configuring the game build failed."
cmake --build "$game_build" --parallel "$jobs" || fail "Compiling the game failed (see the log)."

log "Packaging the game."
# Same layout as the Steam Deck package (scripts/deck/build-deck.sh).
mkdir -p "$stage/fonts" "$stage/licenses" "$stage/tools"
install -m 755 "$game_build/tabletennis" "$game_build/librexruntime.so" "$game_build/librexgpu-xenos.so" "$stage/"
# The C++ runtime the game was built against, found through its $ORIGIN runpath.
install -m 644 "$(clang++ -print-file-name=libstdc++.so.6)" "$(clang++ -print-file-name=libgcc_s.so.1)" "$stage/"
install -m 755 "$src/project/launcher/tt-recomp.sh" "$stage/"
install -m 644 "$src/project/assets/tabletennis.ico" "$stage/"
install -m 644 "$src"/project/assets/fonts/*.ttf "$stage/fonts/"
install -m 644 "$src/project/tools/disc_image.py" "$src/project/tools/patch_menu.py" "$stage/tools/"
# The updater and what it needs, so updates work after the downloaded installer folder is gone.
install -m 755 "$src/scripts/install/tt-update.sh" "$stage/tools/"
install -m 644 "$src/scripts/install/tt_install.py" "$src/scripts/install/release-public-key" "$stage/tools/"
for gui in "$src/install-table-tennis" "$src/tools/installer/bin/install-table-tennis"; do
  if [[ -x "$gui" ]]; then install -m 755 "$gui" "$stage/install-table-tennis"; break; fi
done
cp -r "$src/licenses/." "$stage/licenses/"
if [[ -d "$sdk/share/licenses" ]]; then
  mkdir -p "$stage/licenses/SDK"
  cp -r "$sdk/share/licenses/." "$stage/licenses/SDK/"
fi
if command -v strip >/dev/null; then
  strip --strip-unneeded "$stage/tabletennis" "$stage"/*.so "$stage"/*.so.*
fi
"${helper[@]}" artwork "$src/project/assets/tabletennis.ico" "$stage" >/dev/null \
  || echo "@@warn The icon could not be converted; the shortcut will have no icon."
glibc="$(objdump -T "$stage/tabletennis" "$stage"/librex*.so 2>/dev/null | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1 || true)"
[[ -n "$glibc" ]] && log "Needs ${glibc/_/ } or newer."
touch "$stage/.tt-stage-complete"
