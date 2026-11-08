# SPDX-License-Identifier: Apache-2.0 OR MIT
# hdrgrading-plugin-shared: what several of the plugins share (no plugin itself): the block motion
# estimation of hdrg.interp.mc, hdrg.mctf and hdrg.scale.temporal (blockme.h) and the noise of a
# frame's HDRG_SD_NOISE as the denoisers read it (noise.h). Headers: include/hdrgrading/plugins/.
# In the library's build its objects go into the library
cmake_minimum_required(VERSION 3.29)
project(hdrgrading-plugin-shared LANGUAGES C)

if(NOT COMMAND hdrg_plugin)
    find_package(hdrgrading CONFIG REQUIRED)
    return()
endif()

add_library(hdrgrading_plugin_shared OBJECT blockme.c noise.c)
add_library(hdrgrading::plugin_shared ALIAS hdrgrading_plugin_shared)
# position-independent: the installed static library links into the plugins' modules too
set_target_properties(hdrgrading_plugin_shared PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(hdrgrading_plugin_shared PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_link_libraries(hdrgrading_plugin_shared PUBLIC hdrgrading::headers hdrgrading::foundation)
if(COMMAND hdrg_target_warnings)
    hdrg_target_warnings(hdrgrading_plugin_shared)
endif()
if(HDRG_PLUGINS_BUILTIN)
    set_property(GLOBAL APPEND PROPERTY HDRG_BUILTIN_TARGETS hdrgrading_plugin_shared)
endif()
