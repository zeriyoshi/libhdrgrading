# SPDX-License-Identifier: Apache-2.0 OR MIT
# A project of the installed library through its CMake package (tests/install/run.cmake). C alone:
# a static library's C++ runtime and system libraries come with the package.
cmake_minimum_required(VERSION 3.29)
project(hdrgrading_consumer LANGUAGES C)

find_package(hdrgrading 0.0 REQUIRED CONFIG)

add_executable(consumer consumer.c)
target_link_libraries(consumer PRIVATE hdrgrading::hdrgrading)
