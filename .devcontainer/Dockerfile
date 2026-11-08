# SPDX-License-Identifier: Apache-2.0 OR MIT
# Debian 13 (trixie) development image for libhdrgrading (modeled on zendriver's dev image).
# Used by the DevContainer (.devcontainer/devcontainer.json), scripts/docker_dev.sh and the Linux CI
# jobs (.github/workflows/ci.yml), so local and CI toolchains are identical.

# GCC 16 comes from the official image (Debian trixie only ships GCC 14) and is copied under
# /opt/gcc-16; it is reachable as gcc-16 / g++-16.
FROM gcc:16.2.0-trixie@sha256:07280eb2e744f1b7a2d8d4a3894c089ea07266c109758afc388a634bd098e4eb AS gcc

# debian:13.7-slim, multi-arch index digest
FROM debian:13.7-slim@sha256:a72b4433de1cf7d6bae0c4cee74d0547f58601a643b3375f070c51600d63b6b2

ARG DEBIAN_FRONTEND=noninteractive
# apt.llvm.org: llvm-toolchain-trixie-<LLVM_VERSION> (clang-format / clang-tidy are pinned to 23)
ARG LLVM_VERSION=23
ARG LLVM_APT_FINGERPRINT=6084F3CF814B57C1CF12EFD515CF4D18AF4F7421
# renovate: datasource=custom.valgrind depName=valgrind
ARG VALGRIND_VERSION=3.28.0
ARG VALGRIND_SHA256=9f7a18d128f05d3a80fea53e6953f34aad7200a425ceaee46099a8a798a7dd9c
# Must match the branch of third_party/emsdk in .gitmodules (checked by CI)
ARG EMSDK_VERSION=6.0.13
# Must match the branch of third_party/vapoursynth in .gitmodules (checked by CI)
ARG VAPOURSYNTH_VERSION=R81
# Must match DAWN_TAG of scripts/build_dawn.sh (checked by CI)
ARG DAWN_TAG=v20261019.211215
ARG USERNAME=dev
ARG USER_UID=1000
ARG USER_GID=1000

ENV LANG=C.UTF-8

RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends ca-certificates curl gpg; \
    install -d -m 0755 /etc/apt/keyrings; \
    curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key -o /etc/apt/keyrings/apt.llvm.org.asc; \
    fingerprint="$(gpg --show-keys --with-colons /etc/apt/keyrings/apt.llvm.org.asc \
        | awk -F: '/^fpr:/ { print $10; exit }')"; \
    test "$fingerprint" = "$LLVM_APT_FINGERPRINT"; \
    rm -rf /root/.gnupg; \
    printf '%s\n' \
        'Types: deb' \
        'URIs: https://apt.llvm.org/trixie/' \
        "Suites: llvm-toolchain-trixie-${LLVM_VERSION}" \
        'Components: main' \
        'Signed-By: /etc/apt/keyrings/apt.llvm.org.asc' \
        > /etc/apt/sources.list.d/llvm.sources; \
    apt-get update; \
    apt-get install -y --no-install-recommends \
        "clang-${LLVM_VERSION}" \
        "clangd-${LLVM_VERSION}" \
        "clang-format-${LLVM_VERSION}" \
        "clang-tidy-${LLVM_VERSION}" \
        "libclang-rt-${LLVM_VERSION}-dev" \
        "libc++-${LLVM_VERSION}-dev" \
        "libc++abi-${LLVM_VERSION}-dev" \
        "lld-${LLVM_VERSION}" \
        "llvm-${LLVM_VERSION}" \
        bzip2 \
        ccache \
        cmake \
        file \
        g++ \
        gcc \
        gdb \
        git \
        jq \
        less \
        libc6-dbg \
        libc6-dev \
        make \
        ninja-build \
        patch \
        pkg-config \
        procps \
        python3 \
        strace \
        sudo \
        xz-utils; \
    for tool in clang clang++ clang-cpp clangd clang-format clang-tidy run-clang-tidy \
        clang-apply-replacements ld.lld lld llvm-addr2line llvm-ar llvm-config llvm-cov \
        llvm-cxxfilt llvm-nm llvm-objcopy llvm-objdump llvm-profdata llvm-ranlib llvm-readelf \
        llvm-readobj llvm-size llvm-strings llvm-strip llvm-symbolizer; do \
        if [ -e "/usr/lib/llvm-${LLVM_VERSION}/bin/${tool}" ]; then \
            ln -sf "/usr/lib/llvm-${LLVM_VERSION}/bin/${tool}" "/usr/local/bin/${tool}"; \
        fi; \
    done; \
    clang --version; \
    rm -rf /var/lib/apt/lists/*

COPY --from=gcc /usr/local /opt/gcc-16
RUN set -eux; \
    printf '%s\n' /opt/gcc-16/lib64 /opt/gcc-16/lib > /etc/ld.so.conf.d/gcc-16.conf; \
    ldconfig; \
    ln -sf /opt/gcc-16/bin/gcc /usr/local/bin/gcc-16; \
    ln -sf /opt/gcc-16/bin/g++ /usr/local/bin/g++-16; \
    ln -sf /opt/gcc-16/bin/gcov /usr/local/bin/gcov-16; \
    gcc-16 --version

# Debian trixie ships Valgrind 3.24; build the current release from source
RUN set -eux; \
    curl -fsSL -o /tmp/valgrind.tar.bz2 \
        "https://sourceware.org/pub/valgrind/valgrind-${VALGRIND_VERSION}.tar.bz2"; \
    echo "${VALGRIND_SHA256}  /tmp/valgrind.tar.bz2" | sha256sum -c -; \
    mkdir /tmp/valgrind; \
    tar -xjf /tmp/valgrind.tar.bz2 -C /tmp/valgrind --strip-components=1; \
    cd /tmp/valgrind; \
    ./configure --prefix=/usr/local CC=gcc; \
    make -j"$(nproc)"; \
    make install; \
    cd /; \
    rm -rf /tmp/valgrind /tmp/valgrind.tar.bz2; \
    valgrind --version

# VapourSynth (Debian trixie ships none): the core, vspipe and the Python module, to run the plugin
# (bindings/vapoursynth) the way it is used. glslang and the Vulkan headers come from its meson
# wraps (it needs headers newer than trixie's libvulkan-dev 1.4.309), Cython (3.3 and later, trixie
# has 3.0) from a build-only venv. Everything (the core, vspipe) installs into the vapoursynth package
# of the system Python's site directory; vspipe is linked onto the PATH (configured for the user below)
RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends libzimg-dev meson python3-dev python3-venv; \
    python3 -m venv /tmp/cython; \
    /tmp/cython/bin/pip install --no-cache-dir "Cython>=3.3.0,<3.4"; \
    printf '%s\n' '[binaries]' "cython = '/tmp/cython/bin/cython'" "python = '/usr/bin/python3'" \
        > /tmp/vapoursynth-native.ini; \
    git clone --depth 1 --branch "$VAPOURSYNTH_VERSION" https://github.com/vapoursynth/vapoursynth.git \
        /tmp/vapoursynth; \
    site="$(python3 -c 'import site; print(site.getsitepackages()[0])')"; \
    meson setup /tmp/vapoursynth/build /tmp/vapoursynth --prefix=/usr/local --buildtype=release \
        --native-file /tmp/vapoursynth-native.ini \
        -Dpython.platlibdir="$site" -Dpython.purelibdir="$site"; \
    ninja -C /tmp/vapoursynth/build; \
    ninja -C /tmp/vapoursynth/build install; \
    ln -s "$(find /usr/local/lib -path '*/vapoursynth/vspipe' -type f | head -n 1)" /usr/local/bin/vspipe; \
    rm -rf /tmp/vapoursynth /tmp/cython /tmp/vapoursynth-native.ini /var/lib/apt/lists/*; \
    python3 -c 'import vapoursynth; print(vapoursynth.core.version())'

