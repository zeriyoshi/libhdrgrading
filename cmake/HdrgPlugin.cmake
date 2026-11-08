# SPDX-License-Identifier: Apache-2.0 OR MIT
# The plugins' projects (src/plugins/<name>, one CMake project each): a plugin of the filter or
# codec ABI (include/hdrgrading/filter.h, codec.h) with its CPU, WebGPU and native GPU (Metal,
# Vulkan) parts and their shaders. The library's build links the built-in ones in from the start
# (HDRG_PLUGINS_BUILTIN: their objects in the library, their entry points registered by every
# context in the order added); any other (MODULE, or a project built apart against the installed
# package) is a loadable module of its own, hdrgrading-<name>, entered by HDRG_PLUGIN_DEFINE.
include_guard(GLOBAL)

set(HDRG_PLUGIN_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}")

# 32-bit x86 with GCC or Clang: float and double in SSE2, not the x87's extended precision
include(CheckCSourceCompiles)
check_c_source_compiles("
#if !defined(__i386__) || defined(__SSE2_MATH__)
#error no x87 math
#endif
int main(void) { return 0; }" HDRG_TARGET_X87_MATH)

# hdrg_target_determinism(<target>): the policy every part of the library and every plugin compiles
# under, built in or apart (the determinism the library promises): C11, and the results of the
# floating-point operations the IEEE operations' alone (no contraction into fused multiply-adds,
# no x87 extended precision)
function(hdrg_target_determinism target)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF)
    if(CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        # clang-cl: its default model is precise, and /fp:precise before -ffp-contract=off is an
        # overridden option (-Woverriding-option)
        if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
            target_compile_options(${target} PRIVATE /clang:-ffp-contract=off)
        else()
            target_compile_options(${target} PRIVATE /fp:precise)
        endif()
    else()
        target_compile_options(${target} PRIVATE -ffp-contract=off)
        if(HDRG_TARGET_X87_MATH)
            target_compile_options(${target} PRIVATE -msse2 -mfpmath=sse)
        endif()
    endif()
endfunction()

# a source file of a target: content (written only when it changed, not to rebuild)
function(hdrg_plugin_generated target file content)
    file(CONFIGURE OUTPUT "${file}" CONTENT "${content}" @ONLY)
    target_sources(${target} PRIVATE "${file}")
endfunction()

# a file's bytes as C initializers (0x2a,...)
function(hdrg_plugin_bytes file out)
    file(READ "${file}" hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${file}")
    set(${out} "${bytes}" PARENT_SCOPE)
endfunction()

# a text file's bytes for a char array: ASCII only (a byte from 0x80 overflows char)
function(hdrg_plugin_text file out)
    hdrg_plugin_bytes("${file}" bytes)
    if(bytes MATCHES "0x[89a-f]")
        message(FATAL_ERROR "${file}: not ASCII (a shader is embedded as a char array)")
    endif()
    set(${out} "${bytes}" PARENT_SCOPE)
endfunction()

# A plugin's shaders: of each wgsl/<name>.wgsl, with WebGPU the WGSL (hdrg_wgsl_<name>, a
# NUL-terminated char array), with Metal or Vulkan its entry points' Tint's MSL and SPIR-V
# (msl/<name>_<entry>.metal, spirv/<name>_<entry>.spv: scripts/gen_shaders.sh) as the GPU layer's
# shaders hdrg_gk_<name>_<entry> (hdrgrading/gpu/gk.h)
function(hdrg_plugin_shaders target)
    file(RELATIVE_PATH where "${CMAKE_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
    foreach(name IN LISTS ARGN)
        if(HDRG_WITH_WEBGPU)
            hdrg_plugin_text("${CMAKE_CURRENT_SOURCE_DIR}/wgsl/${name}.wgsl" bytes)
            string(CONCAT text "/* generated from ${where}/wgsl/${name}.wgsl */\n"
                "extern const char hdrg_wgsl_${name}[];\n"
                "const char hdrg_wgsl_${name}[] = {${bytes}0x00};\n")
            hdrg_plugin_generated(${target} "${CMAKE_CURRENT_BINARY_DIR}/wgsl/${name}.c" "${text}")
        endif()
        if(NOT (HDRG_WITH_METAL OR HDRG_WITH_VULKAN))
            continue()
        endif()

        # the entry points: the generated files of the kind the build reads
        if(HDRG_WITH_METAL)
            file(GLOB files CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/msl/${name}_*.metal")
            set(ext ".metal")
        else()
            file(GLOB files CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/spirv/${name}_*.spv")
            set(ext ".spv")
        endif()
        if(files STREQUAL "")
            message(FATAL_ERROR "${where}/wgsl/${name}.wgsl: no generated shaders (scripts/gen_shaders.sh)")
        endif()
        list(SORT files)

        string(CONCAT code "/* generated from ${where}/wgsl/${name}.wgsl's MSL and SPIR-V */\n"
            "#include <stddef.h>\n\n#include \"hdrgrading/gpu/gk.h\"\n\n")
        foreach(file IN LISTS files)
            get_filename_component(stem "${file}" NAME)
            string(REGEX REPLACE "^${name}_(.*)${ext}$" "\\1" entry "${stem}")
            set(msl NULL)
            set(spv NULL)
            set(spv_size NULL)
            if(HDRG_WITH_METAL)
                hdrg_plugin_text("${CMAKE_CURRENT_SOURCE_DIR}/msl/${name}_${entry}.metal" bytes)
                string(APPEND code "static const char msl_${entry}[] = {${bytes}0x00};\n")
                set(msl "msl_${entry}")
            endif()
            if(HDRG_WITH_VULKAN)
                hdrg_plugin_bytes("${CMAKE_CURRENT_SOURCE_DIR}/spirv/${name}_${entry}.spv" bytes)
                string(APPEND code
                    "_Alignas(4) static const unsigned char spv_${entry}[] = {${bytes}};\n"
                    "static const size_t spv_${entry}_size = sizeof(spv_${entry});\n")
                set(spv "spv_${entry}")
                set(spv_size "&spv_${entry}_size")
            endif()
            string(APPEND code "extern const hdrg_gk_shader_t hdrg_gk_${name}_${entry};\n"
                "const hdrg_gk_shader_t hdrg_gk_${name}_${entry} = "
                "{\"${entry}\", ${msl}, ${spv}, ${spv_size}};\n\n")
        endforeach()
        hdrg_plugin_generated(${target} "${CMAKE_CURRENT_BINARY_DIR}/gk/${name}.c" "${code}")
    endforeach()
endfunction()

# hdrg_plugin(<name> ENTRY <init> [MODULE] [SOURCES <c>...] [WEBGPU <c>...] [GK <c>...]
#             [METAL <c>...] [SHADERS <wgsl name>...] [DEFINITIONS <d>...] [LIBRARIES <t>...])
#   ENTRY        the plugin's hdrg_plugin_init_fn_t (declared HDRG_PLUGIN_CALL in its sources)
#   MODULE       always a loadable module (its dependencies of its own), never built in
#   SOURCES      its CPU part; WEBGPU / GK / METAL: its parts on WebGPU, on Metal and Vulkan
#                (hdrgrading/gpu/filter_gk.h) and on Metal alone, built where the build has them
#                (linking hdrgrading::gpu)
#   SHADERS      its WGSL modules (hdrg_plugin_shaders), with a GPU part
#   LIBRARIES    what it links besides hdrgrading::headers (hdrgrading::foundation and so on)
# The target: hdrg_plugin_<name> built in, else hdrgrading-<name> (in <build>/modules)
function(hdrg_plugin name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "MODULE" "ENTRY"
        "SOURCES;WEBGPU;GK;METAL;SHADERS;DEFINITIONS;LIBRARIES")
    if(NOT arg_ENTRY)
        message(FATAL_ERROR "hdrg_plugin(${name}): no ENTRY")
    endif()

    set(sources ${arg_SOURCES})
    set(gpu OFF)
    if(HDRG_WITH_WEBGPU AND arg_WEBGPU)
        list(APPEND sources ${arg_WEBGPU})
        set(gpu ON)
    endif()
    if((HDRG_WITH_METAL OR HDRG_WITH_VULKAN) AND arg_GK)
        list(APPEND sources ${arg_GK})
        set(gpu ON)
    endif()
    if(HDRG_WITH_METAL AND arg_METAL)
        list(APPEND sources ${arg_METAL})
        set(gpu ON)
    endif()

    if(HDRG_PLUGINS_BUILTIN AND NOT arg_MODULE)
        set(target hdrg_plugin_${name})
        add_library(${target} OBJECT ${sources})
        set_property(GLOBAL APPEND PROPERTY HDRG_BUILTIN_PLUGINS "${arg_ENTRY}")
        set_property(GLOBAL APPEND PROPERTY HDRG_BUILTIN_TARGETS ${target})
    else()
        set(target hdrgrading-${name})
        add_library(${target} MODULE ${sources})
        string(CONCAT text "/* generated: the module's entry points (hdrg_plugin_abi_version, hdrg_plugin_init) */\n"
            "#include \"hdrgrading/filter.h\"\n\n"
            "extern hdrg_status_t HDRG_PLUGIN_CALL ${arg_ENTRY}(hdrg_plugin_t *plugin,\n"
            "    const hdrg_plugin_api_t *api);\n\n"
            "HDRG_PLUGIN_DEFINE(${arg_ENTRY})\n")
        hdrg_plugin_generated(${target} "${CMAKE_CURRENT_BINARY_DIR}/entry.c" "${text}")
        set_target_properties(${target} PROPERTIES PREFIX "" C_VISIBILITY_PRESET hidden
            POSITION_INDEPENDENT_CODE ON)
        if(DEFINED HDRG_PLUGIN_OUTPUT_DIRECTORY)
            set_target_properties(${target} PROPERTIES
                LIBRARY_OUTPUT_DIRECTORY "${HDRG_PLUGIN_OUTPUT_DIRECTORY}")
        endif()
        set_property(GLOBAL APPEND PROPERTY HDRG_MODULE_TARGETS ${target})
    endif()

    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${target} PRIVATE hdrgrading::headers ${arg_LIBRARIES})
    if(gpu)
        target_link_libraries(${target} PRIVATE hdrgrading::gpu)
        hdrg_plugin_shaders(${target} ${arg_SHADERS})
    endif()
    if(arg_DEFINITIONS)
        target_compile_definitions(${target} PRIVATE ${arg_DEFINITIONS})
    endif()
    # the project's warnings where it is built in (with the determinism policy), else the policy
    if(COMMAND hdrg_target_warnings)
        hdrg_target_warnings(${target})
    else()
        hdrg_target_determinism(${target})
    endif()
    set(HDRG_PLUGIN_TARGET ${target} PARENT_SCOPE)
endfunction()
