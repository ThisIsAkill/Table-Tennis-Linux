# Linux / Steam Deck installer (backend)

`tt-install.sh` turns your own Table Tennis ISO into a playable install with one command. The
game is compiled on your machine from your disc, so nothing from the game is downloaded or
shipped: the public package holds only this project's source.

```sh
scripts/install/tt-install.sh --iso ~/Downloads/TableTennis.iso
~/Games/TableTennis/tt-recomp.sh
```

| Option | Default | Meaning |
|---|---|---|
| `--iso PATH` | (required) | your USA/Europe retail ISO, Title ID `545407DF`; only read, and kept in place because the game reads it while playing |
| `--prefix DIR` | `~/Games/TableTennis` | install folder; any writable folder, also on an SD card (`/run/media/<user>/<label>/...`) or external drive, as long as it allows running programs |
| `--cache DIR` | `~/.cache/tt-recomp` | build tools, SDK and build files; must be a Linux filesystem (ext4, btrfs, xfs) |
| `--jobs N` | CPU threads, at most one per GB of RAM | parallel compile jobs (a job peaks at about 0.6 GB) |
| `--steam yes\|no` | `no` | add "Table Tennis" to Steam as a non-Steam game (for Game Mode) |
| `--desktop yes\|no` | `yes` | application menu entry |
| `--keep-build` | off | also keep the SDK source and build tree (about 2 GB) |
| `--toolchain T` | `auto` | `podman`, `docker`, `host` or `rootfs` (also `TT_INSTALL_FORCE=T`) |
| `--install-podman` | off | last resort: install podman with the system package manager via `pkexec` |
| `--release-url URL` | latest GitHub release | where `manifest.json` and the prebuilt files are (also `TT_RELEASE_URL`; `file://` works) |
| `--offline` | off | download nothing; use the cache |
| `--no-prebuilt` | off | skip the fast path and build the toolchain and SDK on this computer |

`TT_GENERATED_OPT=-O2` compiles the generated game code at `-O2` instead of `-O3` (the CMake
cache variable `TT_GENERATED_OPT` in `project/CMakeLists.txt`); it is not the default because
the runtime effect has not been measured on a Steam Deck, and on this toolchain it does not
compile measurably faster.

## How it builds

### Fast path (default)