# The host GID may already exist in the image (macOS hosts use 20 = dialout); reuse it then.
RUN set -eux; \
    if ! getent group "$USER_GID" >/dev/null; then \
        groupadd --gid "$USER_GID" "$USERNAME"; \
    fi; \
    useradd --uid "$USER_UID" --gid "$USER_GID" --create-home --shell /bin/bash "$USERNAME"; \
    printf '%s ALL=(ALL) NOPASSWD:ALL\n' "$USERNAME" > "/etc/sudoers.d/$USERNAME"; \
    chmod 0440 "/etc/sudoers.d/$USERNAME"; \
    install -d -o "$USER_UID" -g "$USER_GID" "/home/$USERNAME/.cache"

# The workspace's third_party/emsdk may hold binaries for the host OS (e.g. macOS), so the image
# has its own emsdk. emsdk_env.sh clears EM_CACHE, so the cache location goes into the config.
RUN set -eux; \
    git clone --depth 1 --branch "$EMSDK_VERSION" https://github.com/emscripten-core/emsdk.git /opt/emsdk; \
    /opt/emsdk/emsdk install "$EMSDK_VERSION"; \
    /opt/emsdk/emsdk activate "$EMSDK_VERSION"; \
    printf "CACHE = '/home/%s/.cache/emscripten'\n" "$USERNAME" >> /opt/emsdk/.emscripten; \
    rm -rf /opt/emsdk/.git /opt/emsdk/downloads /opt/emsdk/upstream/emscripten/cache

