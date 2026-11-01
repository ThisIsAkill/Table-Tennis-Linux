#!/usr/bin/env bash
# Builds the game-free release assets for the installer's fast path.
#
#   scripts/release/make-release.sh --out DIR [--version V] [--from-worktree] [--gui FILE]
#                                   [--jobs N] [--work DIR] [--only toolchain|sdk|package]
#
# Writes into DIR:
#   tt-toolchain-<id>.tar.zst                slim Ubuntu 24.04 system with clang 20, lld, CMake,
#                                            Ninja and Python: enough to generate and compile the
#                                            game against the prebuilt SDK (<id>: recipe hash)
#   tt-sdk-<patchset>-x86-64-v3.tar.zst      ReXGlue SDK v0.10.0 with project/sdk-patches, built in
#                                            Ubuntu 24.04 for x86-64-v3, relocatable
#   install-table-tennis-linux-x86_64.tar.gz the download for players: source snapshot, the
#                                            graphical installer and README.txt
#   manifest.json                            what tt-install.sh reads (see scripts/install/README.md)
#
# None of these contain anything from the game: the toolchain and SDK are built from public
# sources only, and the package is a snapshot of tracked files. --from-worktree snapshots the
# working tree (tracked and untracked, not ignored files) instead of HEAD, for local testing.
# The build happens in rootless Ubuntu 24.04 systems under --work (default .tools/release).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
out=""; version=""; from_worktree=0; gui=""; jobs="$(nproc)"; work="$root/.tools/release"; only=""
while (($#)); do
  case "$1" in
    --out) out="${2:?}"; shift ;;
    --version) version="${2:?}"; shift ;;
    --from-worktree) from_worktree=1 ;;
    --gui) gui="${2:?}"; shift ;;
    --jobs) jobs="${2:?}"; shift ;;
    --work) work="${2:?}"; shift ;;
    --only) only="${2:?}"; shift ;;
    -h|--help) sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
  shift
done
[[ -n "$out" ]] || { echo "--out DIR is required" >&2; exit 2; }
for tool in bwrap zstd tar git python3 sha256sum curl; do
  command -v "$tool" >/dev/null || { echo "missing $tool" >&2; exit 1; }
done
mkdir -p "$out" "$work"
out="$(realpath "$out")"; work="$(realpath "$work")"
[[ -n "$version" ]] || version="$(git -C "$root" describe --tags --always --dirty 2>/dev/null || date +%Y.%m.%d)"
patchset="$("$root/scripts/install/sdk-patchset-id.sh")"
slim_recipe="$here/toolchain-slim-recipe.sh"
toolchain_id="$(sha256sum "$slim_recipe" | cut -c1-12)"
toolchain_asset="tt-toolchain-$toolchain_id.tar.zst"
sdk_asset="tt-sdk-$patchset-x86-64-v3.tar.zst"
package_asset="install-table-tennis-linux-x86_64.tar.gz"
say() { echo "== $*"; }

# Fresh Ubuntu 24.04 base system with a recipe applied, rootless (bubblewrap + user namespace).
make_rootfs() {  # dir recipe
  local dir="$1" recipe="$2" dl="$work/downloads" base="https://cdimage.ubuntu.com/ubuntu-base/releases/24.04/release"
  mkdir -p "$dl"
  curl -fsSL -o "$dl/SHA256SUMS" "$base/SHA256SUMS"
  local line sum name
  line="$(grep -E ' \*?ubuntu-base-24\.04(\.[0-9]+)?-base-amd64\.tar\.gz$' "$dl/SHA256SUMS" | sort -V -k2 | tail -1)"
  sum="${line%% *}"; name="${line##* }"; name="${name#\*}"
  if ! echo "$sum  $dl/$name" | sha256sum -c --quiet 2>/dev/null; then
    curl -fsSL -o "$dl/$name" "$base/$name"
    echo "$sum  $dl/$name" | sha256sum -c --quiet
  fi
  if [[ -e "$dir" ]]; then chmod -R u+rwX "$dir"; rm -rf "${dir:?}"; fi
  mkdir -p "$dir"
  tar -xzf "$dl/$name" -C "$dir" --no-same-owner --no-same-permissions --exclude='./dev/*'
  chmod -R u+rwX "$dir"
  echo 'APT::Sandbox::User "root";' > "$dir/etc/apt/apt.conf.d/99tt-rootless"
  mkdir -p "$dir/opt/tt"; cp "$recipe" "$dir/opt/tt/recipe.sh"
  bwrap --bind "$dir" / --dev /dev --proc /proc --tmpfs /tmp --unshare-user --uid 0 --gid 0 \
    --unshare-pid --die-with-parent --clearenv --setenv HOME /root --setenv LANG C.UTF-8 \
    --setenv PATH /usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
    --ro-bind "$(realpath /etc/resolv.conf)" /etc/resolv.conf /bin/bash /opt/tt/recipe.sh
  rm -rf "${dir:?}/opt/tt" "${dir:?}/etc/apt/apt.conf.d/99tt-rootless"
  echo "$name $sum" > "$dir/.tt-base"
}

pack() {  # source dir, output file
  tar -C "$1" --numeric-owner --owner=0 --group=0 --sort=name -cf - . \
    | zstd -q -19 --long=27 -T0 -o "$2.part" -f
  mv -f "$2.part" "$2"
}

