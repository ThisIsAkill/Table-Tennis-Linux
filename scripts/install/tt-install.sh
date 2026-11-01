#!/usr/bin/env bash
# Table Tennis for Linux / Steam Deck: one-command installer.
#
#   scripts/install/tt-install.sh --iso PATH [--prefix DIR] [--cache DIR] [--jobs N]
#                                 [--steam yes|no] [--desktop yes|no] [--keep-build]
#                                 [--toolchain auto|podman|docker|host|rootfs] [--install-podman]
#                                 [--release-url URL] [--offline] [--no-prebuilt]
#
# Builds the game on this machine from your own retail ISO (USA/Europe, Title ID 545407DF) and
# installs it. Nothing from the disc is downloaded or shipped; the ISO is only read.
#
#   --iso PATH        your Xbox 360 Table Tennis ISO (required; kept in place, the game reads it)
#   --prefix DIR      install folder (default ~/Games/TableTennis); any writable folder, also on
#                     an SD card or external drive, as long as it allows running programs
#   --cache DIR       build tools, SDK and build files, about 6 GB (default ~/.cache/tt-recomp);
#                     must be a Linux filesystem (ext4, btrfs, xfs...), not exFAT/FAT
#   --jobs N          parallel compile jobs (default: CPU threads, limited by memory)
#   --steam yes|no    add "Table Tennis" to Steam as a non-Steam game (default no)
#   --desktop yes|no  add an application menu entry (default yes)
#   --keep-build      keep the SDK source and its build tree in the cache (faster SDK updates,
#                     about 2 GB more); the SDK itself and the game build are always kept
#   --toolchain T     how to get compilers (default auto: podman, docker, host tools, then a
#                     rootless Ubuntu 24.04 base system downloaded into the cache)
#   --install-podman  if nothing else works, install podman with the system package manager
#                     (asks for the administrator password through pkexec)
#   --release-url URL where to get the prebuilt build tools and SDK (manifest.json and the files
#                     it names; default: this project's latest GitHub release); file:// works
#   --offline         do not download anything; use what the cache already holds
#   --no-prebuilt     build the toolchain and SDK on this computer instead (slow path)
#
# Fast path (default): download a slim Ubuntu 24.04 toolchain and the prebuilt ReXGlue SDK once
# (game-free, SHA256-checked against the release manifest, about 120 MB), then generate and
# compile the game inside that toolchain with bubblewrap or unshare; no container runtime or
# administrator rights needed. If the release cannot be used (offline without a cached copy, no
# user namespaces, an SDK for other patches), the installer says why (@@warn) and builds the
# toolchain and SDK itself (podman, docker, host tools or a rootless Ubuntu system), which takes
# longer.
#
# Re-running continues after a failure or cancel and updates an existing install; settings
# (tt-recomp.ini) and logs in the install folder are kept. The new build is staged in the cache,
# copied to DIR/.tt-staging and only moved into place (renames) once complete.
#
# Environment: TT_GENERATED_OPT=-O2 (optimization of the generated game code; default -O3);
# TT_RELEASE_URL (same as --release-url); TT_INSTALL_FORCE=<toolchain> (same as --toolchain); TT_USERNS=unshare (rootfs:
# use unshare even if bubblewrap works); TT_CONTAINER="cmd args"
# replaces the podman/docker command (e.g. with custom --root/--storage-driver options).
#
# ---- Progress contract (stdout, one message per line; other lines are plain text) ----------
#   @@step <id> <title>    a stage begins. Ids, in order:
#                            check     Checking your system
#                            tools     Getting build tools        (fast path downloads, or the
#                                      rootless toolchain)
#                            image     Preparing the build container (podman/docker, first run)
#                            extract   Reading your game disc
#                            sdk       Getting the engine (prebuilt) / Building the ReXGlue SDK
#                            codegen   Generating the game code
#                            build     Compiling the game
#                            install   Installing
#                            integrate Adding shortcuts
#                          tools/image appear at most once (whichever applies, or neither for
#                          host tools), except that tools can repeat once when the fast path
#                          falls back to the slow one; every other id appears once per run.
#   @@progress <0-100>     overall percent, integer, never decreases
#   @@log <text>           optional detail line
#   @@warn <text>          something the user should know; installation continues
#   @@error <text>         plain-language failure; the script then exits non-zero
#   @@result steam added|requested|none   outcome of --steam (none: not asked for, or failed; a @@warn says why)
#   @@done <dir>           success; <dir> is the install folder (start <dir>/tt-recomp.sh)
# Exit codes: 0 success, 1 failure, 2 bad arguments, 130 cancelled (SIGINT/SIGTERM).
# The full build log is written to <cache>/logs/; its path is announced with @@log.
set -uo pipefail
# Job control: each background build gets its own process group, so cancel can stop all of it.
set -m

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="$(cd "$here/../.." && pwd)"

iso=""
prefix="$HOME/Games/TableTennis"
cache="${XDG_CACHE_HOME:-$HOME/.cache}/tt-recomp"
jobs=""
steam="no"
desktop="yes"
keep_build=0
toolchain="${TT_INSTALL_FORCE:-auto}"
install_podman=0
release_url="${TT_RELEASE_URL:-https://github.com/ThisIsAkill/Table-Tennis-Rockstar-PC/releases/latest/download}"
offline=0
no_prebuilt=0
sdk_only=0

say() { echo "$*"; }
log() { echo "@@log $*"; [[ -n "${logfile:-}" ]] && echo "$*" >> "$logfile"; return 0; }
warn() { echo "@@warn $*"; [[ -n "${logfile:-}" ]] && echo "WARNING: $*" >> "$logfile"; return 0; }
die() {
  echo "@@error $*"
  [[ -n "${logfile:-}" ]] && echo "ERROR: $*" >> "$logfile"
  exit 1
}
usage() { sed -n '2,29p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }
bad_arg() { usage >&2; echo "@@error $*"; exit 2; }

while (($#)); do
  case "$1" in
    --iso) iso="${2:-}"; shift ;;
    --iso=*) iso="${1#*=}" ;;
    --prefix) prefix="${2:-}"; shift ;;
    --prefix=*) prefix="${1#*=}" ;;
    --cache) cache="${2:-}"; shift ;;
    --cache=*) cache="${1#*=}" ;;
    --jobs|-j) jobs="${2:-}"; shift ;;
    --jobs=*) jobs="${1#*=}" ;;
    --steam) steam="${2:-}"; shift ;;
    --steam=*) steam="${1#*=}" ;;
    --desktop) desktop="${2:-}"; shift ;;
    --desktop=*) desktop="${1#*=}" ;;
    --keep-build) keep_build=1 ;;
    --toolchain) toolchain="${2:-}"; shift ;;
    --toolchain=*) toolchain="${1#*=}" ;;
    --install-podman) install_podman=1 ;;
    --release-url) release_url="${2:-}"; shift ;;
    --release-url=*) release_url="${1#*=}" ;;
    --offline) offline=1 ;;
    --no-prebuilt) no_prebuilt=1 ;;
    --sdk-only) sdk_only=1 ;;  # for scripts/release/make-release.sh: build the SDK only, no ISO
    -h|--help) usage; exit 0 ;;
    *) bad_arg "Unknown option: $1" ;;
  esac
  shift
