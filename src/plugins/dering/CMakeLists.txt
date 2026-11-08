# SPDX-License-Identifier: Apache-2.0 OR MIT
# hdrg.dering: the deringing of the PREFILTER stage, on the CPU and the GPU
# A plugin project (cmake/HdrgPlugin.cmake): built into the library from src/plugins, or apart
# against the installed package (a loadable module)
cmake_minimum_required(VERSION 3.29)
project(hdrgrading-dering LANGUAGES C)

if(NOT COMMAND hdrg_plugin)
    find_package(hdrgrading CONFIG REQUIRED)
endif()

hdrg_plugin(dering ENTRY hdrg_filter_dering_init
    SOURCES dering.c
    WEBGPU webgpu_dering.c
    GK gk_dering.c
    SHADERS dering
    LIBRARIES hdrgrading::foundation)
