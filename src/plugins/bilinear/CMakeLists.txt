# SPDX-License-Identifier: Apache-2.0 OR MIT
# hdrg.scale.bilinear: the bilinear SCALE filter
# A plugin project (cmake/HdrgPlugin.cmake): built into the library from src/plugins, or apart
# against the installed package (a loadable module)
cmake_minimum_required(VERSION 3.29)
project(hdrgrading-bilinear LANGUAGES C)

if(NOT COMMAND hdrg_plugin)
    find_package(hdrgrading CONFIG REQUIRED)
endif()

hdrg_plugin(bilinear ENTRY hdrg_filter_bilinear_init
    SOURCES bilinear.c
    LIBRARIES hdrgrading::foundation)
