#!/usr/bin/env bash
# Portable Linux build for the Steam Deck and other distros older than the build machine.
#
#   scripts/deck/build-deck.sh [--jobs N]
#
# Needs a normal ./build.sh run first (it prepares the patched SDK source in
# .tools/rexglue-src and the generated game code). Builds the SDK and the game inside the
# localhost/tt-recomp-deck image (scripts/deck/make-image.sh), bundles libstdc++/libgcc next
# to the game, and writes dist-deck/ plus logs/build/tt-recomp-deck.tar.xz (stripped). The package holds
# no ISO data: setup-deck.sh extracts the menu archive from your own ISO on the target.
# Keep the package private, like any local build.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
jobs="$(nproc)"
[[ "${1:-}" == "--jobs" ]] && jobs="${2:?--jobs needs a number}"
[[ -f "$root/.tools/rexglue-sdk/.tt-sdk-patches" ]] || { echo "run ./build.sh first"; exit 1; }
[[ -d "$root/project/generated/default" ]] || { echo "run ./build.sh first"; exit 1; }

"$root/scripts/deck/podman.sh" run --rm --userns=keep-id -v "$root:/work:Z" -w /work \
  localhost/tt-recomp-deck:latest bash -euo pipefail -c '
jobs='"$jobs"'
flags="-march=x86-64-v3"
sdk=/work/.tools/deck/sdk
if [[ "$(cat "$sdk/.tt-sdk-patches" 2>/dev/null)" != "$(cat /work/.tools/rexglue-sdk/.tt-sdk-patches)" ]]; then
  cmake -S /work/.tools/rexglue-src -B /work/.tools/deck/sdk-build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_C_FLAGS="$flags" -DCMAKE_CXX_FLAGS="$flags" -DCMAKE_CXX_STANDARD=23 \
    -DCMAKE_INSTALL_PREFIX="$sdk"
  cmake --build /work/.tools/deck/sdk-build --parallel "$jobs"
  cmake --install /work/.tools/deck/sdk-build
  cp /work/.tools/rexglue-sdk/.tt-sdk-patches "$sdk/.tt-sdk-patches"
fi
cmake -S /work/project -B /work/.tools/deck/game-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=clang++ "-DCMAKE_PREFIX_PATH=$sdk"
cmake --build /work/.tools/deck/game-build --parallel "$jobs"
# Bundle the C++ runtime; the game finds it through its $ORIGIN runpath.
mkdir -p /work/.tools/deck/runtime
cp -L "$(clang++ -print-file-name=libstdc++.so.6)" "$(clang++ -print-file-name=libgcc_s.so.1)" \
  /work/.tools/deck/runtime/
'

out="$root/dist-deck"
mkdir -p "$out/tools" "$out/licenses" "$out/fonts"
install -m 644 "$root"/project/assets/fonts/*.ttf "$out/fonts/"
b="$root/.tools/deck/game-build"
install -m 755 "$b/tabletennis" "$b/librexruntime.so" "$b/librexgpu-xenos.so" "$out/"
install -m 644 "$root"/.tools/deck/runtime/* "$out/"
install -m 755 "$root/project/launcher/tt-recomp.sh" "$root/scripts/deck/setup-deck.sh" "$out/"
install -m 644 "$root/project/assets/tabletennis.ico" "$out/"
install -m 644 "$root/project/tools/disc_image.py" "$root/project/tools/patch_menu.py" "$out/tools/"
cp -r "$root/licenses/." "$out/licenses/"
[[ -d "$root/.tools/deck/sdk/share/licenses" ]] && cp -r "$root/.tools/deck/sdk/share/licenses" "$out/licenses/SDK"

# Debug symbols are not needed on the target and triple the download size.
strip --strip-unneeded "$out/tabletennis" "$out"/*.so "$out"/*.so.*
echo "Highest glibc symbol needed:"
for f in "$out/tabletennis" "$out/librexruntime.so" "$out/librexgpu-xenos.so"; do
  printf '  %-20s %s\n' "$(basename "$f")" "$(objdump -T "$f" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)"
done
pkg="$root/logs/build/tt-recomp-deck.tar.xz"
tar -C "$root" --exclude=dist-deck/assets --exclude=dist-deck/tt-recomp.ini -cf - dist-deck \
  | xz -9e -T0 > "$pkg"
echo "Package: $pkg ($(du -h "$pkg" | cut -f1))"
