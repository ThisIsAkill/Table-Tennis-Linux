#!/usr/bin/env bash
# Copyright (c) 2026 Akhil Moola (ThisIsAkill). MIT License, see LICENSE.
# Creates the localhost/tt-recomp-deck build image: Ubuntu 24.04 (glibc 2.39, older than
# current SteamOS) with clang 20, libstdc++ 14 and the headers SDL needs for audio,
# gamepads and windowing (scripts/install/toolchain-recipe.sh, shared with the installer).
# Uses run + commit because `podman build` needs overlay mounts.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
pm="$here/podman.sh"
"$pm" pull -q docker.io/library/ubuntu:24.04 >/dev/null
"$pm" rm -f tt-deck-setup >/dev/null 2>&1 || true
"$pm" run --name tt-deck-setup --security-opt label=disable \
  -v "$here/../install/toolchain-recipe.sh:/opt/tt/toolchain-recipe.sh:ro" \
  docker.io/library/ubuntu:24.04 bash /opt/tt/toolchain-recipe.sh
"$pm" commit -q tt-deck-setup localhost/tt-recomp-deck:latest >/dev/null
"$pm" rm tt-deck-setup >/dev/null
echo "Image localhost/tt-recomp-deck:latest ready."
