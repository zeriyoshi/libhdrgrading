# SPDX-License-Identifier: Apache-2.0 OR MIT
# hdrg.scale.edge: the library's edge-adaptive upscale on the CPU and the GPU, and with
# HDRG_WITH_TEMPORAL_SR its multi-frame upscale hdrg.scale.temporal
# A plugin project (cmake/HdrgPlugin.cmake): built into the library from src/plugins, or apart
# against the installed package (a loadable module)
cmake_minimum_required(VERSION 3.29)
project(hdrgrading-edge LANGUAGES C)

if(NOT COMMAND hdrg_plugin)
    find_package(hdrgrading CONFIG REQUIRED)
endif()

# hdrg.scale.temporal: the library's HDRG_TEMPORAL_SR, or apart an option of its own
if(NOT DEFINED HDRG_TEMPORAL_SR)
    option(HDRG_WITH_TEMPORAL_SR "Video: the multi-frame upscale hdrg.scale.temporal" OFF)
    set(HDRG_TEMPORAL_SR ${HDRG_WITH_TEMPORAL_SR})
endif()
set(hdrg_edge_sources edge.c)
set(hdrg_edge_definitions "")
if(HDRG_TEMPORAL_SR)
    list(APPEND hdrg_edge_sources temporal.c)
    set(hdrg_edge_definitions HDRG_SCALE_TEMPORAL)
endif()
hdrg_plugin(edge ENTRY hdrg_filter_edge_init
    SOURCES ${hdrg_edge_sources}
    WEBGPU webgpu_edge.c
    GK gk_edge.c
    SHADERS edge
    DEFINITIONS ${hdrg_edge_definitions}
    LIBRARIES hdrgrading::foundation hdrgrading::plugin_shared)
