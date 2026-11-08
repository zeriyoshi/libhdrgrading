# SPDX-License-Identifier: Apache-2.0 OR MIT
# hdrg.gif: the GIF reader over giflib
# A plugin project (cmake/HdrgPlugin.cmake): built into the library from src/plugins, or apart
# against the installed package (a loadable module)
cmake_minimum_required(VERSION 3.29)
project(hdrgrading-gif LANGUAGES C)

if(NOT COMMAND hdrg_plugin)
    find_package(hdrgrading CONFIG REQUIRED)
endif()

hdrg_plugin(gif ENTRY hdrg_codec_gif_init
    SOURCES gif.c
    LIBRARIES hdrgrading::foundation hdrg::gif)
