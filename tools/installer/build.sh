#!/usr/bin/env bash
# Copyright (c) 2026 Akhil Moola (ThisIsAkill). MIT License, see LICENSE.
# Builds the graphical installer as one portable binary for release.
#
#   tools/installer/build.sh            build in the Ubuntu 24.04 container (glibc 2.39, runs on SteamOS)
#   tools/installer/build.sh --host     build with this machine's compiler (for development)
#   tools/installer/build.sh --jobs N   parallel jobs (default: all cores)
#
# Output: tools/installer/bin/install-table-tennis. Fonts and the icon are embedded, so the file
# is all that ships; it finds scripts/install/tt-install.sh by walking up from where it sits, so
# place it at the root of the source tree or leave it in tools/installer/bin.
#
# The container image is the one scripts/deck/make-image.sh creates (localhost/tt-recomp-deck);
# SDL3 and Dear ImGui are compiled from .tools/rexglue-src/thirdparty, which ./build.sh checks
# out. Build directories: tools/installer/build/{host,deck} (gitignored).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
jobs="$(nproc)"
mode=container
while [[ $# -gt 0 ]]; do
  case "$1" in
    --host) mode=host; shift ;;
    --jobs) jobs="${2:?--jobs needs a number}"; shift 2 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
[[ -d "$root/.tools/rexglue-src/thirdparty/sdl3" ]] || { echo "run ./build.sh once first: it fetches the SDK sources the installer builds SDL3 and ImGui from" >&2; exit 1; }

configure_and_build() {  # build dir, extra cmake args...
  local build="$1"; shift
  cmake -S "$here" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ "$@"
  cmake --build "$build" --parallel "$jobs"
}

if [[ "$mode" == host ]]; then
  configure_and_build "$here/build/host"
else
  # The binary targets x86-64-v3 like the game; SteamOS and every Steam Deck support it.
  "$root/scripts/deck/podman.sh" run --rm --userns=keep-id -v "$root:/work:Z" -w /work \
    localhost/tt-recomp-deck:latest bash -euo pipefail -c '
cmake -S /work/tools/installer -B /work/tools/installer/build/deck -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_FLAGS="-march=x86-64-v3" -DCMAKE_CXX_FLAGS="-march=x86-64-v3"
cmake --build /work/tools/installer/build/deck --parallel '"$jobs"'
strip --strip-unneeded /work/tools/installer/bin/install-table-tennis /work/tools/installer/bin/tt-chd-read
'
fi

bin="$here/bin/install-table-tennis"
echo "Built: $bin ($(du -h "$bin" | cut -f1))"
echo "Shared libraries:"
ldd "$bin" | sed 's/^/  /'
echo "Highest glibc symbol needed: $(objdump -T "$bin" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)"