done
((sdk_only)) || [[ -n "$iso" ]] || bad_arg "Choose your Table Tennis ISO with --iso PATH."
[[ -n "$prefix" && -n "$cache" ]] || bad_arg "--prefix and --cache need a folder."
case "$steam" in yes|no) ;; *) bad_arg "--steam must be yes or no." ;; esac
case "$desktop" in yes|no) ;; *) bad_arg "--desktop must be yes or no." ;; esac
case "$toolchain" in auto|podman|docker|host|rootfs) ;; *) bad_arg "--toolchain must be auto, podman, docker, host or rootfs." ;; esac
[[ -z "$jobs" || "$jobs" =~ ^[0-9]+$ && "$jobs" -ge 1 && "$jobs" -le 64 ]] || bad_arg "--jobs must be a number from 1 to 64."

# ---- progress -----------------------------------------------------------------------------
# Overall percent ranges per step; Ninja's [n/total] moves the bar inside the current range.
declare -A range_lo=([check]=0 [tools]=2 [image]=2 [extract]=20 [sdk]=22 [sdk-download]=22
  [sdk-build]=32 [codegen]=72 [build]=76 [install]=95 [integrate]=98)
declare -A range_hi=([check]=2 [tools]=20 [image]=20 [extract]=22 [sdk]=72 [sdk-download]=32
  [sdk-build]=72 [codegen]=76 [build]=95 [install]=98 [integrate]=100)
fast_ranges() {  # downloads instead of an SDK build: the game compile is most of the time
  range_lo=([check]=0 [tools]=2 [extract]=35 [sdk]=37 [codegen]=38 [build]=46 [install]=95 [integrate]=98)
  range_hi=([check]=2 [tools]=35 [extract]=37 [sdk]=38 [codegen]=46 [build]=95 [install]=98 [integrate]=100)
}
percent=-1
progress() {
  local p="$1"
  ((p > 100)) && p=100
  if ((p > percent)); then percent=$p; echo "@@progress $p"; fi
}
cur=check; sub=""
step() {
  cur="$1"; sub=""
  echo "@@step $1 $2"
  [[ -n "${logfile:-}" ]] && echo "==== $2" >> "$logfile"
  progress "${range_lo[$1]}"
}

# Reads build output: everything goes to the log; @@ lines pass through; [n/total] -> percent;
# in the tools/image step, apt and download lines move the bar towards an expected count.
recent=()  # last build output lines, shown when a step fails
filter() {
  local line n=0 lo hi key a b
  recent=()
  while IFS= read -r line; do
    echo "$line" >> "$logfile"
    case "$line" in
      "@@step "*)
        read -r _ key _ <<< "$line"
        cur="$key"; sub=""; echo "$line"; progress "${range_lo[$key]:-$percent}" ;;
      "@@substep "*) sub="${line#@@substep }"; progress "${range_lo[$sub]}" ;;
      "@@error "*) last_error="${line#@@error }" ;;
      @@*) echo "$line" ;;
      "["[0-9]*/[0-9]*"]"*)
        a="${line#[}"; a="${a%%/*}"; b="${line#*/}"; b="${b%%]*}"
        [[ "$a" =~ ^[0-9]+$ && "$b" =~ ^[0-9]+$ ]] && ((b > 0)) || continue
        key="${sub:-$cur}"; lo="${range_lo[$key]:-0}"; hi="${range_hi[$key]:-0}"
        progress $((lo + (hi - lo) * a / b)) ;;
      *)
        recent+=("$line"); ((${#recent[@]} > 25)) && recent=("${recent[@]:1}")
        if [[ "$cur" == tools || "$cur" == image ]]; then
          n=$((n + 1))
          ((n % 20 == 0)) && progress $((range_lo[$cur] + (range_hi[$cur] - range_lo[$cur]) * (n < 1400 ? n : 1400) / 1500))
        fi ;;
    esac
  done
}

# ---- cancel handling ----------------------------------------------------------------------
child=""; container_name=""; in_swap=0
cleanup_child() {
  if [[ -n "$container_name" ]]; then "${ct[@]}" kill "$container_name" >/dev/null 2>&1; "${ct[@]}" rm -f "$container_name" >/dev/null 2>&1; fi
  if [[ -n "$child" ]]; then kill -TERM -- "-$child" 2>/dev/null || kill -TERM "$child" 2>/dev/null; fi
}
on_cancel() {
  ((in_swap)) && { cancel_pending=1; return; }
  cleanup_child
  echo "@@error Installation cancelled. Nothing was changed in the install folder; run the installer again to continue."
  exit 130
}
cancel_pending=0
trap on_cancel INT TERM HUP

# Runs a command in its own process group with output through filter(); returns its status.
fifo=""
run_filtered() {
  [[ -n "$fifo" ]] || { fifo="$(mktemp -u "${TMPDIR:-/tmp}/tt-install.XXXXXX")"; mkfifo "$fifo"; }
  last_error=""
  "$@" > "$fifo" 2>&1 < /dev/null &
  child=$!
  filter < "$fifo"
  wait "$child"; local rc=$?
  child=""
  return $rc
}
cleanup_exit() { [[ -n "$fifo" ]] && rm -f "$fifo"; }
trap cleanup_exit EXIT

# ---- 1. check -----------------------------------------------------------------------------
mkdir -p "$cache/logs" 2>/dev/null || { echo "@@error Cannot create the build cache folder $cache. Choose another one with --cache."; exit 1; }
logfile="$cache/logs/install-$(date +%Y%m%d-%H%M%S).log"
: > "$logfile" || { echo "@@error Cannot write to the build cache folder $cache."; exit 1; }
ls -1t "$cache"/logs/install-*.log 2>/dev/null | tail -n +6 | while read -r old; do rm -f "$old"; done
step check "Checking your system"
log "Full log: $logfile"
say "Table Tennis installer - source $src"

[[ "$(uname -s)" == Linux && "$(uname -m)" == x86_64 ]] || die "This game needs a 64-bit x86 Linux PC or a Steam Deck."
grep -qw avx2 /proc/cpuinfo || die "Your processor does not support AVX2, which the game needs (any Steam Deck and most PCs from 2013 on have it)."

if ((sdk_only)); then iso=/dev/null; prefix="$cache"; fi
if ((!sdk_only)); then
iso="$(realpath -e -- "$iso" 2>/dev/null)" || die "The ISO file was not found. Check the path and try again."
[[ -f "$iso" && -r "$iso" ]] || die "The ISO file cannot be read: $iso"
# Host python3 verifies the ISO now (else the build environment does it in the extract step).
if command -v python3 >/dev/null; then
  python3 -I -B "$here/tt_install.py" verify "$iso" "$src/project/tools" >> "$logfile" 2>&1 \
    || die "This ISO is not the supported game. Use your own unmodified USA/Europe retail Xbox 360 Table Tennis ISO (Title ID 545407DF)."
  log "ISO verified: supported retail release."
fi
fi
prefix="$(realpath -m -- "$prefix")"
cache="$(realpath -m -- "$cache")"
case "$prefix/" in "$src/"*) die "Choose an install folder outside the installer's own folder." ;; esac
[[ "$prefix" != / && "$prefix" != "$HOME" ]] || die "Choose a dedicated install folder, for example $HOME/Games/TableTennis."

on_steamos=0
grep -qsE '^ID=steamos' /etc/os-release && on_steamos=1
is_deck=0
if [[ "${SteamDeck:-}" == 1 ]] || grep -qsxE 'Jupiter|Galileo' /sys/class/dmi/id/product_name; then is_deck=1; fi
log "System: $(. /etc/os-release 2>/dev/null; echo "${PRETTY_NAME:-Linux}")$( ((is_deck)) && echo ', Steam Deck')."

# Install folder: must exist (create it), be writable and allow running programs.
if ((!sdk_only)); then
mkdir -p -- "$prefix" 2>/dev/null || die "Cannot create the install folder $prefix. Choose a folder you can write to."
probe="$prefix/.tt-exec-test"
printf '#!/bin/sh\nexit 0\n' > "$probe" 2>/dev/null || die "Cannot write to the install folder $prefix. Choose a folder you can write to."
chmod 755 "$probe" 2>/dev/null
if ! "$probe" 2>/dev/null; then
  rm -f "$probe"
  die "The install folder $prefix is on a drive that does not allow running programs (mounted noexec, or a FAT/exFAT/NTFS drive without execute permission). Choose a folder on internal storage or an ext4-formatted SD card."
fi
rm -f "$probe"
fstype() { stat -f -c %T -- "$1" 2>/dev/null; }
case "$(fstype "$prefix")" in
  exfat|msdos|vfat|fuseblk|ntfs*) warn "The install folder is on a $(fstype "$prefix") drive. It works, but ext4 is more reliable for saves; avoid removing the card while the game runs." ;;
