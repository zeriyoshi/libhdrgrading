# SPDX-License-Identifier: Apache-2.0 OR MIT
# hdrg.scale.nvidia: NVIDIA Image Scaling (MIT), on the CPU and the GPU. Its filter tables are
# taken from NIS_Config.h (HDRG_NIS_DIR: third_party/nis) at configure
# A plugin project (cmake/HdrgPlugin.cmake): built into the library from src/plugins, or apart
# against the installed package (a loadable module)
cmake_minimum_required(VERSION 3.29)
project(hdrgrading-nis LANGUAGES C)

if(NOT COMMAND hdrg_plugin)
    find_package(hdrgrading CONFIG REQUIRED)
endif()

set(HDRG_NIS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party/nis" CACHE PATH
    "NVIDIA Image Scaling's sources (NIS/NIS_Config.h)")
set(hdrg_nis_config "${HDRG_NIS_DIR}/NIS/NIS_Config.h")
if(NOT EXISTS "${hdrg_nis_config}")
    message(FATAL_ERROR "${hdrg_nis_config} is missing (scripts/fetch_deps.sh, HDRG_NIS_DIR)")
endif()
file(READ "${hdrg_nis_config}" hdrg_nis_text)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${hdrg_nis_config}")
set(hdrg_nis_c "/* generated from NVIDIA Image Scaling's NIS/NIS_Config.h (MIT) */\n")
foreach(table IN ITEMS scale usm)
    if(NOT hdrg_nis_text MATCHES
       "constexpr float coef_${table}\\[kPhaseCount\\]\\[kFilterSize\\] = {([^;]*)};")
        message(FATAL_ERROR "${hdrg_nis_config}: no coef_${table} table")
    endif()
    string(APPEND hdrg_nis_c "extern const float hdrg_nis_coef_${table}[64][8];\n"
        "const float hdrg_nis_coef_${table}[64][8] = {${CMAKE_MATCH_1}};\n")
endforeach()
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/nis_coefs.c" CONTENT "${hdrg_nis_c}" @ONLY)

hdrg_plugin(nis ENTRY hdrg_filter_nis_init
    SOURCES nis.c "${CMAKE_CURRENT_BINARY_DIR}/nis_coefs.c"
    WEBGPU webgpu_nis.c
    GK gk_nis.c
    SHADERS nis
    LIBRARIES hdrgrading::foundation)
