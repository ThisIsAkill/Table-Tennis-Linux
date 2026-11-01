#!/usr/bin/env bash
# Updates an installed copy of the game to the latest release, or checks for one.
#
#   tt-update.sh INSTALL_DIR [--check-only] [--release-url URL]
#
# Installed as <install dir>/tools/tt-update.sh, so updates work after the downloaded installer
# folder is gone. It reads the install's stored choices (<install dir>/.tt-install.json, written
# by tt-install.sh; the ISO path comes from tt-recomp.ini), fetches the release manifest and its
# Ed25519 signature, and refuses anything not signed with the project's key
# (release-public-key next to this script). If the release is newer than <install dir>/.tt-version
# it downloads the player package, checks its SHA256 from the manifest, unpacks it into the
# build cache and hands over to that version's tt-install.sh (fast path, atomic install; saves
# and settings are kept). Progress uses the same @@ lines as tt-install.sh.
#
# --check-only writes <install dir>/.tt-update.json (version, title, notes, published) when a
# newer release exists, removes it when not, and installs nothing. The game reads that file.
# Exit codes: 0 success or already up to date, 1 failure, 2 bad arguments.
set -uo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
install="${1:-}"; shift || true
check_only=0; release_url=""
while (($#)); do
  case "$1" in
    --check-only) check_only=1 ;;
    --release-url) release_url="${2:-}"; shift ;;
    *) echo "@@error Unknown option: $1"; exit 2 ;;
  esac
  shift
done
[[ -n "$install" && -d "$install" ]] || { echo "@@error usage: tt-update.sh INSTALL_DIR [--check-only]"; exit 2; }
install="$(realpath -e -- "$install")"
die() { echo "@@error $*"; exit 1; }
log() { echo "@@log $*"; }
command -v python3 >/dev/null || die "Updating needs python3, which is missing."
helper=(python3 -I -B "$here/tt_install.py")
pubkey="${TT_RELEASE_PUBKEY_FILE:-$here/release-public-key}"

# Stored choices from the first install.
iso=""; cache=""; steam=no; desktop=yes; stored_url=""
if [[ -f "$install/.tt-install.json" ]]; then
  eval "$(python3 -I -c '
import json, shlex, sys
d = json.load(open(sys.argv[1]))
for k in ("iso", "cache", "steam", "desktop", "release_url"):
    v = d.get(k, "")
    if isinstance(v, bool):
        v = "yes" if v else "no"
    print("%s=%s" % ("stored_url" if k == "release_url" else k, shlex.quote(str(v))))
' "$install/.tt-install.json")" || die "The install's settings file .tt-install.json cannot be read."
fi
ini_iso="$(sed -n 's/^[[:space:]]*iso_path[[:space:]]*=[[:space:]]*//p' "$install/tt-recomp.ini" 2>/dev/null | tail -1)"
[[ -n "$ini_iso" ]] && iso="${ini_iso%$'\r'}"
release_url="${release_url:-${TT_RELEASE_URL:-${stored_url:-https://github.com/ThisIsAkill/Table-Tennis-Rockstar-PC/releases/latest/download}}}"
cache="${cache:-${XDG_CACHE_HOME:-$HOME/.cache}/tt-recomp}"
installed="$(head -n 1 "$install/.tt-version" 2>/dev/null | tr -d '[:space:]')"
[[ -n "$installed" ]] || die "This install has no version (a development build); update it with tt-install.sh."

work="$cache/update"
mkdir -p "$work" || die "Cannot write to the build cache $cache."
fetch() {  # url out
  if command -v curl >/dev/null; then curl -fsSL --retry 2 --connect-timeout 5 -C - -o "$2" "$1"
  elif command -v wget >/dev/null; then wget -q -c -T 5 -O "$2" "$1"
  else python3 -I -c 'import sys, urllib.request; import socket; socket.setdefaulttimeout(5); urllib.request.urlretrieve(sys.argv[1], sys.argv[2])' "$1" "$2"; fi
}
# Signature check: the install's own installer binary (Monocypher) when it runs here, else Python.
verify() {  # manifest sig
  if [[ -x "$install/install-table-tennis" ]]; then
    "$install/install-table-tennis" --verify-manifest "$1" "$2" "$pubkey" >/dev/null 2>&1
    case $? in 0) return 0 ;; 1) return 1 ;; esac
  fi
  "${helper[@]}" verify-sig "$1" "$2" "$pubkey" >/dev/null 2>&1
}

((check_only)) || echo "@@step check Checking for the update"
rm -f "$work/manifest.json" "$work/manifest.json.sig"
fetch "$release_url/manifest.json" "$work/manifest.json" 2>/dev/null \
  || die "Cannot reach the release at $release_url. Check your internet connection and try again."
fetch "$release_url/manifest.json.sig" "$work/manifest.json.sig" 2>/dev/null \
  || die "The release at $release_url has no signature (manifest.json.sig); refusing to use it."
verify "$work/manifest.json" "$work/manifest.json.sig" \
  || die "The release manifest's signature does not match this project's key; it may have been tampered with. Nothing was changed."
eval "$("${helper[@]}" manifest-get "$work/manifest.json")" || die "The release manifest cannot be read."

if ! "${helper[@]}" newer "$release_version" "$installed"; then
  ((check_only)) && { rm -f "$install/.tt-update.json"; exit 0; }
  log "Version $installed is up to date (latest release: $release_version)."
  echo "@@result update none"
  echo "@@done $install"
  exit 0
fi
if ((check_only)); then
  "${helper[@]}" update-json "$work/manifest.json" "$install/.tt-update.json" || exit 1
  exit 0
fi

log "Updating from $installed to $release_version."
[[ -n "${pkg_name:-}" ]] || die "The release $release_version has no installer package."
[[ -f "$iso" ]] || die "The game's disc image is not at $iso any more. Put it back (or set iso_path in tt-recomp.ini) and try again."
part="$work/$pkg_name.part"
fetch "$release_url/$pkg_name" "$part" || die "Downloading $pkg_name failed. Try again later; the download continues where it stopped."
echo "$pkg_sha  $part" | sha256sum -c --quiet >/dev/null 2>&1 \
  || { rm -f "$part"; die "The downloaded $pkg_name does not match the release manifest; it was deleted. Try again."; }
dest="$work/$release_version"
for old in "$work"/*/; do
  [[ -d "$old" && "${old%/}" != "$dest" ]] && rm -rf "${old:?}"
done
rm -rf "${work:?}/unpack"
mkdir -p "$work/unpack"
tar -xzf "$part" -C "$work/unpack" --no-same-owner || die "Unpacking $pkg_name failed."
rm -f "$part"
[[ -e "$dest" ]] && rm -rf "${dest:?}"
mv "$work/unpack" "$dest"
new="$dest/TableTennis-Installer/scripts/install/tt-install.sh"
[[ -x "$new" ]] || die "The downloaded package has no installer script."
log "Verified and unpacked version $release_version; building it."
exec "$new" --iso "$iso" --prefix "$install" --cache "$cache" --steam "$steam" --desktop "$desktop" \
  --release-url "$release_url"