esac
fi

# Cache: needs a Linux filesystem (symbolic links, permissions) because the SDK and toolchain live there.
probe="$cache/.tt-fs-test"
rm -f "$probe" "$probe.l"
if ! { printf '#!/bin/sh\nexit 0\n' > "$probe" && chmod 755 "$probe" && "$probe" && ln -s "$probe" "$probe.l"; } 2>/dev/null; then
  rm -f "$probe" "$probe.l"
  die "The build cache folder $cache must be on a Linux filesystem (ext4, btrfs, xfs) that allows programs and symbolic links; FAT/exFAT/NTFS drives do not work. Choose another folder with --cache."
fi
rm -f "$probe" "$probe.l"
for p in "$iso" "$prefix" "$cache" "$src"; do
  [[ "$p" == *$'\n'* ]] && die "Folder and file names must not contain line breaks: $p"
done

game_running() {
  local exe
  for exe in /proc/[0-9]*/exe; do
    [[ "$(readlink "$exe" 2>/dev/null)" == "$prefix/tabletennis" ]] && return 0
  done
  return 1
}

# ---- toolchain selection --------------------------------------------------------------------
recipe="$here/toolchain-recipe.sh"
recipe_id="$(sha256sum "$recipe" | cut -c1-12)"
image="localhost/tt-recomp-build:$recipe_id"
rootfs="$cache/rootfs"
build_cache="$cache/build"
mkdir -p "$build_cache"
ct=()

container_usable() {  # $1 = podman|docker
  if [[ -n "${TT_CONTAINER:-}" ]]; then read -r -a ct <<< "$TT_CONTAINER"; else ct=("$1"); fi
  command -v "${ct[0]}" >/dev/null || return 1
  "${ct[@]}" info >/dev/null 2>&1
}
host_usable() {
  local t v
  for t in clang clang++ cmake ninja git python3 pkg-config; do command -v "$t" >/dev/null || return 1; done
  v="$(clang++ -dumpversion 2>/dev/null | cut -d. -f1)"; [[ "$v" =~ ^[0-9]+$ ]] && ((v >= 19)) || return 1
  v="$(cmake --version | head -1 | grep -oE '[0-9]+\.[0-9]+' | head -1)"
  [[ "$(printf '%s\n3.25\n' "$v" | sort -V | head -1)" == 3.25 ]] || return 1
  pkg-config --exists x11 xext xrandr && pkg-config --exists alsa || pkg-config --exists libpulse || return 1
}
userns_runner=""
rootfs_usable() {
  # Needs unprivileged user namespaces: bubblewrap, else util-linux unshare.
  if [[ "${TT_USERNS:-}" != unshare ]] && command -v bwrap >/dev/null && bwrap --unshare-user --uid 0 --gid 0 --ro-bind / / true 2>/dev/null; then
    userns_runner=bwrap; return 0
  fi
  if command -v unshare >/dev/null && unshare --user --map-root-user --mount --pid --fork true 2>/dev/null; then
    userns_runner=unshare; return 0
  fi
  return 1
}
userns_hint() {
  if [[ "$(cat /proc/sys/kernel/unprivileged_userns_clone 2>/dev/null)" == 0 \
     || "$(cat /proc/sys/user/max_user_namespaces 2>/dev/null)" == 0 ]]; then
    echo "Unprivileged user namespaces are disabled on this system (kernel.unprivileged_userns_clone / user.max_user_namespaces)."
  elif ! command -v bwrap >/dev/null && ! command -v unshare >/dev/null; then
    echo "Neither bubblewrap (bwrap) nor unshare is installed."
  else
    echo "This system does not allow unprivileged user namespaces (an AppArmor or SELinux policy may block them)."
  fi
}

