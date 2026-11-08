# SPDX-License-Identifier: Apache-2.0 OR MIT
# Debian 13 (glibc) for the CI's x86 (linux/386) tests and coverage (.github/workflows/ci.yml): the
# development image (.devcontainer) is built for amd64 and arm64 only (its LLVM and Dawn have no
# i386 build), so this one has Debian's own Clang with its profile runtime and llvm-cov, CMake,
# Ninja, Python, NASM and Perl, and Vulkan's loader and headers with
# Mesa's drivers (lavapipe). The commands run as the runner's uid (scripts/coverage.sh in the mounted workspace).
FROM debian:13.7-slim@sha256:a72b4433de1cf7d6bae0c4cee74d0547f58601a643b3375f070c51600d63b6b2

ARG LLVM_VERSION=19
ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        bash binutils ca-certificates "clang-${LLVM_VERSION}" cmake g++ git libc6-dev \
        "libclang-rt-${LLVM_VERSION}-dev" libvulkan-dev "llvm-${LLVM_VERSION}" make \
        mesa-vulkan-drivers nasm ninja-build perl pkgconf python3 \
    && rm -rf /var/lib/apt/lists/*

ENV PATH=/usr/lib/llvm-${LLVM_VERSION}/bin:$PATH \
    CC=clang \
    CXX=clang++
