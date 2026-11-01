#!/usr/bin/env bash
# Installs the slim build toolchain for the installer's fast path into a fresh Ubuntu 24.04
# base system (run as root inside it): just enough to generate and compile the game against
# the prebuilt ReXGlue SDK.
#
# The compiler is clang 20.1.8 from LLVM's official release build (SHA256-pinned): the same
# version the SDK is built with (scripts/install/toolchain-recipe.sh, apt.llvm.org), but
# statically linked and about 1.6 times faster on the generated game code. Only clang, its
# headers and LLVM's license are kept; linking uses binutils. Documentation (except package
# copyright files), locales and apt state are removed.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
llvm_version=20.1.8
llvm_sha256=1ead36b3dfcb774b57be530df42bec70ab2d239fbce9889447c7a29a4ddc1ae6
apt-get update
# The SDK tools and runtime library link the X11/XCB/Wayland client libraries (SDL).
apt-get install -y --no-install-recommends ca-certificates curl xz-utils \
  libstdc++-14-dev libgcc-14-dev libc6-dev binutils cmake ninja-build python3 \
  libx11-6 libx11-xcb1 libxcb1 libwayland-client0
dir="LLVM-$llvm_version-Linux-X64"
curl -fsSL -o /var/tmp/llvm.tar.xz \
  "https://github.com/llvm/llvm-project/releases/download/llvmorg-$llvm_version/$dir.tar.xz"
echo "$llvm_sha256  /var/tmp/llvm.tar.xz" | sha256sum -c --quiet
mkdir -p /opt/llvm
tar -xJf /var/tmp/llvm.tar.xz -C /opt/llvm --strip-components=1 --no-same-owner \
  "$dir/bin/clang-20" "$dir/bin/clang" "$dir/bin/clang++" "$dir/lib/clang/20/include"
# The binary release carries no top-level license file; ship LLVM's (Apache-2.0 WITH LLVM-exception).
curl -fsSL -o /opt/llvm/LICENSE.TXT \
  "https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-$llvm_version/llvm/LICENSE.TXT"
rm -f /var/tmp/llvm.tar.xz
strip /opt/llvm/bin/clang-20
ln -sf /opt/llvm/bin/clang /usr/local/bin/clang
ln -sf /opt/llvm/bin/clang++ /usr/local/bin/clang++
apt-get purge -y --auto-remove curl xz-utils
# Package copyright files stay (/usr/share/doc/*/copyright): they are the licenses of what ships.
find /usr/share/doc -type f ! -name copyright -delete
find /usr/share/doc -type l -delete
rm -rf /var/lib/apt/lists/* /var/cache/apt/* /var/cache/debconf/*-old /var/log/* \
  /usr/share/man/* /usr/share/info/* /usr/share/lintian /usr/share/locale/* \
  /usr/share/cmake-*/Help /usr/lib/x86_64-linux-gnu/lib{asan,tsan,hwasan,ubsan,lsan}.so*
find / -xdev -name __pycache__ -type d -prune -exec rm -rf {} + 2>/dev/null || true
clang++ --version | head -1
echo "Slim toolchain ready."
