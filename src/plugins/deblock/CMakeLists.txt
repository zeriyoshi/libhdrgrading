# SPDX-License-Identifier: Apache-2.0 OR MIT
# hdrg.deblock: the deblocking of the PREFILTER stage, on the CPU and the GPU
# A plugin project (cmake/HdrgPlugin.cmake): built into the library from src/plugins, or apart
# against the installed package (a loadable module)
cmake_minimum_required(VERSION 3.29)
project(hdrgrading-deblock LANGUAGES C)

if(NOT COMMAND hdrg_plugin)
    find_package(hdrgrading CONFIG REQUIRED)
endif()

hdrg_plugin(deblock ENTRY hdrg_filter_deblock_init
    SOURCES deblock.c
    WEBGPU webgpu_deblock.c
    GK gk_deblock.c
    SHADERS deblock
    LIBRARIES hdrgrading::foundation)