# ---- fast path: prebuilt toolchain + SDK from the release ----------------------------------
fast_dir="$cache/fast"
fast_reason=""
patchset="$("$here/sdk-patchset-id.sh")"
downloader() {  # url out: resumes out (a .part file) where possible
  if command -v curl >/dev/null; then curl -fsSL --retry 3 --connect-timeout 20 -C - -o "$2" "$1"
  elif command -v wget >/dev/null; then wget -q -c -O "$2" "$1"
  else python3 -I -c 'import sys, urllib.request; urllib.request.urlretrieve(sys.argv[1], sys.argv[2])' "$1" "$2"; fi
}
# Reads the manifest into tc_name/tc_size/tc_sha/sdk_name/sdk_size/sdk_sha/release_patchset.
read_manifest() {
  local vars
  vars="$(python3 -I -B -c '
import json, re, shlex, sys
m = json.load(open(sys.argv[1]))
files = {f["kind"]: f for f in m["files"]}
for prefix, kind in (("tc", "toolchain"), ("sdk", "sdk")):
    f = files[kind]
    assert re.fullmatch(r"[A-Za-z0-9._-]+", f["name"]) and re.fullmatch(r"[0-9a-f]{64}", f["sha256"])
    print("%s_name=%s %s_size=%d %s_sha=%s" % (prefix, shlex.quote(f["name"]), prefix, int(f["size"]), prefix, f["sha256"]))
print("release_patchset=%s release_version=%s" % (shlex.quote(m["sdk_patchset"]), shlex.quote(str(m.get("version", "?")))))
' "$1" 2>/dev/null)" || return 1
  eval "$vars"
}
fast_check() {  # can the fast path run? sets fast_reason when not
  ((no_prebuilt)) && { fast_reason="--no-prebuilt"; return 1; }
  ((sdk_only)) && return 1
  [[ "$toolchain" == auto || "$toolchain" == rootfs ]] || { fast_reason="--toolchain $toolchain was chosen"; return 1; }
  command -v python3 >/dev/null || { fast_reason="python3 is missing"; return 1; }
  command -v zstd >/dev/null && tar --version >/dev/null 2>&1 || { fast_reason="zstd is missing"; return 1; }
  rootfs_usable || { fast_reason="$(userns_hint)"; return 1; }
  mkdir -p "$fast_dir"
  if ((!offline)); then
    rm -f "$fast_dir/manifest.json.new"
    if downloader "$release_url/manifest.json" "$fast_dir/manifest.json.new" >> "$logfile" 2>&1 \
       && read_manifest "$fast_dir/manifest.json.new"; then
      command mv -f "$fast_dir/manifest.json.new" "$fast_dir/manifest.json"
    else
      rm -f "$fast_dir/manifest.json.new"
      log "Release manifest not available from $release_url."
    fi
  fi
  [[ -f "$fast_dir/manifest.json" ]] && read_manifest "$fast_dir/manifest.json" \
    || { fast_reason="the prebuilt build tools could not be downloaded$( ((offline)) && echo " (offline)")"; return 1; }
  [[ "$release_patchset" == "$patchset" ]] \
    || { fast_reason="the prebuilt SDK ($release_version) was made for other SDK patches than this version of the installer"; return 1; }
  tc_dir="$fast_dir/toolchain-${tc_sha:0:12}"
  sdk_dir="$fast_dir/sdk-$patchset-${sdk_sha:0:12}"
  fast_ready=1
  [[ -f "$tc_dir/.tt-ready" && -f "$sdk_dir/.tt-ready" ]] || fast_ready=0
  ((fast_ready || !offline)) || { fast_reason="the prebuilt build tools are not in the cache yet (offline)"; return 1; }
  return 0
}
# Downloads (resumable, with progress), verifies and unpacks one asset into dir.
fetch_asset() {  # name size sha dir lo hi
  local name="$1" size="$2" sha="$3" dir="$4" lo="$5" hi="$6" part="$fast_dir/$1.part" have rc
  [[ -f "$dir/.tt-ready" ]] && return 0
  log "Downloading $name ($((size / 1048576)) MB)."
  if [[ ! -f "$part" || "$(stat -c %s "$part")" -lt "$size" ]]; then
    downloader "$release_url/$name" "$part" >> "$logfile" 2>&1 &
    child=$!
    while kill -0 "$child" 2>/dev/null; do
      have="$(stat -c %s "$part" 2>/dev/null || echo 0)"
      progress $((lo + (hi - lo) * 9 / 10 * (have > size ? size : have) / size))
      sleep 0.5
    done
    wait "$child"; rc=$?; child=""
    ((rc == 0)) || { fast_reason="downloading $name failed"; return 1; }
  fi
  if ! echo "$sha  $part" | sha256sum -c --quiet >/dev/null 2>&1; then
    rm -f "$part"; fast_reason="$name failed its SHA256 check (deleted; it is downloaded again next time)"; return 1
  fi
  log "Verified $name (SHA256 from the release manifest); unpacking."
  [[ -e "$dir.new" ]] && { chmod -R u+rwX "$dir.new" 2>/dev/null; rm -rf "${dir:?}.new"; }
  mkdir -p "$dir.new"
  zstd -dcq --long=27 "$part" | tar -x -C "$dir.new" --no-same-owner 2>> "$logfile" \
    || { fast_reason="unpacking $name failed"; return 1; }
  touch "$dir.new/.tt-ready"
  [[ -e "$dir" ]] && { chmod -R u+rwX "$dir" 2>/dev/null; rm -rf "${dir:?}"; }
  command mv -f "$dir.new" "$dir"
  rm -f "$part"
  progress "$hi"
}

pick=""
if fast_check; then
  pick=fast
  fast_ranges
elif [[ -n "$fast_reason" && "$fast_reason" != --* ]]; then
  warn "Using the slow path: $fast_reason. Building the build tools and SDK on this computer instead; the first install takes longer."
fi
[[ -n "$pick" ]] || case "$toolchain" in
  podman|docker) container_usable "$toolchain" && pick=container \
                   || die "$toolchain is not installed or not working for your user (\"$toolchain info\" fails)." ;;
  host) host_usable && pick=host || die "The host has no suitable toolchain (clang 19+, cmake 3.25+, ninja, git, python3, pkg-config, X11 and ALSA/PulseAudio headers)." ;;
  rootfs) rootfs_usable && pick=rootfs || die "The rootless build system cannot run here: $(userns_hint)" ;;
  auto)
    if container_usable podman; then pick=container
    elif container_usable docker; then pick=container
    elif host_usable; then pick=host
    elif rootfs_usable; then pick=rootfs
    fi ;;
esac
if [[ -z "$pick" ]]; then
  hint="$(userns_hint)"
  if ((install_podman)) && ((!on_steamos)) && command -v pkexec >/dev/null; then
    pm=()
    if command -v pacman >/dev/null; then pm=(pacman -S --needed --noconfirm podman)
    elif command -v apt-get >/dev/null; then pm=(apt-get install -y podman)
    elif command -v dnf >/dev/null; then pm=(dnf install -y podman)
    elif command -v zypper >/dev/null; then pm=(zypper --non-interactive install podman)
    fi
    ((${#pm[@]})) || die "No container runtime or build tools, and no supported package manager to install podman. $hint"
    step tools "Getting build tools"
    warn "Installing podman with your system package manager; enter your administrator password when asked."
    pkexec "${pm[@]}" >> "$logfile" 2>&1 || die "Installing podman failed or was cancelled. Install podman yourself and run the installer again."
    container_usable podman && pick=container || die "podman was installed but does not work for your user yet. Log out and back in, then run the installer again."
  else
    die "No way to build was found: no working podman or docker, no compilers, and the rootless build system cannot run. $hint Install podman (or run the installer with --install-podman to have it installed for you), then try again."
  fi
fi
[[ "$pick" == container ]] || ct=()
log "Toolchain: $pick${ct:+ (${ct[*]})}$([[ $pick == rootfs || $pick == fast ]] && echo " via $userns_runner")$([[ $pick == fast ]] && echo ", prebuilt release $release_version")."

# ---- what is cached / disk space / network -------------------------------------------------
kind=ubuntu2404; [[ "$pick" == host ]] && kind=host; [[ "$pick" == fast ]] && kind=fast
toolchain_ready=1
case "$pick" in
  fast) toolchain_ready=$fast_ready ;;
  container) "${ct[@]}" image inspect "$image" >/dev/null 2>&1 || toolchain_ready=0 ;;
  rootfs) [[ -f "$rootfs/.tt-toolchain-$recipe_id" ]] || toolchain_ready=0 ;;
