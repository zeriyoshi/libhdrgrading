# SPDX-License-Identifier: Apache-2.0 OR MIT
# hdrg.heif: the HEIF writer over libheif and its HEVC encoder (cmake/HdrgHeif.cmake: the system's,
# else fetched; x265 is GPL), always a loadable module (never linked into the library)
# A plugin project (cmake/HdrgPlugin.cmake): built into the library from src/plugins, or apart
# against the installed package (a loadable module)
cmake_minimum_required(VERSION 3.29)
project(hdrgrading-heif LANGUAGES C)

if(NOT COMMAND hdrg_plugin)
    find_package(hdrgrading CONFIG REQUIRED)
endif()

if(NOT TARGET hdrg::heif)
    include(HdrgHeif)
endif()
hdrg_plugin(heif ENTRY hdrg_heif_init MODULE
    SOURCES heif.c bmff.c
    LIBRARIES hdrg::heif)
# the fetched libheif (its install name @rpath) is found by the build tree's path, an install's
# beside the library: one level up from the module's
if(HDRG_HEIF_FETCHED)
    set_property(TARGET ${HDRG_PLUGIN_TARGET} APPEND PROPERTY BUILD_RPATH "${HDRG_HEIF_PREFIX}/lib")
    if(APPLE)
        set_target_properties(${HDRG_PLUGIN_TARGET} PROPERTIES INSTALL_RPATH "@loader_path/..")
    else()
        set_target_properties(${HDRG_PLUGIN_TARGET} PROPERTIES INSTALL_RPATH "$ORIGIN/..")
    endif()
endif()
