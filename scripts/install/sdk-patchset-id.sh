#!/usr/bin/env bash
# Prints the id of the SDK patch set (project/sdk-patches plus the SDK build options), the same
# value build.sh records in .tools/rexglue-sdk/.tt-sdk-patches. A prebuilt SDK is only used when
# its id matches the source tree's.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
{ cat "$root"/project/sdk-patches/*.patch; echo "-DREXGLUE_ENABLE_FIDELITYFX=OFF"; } | sha256sum | cut -c1-16