esac
sdk_ready=0
[[ -x "$build_cache/sdk-$kind/bin/rexglue" || "$pick" == fast ]] && sdk_ready=1
sdk_src_ready=0
[[ -d "$build_cache/rexglue-src/.git" ]] && sdk_src_ready=1

# Measured sizes (GB, rounded up): toolchain 1.6 (rootfs or image); SDK source with submodules
# 1.4, SDK build tree 0.4, SDK 0.1; game code + build 0.4; install 0.1 plus staging.
need_cache=1
((toolchain_ready)) || { [[ "$pick" == rootfs || "$pick" == fast ]] && need_cache=$((need_cache + 2)); }
((sdk_ready)) || need_cache=$((need_cache + 3))
need_prefix=1
need_home=0
((toolchain_ready)) || { [[ "$pick" == container ]] && need_home=2; }
free_gb() { df -Pk -- "$1" 2>/dev/null | awk 'NR == 2 { printf "%d", $4 / 1048576 }'; }
free_cache="$(free_gb "$cache")"; free_prefix="$(free_gb "$prefix")"
log "Free space: build cache $cache ${free_cache} GB (needs about $need_cache GB), install folder $prefix ${free_prefix} GB (needs about $need_prefix GB)."
short=""
if [[ "$(stat -c %d -- "$cache")" == "$(stat -c %d -- "$prefix")" ]]; then
  ((free_cache < need_cache + need_prefix)) && short+=" The drive holding $cache and $prefix needs about $((need_cache + need_prefix)) GB free but has ${free_cache} GB."
  need_cache=0; need_prefix=0
fi
((free_cache < need_cache)) && short+=" The build cache at $cache needs about $need_cache GB free but has ${free_cache} GB."
((free_prefix < need_prefix)) && short+=" The install folder $prefix needs about $need_prefix GB free but has ${free_prefix} GB."
if ((need_home)); then
  store="$("${ct[@]}" info --format '{{.Store.GraphRoot}}' 2>/dev/null || true)"
  [[ -n "$store" && -d "$store" ]] || store="$HOME"
  free_store="$(free_gb "$store")"
  log "Container storage $store: ${free_store} GB free (the build image needs about $need_home GB)."
  ((free_store < need_home)) && short+=" The container storage at $store needs about $need_home GB free but has ${free_store} GB."
fi
[[ -n "$short" ]] && die "Not enough free space.$short Free up space or choose other folders (--prefix, --cache)."

online() { timeout 8 bash -c "exec 3<>/dev/tcp/$1/443" 2>/dev/null; }
if [[ "$pick" != fast ]] && { ((!toolchain_ready)) || ((!sdk_ready && !sdk_src_ready)); }; then
  ((offline)) && die "The build tools are not in the cache yet and --offline was given. Connect to the internet and run the installer without --offline."
  online github.com || die "No internet connection. The first installation downloads build tools and the ReXGlue SDK (about 2 GB); connect to the internet and try again."
  [[ "$pick" == rootfs ]] && ((!toolchain_ready)) && { online cdimage.ubuntu.com || die "Cannot reach cdimage.ubuntu.com to download the build system. Check your internet connection and try again."; }
fi

if [[ -z "$jobs" ]]; then
  # A compile job peaks at about 0.6 GB (measured on the largest generated files); allow 1 GB
  # each, so a 16 GB Steam Deck uses all 8 threads.
  mem_gb=$(awk '/^MemTotal:/ { printf "%d", $2 / 1048576 }' /proc/meminfo)
  jobs=$(nproc); ((jobs > mem_gb)) && jobs=$mem_gb; ((jobs < 1)) && jobs=1
fi
log "Compile jobs: $jobs."

game_running && die "Table Tennis is running from $prefix. Quit the game and run the installer again."
progress 2

# ---- 2. toolchain -------------------------------------------------------------------------
in_rootfs() {  # run a command inside the rootless Ubuntu system; extra binds via RF_BINDS
  local resolv; resolv="$(realpath /etc/resolv.conf 2>/dev/null)"
  if [[ "$userns_runner" == bwrap ]]; then
    local args=(--bind "$1" / --dev /dev --proc /proc --tmpfs /tmp --unshare-user --uid 0 --gid 0
      --unshare-pid --unshare-ipc --die-with-parent --clearenv
      --setenv HOME /root --setenv PATH /usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
      --setenv LANG C.UTF-8 --setenv DEBIAN_FRONTEND noninteractive)
    [[ -f "$resolv" ]] && args+=(--ro-bind "$resolv" /etc/resolv.conf)
    args+=("${RF_BINDS[@]}" "${RF_ENV[@]}")
    shift
    bwrap "${args[@]}" "$@"
  else
    # util-linux unshare: mount namespace with bind mounts, then chroot.
    local root="$1"; shift
    unshare --user --map-root-user --mount --pid --fork --kill-child /bin/bash -c '
      set -e; r="$1"; shift
      mount --rbind /dev "$r/dev"; mount -t proc proc "$r/proc"; mount -t tmpfs tmpfs "$r/tmp"
      [ -f "$RESOLV" ] && mount --bind "$RESOLV" "$r/etc/resolv.conf"
      while [ "$1" != -- ]; do
        mode="$1" from="$2" to="$3"; shift 3
        [ -d "$from" ] && mkdir -p "$r$to" || { mkdir -p "$(dirname "$r$to")"; touch "$r$to"; }
        mount --bind "$from" "$r$to"
        [ "$mode" = ro ] && mount -o remount,bind,ro "$r$to"
      done
      shift
      exec chroot "$r" /usr/bin/env -i HOME=/root PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
        LANG=C.UTF-8 DEBIAN_FRONTEND=noninteractive "$@"' tt "$root" "${RF_UBINDS[@]}" -- "${RF_UENV[@]}" "$@"
  fi
}
RF_BINDS=(); RF_ENV=(); RF_UBINDS=(); RF_UENV=()
export RESOLV="$(realpath /etc/resolv.conf 2>/dev/null)"

