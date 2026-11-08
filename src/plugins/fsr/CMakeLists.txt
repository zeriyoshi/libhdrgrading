# SPDX-License-Identifier: Apache-2.0 OR MIT
# hdrg.scale.fsr: AMD FidelityFX Super Resolution 1 (MIT), on the CPU and the GPU
# A plugin project (cmake/HdrgPlugin.cmake): built into the library from src/plugins, or apart
# against the installed package (a loadable module)
cmake_minimum_required(VERSION 3.29)
project(hdrgrading-fsr LANGUAGES C)

if(NOT COMMAND hdrg_plugin)
    find_package(hdrgrading CONFIG REQUIRED)
endif()

hdrg_plugin(fsr ENTRY hdrg_filter_fsr_init
    SOURCES fsr.c
    WEBGPU webgpu_fsr.c
    GK gk_fsr.c
    SHADERS fsr
    LIBRARIES hdrgrading::foundation)
