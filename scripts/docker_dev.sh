#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0 OR MIT
# Builds the Debian 13 development image (.devcontainer/Dockerfile) as hdrg-dev:<arch> and runs a
# command in it with the workspace mounted at /workspaces/libhdrgrading, as the host uid and gid.
#
#   scripts/docker_dev.sh                                 interactive bash
#   scripts/docker_dev.sh cmake --workflow --preset dev
#   scripts/docker_dev.sh --build                         rebuild the image, then interactive bash
#
# HDRG_DEV_ARCH (arm64 / amd64) picks the image architecture (default: the host's). The command runs
# under scripts/jobs.sh: the compiles and tests run at once fit the cores and the memory free in the
# container and on the host (HDRG_JOBS_CAP; HDRG_JOB_MIB and the levels already set pass through, as
# does HDRG_COVERAGE_LABEL for scripts/coverage.sh).
#
# The container's ~/.cache (Emscripten's system libraries and ports, ccache) is the volume
# hdrg-dev-<arch>-cache, kept between runs; HDRG_DEV_CACHE names a host directory to mount there
# instead (CI keeps its parts in the GitHub Actions cache).
set -euo pipefail

fail() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

workspace="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
container_workspace=/workspaces/libhdrgrading

force_build=0
if [[ "${1:-}" == "--build" ]]; then
    force_build=1
    shift
fi
if [[ "${1:-}" == "--" ]]; then
    shift
fi

case "${HDRG_DEV_ARCH:-$(uname -m)}" in
    arm64 | aarch64) arch=arm64 ;;
    x86_64 | amd64) arch=amd64 ;;
    *) fail "unsupported architecture: ${HDRG_DEV_ARCH:-$(uname -m)} (set HDRG_DEV_ARCH to arm64 or amd64)" ;;
esac

image="hdrg-dev:${arch}"
uid="$(id -u)"
gid="$(id -g)"

if [[ "$force_build" == 1 ]] || ! docker image inspect "$image" >/dev/null 2>&1; then
    docker build --platform "linux/${arch}" \
        --build-arg "USER_UID=${uid}" --build-arg "USER_GID=${gid}" \
        -t "$image" -f "$workspace/.devcontainer/Dockerfile" "$workspace/.devcontainer"
fi

tty_args=()
if [[ -t 0 && -t 1 ]]; then
    tty_args=(-it)
fi

if [[ $# -eq 0 ]]; then
    set -- bash
fi

env_args=(-e "HDRG_JOBS_CAP=$("$workspace/scripts/jobs.sh")")
for name in HDRG_JOB_MIB CMAKE_BUILD_PARALLEL_LEVEL CTEST_PARALLEL_LEVEL HDRG_COVERAGE_LABEL; do
    if [[ -n "${!name:-}" ]]; then
        env_args+=(-e "${name}=${!name}")
    fi
done

cache="hdrg-dev-${arch}-cache"
if [[ -n "${HDRG_DEV_CACHE:-}" ]]; then
    mkdir -p "$HDRG_DEV_CACHE"
    cache="$(cd "$HDRG_DEV_CACHE" && pwd)"
fi

docker run --rm "${tty_args[@]}" --platform "linux/${arch}" \
    --cap-add=SYS_PTRACE --security-opt seccomp=unconfined \
    -v "${workspace}:${container_workspace}" \
    -v "${cache}:/home/dev/.cache" \
    -w "$container_workspace" \
    "${env_args[@]}" \
    "$image" "$container_workspace/scripts/jobs.sh" "$@"
