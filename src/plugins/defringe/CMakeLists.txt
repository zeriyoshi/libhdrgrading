# SPDX-License-Identifier: Apache-2.0 OR MIT
# hdrg.defringe: the color fringe removal of the PREFILTER stage, on the CPU and the GPU
# A plugin project (cmake/HdrgPlugin.cmake): built into the library from src/plugins, or apart
# against the installed package (a loadable module)
cmake_minimum_required(VERSION 3.29)
project(hdrgrading-defringe LANGUAGES C)

if(NOT COMMAND hdrg_plugin)
    find_package(hdrgrading CONFIG REQUIRED)
endif()

hdrg_plugin(defringe ENTRY hdrg_filter_defringe_init
    SOURCES defringe.c
    WEBGPU webgpu_defringe.c
    GK gk_defringe.c
    SHADERS defringe
    LIBRARIES hdrgrading::foundation)
