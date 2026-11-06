#!/usr/bin/env bash
# Copyright (c) 2026 Akhil Moola (ThisIsAkill). MIT License, see LICENSE.
# Installs the build toolchain into a fresh Ubuntu 24.04 system (run as root inside it):
# clang 20, libstdc++ 14, CMake 3.31, Ninja, Python, Git and the headers SDL needs for audio,
# gamepads and windowing. Used for the podman/docker image (scripts/deck/make-image.sh,
# scripts/install/tt-install.sh) and for the rootless Ubuntu base system the installer sets
# up when no container runtime is available. Ubuntu 24.04 has glibc 2.39, older than current
# SteamOS, so what it builds runs there and on most other distributions.
# Changing this file changes the toolchain id, so installs rebuild their image once.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
  ca-certificates curl gnupg git python3 ninja-build pkg-config patchelf file \
  g++-14 libstdc++-14-dev \
  libx11-dev libx11-xcb-dev libxcb1-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxtst-dev \
  libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libegl-dev libgl-dev \
  libasound2-dev libpulse-dev libpipewire-0.3-dev libjack-jackd2-dev \
  libdbus-1-dev libudev-dev libibus-1.0-dev libdrm-dev libgbm-dev
curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key | gpg --batch --yes --dearmor -o /usr/share/keyrings/llvm.gpg
echo "deb [signed-by=/usr/share/keyrings/llvm.gpg] http://apt.llvm.org/noble/ llvm-toolchain-noble-20 main" \
  > /etc/apt/sources.list.d/llvm.list
apt-get update
apt-get install -y --no-install-recommends clang-20 lld-20
cmake_version=3.31.6
cmake_sha256=5a1133ff103c71eb5120e2cc3de922733e7d8a26a98ae716397e8676adb367bf
curl -fsSL -o /tmp/cmake.tar.gz \
  "https://github.com/Kitware/CMake/releases/download/v$cmake_version/cmake-$cmake_version-linux-x86_64.tar.gz"
echo "$cmake_sha256  /tmp/cmake.tar.gz" | sha256sum -c --quiet
tar -xzf /tmp/cmake.tar.gz -C /opt
rm -f /tmp/cmake.tar.gz
ln -sf /opt/cmake-$cmake_version-linux-x86_64/bin/* /usr/local/bin/
ln -sf /usr/bin/clang-20 /usr/local/bin/clang
ln -sf /usr/bin/clang++-20 /usr/local/bin/clang++
rm -rf /var/lib/apt/lists/*
echo "Toolchain ready."
