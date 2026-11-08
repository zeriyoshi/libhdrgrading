# SPDX-License-Identifier: Apache-2.0 OR MIT
# hdrg.metalfx: MetalFX's spatial scaler as a SCALE filter (Apple, HDRG_WITH_METAL)
# A plugin project (cmake/HdrgPlugin.cmake): built into the library from src/plugins, or apart
# against the installed package (a loadable module)
cmake_minimum_required(VERSION 3.29)
project(hdrgrading-metalfx LANGUAGES C)

if(NOT COMMAND hdrg_plugin)
    find_package(hdrgrading CONFIG REQUIRED)
endif()

if(NOT HDRG_WITH_METAL)
    message(FATAL_ERROR "hdrg.metalfx needs the GPU layer's Metal (HDRG_WITH_METAL)")
endif()
hdrg_plugin(metalfx ENTRY hdrg_filter_metalfx_init
    METAL metalfx.c
    LIBRARIES hdrgrading::foundation)
