#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0 OR MIT
# Builds the browser modules ({SIMD128, scalar} x {pthreads, single thread} and SIMD128 with
# WebGPU) and assembles the site into <site dir> (default build/web): the docs as GitHub Pages
# publishes them (docs/scripts/build.py --site) and the WASM demo under demo/ (src/wasm/demo, the
# modules, a sample). Runs in the dev image (it has emsdk):
#   scripts/docker_dev.sh scripts/wasm_demo.sh [site dir]
set -euo pipefail

site="${1:-build/web}"
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

# shellcheck disable=SC1091
source "${HDRG_EMSDK_DIR:?HDRG_EMSDK_DIR is not set: run this in the dev image}/emsdk_env.sh" > /dev/null 2>&1

# the docs first: --out writes the directory afresh (their source links relative to the repository)
python3 -I docs/scripts/build.py --out "$site" --site
mkdir -p "$site/demo"

for preset in wasm wasm-mt wasm-scalar wasm-scalar-mt wasm-webgpu; do
    cmake --preset "$preset" > /dev/null
    cmake --build --preset "$preset" --target hdrg_wasm
    cp "build/${HDRG_BUILD_PREFIX:-}$preset"/hdrgrading-*.mjs \
        "build/${HDRG_BUILD_PREFIX:-}$preset"/hdrgrading-*.wasm "$site/demo/"
done

cp src/wasm/demo/index.html src/wasm/demo/demo.js src/wasm/demo/worker.js src/wasm/demo/preview.js \
    src/wasm/demo/coi-sw.js "$site/demo/"
cp assets/kodim23.png "$site/demo/sample.png"
