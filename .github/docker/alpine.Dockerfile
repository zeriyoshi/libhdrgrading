# SPDX-License-Identifier: Apache-2.0 OR MIT
# Alpine (musl) for the CI's tests and coverage (.github/workflows/ci.yml), built for linux/amd64,
# linux/386 and linux/arm64: Clang with its profile runtime (compiler-rt) and llvm-cov, CMake,
# samurai (Ninja), Python, NASM and Perl, and Vulkan's loader and headers with lavapipe. The
# commands run as the runner's uid (scripts/coverage.sh in the mounted workspace).
FROM alpine:3.24@sha256:3f4d6b32e26703c6082839234f43769d5046b3ac7c910033911d1e71d8cd630a

ARG LLVM_VERSION=22

RUN apk add --no-cache \
        bash build-base "clang${LLVM_VERSION}" compiler-rt cmake git linux-headers \
        "llvm${LLVM_VERSION}" mesa-vulkan-swrast nasm perl pkgconf python3 samurai vulkan-headers \
        vulkan-loader-dev

ENV PATH=/usr/lib/llvm${LLVM_VERSION}/bin:$PATH \
    CC=clang \
    CXX=clang++
