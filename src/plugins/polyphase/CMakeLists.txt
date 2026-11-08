# SPDX-License-Identifier: Apache-2.0 OR MIT
# hdrg.scale.polyphase: Lanczos and bicubic SCALE filters, and Lanczos on the GPU
# A plugin project (cmake/HdrgPlugin.cmake): built into the library from src/plugins, or apart
# against the installed package (a loadable module)
cmake_minimum_required(VERSION 3.29)
project(hdrgrading-polyphase LANGUAGES C)

if(NOT COMMAND hdrg_plugin)
    find_package(hdrgrading CONFIG REQUIRED)
endif()

hdrg_plugin(polyphase ENTRY hdrg_filter_polyphase_init
    SOURCES polyphase.c
    WEBGPU webgpu_lanczos.c
    GK gk_lanczos.c
    SHADERS lanczos
    LIBRARIES hdrgrading::foundation)
