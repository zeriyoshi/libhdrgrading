# SPDX-License-Identifier: Apache-2.0 OR MIT
# hdrg.jxl: the JPEG XL writer over the bundled libjxl (third_party/libjxl, its brotli and the
# library's lcms2: cmake/HdrgDeps.cmake)
# A plugin project (cmake/HdrgPlugin.cmake): built into the library from src/plugins, or apart
# against the installed package (a loadable module)
cmake_minimum_required(VERSION 3.29)
project(hdrgrading-jxl LANGUAGES C)

if(NOT COMMAND hdrg_plugin)
    find_package(hdrgrading CONFIG REQUIRED)
endif()

hdrg_plugin(jxl ENTRY hdrg_codec_jxl_init
    SOURCES jxl.c
    LIBRARIES hdrgrading::foundation hdrg::jxl)
