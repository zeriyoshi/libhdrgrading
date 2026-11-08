# SPDX-License-Identifier: Apache-2.0 OR MIT
# hdrg.interp: the INTERP stage's repeat and blend, and with HDRG_WITH_TEMPORAL_INTERP the
# motion-compensated interpolation hdrg.interp.mc on the CPU and the GPU
# A plugin project (cmake/HdrgPlugin.cmake): built into the library from src/plugins, or apart
# against the installed package (a loadable module)
cmake_minimum_required(VERSION 3.29)
project(hdrgrading-interp LANGUAGES C)

if(NOT COMMAND hdrg_plugin)
    find_package(hdrgrading CONFIG REQUIRED)
endif()

# hdrg.interp.mc: the library's HDRG_TEMPORAL_INTERP, or apart an option of its own
if(NOT DEFINED HDRG_TEMPORAL_INTERP)
    option(HDRG_WITH_TEMPORAL_INTERP "Video: the motion-compensated frame interpolation hdrg.interp.mc" OFF)
    set(HDRG_TEMPORAL_INTERP ${HDRG_WITH_TEMPORAL_INTERP})
endif()
if(HDRG_TEMPORAL_INTERP)
    hdrg_plugin(interp ENTRY hdrg_filter_interp_init
        SOURCES interp.c interp_mc.c
        WEBGPU webgpu_interp.c
        GK gk_interp.c
        SHADERS interp
        DEFINITIONS HDRG_INTERP_MC
        LIBRARIES hdrgrading::foundation hdrgrading::plugin_shared)
else()
    hdrg_plugin(interp ENTRY hdrg_filter_interp_init
        SOURCES interp.c
        LIBRARIES hdrgrading::foundation)
endif()
