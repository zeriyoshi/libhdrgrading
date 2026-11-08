# SPDX-License-Identifier: Apache-2.0 OR MIT
# hdrg.stats.temporal: the STATS stage's statistics of a video session across its frames (its
# temporal modes WINDOWED with a window, RECURSIVE and TWO_PASS), built with
# HDRG_WITH_TEMPORAL_FILTER alone
# A plugin project (cmake/HdrgPlugin.cmake): built into the library from src/plugins, or apart
# against the installed package (a loadable module)
cmake_minimum_required(VERSION 3.29)
project(hdrgrading-stats LANGUAGES C)

if(NOT COMMAND hdrg_plugin)
    find_package(hdrgrading CONFIG REQUIRED)
endif()

hdrg_plugin(stats ENTRY hdrg_filter_stats_init
    SOURCES stats.c
    LIBRARIES hdrgrading::foundation)
