#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0 OR MIT
# Initializes the third_party submodules and the two nested jpegli dependencies the build uses
# (highway, libjpeg-turbo headers; libjxl's own nested ones are not: it builds on jpegli's highway,
# the bundled brotli and lcms2). Everything else is built by CMake from these sources.
#
#   scripts/fetch_deps.sh            shallow where the remote allows it
#   scripts/fetch_deps.sh --full     full history
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

depth=(--depth 1)
if [[ "${1:-}" == "--full" ]]; then
    depth=()
fi

update() {
    local repo="$1"
    shift

    if ! git -C "$repo" submodule update --init "${depth[@]}" "$@"; then
        git -C "$repo" submodule update --init "$@"
    fi
}

update "$root"

update "$root/third_party/jpegli" third_party/highway third_party/libjpeg-turbo
