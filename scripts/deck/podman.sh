#!/usr/bin/env bash
# podman with storage kept inside the project (.tools/podman, gitignored) on the btrfs
# driver, since the default overlay driver does not work on a btrfs home.
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
exec podman --root "$root/.tools/podman/storage" --storage-driver btrfs "$@"