# Dawn (WebGPU) for the GPU filters (HDRG_WITH_WEBGPU), built as scripts/build_dawn.sh builds it, with
# the Vulkan backend, into /opt/dawn (HDRG_DAWN_DIR); Mesa's lavapipe (Vulkan on the CPU) runs the GPU
# tests without a GPU, and the library's own Vulkan backend (HDRG_WITH_VULKAN) builds on the loader
# and the headers of libvulkan-dev (installed after VapourSynth, which builds on newer headers of its
# own). Tint's CLI as scripts/build_dawn.sh --tint builds it (the MSL, SPIR-V and HLSL writers), into
# /opt/dawn/bin/tint (scripts/gen_shaders.sh --check)
RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends libvulkan1 libvulkan-dev mesa-vulkan-drivers; \
    git clone --depth 1 --branch "$DAWN_TAG" https://github.com/google/dawn.git /tmp/dawn; \
    CC=clang CXX=clang++ cmake -S /tmp/dawn -B /tmp/dawn/out -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DDAWN_FETCH_DEPENDENCIES=ON -DDAWN_ENABLE_INSTALL=ON \
        -DDAWN_BUILD_MONOLITHIC_LIBRARY=SHARED \
        -DDAWN_BUILD_SAMPLES=OFF -DDAWN_BUILD_TESTS=OFF -DDAWN_BUILD_PROTOBUF=OFF \
        -DTINT_BUILD_TESTS=OFF -DTINT_BUILD_CMD_TOOLS=OFF -DDAWN_SUPPORTS_CXX_MODULES=OFF \
        -DDAWN_USE_GLFW=OFF -DDAWN_USE_X11=OFF -DDAWN_USE_WAYLAND=OFF \
        -DDAWN_ENABLE_DESKTOP_GL=OFF -DDAWN_ENABLE_OPENGLES=OFF; \
    cmake --build /tmp/dawn/out; \
    cmake --install /tmp/dawn/out --prefix /opt/dawn; \
    install -m 0644 /tmp/dawn/out/gen/include/dawn/dawn_version.h /opt/dawn/include/dawn/dawn_version.h; \
    CC=clang CXX=clang++ cmake -S /tmp/dawn -B /tmp/dawn/out-tint -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DDAWN_FETCH_DEPENDENCIES=ON -DDAWN_BUILD_SAMPLES=OFF -DDAWN_BUILD_TESTS=OFF \
        -DDAWN_BUILD_PROTOBUF=OFF -DDAWN_SUPPORTS_CXX_MODULES=OFF -DDAWN_USE_GLFW=OFF \
        -DDAWN_USE_X11=OFF -DDAWN_USE_WAYLAND=OFF -DDAWN_ENABLE_DESKTOP_GL=OFF \
        -DDAWN_ENABLE_OPENGLES=OFF -DDAWN_ENABLE_VULKAN=OFF -DDAWN_ENABLE_METAL=OFF \
        -DDAWN_ENABLE_D3D11=OFF -DDAWN_ENABLE_D3D12=OFF -DDAWN_ENABLE_NULL=OFF \
        -DTINT_BUILD_TESTS=OFF -DTINT_BUILD_CMD_TOOLS=ON -DTINT_BUILD_WGSL_READER=ON \
        -DTINT_BUILD_WGSL_WRITER=ON -DTINT_BUILD_MSL_WRITER=ON -DTINT_BUILD_SPV_READER=OFF \
        -DTINT_BUILD_SPV_WRITER=ON -DTINT_BUILD_HLSL_WRITER=ON -DTINT_BUILD_GLSL_WRITER=OFF \
        -DTINT_BUILD_GLSL_VALIDATOR=OFF; \
    cmake --build /tmp/dawn/out-tint --target tint_cmd_tint_cmd; \
    install -D -m 0755 /tmp/dawn/out-tint/tint /opt/dawn/bin/tint; \
    rm -rf /tmp/dawn /var/lib/apt/lists/*

# FFmpeg's libraries for bindings/ffmpeg and hdrg-ffmpeg (trixie: FFmpeg 7.1), GStreamer's for
# bindings/gstreamer with the base elements and tools its tests and checks use (1.26)
RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends libavcodec-dev libavformat-dev libavutil-dev \
        libswscale-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
        gstreamer1.0-plugins-base gstreamer1.0-tools; \
    rm -rf /var/lib/apt/lists/*

# the AVIF module (src/plugins/avif, HDRG_WITH_AVIF: the system's libavif by pkg-config, 1.2 or
# later, with its libaom and dav1d), and the x86 assembly of x265 (HDRG_WITH_HEIF fetching it)
RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends libavif-dev nasm; \
    rm -rf /var/lib/apt/lists/*

# hdrg-gui (tools/gui, HDRG_BUILD_GUI): SDL3's X11 and Wayland windows, and GStreamer's decoders
# its media come through (good: PNG / JPEG / MP4; libav: H.264, HEVC, ...)
RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends libx11-dev libxext-dev libxrandr-dev libxcursor-dev \
        libxi-dev libxss-dev libxkbcommon-dev libwayland-dev libegl-dev libdecor-0-dev \
        gstreamer1.0-plugins-good gstreamer1.0-libav; \
    rm -rf /var/lib/apt/lists/*

ENV HDRG_EMSDK_DIR=/opt/emsdk \
    HDRG_DAWN_DIR=/opt/dawn \
    CMAKE_GENERATOR=Ninja \
    HDRG_BUILD_PREFIX=linux- \
    CCACHE_DIR=/home/dev/.cache/ccache

USER $USERNAME
# vspipe embeds Python through VSScript, which reads where Python is from the user's configuration
RUN python3 -m vapoursynth config && vspipe --version
WORKDIR /workspaces/libhdrgrading
