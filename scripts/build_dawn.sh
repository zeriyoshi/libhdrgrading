#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0 OR MIT
# Builds Dawn (WebGPU, https://github.com/google/dawn) at the pinned tag from source and installs
# it under PREFIX for HDRG_WITH_WEBGPU (found through CMAKE_PREFIX_PATH or HDRG_DAWN_DIR). Dawn is
# not vendored: the dev image installs it into /opt/dawn, a macOS host into build/deps/dawn. Its
# own dependencies come from its fetch_dawn_dependencies.py, pinned by its DEPS.
#
#   scripts/build_dawn.sh [PREFIX]          (default: build/deps/dawn)
#   scripts/build_dawn.sh --tint [PREFIX]   Tint's CLI alone, from the same source, into
#                                           PREFIX/bin/tint (scripts/gen_shaders.sh: WGSL -> MSL,
#                                           SPIR-V, HLSL)
#
# HDRG_DAWN_WORK: where the source and its build go (default: build/deps).
set -euo pipefail

# a daily tag of Dawn's: updated by hand (not Renovate), the whole matrix run on the new one, and kept
# equal to the dev image's DAWN_TAG (checked by CI)
DAWN_TAG=v20261019.211215

tint=0
if [[ "${1:-}" == "--tint" ]]; then
    tint=1
    shift
fi

workspace="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
prefix="${1:-$workspace/build/deps/dawn}"
work="${HDRG_DAWN_WORK:-$workspace/build/deps}"
src="$work/dawn-$DAWN_TAG-src"
jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

mkdir -p "$work"

if [[ ! -d "$src/.git" ]]; then
    git clone --depth 1 --branch "$DAWN_TAG" https://github.com/google/dawn.git "$src"
fi

# the CLI in a tree of its own (the library's is left as it is): WGSL in, MSL out (the writer is
# built on any host), no Dawn backends; the generated MSL is Tint's alone, so equal on every host
if [[ "$tint" == 1 ]]; then
    cmake -S "$src" -B "$src/out-tint" -DCMAKE_BUILD_TYPE=Release -DDAWN_FETCH_DEPENDENCIES=ON \
        -DDAWN_BUILD_SAMPLES=OFF -DDAWN_BUILD_TESTS=OFF -DDAWN_BUILD_PROTOBUF=OFF \
        -DDAWN_SUPPORTS_CXX_MODULES=OFF -DDAWN_USE_GLFW=OFF -DDAWN_USE_X11=OFF -DDAWN_USE_WAYLAND=OFF \
        -DDAWN_ENABLE_DESKTOP_GL=OFF -DDAWN_ENABLE_OPENGLES=OFF -DDAWN_ENABLE_VULKAN=OFF \
        -DDAWN_ENABLE_METAL=OFF -DDAWN_ENABLE_D3D11=OFF -DDAWN_ENABLE_D3D12=OFF \
        -DDAWN_ENABLE_NULL=OFF -DTINT_BUILD_TESTS=OFF -DTINT_BUILD_CMD_TOOLS=ON \
        -DTINT_BUILD_WGSL_READER=ON -DTINT_BUILD_WGSL_WRITER=ON -DTINT_BUILD_MSL_WRITER=ON \
        -DTINT_BUILD_SPV_READER=OFF -DTINT_BUILD_SPV_WRITER=ON -DTINT_BUILD_HLSL_WRITER=ON \
        -DTINT_BUILD_GLSL_WRITER=OFF -DTINT_BUILD_GLSL_VALIDATOR=OFF
    cmake --build "$src/out-tint" -j "$jobs" --target tint_cmd_tint_cmd
    install -d "$prefix/bin"
    install -m 0755 "$src/out-tint/tint" "$prefix/bin/tint"
    exit 0
fi

# the library alone, shared (a C++ library a C program links without the C++ runtime's details),
# without the windowing, the GL backends, the samples, the tests and the C++ module (whose
# dependency scan under Ninja runs before the version header it includes is generated)
cmake -S "$src" -B "$src/out" -DCMAKE_BUILD_TYPE=Release \
    -DDAWN_FETCH_DEPENDENCIES=ON -DDAWN_ENABLE_INSTALL=ON \
    -DDAWN_BUILD_MONOLITHIC_LIBRARY=SHARED \
    -DDAWN_BUILD_SAMPLES=OFF -DDAWN_BUILD_TESTS=OFF -DDAWN_BUILD_PROTOBUF=OFF \
    -DTINT_BUILD_TESTS=OFF -DTINT_BUILD_CMD_TOOLS=OFF -DDAWN_SUPPORTS_CXX_MODULES=OFF \
    -DDAWN_USE_GLFW=OFF -DDAWN_USE_X11=OFF -DDAWN_USE_WAYLAND=OFF \
    -DDAWN_ENABLE_DESKTOP_GL=OFF -DDAWN_ENABLE_OPENGLES=OFF
cmake --build "$src/out" -j "$jobs"
cmake --install "$src/out" --prefix "$prefix"

# the installed dawn/webgpu.h includes the generated dawn/dawn_version.h, which the install leaves out
if [[ ! -f "$prefix/include/dawn/dawn_version.h" ]]; then
    install -m 0644 "$src/out/gen/include/dawn/dawn_version.h" "$prefix/include/dawn/dawn_version.h"
fi