setup_rootfs() {
  local dl="$cache/downloads" base="https://cdimage.ubuntu.com/ubuntu-base/releases/24.04/release"
  mkdir -p "$dl"
  log "Downloading the Ubuntu 24.04 base system (about 30 MB) from cdimage.ubuntu.com."
  curl_get() { if command -v curl >/dev/null; then curl -fsSL --retry 3 -o "$2" "$1"; elif command -v wget >/dev/null; then wget -q -O "$2" "$1"; else python3 -I -c 'import sys, urllib.request; urllib.request.urlretrieve(sys.argv[1], sys.argv[2])' "$1" "$2"; fi; }
  curl_get "$base/SHA256SUMS" "$dl/SHA256SUMS" || die "Downloading the build system checksum list failed. Check your internet connection and try again."
  local line name sum
  line="$(grep -E ' \*?ubuntu-base-24\.04(\.[0-9]+)?-base-amd64\.tar\.gz$' "$dl/SHA256SUMS" | sort -V -k2 | tail -1)"
  sum="${line%% *}"; name="${line##* }"; name="${name#\*}"
  [[ "$sum" =~ ^[0-9a-f]{64}$ && -n "$name" ]] || die "Unexpected Ubuntu checksum list; cannot verify the download."
  if [[ ! -f "$dl/$name" ]] || ! echo "$sum  $dl/$name" | sha256sum -c --quiet >/dev/null 2>&1; then
    curl_get "$base/$name" "$dl/$name.part" || die "Downloading the Ubuntu base system failed. Check your internet connection and try again."
    command mv -f "$dl/$name.part" "$dl/$name"
  fi
  echo "$sum  $dl/$name" | sha256sum -c --quiet >/dev/null 2>&1 \
    || { rm -f "$dl/$name"; die "The downloaded Ubuntu base system failed its SHA256 check; it was deleted. Try again."; }
  log "Verified $name (SHA256 from the published SHA256SUMS)."
  local tmp="$cache/rootfs.new"
  [[ -e "$tmp" ]] && { chmod -R u+rwX "$tmp" 2>/dev/null; rm -rf "${cache:?}/rootfs.new"; }
  mkdir -p "$tmp"
  tar -xzf "$dl/$name" -C "$tmp" --no-same-owner --no-same-permissions --exclude='./dev/*' 2>> "$logfile" \
    || die "Unpacking the Ubuntu base system failed."
  chmod -R u+rwX "$tmp"
  # apt drops privileges to a separate user, which a one-user namespace cannot do.
  echo 'APT::Sandbox::User "root";' > "$tmp/etc/apt/apt.conf.d/99tt-rootless"
  mkdir -p "$tmp/opt/tt"; cp "$recipe" "$tmp/opt/tt/toolchain-recipe.sh"
  RF_BINDS=(); RF_ENV=(); RF_UBINDS=(); RF_UENV=()
  run_filtered in_rootfs "$tmp" /bin/bash /opt/tt/toolchain-recipe.sh \
    || die "Installing the build tools into the rootless build system failed (see the log: $logfile)."
  touch "$tmp/.tt-toolchain-$recipe_id"
  [[ -e "$rootfs" ]] && { chmod -R u+rwX "$rootfs" 2>/dev/null; rm -rf "${cache:?}/rootfs"; }
  command mv -f "$tmp" "$rootfs"
}

build_image() {
  log "Building the container image $image (Ubuntu 24.04, clang 20; about 1.5 GB, once)."
  "${ct[@]}" pull -q docker.io/library/ubuntu:24.04 >> "$logfile" 2>&1 \
    || die "Downloading the Ubuntu container image failed. Check your internet connection and try again."
  local name="tt-install-image-$$"
  container_name="$name"
  "${ct[@]}" rm -f "$name" >/dev/null 2>&1
  run_filtered "${ct[@]}" run --name "$name" --security-opt label=disable \
    -v "$recipe:/opt/tt/toolchain-recipe.sh:ro" docker.io/library/ubuntu:24.04 \
    bash /opt/tt/toolchain-recipe.sh \
    || { "${ct[@]}" rm -f "$name" >/dev/null 2>&1; container_name=""; die "Building the container image failed (see the log: $logfile)."; }
  "${ct[@]}" commit -q "$name" "$image" >> "$logfile" 2>&1 || die "Saving the container image failed."
  "${ct[@]}" rm -f "$name" >/dev/null 2>&1
  container_name=""
  # Older images from previous recipes are no longer needed.
  "${ct[@]}" images --format '{{.Repository}}:{{.Tag}}' 2>/dev/null | grep '^localhost/tt-recomp-build:' \
    | grep -vx "$image" | while read -r old; do "${ct[@]}" rmi "$old" >/dev/null 2>&1; done
}

case "$pick" in
  fast)
    step tools "Getting build tools"
    if ((fast_ready)); then
      log "Build tools and engine ready (cached)."
    else
      total=$((tc_size + sdk_size)); mid=$((2 + 31 * tc_size / total))
      if ! fetch_asset "$tc_name" "$tc_size" "$tc_sha" "$tc_dir" 2 "$mid" \
         || ! fetch_asset "$sdk_name" "$sdk_size" "$sdk_sha" "$sdk_dir" "$mid" 33; then
        die "Getting the prebuilt build tools failed: $fast_reason. Run the installer again to retry (finished downloads are kept), or use --no-prebuilt to build them on this computer."
      fi
    fi ;;
  container)
    step image "Preparing the build container"
    if ((toolchain_ready)); then log "Build container ready (cached)."; else build_image; fi ;;
  rootfs)
    step tools "Getting build tools"
    if ((toolchain_ready)); then log "Build tools ready (cached)."; else setup_rootfs; fi ;;
esac
[[ "$pick" == fast ]] || progress 20

# ---- 3-7. extract, SDK, codegen, build, stage ---------------------------------------------
# The build is staged in the cache (a Linux filesystem, so permissions work) and copied into
# the install folder only when complete; that copy is then moved into place with renames.
build_stage="$build_cache/stage"
[[ -e "$build_stage" ]] && rm -rf "${build_cache:?}/stage"
mkdir -p "$build_stage"
stage="$prefix/.tt-staging"
case "$iso$prefix$cache$src" in *:*) [[ "$pick" == container ]] && die "With podman/docker, paths must not contain ':'. Rename the folder or ISO and try again." ;; esac

