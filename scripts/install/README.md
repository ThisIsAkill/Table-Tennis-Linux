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
| `--jobs N` | CPU threads, at most RAM/2 GB | parallel compile jobs |
| `--steam yes\|no` | `no` | add "Table Tennis" to Steam as a non-Steam game (for Game Mode) |
| `--desktop yes\|no` | `yes` | application menu entry |
| `--keep-build` | off | also keep the SDK source and build tree (about 2 GB) |
| `--toolchain T` | `auto` | `podman`, `docker`, `host` or `rootfs` (also `TT_INSTALL_FORCE=T`) |
| `--install-podman` | off | last resort: install podman with the system package manager via `pkexec` |

## How it builds

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

Everything after that runs inside the chosen environment with the ISO and source mounted read
only ([build-inside.sh](build-inside.sh)): read and verify the disc, patch the menu archive
(including the PC SETTINGS entry), build the pinned ReXGlue SDK v0.10.0 with
`project/sdk-patches/`, run code generation on a private copy of the project, apply the native
edits, compile the game, and stage the same layout as the Steam Deck package (stripped, with
`libstdc++`/`libgcc_s` bundled, so a container build runs on SteamOS's newer glibc).

### Time and disk space

Measured on a Ryzen 5 5600X (12 threads, 12 compile jobs), first run from nothing, downloads included:

| Toolchain | First run | Re-run (nothing changed) |
|---|---|---|
| podman (image built first) | 5 min 24 s | 4 s |
| rootless Ubuntu base | 7 min (2 min of it installing packages) | 4 s |
| host tools (clang 23) | 2 min 54 s | 4 s |

A Steam Deck (8 threads, 16 GB, 8 compile jobs) should take roughly two to three times as long.
Disk use: the image or rootless system takes about 1.5 GB (podman: 2.3 GB of container storage
including the Ubuntu base image); the cache peaks at about 2.9 GB during the first build
(rootless, including the 1.4 GB system) and keeps about 0.4 GB of build files afterwards; the
install itself is 57 MB. The pre-check asks for about 6 GB in the cache (plus 2 GB of container
storage when the image is missing) and 1 GB in the install folder, with headroom.

### What is cached

`<cache>/build/` holds the SDK (`sdk-<flavour>`), the game build tree and the generated code;
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
| `@@step <id> <title>` | a stage begins: `check`, then `tools` (rootless toolchain) or `image` (podman/docker) or neither (host tools), then `extract`, `sdk`, `codegen`, `build`, `install`, `integrate` |
| `@@progress <0-100>` | overall percent, integer, never decreases; Ninja's `[n/total]` moves it during the SDK and game builds |
| `@@log <text>` | detail line (the first one names the full log file) |
| `@@warn <text>` | something to show the user; installation continues |
| `@@error <text>` | plain-language failure, followed by a non-zero exit |
| `@@result steam added\|requested\|none` | outcome of `--steam`: added, asked the running Steam (not confirmed), or none (not requested or failed; a `@@warn` explains) |
| `@@done <dir>` | success; start `<dir>/tt-recomp.sh` |

Exit codes: 0 success, 1 failure, 2 bad arguments, 130 cancelled. To cancel, send SIGTERM (or
SIGINT) to the script's process: it stops the build (including the container) and exits with
`@@error Installation cancelled...`; running it again continues where it stopped.

Approximate percent ranges: check 0-2, tools/image 2-20, extract 20-22, sdk 22-72 (download
22-32, compile 32-72), codegen 72-76, build 76-95, install 95-98, integrate 98-100. Cached
stages are skipped quickly, so the bar jumps.
