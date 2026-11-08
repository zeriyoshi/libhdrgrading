# SPDX-License-Identifier: Apache-2.0 OR MIT
# hdrg.denoise.spatial: the spatial noise reduction of the PREFILTER stage, on the CPU and the GPU
# A plugin project (cmake/HdrgPlugin.cmake): built into the library from src/plugins, or apart
# against the installed package (a loadable module)
cmake_minimum_required(VERSION 3.29)
project(hdrgrading-denoise LANGUAGES C)

if(NOT COMMAND hdrg_plugin)
    find_package(hdrgrading CONFIG REQUIRED)
endif()

hdrg_plugin(denoise ENTRY hdrg_filter_denoise_init
    SOURCES denoise.c
    WEBGPU webgpu_denoise.c
    GK gk_denoise.c
    SHADERS denoise
    LIBRARIES hdrgrading::foundation hdrgrading::plugin_shared)
