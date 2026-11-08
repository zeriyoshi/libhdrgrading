#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0 OR MIT
# Builds FFmpeg's latest stable release from source and installs it under PREFIX: its shared
# libraries for HDRG_WITH_FFMPEG (tools/ffmpeg, hdrg-ffmpeg; found through pkg-config with
# PKG_CONFIG_PATH=PREFIX/lib/pkgconfig) and its ffmpeg / ffprobe for a macOS host, which has no
# system FFmpeg. LGPL only and no external libraries: its own decoders and encoders, and on macOS
# VideoToolbox (hevc_videotoolbox for hdrg-ffmpeg's 10-bit HEVC). Nothing is detected from the
# host (no Homebrew libraries mixed in).
#
#   scripts/build_ffmpeg.sh [PREFIX]          (default: build/deps/ffmpeg)
#   scripts/build_ffmpeg.sh --filter HDRG_PREFIX [PREFIX]
#                                             (default: build/deps/ffmpeg-filter)
#
# --filter: the same FFmpeg with src/bindings/ffmpeg/patches applied (the libhdrgrading filter,
# --enable-libhdrgrading) against the libhdrgrading installed in HDRG_PREFIX (a shared one: the
# release preset's), found through its pkg-config file and at run time through an rpath to its lib.
#
# HDRG_FFMPEG_WORK: where the source and its build go (default: build/deps).
# HDRG_FFMPEG_CACHE: a directory keeping the tag's clone as fetched (ffmpeg-<tag>, cloned there
# when missing, never patched or built in); a work tree without its source is copied from it instead
# of cloned (CI keeps it in the GitHub Actions cache).
set -euo pipefail

# the latest stable release (the dev image has Debian's 7.1: the bindings build with both); updated
# by hand
FFMPEG_TAG=n9.0.2

workspace="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
hdrg=""
if [[ "${1:-}" == --filter ]]; then
    hdrg="$(cd "${2:?--filter needs the libhdrgrading prefix}" && pwd)"
    shift 2
fi
prefix="${1:-$workspace/build/deps/ffmpeg${hdrg:+-filter}}"
work="${HDRG_FFMPEG_WORK:-$workspace/build/deps}"
src="$work/ffmpeg-$FFMPEG_TAG${hdrg:+-filter}-src"
jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

mkdir -p "$work"

if [[ ! -d "$src/.git" ]]; then
    if [[ -n "${HDRG_FFMPEG_CACHE:-}" ]]; then
        clean="$HDRG_FFMPEG_CACHE/ffmpeg-$FFMPEG_TAG"
        if [[ ! -d "$clean/.git" ]]; then
            rm -rf "$clean"
            mkdir -p "$HDRG_FFMPEG_CACHE"
            git clone --depth 1 --branch "$FFMPEG_TAG" https://github.com/FFmpeg/FFmpeg.git "$clean"
        fi
        rm -rf "$src"
        cp -R "$clean" "$src"
    else
        git clone --depth 1 --branch "$FFMPEG_TAG" https://github.com/FFmpeg/FFmpeg.git "$src"
    fi
fi

# macOS: VideoToolbox (the libraries' install names are their paths); elsewhere the libraries find
# each other through an rpath to their directory
options=(--enable-rpath)
if [[ "$(uname -s)" == Darwin ]]; then
    options=(--enable-videotoolbox --enable-audiotoolbox)
fi

# the patches once (a tree they are in already is left as it is; one with another version of them,
# from before a patch changed, goes back to the tag first)
if [[ -n "$hdrg" ]]; then
    patches=("$workspace"/src/bindings/ffmpeg/patches/*.patch)
    for patch in "${patches[@]}"; do
        if ! git -C "$src" apply --check "$patch" 2>/dev/null &&
            ! git -C "$src" apply --check --reverse "$patch" 2>/dev/null; then
            printf 'resetting %s to %s: it has another version of the patches\n' "$src" "$FFMPEG_TAG" >&2
            git -C "$src" reset -q --hard
            git -C "$src" clean -q -fdx
            break
        fi
    done
    for patch in "${patches[@]}"; do
        if git -C "$src" apply --check "$patch" 2>/dev/null; then
            git -C "$src" apply "$patch"
        elif ! git -C "$src" apply --check --reverse "$patch" 2>/dev/null; then
            printf 'cannot apply %s to %s\n' "$patch" "$src" >&2
            exit 1
        fi
    done
    options+=(--enable-libhdrgrading "--extra-ldflags=-Wl,-rpath,$hdrg/lib")
    export PKG_CONFIG_PATH="$hdrg/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
fi

cd "$src"
./configure --prefix="$prefix" --enable-shared --disable-static --disable-autodetect \
    --disable-doc --disable-debug --enable-zlib "${options[@]}"
make -j"$jobs"
make install

printf 'FFmpeg %s in %s (PKG_CONFIG_PATH=%s/lib/pkgconfig)\n' "$FFMPEG_TAG" "$prefix" "$prefix"
