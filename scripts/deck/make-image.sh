#!/usr/bin/env bash
# Creates the localhost/tt-recomp-deck build image: Ubuntu 24.04 (glibc 2.39, older than
# current SteamOS) with clang 20, libstdc++ 14 and the headers SDL needs for audio,
# gamepads and windowing. Uses run + commit because `podman build` needs overlay mounts.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
pm="$here/podman.sh"
"$pm" pull -q docker.io/library/ubuntu:24.04 >/dev/null
"$pm" rm -f tt-deck-setup >/dev/null 2>&1 || true
"$pm" run --name tt-deck-setup -e DEBIAN_FRONTEND=noninteractive docker.io/library/ubuntu:24.04 bash -euo pipefail -c '
apt-get update
apt-get install -y --no-install-recommends \
  ca-certificates curl gnupg git python3 ninja-build pkg-config patchelf file \
  g++-14 libstdc++-14-dev \
  libx11-dev libx11-xcb-dev libxcb1-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxtst-dev \
  libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libegl-dev libgl-dev \
  libasound2-dev libpulse-dev libpipewire-0.3-dev libjack-jackd2-dev \
  libdbus-1-dev libudev-dev libibus-1.0-dev libdrm-dev libgbm-dev
curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key | gpg --dearmor -o /usr/share/keyrings/llvm.gpg
echo "deb [signed-by=/usr/share/keyrings/llvm.gpg] http://apt.llvm.org/noble/ llvm-toolchain-noble-20 main" \
  > /etc/apt/sources.list.d/llvm.list
apt-get update
apt-get install -y --no-install-recommends clang-20 lld-20
curl -fsSL https://github.com/Kitware/CMake/releases/download/v3.31.6/cmake-3.31.6-linux-x86_64.tar.gz \
  | tar -xz -C /opt
ln -s /opt/cmake-3.31.6-linux-x86_64/bin/* /usr/local/bin/
ln -s /usr/bin/clang-20 /usr/local/bin/clang
ln -s /usr/bin/clang++-20 /usr/local/bin/clang++
rm -rf /var/lib/apt/lists/*'
"$pm" commit -q tt-deck-setup localhost/tt-recomp-deck:latest >/dev/null
"$pm" rm tt-deck-setup >/dev/null
echo "Image localhost/tt-recomp-deck:latest ready."
