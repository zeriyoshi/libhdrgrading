# SPDX-License-Identifier: Apache-2.0 OR MIT
# hdrgrading-gpu: the GPU layers the library and its plugins build on (no public API of the
# library): WebGPU (webgpu.c: Dawn's webgpu.h, a device's thread), Metal (metal.m: Objective-C with
# ARC, the only file that sees Metal and MetalFX) and Vulkan (vulkan.c: its loader opened at run
# time), the layer of the two native APIs (gk.h: gk_metal.c, gk_vulkan.c), an app's CVPixelBuffer
# (apple.c), and the filters' part (filter_*.c: the plugin's devices, the jobs' frames, the
# backends). Which of them: the library's HDRG_WITH_WEBGPU, HDRG_WITH_METAL, HDRG_WITH_VULKAN and
# HDRG_WITH_CVPIXELBUFFER, public definitions of the target hdrgrading::gpu. Headers:
# include/hdrgrading/gpu/. In the library's build its objects go into the library; a project of
# its own (a plugin built apart) finds the installed package's hdrgrading::gpu instead.
cmake_minimum_required(VERSION 3.29)
project(hdrgrading-gpu LANGUAGES C)

if(NOT TARGET hdrgrading::headers)
    find_package(hdrgrading CONFIG REQUIRED)
    return()
endif()

set(hdrg_gpu_sources "")
set(hdrg_gpu_definitions "")
if(HDRG_WITH_WEBGPU)
    list(APPEND hdrg_gpu_sources webgpu.c filter_webgpu.c)
    list(APPEND hdrg_gpu_definitions HDRG_WITH_WEBGPU)
endif()
if(HDRG_WITH_METAL)
    enable_language(OBJC)
    list(APPEND hdrg_gpu_sources metal.m gk_metal.c filter_metal.c)
    list(APPEND hdrg_gpu_definitions HDRG_WITH_METAL)
    set_source_files_properties(metal.m PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
endif()
if(HDRG_WITH_VULKAN)
    list(APPEND hdrg_gpu_sources vulkan.c gk_vulkan.c filter_vulkan.c)
    list(APPEND hdrg_gpu_definitions HDRG_WITH_VULKAN)
endif()
if(HDRG_WITH_METAL OR HDRG_WITH_VULKAN)
    list(APPEND hdrg_gpu_sources filter_gk.c)
endif()
if(HDRG_WITH_CVPIXELBUFFER)
    list(APPEND hdrg_gpu_sources apple.c)
    list(APPEND hdrg_gpu_definitions HDRG_WITH_CVPIXELBUFFER)
endif()

# no GPU: the headers alone (the library's GPU parts compile out)
if(hdrg_gpu_sources STREQUAL "")
    add_library(hdrgrading_gpu INTERFACE)
    add_library(hdrgrading::gpu ALIAS hdrgrading_gpu)
    target_include_directories(hdrgrading_gpu INTERFACE
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
    target_link_libraries(hdrgrading_gpu INTERFACE hdrgrading::headers hdrgrading::foundation)
    return()
endif()

list(APPEND hdrg_gpu_sources filter_plugin.c)
add_library(hdrgrading_gpu OBJECT ${hdrg_gpu_sources})
add_library(hdrgrading::gpu ALIAS hdrgrading_gpu)
# position-independent: the installed static library links into the plugins' modules too
set_target_properties(hdrgrading_gpu PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(hdrgrading_gpu PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_compile_definitions(hdrgrading_gpu PUBLIC ${hdrg_gpu_definitions})
target_link_libraries(hdrgrading_gpu PUBLIC hdrgrading::headers hdrgrading::foundation)
if(HDRG_WITH_WEBGPU AND NOT EMSCRIPTEN)
    target_link_libraries(hdrgrading_gpu PUBLIC dawn::webgpu_dawn)
endif()
if(HDRG_WITH_VULKAN)
    target_link_libraries(hdrgrading_gpu PUBLIC Vulkan::Headers)
endif()
if(COMMAND hdrg_target_warnings)
    hdrg_target_warnings(hdrgrading_gpu)
endif()
