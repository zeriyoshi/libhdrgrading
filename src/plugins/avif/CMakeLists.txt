# SPDX-License-Identifier: Apache-2.0 OR MIT
# hdrg.avif: the AVIF codec over the system's libavif (pkg-config, 1.2 or later: the gain map API),
# always a loadable module (never linked into the library)
# A plugin project (cmake/HdrgPlugin.cmake): built into the library from src/plugins, or apart
# against the installed package (a loadable module)
cmake_minimum_required(VERSION 3.29)
project(hdrgrading-avif LANGUAGES C)

if(NOT COMMAND hdrg_plugin)
    find_package(hdrgrading CONFIG REQUIRED)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(HDRG_LIBAVIF QUIET IMPORTED_TARGET GLOBAL libavif>=1.2)
if(NOT HDRG_LIBAVIF_FOUND)
    message(FATAL_ERROR "HDRG_WITH_AVIF: no libavif 1.2 or later by pkg-config (libavif-dev, "
        "brew install libavif), or configure with -DHDRG_WITH_AVIF=OFF")
endif()
message(STATUS "HDRG_WITH_AVIF: the module over the system's libavif ${HDRG_LIBAVIF_VERSION}")
hdrg_plugin(avif ENTRY hdrg_avif_init MODULE
    SOURCES avif.c
    LIBRARIES PkgConfig::HDRG_LIBAVIF)