if [[ -z "$only" || "$only" == toolchain ]]; then
  say "Slim toolchain $toolchain_id"
  tc="$work/toolchain-$toolchain_id"
  [[ -f "$tc/.tt-base" ]] || make_rootfs "$tc" "$slim_recipe"
  pack "$tc" "$out/$toolchain_asset"
fi

if [[ -z "$only" || "$only" == sdk ]]; then
  say "SDK $patchset"
  # The installer's own SDK build (rootless Ubuntu 24.04, clang 20, x86-64-v3), without an ISO.
  "$root/scripts/install/tt-install.sh" --sdk-only --toolchain rootfs --cache "$work/sdk-cache" --jobs "$jobs" \
    | grep -E '^@@(step|error|warn)' || true
  sdk="$work/sdk-cache/build/sdk-ubuntu2404"
  [[ -x "$sdk/bin/rexglue" ]] || { echo "SDK build failed; see $work/sdk-cache/logs" >&2; exit 1; }
  stage="$work/sdk-stage"
  rm -rf "${work:?}/sdk-stage"; cp -a "$sdk" "$stage"
  # Relocatable: pkg-config files get a prefix relative to themselves; CMake package files are
  # already relative. Neither may keep the build machine's install path.
  sed -i -e 's|^prefix=/cache/sdk-ubuntu2404$|prefix=${pcfiledir}/../..|' \
    -e 's|/cache/sdk-ubuntu2404|${prefix}|g' "$stage"/lib/pkgconfig/*.pc
  if grep -rlE '/cache/|/home/|/work/' "$stage/lib/cmake" "$stage/lib/pkgconfig" "$stage/share" 2>/dev/null; then
    echo "SDK package files contain absolute build paths" >&2; exit 1
  fi
  echo "$patchset" > "$stage/.tt-sdk-patchset"
  rm -f "$stage/.tt-sdk-stamp"
  pack "$stage" "$out/$sdk_asset"
  rm -rf "${work:?}/sdk-stage"
fi

if [[ -z "$only" || "$only" == package ]]; then
  say "Player package"
  if [[ -z "$gui" ]]; then
    "$root/tools/installer/build.sh" >/dev/null
    gui="$root/tools/installer/bin/install-table-tennis"
  fi
  [[ -x "$gui" ]] || { echo "installer binary $gui missing" >&2; exit 1; }
  pkgdir="$work/package"
  rm -rf "${work:?}/package"
  top="$pkgdir/TableTennis-Installer"
  mkdir -p "$top"
  if ((from_worktree)); then
    git -C "$root" ls-files -z -co --exclude-standard | (cd "$root" && tar --null -T - -cf -) | tar -C "$top" -xf -
  else
    git -C "$root" archive HEAD | tar -C "$top" -xf -
  fi
  # Nothing from the game may ship: no disc images, executables, archives or generated code.
  bad="$(cd "$top" && find . \( -iname '*.iso' -o -iname '*.xex' -o -iname '*.rpf' -o -path '*/generated/*' \
    -o -path './private_work*' -o -path './dist*' -o -path './.tools*' -o -path './logs*' \) -print | head -5)"
  [[ -z "$bad" ]] || { echo "refusing to package game-derived files:"; echo "$bad"; exit 1; } >&2
  install -m 755 "$gui" "$top/install-table-tennis"
  cat > "$top/README.txt" <<EOF
Rockstar Games presents Table Tennis - Linux and Steam Deck installer ($version)

Double-click install-table-tennis, or run it from a terminal:

    ./install-table-tennis

Choose your own Xbox 360 Table Tennis ISO (USA/Europe, Title ID 545407DF) and press Install.
The game is built on your computer from your disc; nothing from the game is downloaded.
The first install downloads build tools (about $(( ($(stat -c %s "$out/$toolchain_asset" 2>/dev/null || echo 0) + $(stat -c %s "$out/$sdk_asset" 2>/dev/null || echo 0)) / 1048576 )) MB) once.

Command line instead of the window:

    scripts/install/tt-install.sh --iso /path/to/TableTennis.iso

Details: scripts/install/README.md and README.md. License: LICENSE and licenses/.
EOF
  tar -C "$pkgdir" --numeric-owner --owner=0 --group=0 --sort=name -czf "$out/$package_asset.part" TableTennis-Installer
  mv -f "$out/$package_asset.part" "$out/$package_asset"
  rm -rf "${work:?}/package"
fi

say "Manifest"
python3 -I - "$out" "$version" "$patchset" "$toolchain_id" "$toolchain_asset" "$sdk_asset" "$package_asset" <<'PY'
import hashlib, json, os, sys, time
out, version, patchset, toolchain_id, *names = sys.argv[1:]
kinds = ['toolchain', 'sdk', 'package']
files = []
for kind, name in zip(kinds, names):
    path = os.path.join(out, name)
    if not os.path.exists(path):
        continue
    digest = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            digest.update(block)
    files.append({'kind': kind, 'name': name, 'size': os.path.getsize(path), 'sha256': digest.hexdigest()})
manifest = {
    'format': 1,
    'version': version,
    'created': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
    'sdk_patchset': patchset,
    'toolchain_id': toolchain_id,
    'arch': 'x86_64',
    'march': 'x86-64-v3',
    'files': files,
}
with open(os.path.join(out, 'manifest.json'), 'w') as f:
    json.dump(manifest, f, indent=2)
    f.write('\n')
print(json.dumps(manifest, indent=2))
PY
