# SPDX-License-Identifier: Apache-2.0 OR MIT
# hdrg.mctf: the motion-compensated temporal noise reduction of the PREFILTER stage, on the CPU
# and the GPU
# A plugin project (cmake/HdrgPlugin.cmake): built into the library from src/plugins, or apart
# against the installed package (a loadable module)
cmake_minimum_required(VERSION 3.29)
project(hdrgrading-mctf LANGUAGES C)

if(NOT COMMAND hdrg_plugin)
    find_package(hdrgrading CONFIG REQUIRED)
endif()

hdrg_plugin(mctf ENTRY hdrg_filter_mctf_init
    SOURCES mctf.c
    WEBGPU webgpu_mctf.c
    GK gk_mctf.c
    SHADERS mctf
    LIBRARIES hdrgrading::foundation hdrgrading::plugin_shared)
