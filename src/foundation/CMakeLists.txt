# SPDX-License-Identifier: Apache-2.0 OR MIT
# hdrgrading-foundation: the parts the library and its plugins build on, deterministic on every
# build: libm-free math (math.h: correctly rounded references; fmath.h: the per-pixel float
# polynomials), the counter-based hash (hash.h), the pinned floating-point environment (fpenv.h),
# threads, mutexes and condition variables (thread.h), the persistent thread pool (pool.h), the
# monotonic clock (clock.h) and the build's default allocation (memory.h, HDRG_MEMORY_HEADER).
# Headers: include/hdrgrading/foundation/. In the library's build its objects go into the library;
# a project of its own (a plugin built apart) finds the installed package's
# hdrgrading::foundation instead.
cmake_minimum_required(VERSION 3.29)
project(hdrgrading-foundation LANGUAGES C)

if(NOT TARGET hdrgrading::headers)
    find_package(hdrgrading CONFIG REQUIRED)
    return()
endif()

find_package(Threads REQUIRED)
add_library(hdrgrading_foundation OBJECT clock.c fpenv.c math.c pool.c thread.c)
add_library(hdrgrading::foundation ALIAS hdrgrading_foundation)
# position-independent: the installed static library links into the plugins' modules too
set_target_properties(hdrgrading_foundation PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(hdrgrading_foundation PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_link_libraries(hdrgrading_foundation PUBLIC hdrgrading::headers Threads::Threads)
if(NOT HDRG_MEMORY_HEADER STREQUAL "")
    target_compile_definitions(hdrgrading_foundation PUBLIC
        HDRG_MEMORY_HEADER="${HDRG_MEMORY_HEADER}")
endif()
if(COMMAND hdrg_target_warnings)
    hdrg_target_warnings(hdrgrading_foundation)
endif()