inside_env=(TT_JOBS="$jobs" TT_KIND="$kind" TT_SDK_ONLY="$sdk_only" TT_GENERATED_OPT="${TT_GENERATED_OPT:-}")
iso_bind=(--ro-bind "$iso" /disc/game.iso); iso_ubind=(ro "$iso" /disc/game.iso); iso_vol=(-v "$iso:/disc/game.iso:ro")
((sdk_only)) && { iso_bind=(); iso_ubind=(); iso_vol=(); }
case "$pick" in
  fast)
    RF_BINDS=(--ro-bind "$src" /src "${iso_bind[@]}" --bind "$build_cache" /cache --bind "$build_stage" /stage --ro-bind "$sdk_dir" /sdk)
    RF_ENV=(--setenv TT_SRC /src --setenv TT_ISO /disc/game.iso --setenv TT_CACHE /cache --setenv TT_STAGE /stage
      --setenv TT_JOBS "$jobs" --setenv TT_KIND "$kind" --setenv TT_PREBUILT_SDK /sdk --setenv TT_GENERATED_OPT "${TT_GENERATED_OPT:-}")
    RF_UBINDS=(ro "$src" /src "${iso_ubind[@]}" rw "$build_cache" /cache rw "$build_stage" /stage ro "$sdk_dir" /sdk)
    RF_UENV=(TT_SRC=/src TT_ISO=/disc/game.iso TT_CACHE=/cache TT_STAGE=/stage TT_JOBS="$jobs" TT_KIND="$kind" TT_PREBUILT_SDK=/sdk TT_GENERATED_OPT="${TT_GENERATED_OPT:-}")
    run_filtered in_rootfs "$tc_dir" /bin/bash /src/scripts/install/build-inside.sh
    rc=$? ;;
  container)
    container_name="tt-install-$$"
    user=(--userns=keep-id)
    [[ "$(basename "${ct[0]}")" == docker ]] && user=(--user "$(id -u):$(id -g)")
    run_filtered "${ct[@]}" run --rm --name "$container_name" "${user[@]}" --security-opt label=disable \
      -v "$src:/src:ro" "${iso_vol[@]}" -v "$build_cache:/cache" -v "$build_stage:/stage" \
      -e HOME=/tmp -e TT_SRC=/src -e TT_ISO=/disc/game.iso -e TT_CACHE=/cache -e TT_STAGE=/stage \
      -e "TT_JOBS=$jobs" -e "TT_KIND=$kind" -e "TT_SDK_ONLY=$sdk_only" -e "TT_GENERATED_OPT=${TT_GENERATED_OPT:-}" -w /tmp "$image" bash /src/scripts/install/build-inside.sh
    rc=$?; container_name="" ;;
  rootfs)
    RF_BINDS=(--ro-bind "$src" /src "${iso_bind[@]}" --bind "$build_cache" /cache --bind "$build_stage" /stage)
    RF_ENV=(--setenv TT_SRC /src --setenv TT_ISO /disc/game.iso --setenv TT_CACHE /cache --setenv TT_STAGE /stage
      --setenv TT_JOBS "$jobs" --setenv TT_KIND "$kind" --setenv TT_SDK_ONLY "$sdk_only" --setenv TT_GENERATED_OPT "${TT_GENERATED_OPT:-}")
    RF_UBINDS=(ro "$src" /src "${iso_ubind[@]}" rw "$build_cache" /cache rw "$build_stage" /stage)
    RF_UENV=(TT_SRC=/src TT_ISO=/disc/game.iso TT_CACHE=/cache TT_STAGE=/stage TT_JOBS="$jobs" TT_KIND="$kind" TT_SDK_ONLY="$sdk_only" TT_GENERATED_OPT="${TT_GENERATED_OPT:-}")
    run_filtered in_rootfs "$rootfs" /bin/bash /src/scripts/install/build-inside.sh
    rc=$? ;;
  host)
    # A private HOME keeps CMake's user package registry (~/.cmake/packages) untouched.
    mkdir -p "$build_cache/home"
    run_filtered env HOME="$build_cache/home" TT_SRC="$src" TT_ISO="$iso" TT_CACHE="$build_cache" TT_STAGE="$build_stage" "${inside_env[@]}" \
      bash "$here/build-inside.sh"
    rc=$? ;;
esac
if ((sdk_only)); then
  ((rc == 0)) && [[ -x "$build_cache/sdk-$kind/bin/rexglue" ]] || die "${last_error:-The SDK build failed.} Log: $logfile"
  echo "@@done $build_cache/sdk-$kind"
  exit 0
fi
if ((rc != 0)) || [[ ! -f "$build_stage/.tt-stage-complete" ]]; then
  for line in "${recent[@]}"; do echo "@@log $line"; done
  die "${last_error:-The build stopped unexpectedly.} Fix the problem and run the installer again; finished parts are reused. Log: $logfile"
fi

# ---- 8. install ---------------------------------------------------------------------------
step install "Installing"
game_running && die "Table Tennis is running from $prefix. Quit the game and run the installer again."
rm -f "$build_stage/.tt-stage-complete"
[[ -e "$stage" ]] && rm -rf "${prefix:?}/.tt-staging"
# Plain copy (no ownership/permission preservation calls), so FAT/exFAT drives work too.
cp -r -- "$build_stage" "$stage" || { rm -rf "${prefix:?}/.tt-staging"; die "Copying the game into $prefix failed. Is the drive full or read-only?"; }
rm -rf "${build_cache:?}/stage"
# Program files are moved in with renames (same filesystem); cancel waits until this is done.
in_swap=1
installed_list="$prefix/.tt-installed"
new_items=()
for item in "$stage"/* "$stage"/.[!.]*; do
  [[ -e "$item" ]] || continue
  name="$(basename "$item")"
  new_items+=("$name")
  if [[ -d "$item" && -e "$prefix/$name" ]]; then
    [[ -e "$prefix/.tt-old-$name" ]] && rm -rf "${prefix:?}/.tt-old-$name"
    command mv -f "$prefix/$name" "$prefix/.tt-old-$name"
  fi
  command mv -f "$item" "$prefix/$name" || { in_swap=0; die "Moving files into $prefix failed."; }
done
# Remove program files a previous version installed that this one no longer has.
if [[ -f "$installed_list" ]]; then
  while IFS= read -r name; do
    [[ -n "$name" && "$name" != */* && "$name" != .* ]] || continue
    printf '%s\n' "${new_items[@]}" | grep -qxF -- "$name" && continue
    case "$name" in tt-recomp.ini|user_data|logs|tt-recomp.desktop) continue ;; esac
    [[ -e "$prefix/$name" ]] && rm -rf "${prefix:?}/$name"
  done < "$installed_list"
fi
printf '%s\n' "${new_items[@]}" > "$installed_list"
for old in "$prefix"/.tt-old-*; do [[ -e "$old" ]] && rm -rf "${old:?}"; done
rmdir "$stage" 2>/dev/null
in_swap=0
((cancel_pending)) && { echo "@@error Installation cancelled after the files were installed; the install is complete but shortcuts were not added."; exit 130; }