The parts of the build that are the same for everyone and contain nothing from the game are
published as release files (see [Release files](#release-files)): a slim Ubuntu 24.04 toolchain
(clang 20.1.8, CMake, Ninja, Python, binutils) and the ReXGlue SDK built with
`project/sdk-patches/`. The installer reads `manifest.json` from the release, downloads both
once into `<cache>/fast/` (resumable, with progress, SHA256-checked against the manifest),
unpacks them, and runs the game build inside the toolchain with bubblewrap, else `unshare`. No
container runtime, compilers or administrator rights are needed; it needs `python3`, `zstd`,
`tar` and unprivileged user namespaces (SteamOS ships `bwrap` with Flatpak).

The prebuilt SDK is used only if the manifest's `sdk_patchset` equals this source tree's
(`scripts/install/sdk-patchset-id.sh`, the same id `build.sh` records). Otherwise, or when the
release cannot be reached and nothing is cached, the installer prints
`@@warn Using the slow path: <reason>...` and continues with the slow path below.

### Slow path

The build needs clang 19+, CMake, Ninja, Git, Python and SDL's development headers. The
installer finds or fetches them itself, in this order (`--toolchain auto`):

1. **podman, else docker**, when `podman info` / `docker info` works for the user (stock
   SteamOS has podman). An image `localhost/tt-recomp-build:<id>` is built once from
   [toolchain-recipe.sh](toolchain-recipe.sh) (Ubuntu 24.04, clang 20, CMake 3.31) in the
   user's normal container storage and reused; `<id>` is a hash of the recipe, so changing
   the recipe builds a new image and removes the old one.
2. **The host's own tools**, if clang 19+, CMake 3.25+, Ninja, Git, Python, pkg-config and the
   X11 and ALSA/PulseAudio headers are installed.
3. **A rootless Ubuntu 24.04 base system** in the cache: the official
   `ubuntu-base-24.04.x-base-amd64.tar.gz` from cdimage.ubuntu.com, checked against the
   published `SHA256SUMS`, unpacked, and given the same recipe with apt inside an unprivileged
   user namespace (bubblewrap, else `unshare`). No administrator rights are needed; it needs
   unprivileged user namespaces, which SteamOS and most distributions allow.
4. With `--install-podman` only: `pkexec` + pacman/apt/dnf/zypper installs podman (not on
   SteamOS, whose system is read-only).

Everything after that (on both paths) runs inside the chosen environment with the ISO and source mounted read
only ([build-inside.sh](build-inside.sh)): read and verify the disc, patch the menu archive
(including the PC SETTINGS entry), build the pinned ReXGlue SDK v0.10.0 with
`project/sdk-patches/` (slow path only), run code generation on a private copy of the project, apply the native
edits, compile the game, and stage the same layout as the Steam Deck package (stripped, with
`libstdc++`/`libgcc_s` bundled, so a container build runs on SteamOS's newer glibc).

### Time and disk space

Measured on a Ryzen 5 5600X (12 threads, 12 compile jobs), first run from nothing, downloads included:

| Toolchain | First run | Re-run (nothing changed) |
|---|---|---|
| fast path (release files served locally; add the one-time 123 MB download) | 45 s (game compile 35 s) | 2.3 s |
| podman (image built first) | 5 min 24 s | 4 s |
| rootless Ubuntu base | 7 min (2 min of it installing packages) | 4 s |
| host tools (clang 23) | 2 min 54 s | 4 s |

A Steam Deck (8 threads, 16 GB, 8 compile jobs) should take roughly two to three times as long;
for the fast path that is an estimated 2 to 2.5 minutes plus the download (not yet measured on a
Deck). Fast path disk use: the 123 MB download (deleted once unpacked), about 620 MB in
`<cache>/fast` and 230 MB of build files; the pre-check asks for 3 GB in the cache. Slow path
disk use: the image or rootless system takes about 1.5 GB (podman: 2.3 GB of container storage
including the Ubuntu base image); the cache peaks at about 2.9 GB during the first build
(rootless, including the 1.4 GB system) and keeps about 0.4 GB of build files afterwards; the
install itself is 57 MB. The pre-check asks for about 6 GB in the cache (plus 2 GB of container
storage when the image is missing) and 1 GB in the install folder, with headroom.

### What is cached

`<cache>/fast/` holds the fast path's toolchain and SDK (about 620 MB unpacked) and the last manifest;
`<cache>/build/` holds the SDK (`sdk-<flavour>`, slow path), the game build tree and the generated code;
`<cache>/rootfs/` the rootless toolchain; `<cache>/downloads/` the Ubuntu tarball;
`<cache>/logs/` the last five install logs. After a successful install the SDK source and its
build tree are deleted unless `--keep-build` is given; they are only needed again when the SDK
patches change. Running the installer again reuses all of it: unchanged code is not
regenerated or recompiled.

Delete `<cache>` (and `podman rmi localhost/tt-recomp-build:<id>`) to reclaim the space; the
install itself does not need the cache.

## Install folder

```
tt-recomp.sh          launcher (reads tt-recomp.ini; see the main README)
tt-recomp.ini         settings: kept on reinstall, only iso_path and missing keys are written
tt-recomp.desktop     launcher entry, also what is handed to Steam on SteamOS
tabletennis, *.so*    the game, built on this machine
assets/               menu archive extracted from your ISO and patched
fonts/ licenses/ tools/ steam/ tabletennis.png tabletennis.ico
logs/                 written by the game
```

Saves, achievements and the shader cache are not in the install folder: the game keeps them in
`~/.local/share/tabletennis/` (`$XDG_DATA_HOME/tabletennis`), so they survive reinstalling or
moving the install. (Exception inherited from the Windows layout: if the install folder's
parent directory contains `user_data/` or `reports/`, the game uses `<parent>/user_data` and
`<parent>/cache` instead.)

New settings files get 800p fullscreen on a Steam Deck (`SteamDeck=1` or DMI product
Jupiter/Galileo) and 1080p windowed elsewhere. The new build is copied to
`<prefix>/.tt-staging` and moved into place with renames only when complete (cancel is held
off for that moment), so an interrupted install leaves the previous one working. Files a
previous version installed and this one no longer has are removed; nothing else in the folder
is touched. The build is staged in `<cache>/build/stage` and copied with a plain copy, so FAT
and exFAT install folders work when the drive allows running programs. The folder must allow executing programs: an exFAT or NTFS drive mounted without
execute permission is refused with an explanation. Paths may contain spaces; with podman or
docker they must not contain `:`.

## Versions and updates

A release package carries a `VERSION` file; installing it writes `<install dir>/.tt-version`
(a source checkout without `VERSION` installs a development build: no `.tt-version`, no
updates). The install also records the choices it was made with in `.tt-install.json` (ISO,
install folder, cache, Steam, desktop entry, release URL) and keeps what updates need:
`install-table-tennis` (the graphical installer), `tools/tt-update.sh`, `tools/tt_install.py` and
`tools/release-public-key`.

- **Check:** at each start with `check_updates=true` (default) the launcher runs
  `tools/tt-update.sh <dir> --check-only` in the background, at most every 6 hours
  (`.tt-update-check`), with 5 s network timeouts. If the signed manifest's version is newer
  than `.tt-version`, it writes `.tt-update.json` (`version`, `title`, `notes`, `published`),
  which the game shows in OPTIONS > PC SETTINGS > UPDATES.
- **Update:** when the game exits after the player chose to update (`.tt-update-requested`), or
  before starting when `auto_update=true` and an update is known, the launcher runs
  `install-table-tennis --update <dir>` when a display is available (progress screen; on success
  it returns 0 and the launcher starts the new version; on failure "Play the current version"
  returns 10), otherwise `tools/tt-update.sh <dir>` without a window (log in `logs/update.log`).
- `tt-update.sh` fetches `manifest.json` and `manifest.json.sig`, refuses a manifest whose
  Ed25519 signature does not match `release-public-key`, downloads the player package, checks its
  SHA256, unpacks it to `<cache>/update/<version>/` and runs that version's `tt-install.sh` with
  the stored choices (the ISO path is taken from `tt-recomp.ini`). The install is atomic as
  always; settings, saves and the Steam shortcut are kept.

`tt-install.sh` also refuses an unsigned or altered manifest (`@@error`) instead of falling back.
For tests, `TT_RELEASE_PUBKEY_FILE` points both scripts at another public key.

## Shortcuts

- **Application menu:** `~/.local/share/applications/tt-recomp.desktop` (removed again with
  `--desktop no` if it points at this install).
- **Steam** (`--steam yes`), named "Table Tennis" with the project icon:
  - SteamOS: `steamos-add-to-steam <prefix>/tt-recomp.desktop` (Steam reads name, launcher and
    icon from the entry).
  - Other distributions with Steam not running: the shortcut is written into
    `userdata/<account>/config/shortcuts.vdf` of the most recently signed-in account. The
    file is parsed and re-serialized with a round-trip check; the original is kept as
    `shortcuts.vdf.before-tt-recomp` and the previous state as `shortcuts.vdf.tt-backup`.
  - Steam running: the client is asked through `steam://addnonsteamgame/`; if the shortcut
    does not show up, a `@@warn` explains how to finish (quit Steam and rerun, or add it by
    hand).
  - An existing shortcut for this launcher is never duplicated. Library artwork (capsule,
    wide capsule, hero, logo) is generated from the project's own icon only and copied into
    Steam's `config/grid/` once the shortcut id is known.

The icon artwork is by Gary aka POOTERMAN and is credited separately from the MIT license (see
the main README).

## Progress protocol (for front ends)

The script writes one message per line on stdout; lines without `@@` are plain text for people.

| Line | Meaning |
|---|---|
| `@@step <id> <title>` | a stage begins: `check`, then `tools` (fast path downloads, or the rootless toolchain) or `image` (podman/docker) or neither (host tools), then `extract`, `sdk` ("Getting the engine" when prebuilt), `codegen`, `build`, `install`, `integrate`; `tools` can appear twice if the fast path falls back |
| `@@progress <0-100>` | overall percent, integer, never decreases; Ninja's `[n/total]` moves it during the SDK and game builds |
| `@@log <text>` | detail line (the first one names the full log file) |
| `@@warn <text>` | something to show the user; installation continues |
| `@@error <text>` | plain-language failure, followed by a non-zero exit |
| `@@result steam added\|requested\|none` | outcome of `--steam`: added, asked the running Steam (not confirmed), or none (not requested or failed; a `@@warn` explains) |
| `@@done <dir>` | success; start `<dir>/tt-recomp.sh` |

Exit codes: 0 success, 1 failure, 2 bad arguments, 130 cancelled. To cancel, send SIGTERM (or
SIGINT) to the script's process: it stops the build (including the container) and exits with
`@@error Installation cancelled...`; running it again continues where it stopped.

Approximate percent ranges, fast path: check 0-2, tools 2-35 (by downloaded bytes), extract
35-37, sdk 37-38, codegen 38-46, build 46-95, install 95-98, integrate 98-100. Slow path:
check 0-2, tools/image 2-20, extract 20-22, sdk 22-72 (download 22-32, compile 32-72), codegen
72-76, build 76-95, install 95-98, integrate 98-100. Cached stages are skipped quickly, so the
bar jumps.

## Release files

`scripts/release/make-release.sh --out DIR` builds them (rootless, bubblewrap; about 8 minutes
from nothing on the machine above, mostly the SDK build and the 1.9 GB LLVM download):

| File | Contents |
|---|---|
| `tt-toolchain-<id>.tar.zst` | slim Ubuntu 24.04 root filesystem from [toolchain-slim-recipe.sh](../release/toolchain-slim-recipe.sh): LLVM's official clang 20.1.8 (static, SHA256-pinned, with its license), libstdc++ 14, binutils, CMake 3.28, Ninja, Python 3.12, the X11/XCB/Wayland client libraries the SDK links, and the Ubuntu packages' copyright files; `<id>` is a hash of the recipe |
| `tt-sdk-<patchset>-x86-64-v3.tar.zst` | the installed ReXGlue SDK v0.10.0 with `project/sdk-patches/`, built in Ubuntu 24.04 for x86-64-v3; relocatable (CMake and pkg-config files are prefix-relative) |
| `install-table-tennis-linux-x86_64.tar.gz` | what players download: `TableTennis-Installer/` with a snapshot of the tracked source (`git archive HEAD`), the graphical installer `install-table-tennis` and `README.txt` |
| `manifest.json` | read by `tt-install.sh` |

None of them contains anything from the game; the package step refuses to pack ISO, XEX or RPF
files, generated code or build output. `manifest.json`:

```json
{
  "format": 1,
  "version": "v1.1.0",
  "created": "2026-11-01T16:57:02Z",
  "sdk_patchset": "d65bea6eba1ffff4",
  "toolchain_id": "<id>",
  "arch": "x86_64",
  "march": "x86-64-v3",
  "files": [
    {"kind": "toolchain", "name": "tt-toolchain-<id>.tar.zst", "size": 0, "sha256": "..."},
    {"kind": "sdk", "name": "tt-sdk-d65bea6eba1ffff4-x86-64-v3.tar.zst", "size": 0, "sha256": "..."},
    {"kind": "package", "name": "install-table-tennis-linux-x86_64.tar.gz", "size": 0, "sha256": "..."}
  ]
}
```

The installer fetches `<release-url>/manifest.json` and `<release-url>/<name>`; with the default
URL (`https://github.com/ThisIsAkill/Table-Tennis-Rockstar-PC/releases/latest/download`) all four
files must be assets of the latest release. Upload a new SDK file whenever
`project/sdk-patches/` changes; installs from an older source tree then fall back to the slow
path instead of using a mismatched SDK.
