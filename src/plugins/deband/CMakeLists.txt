# SPDX-License-Identifier: Apache-2.0 OR MIT
# hdrg.deband: the debanding of the PREFILTER stage, on the CPU and the GPU
# A plugin project (cmake/HdrgPlugin.cmake): built into the library from src/plugins, or apart
# against the installed package (a loadable module)
cmake_minimum_required(VERSION 3.29)
project(hdrgrading-deband LANGUAGES C)

if(NOT COMMAND hdrg_plugin)
    find_package(hdrgrading CONFIG REQUIRED)
endif()

hdrg_plugin(deband ENTRY hdrg_filter_deband_init
    SOURCES deband.c
    WEBGPU webgpu_deband.c
    GK gk_deband.c
    SHADERS deband
    LIBRARIES hdrgrading::foundation)