# Settings: keep every existing choice; set iso_path and add missing keys with defaults.
ini="$prefix/tt-recomp.ini"
if ((is_deck)); then res=800p; fs=true; else res=1080p; fs=false; fi
defaults=("resolution=$res" "framerate=60" "fullscreen=$fs" "show_fps=false" "aa=none" "post=bilinear"
  "aspect=letterbox" "achievement_sound=true" "haptics=true" "render_scale=auto" "gamemode=auto")
{
  echo "iso_path=$iso"
  if [[ -f "$ini" ]]; then grep -v '^[[:space:]]*iso_path[[:space:]]*=' "$ini"; fi
  for kv in "${defaults[@]}"; do
    [[ -f "$ini" ]] && grep -qE "^[[:space:]]*${kv%%=*}[[:space:]]*=" "$ini" && continue
    echo "$kv"
  done
} > "$ini.tmp" && command mv -f "$ini.tmp" "$ini"
log "Settings: $ini ($( [[ $res == 800p ]] && echo "Steam Deck defaults: 800p fullscreen" || echo "defaults: 1080p windowed" ) for new keys; existing choices kept)."
{
  echo "installed=$(date -Iseconds)"
  echo "source=$(git -C "$src" describe --always --dirty 2>/dev/null || echo unknown)"
  echo "toolchain=$pick$([[ $pick == fast ]] && echo " (release $release_version)")"
} > "$prefix/.tt-install-info"

# Remove the large SDK intermediates unless asked to keep them; the SDK and game build stay.
if ((!keep_build)); then
  for d in "$build_cache"/rexglue-src "$build_cache"/sdk-build-*; do
    [[ -e "$d" ]] || continue
    chmod -R u+rwX "$d" 2>/dev/null; rm -rf "${d:?}"
  done
fi
progress 98

# ---- 9. integrate -------------------------------------------------------------------------
step integrate "Adding shortcuts"
launcher="$prefix/tt-recomp.sh"
icon="$prefix/tabletennis.png"
[[ -f "$icon" ]] || icon="$prefix/tabletennis.ico"
# Desktop Entry escaping: string values escape backslashes; a quoted Exec argument also escapes
# " ` $ and \ (so those become doubled again) and % (field codes).
desk_quote() { printf '"%s"' "$(printf '%s' "$1" | sed -e 's/\\/\\\\\\\\/g' -e 's/["`$]/\\\\&/g' -e 's/%/%%/g')"; }
desk_value() { printf '%s' "$1" | sed -e 's/\\/\\\\/g'; }
desktop_file="$prefix/tt-recomp.desktop"
cat > "$desktop_file" <<EOF
[Desktop Entry]
Type=Application
Name=Table Tennis
GenericName=Table tennis game
Comment=Rockstar Games presents Table Tennis (native Linux port, built from your own disc)
Exec=$(desk_quote "$launcher")
Path=$(desk_value "$prefix")
Icon=$(desk_value "$icon")
Terminal=false
Categories=Game;SportsGame;
StartupNotify=true
EOF
chmod 755 "$desktop_file"
apps="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
if [[ "$desktop" == yes ]]; then
  mkdir -p "$apps" && cp "$desktop_file" "$apps/tt-recomp.desktop" \
    && log "Application menu entry added: $apps/tt-recomp.desktop" \
    || warn "Could not add the application menu entry."
  command -v update-desktop-database >/dev/null && update-desktop-database -q "$apps" 2>/dev/null
elif [[ -f "$apps/tt-recomp.desktop" ]] && grep -qF -- "$(desk_value "$prefix")" "$apps/tt-recomp.desktop"; then
  rm -f "$apps/tt-recomp.desktop"; log "Removed the application menu entry (--desktop no)."
fi

steam_has_shortcut() { command -v python3 >/dev/null && python3 -I -B "$here/tt_install.py" steam-find --exe "$launcher" --art-dir "$prefix/steam" >> "$logfile" 2>&1; }
steam_running() { pgrep -x steam >/dev/null 2>&1 || pgrep -f 'ubuntu12_32/steam' >/dev/null 2>&1; }
steam_installed() { [[ -d "$HOME/.local/share/Steam/userdata" || -d "$HOME/.steam/steam/userdata" || -d "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam/userdata" ]]; }
manual_steam="In Steam's desktop mode choose Games > Add a Non-Steam Game to My Library, Browse, and select $launcher."
steam_result="none"
if [[ "$steam" == yes ]]; then
  [[ -d "$HOME/.var/app/com.valvesoftware.Steam" && ! -d "$HOME/.local/share/Steam/userdata" ]] \
    && warn "Steam is the Flatpak version: it may need access to $prefix (for example: flatpak override --user --filesystem=\"$prefix\" com.valvesoftware.Steam)."
  if steam_has_shortcut; then
    log "Table Tennis is already in your Steam library; artwork refreshed."; steam_result="added"
  elif ((on_steamos)) && command -v steamos-add-to-steam >/dev/null; then
    # SteamOS's own tool hands the .desktop file (name, icon, launcher) to the running Steam client.
    if steamos-add-to-steam "$desktop_file" >> "$logfile" 2>&1; then
      added=0
      for _ in $(seq 1 15); do steam_has_shortcut && { added=1; break; }; sleep 1; done
      steam_result="added"
      if ((added)); then log "Added to Steam as \"Table Tennis\" with artwork."
      else log "Sent to Steam; it appears in your library as \"Table Tennis\" (artwork is added on the next installer run if Steam had not saved the shortcut yet)."; fi
    else
      warn "Adding to Steam failed. $manual_steam"
    fi
  elif ! steam_installed; then
    warn "Steam was not found for this user, so no Steam shortcut was added. $manual_steam"
  elif ! command -v python3 >/dev/null; then
    warn "Adding to Steam needs python3, which is missing. $manual_steam"
  elif steam_running; then
    # Steam rewrites shortcuts.vdf itself while running; ask the client instead.
    url="steam://addnonsteamgame/$(python3 -I -B "$here/tt_install.py" quote "$desktop_file")"
    touch /tmp/addnonsteamgamefile 2>/dev/null
    if command -v steam >/dev/null; then steam "$url" >> "$logfile" 2>&1 & disown
    else xdg-open "$url" >> "$logfile" 2>&1 & disown; fi
    added=0
    for _ in $(seq 1 15); do sleep 1; steam_has_shortcut && { added=1; break; }; done
    if ((added)); then log "Added to Steam as \"Table Tennis\" with artwork."; steam_result="added"
    else steam_result="requested"; warn "Steam was asked to add Table Tennis. If it is not in your library, quit Steam and run the installer again (it then writes the shortcut directly), or: $manual_steam"; fi
  else
    if python3 -I -B "$here/tt_install.py" steam-shortcut --name "Table Tennis" --exe "$launcher" \
         --start-dir "$prefix/" --icon "$icon" --art-dir "$prefix/steam" >> "$logfile" 2>&1; then
      log "Added to Steam as \"Table Tennis\" with artwork (shortcuts.vdf backed up as shortcuts.vdf.tt-backup)."
      steam_result="added"
    else
      warn "Could not add the Steam shortcut ($(tail -n 1 "$logfile")). $manual_steam"
    fi
  fi
fi
# Tell the GUI what happened with Steam: added, requested (asked the running client, not confirmed)
# or none (not requested, or it failed; a @@warn says why).
echo "@@result steam $steam_result"
progress 100
say "Installed. Start the game with: $launcher"
echo "@@done $prefix"
