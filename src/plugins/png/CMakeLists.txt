# SPDX-License-Identifier: Apache-2.0 OR MIT
# hdrg.png: the PNG reader over libpng
# A plugin project (cmake/HdrgPlugin.cmake): built into the library from src/plugins, or apart
# against the installed package (a loadable module)
cmake_minimum_required(VERSION 3.29)
project(hdrgrading-png LANGUAGES C)

if(NOT COMMAND hdrg_plugin)
    find_package(hdrgrading CONFIG REQUIRED)
endif()

hdrg_plugin(png ENTRY hdrg_codec_png_init
    SOURCES png.c
    LIBRARIES hdrgrading::foundation hdrg::png)
