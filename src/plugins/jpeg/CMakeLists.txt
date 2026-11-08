# SPDX-License-Identifier: Apache-2.0 OR MIT
# hdrg.jpeg: the JPEG codec over the bundled jpegli (reading, and the gain map, hybrid and PQ forms)
# A plugin project (cmake/HdrgPlugin.cmake): built into the library from src/plugins, or apart
# against the installed package (a loadable module)
cmake_minimum_required(VERSION 3.29)
project(hdrgrading-jpeg LANGUAGES C)

if(NOT COMMAND hdrg_plugin)
    find_package(hdrgrading CONFIG REQUIRED)
endif()

hdrg_plugin(jpeg ENTRY hdrg_codec_jpeg_init
    SOURCES jpeg.c jpeg_mux.c
    LIBRARIES hdrgrading::foundation hdrg::jpegli)
