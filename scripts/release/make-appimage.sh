#!/usr/bin/env bash
# Copyright (c) 2026 Akhil Moola (ThisIsAkill). MIT License, see LICENSE.
# Wraps the player package in an AppImage: one file players download, mark executable and run.
#
#   scripts/release/make-appimage.sh PACKAGE.tar.gz OUT.AppImage
#
# PACKAGE is install-table-tennis-linux-x86_64.tar.gz from make-release.sh (the installer window,
# tt-chd-read and the source snapshot, nothing from the game). The AppImage holds that tree
# unchanged; its AppRun copies it to ~/.cache/tt-recomp/installer-<version> on first start and
# runs the installer from there, because container builds (podman, docker) cannot bind-mount
# the AppImage's own read-only FUSE mount.
#
# Tools: appimagetool 1.9.1 and the type2 runtime 20251108 from github.com/AppImage, kept in
# .tools/appimage/ and checked against the hashes below before use.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
package="${1:?usage: make-appimage.sh PACKAGE.tar.gz OUT.AppImage}"
out="${2:?usage: make-appimage.sh PACKAGE.tar.gz OUT.AppImage}"
tools="$root/.tools/appimage"
tool="$tools/appimagetool-x86_64.AppImage"
runtime="$tools/runtime-x86_64"
tool_url="https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage"
runtime_url="https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64"
tool_sha256="ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0"
runtime_sha256="2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d"

fetch() {  # url file sha256
  if [[ ! -f "$2" ]] || ! echo "$3  $2" | sha256sum -c --quiet - 2>/dev/null; then
    mkdir -p "$(dirname "$2")"
    curl -fsSL -o "$2.part" "$1"
    echo "$3  $2.part" | sha256sum -c --quiet - || { echo "checksum mismatch for $1" >&2; exit 1; }
    mv -f "$2.part" "$2"
  fi
}
fetch "$tool_url" "$tool" "$tool_sha256"
fetch "$runtime_url" "$runtime" "$runtime_sha256"
chmod +x "$tool"

work="$(mktemp -d "${TMPDIR:-/tmp}/tt-appimage.XXXXXX")"
trap 'rm -rf "${work:?}"' EXIT
appdir="$work/TableTennis.AppDir"
mkdir -p "$appdir"
tar -xzf "$package" -C "$appdir"
payload="$appdir/TableTennis-Installer"
[[ -x "$payload/install-table-tennis" && -f "$payload/VERSION" ]] || { echo "not a player package: $package" >&2; exit 1; }
version="$(head -n 1 "$payload/VERSION" | tr -d '[:space:]')"

# Icon: the largest PNG inside the game's .ico (the same artwork the installed game uses).
python3 -I - "$payload/project/assets/tabletennis.ico" "$appdir/table-tennis-installer.png" <<'EOF'
import struct, sys
data = open(sys.argv[1], 'rb').read()
best = None
for i in range(struct.unpack_from('<HHH', data)[2]):
    w, h, _, _, _, _, size, offset = struct.unpack_from('<BBBBHHII', data, 6 + 16 * i)
    w = w or 256
    if data[offset:offset + 4] == b'\x89PNG' and (best is None or w > best[0]):
        best = (w, data[offset:offset + size])
if best is None:
    raise SystemExit('no PNG image inside the icon')
open(sys.argv[2], 'wb').write(best[1])
EOF
ln -s table-tennis-installer.png "$appdir/.DirIcon"

cat > "$appdir/table-tennis-installer.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Table Tennis Installer
Comment=Builds Rockstar Games presents Table Tennis for Linux from your own Xbox 360 disc image
Exec=install-table-tennis
Icon=table-tennis-installer
Categories=Game;
Terminal=false
EOF

cat > "$appdir/AppRun" <<'EOF'
#!/usr/bin/env bash
# Copies the installer to the user's cache once per version, then runs it from there.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
version="$(head -n 1 "$here/TableTennis-Installer/VERSION" | tr -d '[:space:]')"
base="${XDG_CACHE_HOME:-$HOME/.cache}/tt-recomp"
dest="$base/installer-$version"
if [[ ! -f "$dest/.complete" ]]; then
  mkdir -p "$base"
  tmp="$(mktemp -d "$base/.installer-$version.XXXXXX")"
  cp -a "$here/TableTennis-Installer/." "$tmp/"
  touch "$tmp/.complete"
  rm -rf "${dest:?}"
  mv "$tmp" "$dest"
fi
exec "$dest/install-table-tennis" "$@"
EOF
chmod +x "$appdir/AppRun"

# appimagetool is itself an AppImage; extract-and-run avoids needing FUSE on the build machine.
ARCH=x86_64 "$tool" --appimage-extract-and-run --no-appstream --runtime-file "$runtime" \
  "$appdir" "$out.part" >/dev/null
mv -f "$out.part" "$out"
chmod +x "$out"
echo "AppImage: $out ($(du -h "$out" | cut -f1), installer $version)"
