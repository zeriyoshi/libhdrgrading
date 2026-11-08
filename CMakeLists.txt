# SPDX-License-Identifier: Apache-2.0 OR MIT
cmake_minimum_required(VERSION 3.29)
project(hdrgrading VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(HdrgCommon)
include(HdrgPlugin)
include(GNUInstallDirs)

# The options: HDRG_WITH_<name> a dependency, a backend or a feature built in, HDRG_BUILD_<name>
# something made beside the library, HDRG_PLUGINS_EXCLUDE the built-in plugins left out
option(HDRG_BUILD_TOOLS
       "Build the tools (tools/: hdrg-cli, hdrg-video, hdrg-inspect; hdrg-ffmpeg with HDRG_WITH_FFMPEG)"
       ${PROJECT_IS_TOP_LEVEL})
option(HDRG_BUILD_TESTS
       "Build the tests and the developers' tools (tests/tools: hdrg-bench, hdrg-predictor-data)"
       ${PROJECT_IS_TOP_LEVEL})
option(HDRG_WITH_LCMS2
       "lcms2: arbitrary ICC input, the P3 / PQ profiles (the JPEG XL writer's color management)" ON)
option(HDRG_WITH_AVIF
       "The AVIF codec module (src/plugins/avif: the system's libavif, by pkg-config), not linked into the library; off by default for third-party rights, on at your own risk"
       OFF)
option(HDRG_WITH_HEIF
       "The HEIF writer module (src/plugins/heif: libheif and its HEVC encoder, the system's else fetched; x265 is GPL), not linked into the library; off by default for third-party rights, on at your own risk"
       OFF)
option(HDRG_WITH_SYSTEM_DEPS
       "The system's zlib, libpng, giflib and lcms2 instead of third_party's (jpegli, Highway, libjxl and brotli are always bundled)"
       OFF)
option(BUILD_SHARED_LIBS "Build libhdrgrading as a shared library (the HDRG_API functions alone)" OFF)
set(HDRG_MEMORY_HEADER "" CACHE FILEPATH
    "A header replacing the default allocation (HDRG_MALLOC and the others, src/foundation's memory.h)")
option(HDRG_INSTALL "Install the library, its headers, CMake package, pkg-config file and tools"
       ${PROJECT_IS_TOP_LEVEL})

hdrg_apply_sanitizers()
# before the dependencies: -msimd128 / -pthread apply to every target of a WASM build
option(HDRG_WITH_SIMD "The SIMD kernels" ON)
# Video's temporal features, each a class of a built-in plugin (the library reaches them through
# the stages alone): the statistics across a session's frames (hdrg.stats.temporal: WINDOWED
# windows, RECURSIVE, TWO_PASS), the motion-compensated interpolation (hdrg.interp.mc) and the
# multi-frame upscale (hdrg.scale.temporal). HDRG_WITH_TEMPORAL turns them on together; each
# HDRG_WITH_TEMPORAL_<feature> (AUTO: as HDRG_WITH_TEMPORAL) overrides one. HDRG_TEMPORAL_<feature>:
# whether the build has it
option(HDRG_WITH_TEMPORAL "Video: the temporal features (the statistics across frames, the motion-compensated interpolation, the multi-frame upscale); off by default for third-party rights, on at your own risk" OFF)
foreach(feature IN ITEMS FILTER INTERP SR)
    set(HDRG_WITH_TEMPORAL_${feature} AUTO CACHE STRING
        "A temporal feature (FILTER: hdrg.stats.temporal, INTERP: hdrg.interp.mc, SR: hdrg.scale.temporal): AUTO (as HDRG_WITH_TEMPORAL), ON or OFF")
    set_property(CACHE HDRG_WITH_TEMPORAL_${feature} PROPERTY STRINGS AUTO ON OFF)
    mark_as_advanced(HDRG_WITH_TEMPORAL_${feature})
    if(HDRG_WITH_TEMPORAL_${feature} STREQUAL "AUTO")
        set(HDRG_TEMPORAL_${feature} ${HDRG_WITH_TEMPORAL})
    elseif(HDRG_WITH_TEMPORAL_${feature})
        set(HDRG_TEMPORAL_${feature} ON)
    else()
        set(HDRG_TEMPORAL_${feature} OFF)
    endif()
endforeach()
# the built-in plugins (src/plugins): optional families, in groups. polyphase (the SCALE stage's
# Lanczos) always stays
set(HDRG_PLUGIN_FILTERS bilinear denoise deblock dering defringe deband mctf interp fsr nis edge
    predict)
set(HDRG_PLUGIN_CODECS jpeg png gif jxl)
set(HDRG_PLUGINS_EXCLUDE "" CACHE STRING
    "The built-in plugins left out of the library (all built in by default): any of ${HDRG_PLUGIN_FILTERS} ${HDRG_PLUGIN_CODECS}, or the groups filters, codecs and all")
set(HDRG_PLUGINS_EXCLUDED "")
foreach(name IN LISTS HDRG_PLUGINS_EXCLUDE)
    if(name STREQUAL "all")
        list(APPEND HDRG_PLUGINS_EXCLUDED ${HDRG_PLUGIN_FILTERS} ${HDRG_PLUGIN_CODECS})
    elseif(name STREQUAL "filters")
        list(APPEND HDRG_PLUGINS_EXCLUDED ${HDRG_PLUGIN_FILTERS})
    elseif(name STREQUAL "codecs")
        list(APPEND HDRG_PLUGINS_EXCLUDED ${HDRG_PLUGIN_CODECS})
    elseif(name IN_LIST HDRG_PLUGIN_FILTERS OR name IN_LIST HDRG_PLUGIN_CODECS)
        list(APPEND HDRG_PLUGINS_EXCLUDED ${name})
    else()
        message(FATAL_ERROR "HDRG_PLUGINS_EXCLUDE: ${name} is not an optional built-in plugin "
            "(${HDRG_PLUGIN_FILTERS} ${HDRG_PLUGIN_CODECS}) nor a group (filters, codecs, all)")
    endif()
endforeach()
# what a family needs: the JPEG XL writer lcms2, a temporal feature its family
if(NOT HDRG_WITH_LCMS2 AND NOT "jxl" IN_LIST HDRG_PLUGINS_EXCLUDED)
    message(STATUS "jxl: left out without lcms2 (HDRG_WITH_LCMS2)")
    list(APPEND HDRG_PLUGINS_EXCLUDED jxl)
endif()
list(REMOVE_DUPLICATES HDRG_PLUGINS_EXCLUDED)
foreach(pair IN ITEMS INTERP:interp SR:edge)
    string(REPLACE ":" ";" pair "${pair}")
    list(GET pair 0 feature)
    list(GET pair 1 family)
    if(HDRG_TEMPORAL_${feature} AND family IN_LIST HDRG_PLUGINS_EXCLUDED)
        message(STATUS "HDRG_TEMPORAL_${feature}: off, ${family} is left out")
        set(HDRG_TEMPORAL_${feature} OFF)
    endif()
endforeach()
# off by default as their use could not be called safe with regard to third-party rights: a build
# turning them on is its builder's own (README, the docs' build page)
set(hdrg_own_risk "")
if(HDRG_TEMPORAL_FILTER OR HDRG_TEMPORAL_INTERP OR HDRG_TEMPORAL_SR)
    list(APPEND hdrg_own_risk "the temporal features (HDRG_WITH_TEMPORAL)")
endif()
foreach(name IN ITEMS HDRG_WITH_AVIF HDRG_WITH_HEIF)
    if(${name})
        list(APPEND hdrg_own_risk ${name})
    endif()
endforeach()
if(hdrg_own_risk)
    list(JOIN hdrg_own_risk ", " hdrg_own_risk)
    message(NOTICE "${hdrg_own_risk}: off by default, since we cannot say for certain that using "
        "them is safe with regard to third-party rights. This build turns them on at its builder's "
        "own risk.")
endif()
# hdrg_builtins_have(<var> <name>...): TRUE where the build has every one of these built-in plugins
function(hdrg_builtins_have var)
    set(have TRUE)
    foreach(name IN LISTS ARGN)
        if(name IN_LIST HDRG_PLUGINS_EXCLUDED)
            set(have FALSE)
        endif()
    endforeach()
    set(${var} ${have} PARENT_SCOPE)
endfunction()
# HDRG_BUILTIN_<NAME> 1 or 0 for each optional family and each temporal class, to the library's
# objects, the tests and the tools that reach the internals
set(HDRG_BUILTIN_DEFINITIONS "")
foreach(name IN LISTS HDRG_PLUGIN_FILTERS HDRG_PLUGIN_CODECS)
    string(TOUPPER "${name}" upper)
    hdrg_builtins_have(have ${name})
    list(APPEND HDRG_BUILTIN_DEFINITIONS HDRG_BUILTIN_${upper}=$<BOOL:${have}>)
endforeach()
list(APPEND HDRG_BUILTIN_DEFINITIONS HDRG_BUILTIN_STATS=$<BOOL:${HDRG_TEMPORAL_FILTER}>
    HDRG_BUILTIN_INTERP_MC=$<BOOL:${HDRG_TEMPORAL_INTERP}>
    HDRG_BUILTIN_SCALE_TEMPORAL=$<BOOL:${HDRG_TEMPORAL_SR}>)
include(HdrgWasm)

# the VapourSynth plugin (src/bindings/vapoursynth, its header from third_party/vapoursynth) is a
# module the library links into, so everything is position-independent then
if(EMSCRIPTEN OR NOT HDRG_SANITIZER STREQUAL "" OR HDRG_COVERAGE OR HDRG_BUILD_FUZZERS OR
   NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vapoursynth/include/VapourSynth4.h")
    set(HDRG_VAPOURSYNTH_DEFAULT OFF)
else()
    set(HDRG_VAPOURSYNTH_DEFAULT ${PROJECT_IS_TOP_LEVEL})
endif()
option(HDRG_BUILD_VAPOURSYNTH "Build the VapourSynth plugin (src/bindings/vapoursynth)"
       ${HDRG_VAPOURSYNTH_DEFAULT})

# the GStreamer plugin (src/bindings/gstreamer), the same kind of module, where GStreamer 1.20+ is found
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND AND NOT EMSCRIPTEN)
    pkg_check_modules(HDRG_GST QUIET IMPORTED_TARGET
        gstreamer-1.0>=1.20 gstreamer-base-1.0>=1.20 gstreamer-video-1.0>=1.20)
endif()
if(HDRG_GST_FOUND AND HDRG_SANITIZER STREQUAL "" AND NOT HDRG_COVERAGE AND NOT HDRG_BUILD_FUZZERS)
    set(HDRG_GSTREAMER_DEFAULT ${PROJECT_IS_TOP_LEVEL})
else()
    set(HDRG_GSTREAMER_DEFAULT OFF)
endif()
option(HDRG_BUILD_GSTREAMER "Build the GStreamer plugin (src/bindings/gstreamer)"
       ${HDRG_GSTREAMER_DEFAULT})

if(HDRG_BUILD_VAPOURSYNTH OR HDRG_BUILD_GSTREAMER OR BUILD_SHARED_LIBS)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()
if(BUILD_SHARED_LIBS AND EMSCRIPTEN)
    message(FATAL_ERROR "BUILD_SHARED_LIBS: Emscripten builds the static library (the module links it)")
endif()
# an installed program, module or shared library keeps the rpath to the libraries outside the
# project it links (Dawn, FFmpeg of scripts/build_ffmpeg.sh)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)
# hidden by default everywhere (the library, the bundled dependencies): what a shared library
# exports is the HDRG_API list
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_OBJC_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

configure_file(include/hdrgrading/version.h.in
    "${CMAKE_CURRENT_BINARY_DIR}/include/hdrgrading/version.h" @ONLY)

# the dependencies of the families built in (lcms2 the core's ICC, the rest the codecs'), and
# the HEIF module's
include(HdrgDeps)
if(HDRG_WITH_HEIF)
    include(HdrgHeif)
endif()

# the public headers (include/hdrgrading, the build's version.h) as a target: the library's parts
# and its plugins' projects (src/foundation, src/gpu, src/plugins) build on it
add_library(hdrgrading_headers INTERFACE)
add_library(hdrgrading::headers ALIAS hdrgrading_headers)
target_include_directories(hdrgrading_headers INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
add_subdirectory(src/foundation)

set(HDRG_SOURCES
    src/context.c
    src/params.c
    src/pipeline/pipeline.c
    src/pipeline/chain.c
    src/pipeline/scale.c
    src/session/session.c
    src/session/flash.c
    src/session/rate.c
    src/session/resident.c
    src/session/latency.c
    src/session/export.c
    src/lut3d.c
    src/core/exec.c
    src/core/dynamic.c
    src/core/color.c
    src/core/image.c
    src/core/analyze.c
    src/core/predict.c
    src/core/motion.c
    src/core/noise.c
    src/core/grain.c
    src/core/plan.c
    src/core/render.c
    src/core/apply.c
    src/core/gpu_analyze.c
    src/core/gpu_apply.c
    src/core/gpu_convert.c
    src/core/gpu_core.c
    src/core/gpu_gather.c
    src/core/yuv.c
    src/core/recycle.c
    # the plugin host (ABI include/hdrgrading/filter.h, codec.h)
    src/host/validate.c
    src/host/defaults.c
    src/host/host.c
    src/host/job.c
    src/host/device.c
    src/host/public.c
    src/host/loader.c
    src/host/analyzer.c
    src/host/stats.c
    src/host/format.c
    src/host/stage.c
)
# WebGPU (Dawn, built by scripts/build_dawn.sh) for the GPU filters, where it is
# found: HDRG_DAWN_DIR, CMAKE_PREFIX_PATH or the script's default prefix. Not under the sanitizers
# (the uninstrumented Dawn and driver would be what they report, their allocations the leaks) nor
# the fuzzers; the valgrind preset turns it off
if(NOT EMSCRIPTEN)
    find_package(Dawn CONFIG QUIET
        HINTS "${HDRG_DAWN_DIR}" "$ENV{HDRG_DAWN_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/build/deps/dawn")
endif()
if((Dawn_FOUND OR HDRG_WASM_WEBGPU) AND HDRG_SANITIZER STREQUAL "" AND NOT HDRG_BUILD_FUZZERS)
    set(HDRG_WEBGPU_DEFAULT ON)
else()
    set(HDRG_WEBGPU_DEFAULT OFF)
endif()
option(HDRG_WITH_WEBGPU "Build the WebGPU filters (Dawn's webgpu.h)" ${HDRG_WEBGPU_DEFAULT})
# Dawn's library: the native builds (Emscripten's is the emdawnwebgpu port, cmake/HdrgWasm.cmake)
if(HDRG_WITH_WEBGPU AND NOT EMSCRIPTEN)
    set(HDRG_DAWN ON)
else()
    set(HDRG_DAWN OFF)
endif()
# the WGSL sources as NUL-terminated arrays (a C string literal's length is limited): the core's
# apply and conversion always (hdrg_shader gives them to the app), with WebGPU the rest of the
# core's (src/core/wgsl). The plugins embed theirs (cmake/HdrgPlugin.cmake)
set(HDRG_WGSL_SHADERS core/apply core/convert)
if(HDRG_WITH_WEBGPU)
    if(EMSCRIPTEN AND NOT HDRG_WASM_WEBGPU)
        message(FATAL_ERROR "HDRG_WITH_WEBGPU under Emscripten needs HDRG_WASM_WEBGPU")
    endif()
    if(HDRG_DAWN AND NOT Dawn_FOUND)
        message(FATAL_ERROR "HDRG_WITH_WEBGPU needs Dawn (scripts/build_dawn.sh, HDRG_DAWN_DIR)")
    endif()
    list(APPEND HDRG_WGSL_SHADERS core/gather core/io core/analyze)
    list(APPEND HDRG_SOURCES src/core/gpu_core_webgpu.c)
endif()
foreach(shader IN LISTS HDRG_WGSL_SHADERS)
    get_filename_component(HDRG_SHADER_NAME "${shader}" NAME)
    get_filename_component(HDRG_SHADER_DIR "${shader}" DIRECTORY)
    set(HDRG_WGSL "${CMAKE_CURRENT_SOURCE_DIR}/src/${HDRG_SHADER_DIR}/wgsl/${HDRG_SHADER_NAME}.wgsl")
    file(READ "${HDRG_WGSL}" HDRG_WGSL_HEX HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," HDRG_WGSL_BYTES "${HDRG_WGSL_HEX}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/wgsl/${HDRG_SHADER_NAME}.c"
        "/* generated from src/${HDRG_SHADER_DIR}/wgsl/${HDRG_SHADER_NAME}.wgsl */\n"
        "extern const char hdrg_wgsl_${HDRG_SHADER_NAME}[];\n"
        "const char hdrg_wgsl_${HDRG_SHADER_NAME}[] = {${HDRG_WGSL_BYTES}0x00};\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${HDRG_WGSL}")
    list(APPEND HDRG_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/wgsl/${HDRG_SHADER_NAME}.c")
endforeach()
# the apps' shaders (hdrg_shader): the core's apply and conversion as Tint's SPIR-V (word aligned,
# with its size) and HLSL (NUL-terminated), from scripts/gen_shaders.sh, always
foreach(shader IN ITEMS apply_apply apply_yuv convert_sites convert_slopes convert_pixels)
    set(HDRG_SPV "${CMAKE_CURRENT_SOURCE_DIR}/src/core/spirv/${shader}.spv")
    set(HDRG_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/src/core/hlsl/${shader}.hlsl")
    file(READ "${HDRG_SPV}" HDRG_SPV_HEX HEX)
    file(READ "${HDRG_HLSL}" HDRG_HLSL_HEX HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," HDRG_SPV_BYTES "${HDRG_SPV_HEX}")
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," HDRG_HLSL_BYTES "${HDRG_HLSL_HEX}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/shaders/${shader}.c"
        "/* generated from src/core/spirv/${shader}.spv and src/core/hlsl/${shader}.hlsl */\n"
        "#include <stddef.h>\n"
        "extern const unsigned char hdrg_spv_${shader}[], hdrg_hlsl_${shader}[];\n"
        "extern const size_t hdrg_spv_${shader}_size;\n"
        "_Alignas(4) const unsigned char hdrg_spv_${shader}[] = {${HDRG_SPV_BYTES}};\n"
        "const size_t hdrg_spv_${shader}_size = sizeof(hdrg_spv_${shader});\n"
        "const unsigned char hdrg_hlsl_${shader}[] = {${HDRG_HLSL_BYTES}0x00};\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${HDRG_SPV}" "${HDRG_HLSL}")
    list(APPEND HDRG_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/shaders/${shader}.c")
endforeach()
# Metal for the Metal filters: the system's frameworks, on Apple
# platforms. The MSL is Tint's of the WGSL (scripts/gen_shaders.sh, src/core/msl/), embedded as the
# WGSL is; src/gpu/metal.m (Objective-C with ARC) is the only file that sees Metal and MetalFX
if(APPLE)
    set(HDRG_METAL_DEFAULT ON)
else()
    set(HDRG_METAL_DEFAULT OFF)
endif()
option(HDRG_WITH_METAL "Build the Metal filters (Apple: Metal, MetalFX)" ${HDRG_METAL_DEFAULT})
if(HDRG_WITH_METAL)
    if(NOT APPLE)
        message(FATAL_ERROR "HDRG_WITH_METAL needs an Apple platform")
    endif()
    enable_language(OBJC)
    foreach(shader IN ITEMS core/apply_apply core/apply_yuv core/convert_sites core/convert_slopes
                            core/convert_pixels core/gather_proxy core/gather_tiles
                            core/io_copy_rows core/analyze_edges core/analyze_cells_of
                            core/analyze_hist core/analyze_moments_of)
        get_filename_component(HDRG_SHADER_NAME "${shader}" NAME)
        get_filename_component(HDRG_SHADER_DIR "${shader}" DIRECTORY)
        set(HDRG_MSL "${CMAKE_CURRENT_SOURCE_DIR}/src/${HDRG_SHADER_DIR}/msl/${HDRG_SHADER_NAME}.metal")
        file(READ "${HDRG_MSL}" HDRG_MSL_HEX HEX)
        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," HDRG_MSL_BYTES "${HDRG_MSL_HEX}")
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/msl/${HDRG_SHADER_NAME}.c"
            "/* generated from src/${HDRG_SHADER_DIR}/msl/${HDRG_SHADER_NAME}.metal */\n"
            "extern const char hdrg_msl_${HDRG_SHADER_NAME}[];\n"
            "const char hdrg_msl_${HDRG_SHADER_NAME}[] = {${HDRG_MSL_BYTES}0x00};\n")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${HDRG_MSL}")
        list(APPEND HDRG_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/msl/${HDRG_SHADER_NAME}.c")
    endforeach()
endif()
# Vulkan for the Vulkan filters and the core where its headers are found: the
# system's loader opened at run time (dlopen: not linked), the SPIR-V Tint's of the WGSL
# (scripts/gen_shaders.sh, src/<filters|core>/spirv/). Not on Apple (Metal there) nor under
# Emscripten, MemorySanitizer (the uninstrumented driver's memory) or the fuzzers; under ASan /
# UBSan and TSan it is on (lavapipe runs clean: the layer's completion thread checked); the
# valgrind preset turns it off
if(NOT APPLE AND NOT EMSCRIPTEN)
    find_package(Vulkan QUIET)
endif()
if(TARGET Vulkan::Headers AND NOT HDRG_SANITIZER STREQUAL "memory" AND NOT HDRG_BUILD_FUZZERS)
    set(HDRG_VULKAN_DEFAULT ON)
else()
    set(HDRG_VULKAN_DEFAULT OFF)
endif()
option(HDRG_WITH_VULKAN "Build the Vulkan filters and core (Vulkan's headers)"
    ${HDRG_VULKAN_DEFAULT})
if(HDRG_WITH_VULKAN)
    if(NOT TARGET Vulkan::Headers)
        message(FATAL_ERROR "HDRG_WITH_VULKAN needs Vulkan's headers (find_package(Vulkan))")
    endif()
    foreach(shader IN ITEMS core/gather_proxy core/gather_tiles core/io_copy_rows
                            core/analyze_edges core/analyze_cells_of core/analyze_hist
                            core/analyze_moments_of)
        get_filename_component(HDRG_SHADER_NAME "${shader}" NAME)
        get_filename_component(HDRG_SHADER_DIR "${shader}" DIRECTORY)
        set(HDRG_SPV "${CMAKE_CURRENT_SOURCE_DIR}/src/${HDRG_SHADER_DIR}/spirv/${HDRG_SHADER_NAME}.spv")
        file(READ "${HDRG_SPV}" HDRG_SPV_HEX HEX)
        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," HDRG_SPV_BYTES "${HDRG_SPV_HEX}")
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/spirv/${HDRG_SHADER_NAME}.c"
            "/* generated from src/${HDRG_SHADER_DIR}/spirv/${HDRG_SHADER_NAME}.spv */\n"
            "#include <stddef.h>\n"
            "extern const unsigned char hdrg_spv_${HDRG_SHADER_NAME}[];\n"
            "extern const size_t hdrg_spv_${HDRG_SHADER_NAME}_size;\n"
            "_Alignas(4) const unsigned char hdrg_spv_${HDRG_SHADER_NAME}[] = {${HDRG_SPV_BYTES}};\n"
            "const size_t hdrg_spv_${HDRG_SHADER_NAME}_size = sizeof(hdrg_spv_${HDRG_SHADER_NAME});\n")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${HDRG_SPV}")
        list(APPEND HDRG_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/spirv/${HDRG_SHADER_NAME}.c")
    endforeach()
endif()
# the core on a native API (hdrgrading/gpu/gk.h): Metal's and Vulkan's alike
if(HDRG_WITH_METAL OR HDRG_WITH_VULKAN)
    list(APPEND HDRG_SOURCES src/core/gpu_core_gk.c)
endif()

# an app's pixel buffers on the GPU (HDRG_FRAME_CVPIXELBUFFER: CoreVideo, IOSurface), on Apple with
# a GPU backend
if(APPLE AND (HDRG_WITH_METAL OR HDRG_WITH_WEBGPU))
    set(HDRG_WITH_CVPIXELBUFFER ON)
    set(HDRG_APPLE_FRAMEWORKS "-framework CoreFoundation" "-framework CoreVideo"
        "-framework IOSurface")
else()
    set(HDRG_WITH_CVPIXELBUFFER OFF)
    set(HDRG_APPLE_FRAMEWORKS)
endif()

# the GPU layers (src/gpu) and the built-in plugins (src/plugins): projects of their own, their
# objects in the library
add_subdirectory(src/gpu)
if((HDRG_WITH_AVIF OR HDRG_WITH_HEIF) AND EMSCRIPTEN)
    message(FATAL_ERROR "HDRG_WITH_AVIF / HDRG_WITH_HEIF: no loadable modules in a WASM build")
endif()
set(HDRG_PLUGIN_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/modules")
add_subdirectory(src/plugins)
get_property(HDRG_BUILTIN_PLUGINS GLOBAL PROPERTY HDRG_BUILTIN_PLUGINS)
get_property(HDRG_BUILTIN_TARGETS GLOBAL PROPERTY HDRG_BUILTIN_TARGETS)
# the registry the contexts register them by (src/host/host.h: hdrg_builtin_plugins)
string(CONCAT hdrg_registry "/* generated: the built-in plugins (src/plugins), in the order registered */\n"
    "#include \"hdrgrading/plugin.h\"\n\n")
set(hdrg_registry_list "")
foreach(entry IN LISTS HDRG_BUILTIN_PLUGINS)
    string(APPEND hdrg_registry "extern hdrg_status_t HDRG_PLUGIN_CALL ${entry}(hdrg_plugin_t *plugin, "
        "const hdrg_plugin_api_t *api);\n")
    string(APPEND hdrg_registry_list "    ${entry},\n")
endforeach()
list(LENGTH HDRG_BUILTIN_PLUGINS hdrg_registry_count)
get_property(hdrg_registry_codecs GLOBAL PROPERTY HDRG_BUILTIN_CODECS_FIRST)
string(APPEND hdrg_registry "\nextern const hdrg_plugin_init_fn_t hdrg_builtin_plugins[];\n"
    "extern const uint32_t hdrg_builtin_plugin_count, hdrg_builtin_codecs_first;\n"
    "const hdrg_plugin_init_fn_t hdrg_builtin_plugins[] = {\n${hdrg_registry_list}};\n"
    "const uint32_t hdrg_builtin_plugin_count = ${hdrg_registry_count}u;\n"
    "const uint32_t hdrg_builtin_codecs_first = ${hdrg_registry_codecs}u;\n")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/builtin_plugins.c" CONTENT "${hdrg_registry}"
    @ONLY)
list(APPEND HDRG_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/builtin_plugins.c")

# the still images: the codec host, the writers' container, the conversions
list(APPEND HDRG_SOURCES
    src/io/icc.c
    src/io/gainmeta.c
    src/host/codec_host.c
    src/container.c
    src/convert.c
)

# SIMD kernels: one translation unit per ISA with its own flags.
# The architecture is the compiler's target (cross builds and a single
# CMAKE_OSX_ARCHITECTURES work); universal binaries run the scalar kernels.
# WASM has SIMD128 when HDRG_WITH_SIMD is on (the whole module is built with
# -msimd128, cmake/HdrgWasm.cmake). AVX-512 and SVE are provisional: built
# where the compiler has them, run only when HDRG_SIMD_MAX names them.
# Fused multiply-adds in the SIMD kernels: faster, but no longer byte-identical to the scalar
# reference (L0x). The Debug presets turn it off (the tests check the exact tiers there)
option(HDRG_WITH_SIMD_FMA "Fuse multiply-adds in the SIMD kernels (within L0x of the scalar reference)" ON)
include(CheckCSourceCompiles)
check_c_source_compiles("
#if !defined(__x86_64__) && !defined(_M_X64)
#error not x86-64
#endif
int main(void) { return 0; }" HDRG_TARGET_X86_64)
check_c_source_compiles("
#if !defined(__aarch64__) && !defined(_M_ARM64)
#error not arm64
#endif
int main(void) { return 0; }" HDRG_TARGET_ARM64)

list(APPEND HDRG_SOURCES src/kernels/cpu.c src/kernels/k_scalar.c src/kernels/pq_table.c)
set(HDRG_KERNELS_DEFINITIONS "")
if(HDRG_WITH_SIMD AND HDRG_TARGET_X86_64)
    list(APPEND HDRG_SOURCES src/kernels/k_sse41.c src/kernels/k_avx2.c src/kernels/k_avx512.c)
    if(HDRG_COMPILER_IS_CLANG_CL)
        set(HDRG_FLAGS_SSE41 /clang:-msse4.1)
        set(HDRG_FLAGS_AVX2 /arch:AVX2)
        set(HDRG_FLAGS_AVX512 /arch:AVX512)
    elseif(HDRG_COMPILER_IS_MSVC_FRONTEND)
        set(HDRG_FLAGS_SSE41 "")
        set(HDRG_FLAGS_AVX2 /arch:AVX2)
        set(HDRG_FLAGS_AVX512 /arch:AVX512)
    else()
        set(HDRG_FLAGS_SSE41 -msse4.1)
        set(HDRG_FLAGS_AVX2 -mavx2 -mfma)
        set(HDRG_FLAGS_AVX512 -mavx512f -mavx512dq -mavx512bw -mavx512vl -mfma)
    endif()
    set_source_files_properties(src/kernels/k_sse41.c PROPERTIES COMPILE_OPTIONS "${HDRG_FLAGS_SSE41}")
    set_source_files_properties(src/kernels/k_avx2.c PROPERTIES COMPILE_OPTIONS "${HDRG_FLAGS_AVX2}")
    set_source_files_properties(src/kernels/k_avx512.c PROPERTIES COMPILE_OPTIONS "${HDRG_FLAGS_AVX512}")
    set(HDRG_KERNELS_DEFINITIONS HDRG_KERNELS_X86 HDRG_KERNELS_AVX512)
elseif(HDRG_WITH_SIMD AND EMSCRIPTEN)
    list(APPEND HDRG_SOURCES src/kernels/k_wasm128.c)
    set(HDRG_KERNELS_DEFINITIONS HDRG_KERNELS_WASM128)
elseif(HDRG_WITH_SIMD AND HDRG_TARGET_ARM64)
    list(APPEND HDRG_SOURCES src/kernels/k_neon.c)
    set(HDRG_KERNELS_DEFINITIONS HDRG_KERNELS_NEON)
    # SVE: Linux reports it in AT_HWCAP (macOS and Windows on Arm have none)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT HDRG_COMPILER_IS_MSVC_FRONTEND)
        set(CMAKE_REQUIRED_FLAGS "-march=armv8.2-a+sve")
        check_c_source_compiles("
#include <arm_sve.h>
int main(void) { return (int)svcntw(); }" HDRG_COMPILER_HAS_SVE)
        unset(CMAKE_REQUIRED_FLAGS)
        if(HDRG_COMPILER_HAS_SVE)
            list(APPEND HDRG_SOURCES src/kernels/k_sve.c)
            set_source_files_properties(src/kernels/k_sve.c PROPERTIES COMPILE_OPTIONS "-march=armv8.2-a+sve")
            list(APPEND HDRG_KERNELS_DEFINITIONS HDRG_KERNELS_SVE)
        endif()
    endif()
endif()

# The sources compile once (hdrgrading_objects). libhdrgrading (static or shared, BUILD_SHARED_LIBS)
# is those objects and the bundled dependencies' (HDRG_BUNDLED_OBJECTS); a shared one exports the
# HDRG_API functions of the public headers alone. The tests and the tools that reach the internals
# link HDRG_INTERNAL: libhdrgrading when static, else a static library of the same objects
add_library(hdrgrading_objects OBJECT ${HDRG_SOURCES})
hdrg_target_warnings(hdrgrading_objects)
hdrg_target_determinism(hdrgrading_objects)
target_link_libraries(hdrgrading_objects PRIVATE hdrgrading::foundation hdrgrading::gpu)
target_compile_definitions(hdrgrading_objects PRIVATE HDRG_BUILDING ${HDRG_KERNELS_DEFINITIONS}
    $<$<BOOL:${HDRG_WITH_SIMD_FMA}>:HDRG_SIMD_FMA>
    $<$<BOOL:${BUILD_SHARED_LIBS}>:HDRG_SHARED>)
target_include_directories(hdrgrading_objects PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_BINARY_DIR}/include")
if(NOT HDRG_MEMORY_HEADER STREQUAL "")
    target_compile_definitions(hdrgrading_objects PRIVATE HDRG_MEMORY_HEADER="${HDRG_MEMORY_HEADER}")
endif()
find_package(Threads REQUIRED)
target_link_libraries(hdrgrading_objects PRIVATE Threads::Threads)
target_compile_definitions(hdrgrading_objects PRIVATE ${HDRG_BUILTIN_DEFINITIONS})
if(HDRG_WITH_LCMS2)
    target_compile_definitions(hdrgrading_objects PRIVATE HDRG_WITH_LCMS2)
    target_link_libraries(hdrgrading_objects PRIVATE hdrg::lcms2)
endif()

# the HDRG_API functions of the public headers: a shared library's export list (a version script,
# Apple's exported symbols), every declaration on one line (checked) for the list to be complete
set(HDRG_PUBLIC_HEADERS include/hdrgrading/hdrgrading.h include/hdrgrading/video.h
    include/hdrgrading/gpu.h include/hdrgrading/metadata.h include/hdrgrading/sidedata.h
    include/hdrgrading/plugin.h include/hdrgrading/filter.h include/hdrgrading/codec.h)
set(HDRG_EXPORTS "")
foreach(header IN LISTS HDRG_PUBLIC_HEADERS)
    file(STRINGS "${header}" hdrg_api_lines REGEX "HDRG_API")
    foreach(line IN LISTS hdrg_api_lines)
        if(line MATCHES "^#|^ *\\*|^/\\*")
            continue()
        endif()
        if(NOT line MATCHES "^HDRG_API [^(]*[ *]([A-Za-z_][A-Za-z0-9_]*)\\(")
            message(FATAL_ERROR "${header}: an HDRG_API declaration not on one line: ${line}")
        endif()
        list(APPEND HDRG_EXPORTS "${CMAKE_MATCH_1}")
    endforeach()
endforeach()
list(JOIN HDRG_EXPORTS "\n" hdrg_exports_text)
list(TRANSFORM HDRG_EXPORTS PREPEND "_" OUTPUT_VARIABLE hdrg_apple_exports)
list(JOIN hdrg_apple_exports "\n" hdrg_apple_exports_text)
list(JOIN HDRG_EXPORTS ";\n    " hdrg_version_script_text)
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/hdrgrading.exports"
    CONTENT "${hdrg_exports_text}\n" @ONLY)
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/hdrgrading.exp"
    CONTENT "${hdrg_apple_exports_text}\n" @ONLY)
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/hdrgrading.map"
    CONTENT "{\n  global:\n    ${hdrg_version_script_text};\n  local:\n    *;\n};\n" @ONLY)

set(HDRG_LIBRARY_OBJECTS $<TARGET_OBJECTS:hdrgrading_objects> $<TARGET_OBJECTS:hdrgrading_foundation>
    ${HDRG_BUNDLED_OBJECTS})
get_target_property(hdrg_gpu_type hdrgrading_gpu TYPE)
if(hdrg_gpu_type STREQUAL "OBJECT_LIBRARY")
    list(APPEND HDRG_LIBRARY_OBJECTS $<TARGET_OBJECTS:hdrgrading_gpu>)
endif()
foreach(target IN LISTS HDRG_BUILTIN_TARGETS)
    list(APPEND HDRG_LIBRARY_OBJECTS $<TARGET_OBJECTS:${target}>)
endforeach()
# the bundled jpegli, Highway and libjxl are C++: a static library names the C++ runtime, for a
# program of C alone to link it (the C++ compiler's own libraries beyond the C compiler's)
set(HDRG_CXX_RUNTIME "")
if(HDRG_BUNDLED_CXX)
    set(HDRG_LINKER_LANGUAGE CXX)
    set(hdrg_runtime ${CMAKE_CXX_IMPLICIT_LINK_LIBRARIES})
    if(CMAKE_C_IMPLICIT_LINK_LIBRARIES)
        list(REMOVE_ITEM hdrg_runtime ${CMAKE_C_IMPLICIT_LINK_LIBRARIES})
    endif()
    foreach(library IN LISTS hdrg_runtime)
        if(library MATCHES "^-|/")
            list(APPEND HDRG_CXX_RUNTIME "${library}")
        else()
            list(APPEND HDRG_CXX_RUNTIME "-l${library}")
        endif()
    endforeach()
else()
    set(HDRG_LINKER_LANGUAGE C)
endif()
# what libhdrgrading (and HDRG_INTERNAL) links besides the objects
function(hdrg_library_setup target)
    target_include_directories(${target} PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/foundation/include>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
    target_link_libraries(${target} PRIVATE Threads::Threads ${CMAKE_DL_LIBS} ${HDRG_DEPS_LIBRARIES})
    if(NOT HDRG_COMPILER_IS_MSVC_FRONTEND)
        target_link_libraries(${target} PRIVATE m)
    endif()
    if(HDRG_DAWN)
        target_link_libraries(${target} PRIVATE dawn::webgpu_dawn)
    endif()
    # MetalFX weakly: a system before it (macOS 13, iOS 16) has no spatial scaler
    if(HDRG_WITH_METAL)
        target_link_libraries(${target} PRIVATE "-framework Foundation" "-framework Metal"
            "-weak_framework MetalFX")
    endif()
    if(HDRG_WITH_CVPIXELBUFFER)
        target_link_libraries(${target} PRIVATE ${HDRG_APPLE_FRAMEWORKS})
    endif()
    # in the build tree the targets link with the C++ compiler (LINKER_LANGUAGE) already
    get_target_property(type ${target} TYPE)
    if(type STREQUAL "STATIC_LIBRARY")
        foreach(library IN LISTS HDRG_CXX_RUNTIME)
            target_link_libraries(${target} INTERFACE "$<INSTALL_INTERFACE:${library}>")
        endforeach()
    endif()
    set_target_properties(${target} PROPERTIES LINKER_LANGUAGE ${HDRG_LINKER_LANGUAGE})
endfunction()

add_library(hdrgrading ${HDRG_LIBRARY_OBJECTS})
add_library(hdrgrading::hdrgrading ALIAS hdrgrading)
hdrg_library_setup(hdrgrading)
# 0.x until the API is frozen: SOVERSION 0
set_target_properties(hdrgrading PROPERTIES VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})
hdrg_install_rpath(hdrgrading "${CMAKE_INSTALL_LIBDIR}")
if(BUILD_SHARED_LIBS)
    target_compile_definitions(hdrgrading INTERFACE HDRG_SHARED)
    if(APPLE)
        target_link_options(hdrgrading PRIVATE
            "LINKER:-exported_symbols_list,${CMAKE_CURRENT_BINARY_DIR}/hdrgrading.exp")
        set_property(TARGET hdrgrading APPEND PROPERTY
            LINK_DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/hdrgrading.exp")
    elseif(NOT WIN32)
        target_link_options(hdrgrading PRIVATE
            "LINKER:--version-script=${CMAKE_CURRENT_BINARY_DIR}/hdrgrading.map")
        set_property(TARGET hdrgrading APPEND PROPERTY
            LINK_DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/hdrgrading.map")
    endif()
    add_library(hdrgrading_internal STATIC ${HDRG_LIBRARY_OBJECTS})
    hdrg_library_setup(hdrgrading_internal)
    set(HDRG_INTERNAL hdrgrading_internal)
else()
    set(HDRG_INTERNAL hdrgrading)
endif()

# The codec modules (src/plugins/avif, src/plugins/heif): plugins of the codec ABI with dependencies
# of their own, built as loadable modules and never linked into the library. AVIF: the system's
# libavif (pkg-config, 1.2 or later: the gain map API); HEIF (writing alone): libheif and its HEVC
# encoder (cmake/HdrgHeif). A build tree's are in <build>/modules, an install's in
# <libdir>/hdrgrading: hdrg-cli and hdrg-gui load them from beside themselves (tools/modules.h),
# the tests by their paths
get_property(HDRG_MODULES GLOBAL PROPERTY HDRG_MODULE_TARGETS)
set(HDRG_MODULE_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/hdrgrading")
file(RELATIVE_PATH HDRG_MODULE_INSTALL_REL "/${CMAKE_INSTALL_BINDIR}" "/${HDRG_MODULE_INSTALL_DIR}")

# tools/: the programs installed (HDRG_BUILD_TOOLS); tests/tools/: the developers'
# (HDRG_BUILD_TESTS, not installed: they reach the internals)
set(hdrg_programs "")
if(HDRG_BUILD_TOOLS)
    list(APPEND hdrg_programs hdrg-cli hdrg-inspect hdrg-video)
endif()
if(HDRG_BUILD_TESTS)
    list(APPEND hdrg_programs hdrg-bench hdrg-predictor-data)
endif()
hdrg_builtins_have(hdrg_has_jxl jxl)
if(hdrg_programs)
    foreach(tool IN LISTS hdrg_programs)
        if(tool MATCHES "^hdrg-(bench|predictor-data)$")
            add_executable(${tool} tests/tools/${tool}.c)
        else()
            add_executable(${tool} tools/${tool}.c)
        endif()
        # the CLIs' shared parsing: filters, --set, the enumerated options (tools/common.c)
        if(tool MATCHES "^hdrg-(cli|video)$")
            target_sources(${tool} PRIVATE tools/common.c)
        endif()
        hdrg_target_warnings(${tool})
        hdrg_wasm_node_program(${tool})
        # libm of their own (a shared libhdrgrading does not pass its on)
        if(NOT HDRG_COMPILER_IS_MSVC_FRONTEND)
            target_link_libraries(${tool} PRIVATE m)
        endif()
        # hdrg-bench times the core passes one by one; hdrg-predictor-data writes the light
        # predictor's training rows (scripts/predictor)
        if(tool MATCHES "^hdrg-(bench|predictor-data)$")
            target_include_directories(${tool} PRIVATE "${PROJECT_SOURCE_DIR}/src")
            target_compile_definitions(${tool} PRIVATE ${HDRG_BUILTIN_DEFINITIONS})
            target_link_libraries(${tool} PRIVATE ${HDRG_INTERNAL})
        elseif(tool STREQUAL "hdrg-inspect" AND hdrg_has_jxl)
            target_link_libraries(${tool} PRIVATE ${HDRG_INTERNAL})
        else()
            target_link_libraries(${tool} PRIVATE hdrgrading)
        endif()
        # hdrg-cli loads the codec modules from beside itself (tools/modules.h)
        if(tool STREQUAL "hdrg-cli")
            target_compile_definitions(${tool} PRIVATE
                HDRG_MODULE_SUFFIX="${CMAKE_SHARED_MODULE_SUFFIX}" HDRG_MODULE_BUILD_REL="modules"
                HDRG_MODULE_INSTALL_REL="${HDRG_MODULE_INSTALL_REL}")
            if(HDRG_MODULES)
                add_dependencies(${tool} ${HDRG_MODULES})
            endif()
        endif()
        # hdrg-inspect reads the ICC HAGC payload with tools/hagc.c, a JPEG XL with the bundled
        # libjxl (the library has its objects: the static one, internal in a shared build)
        if(tool STREQUAL "hdrg-inspect")
            target_sources(${tool} PRIVATE tools/hagc.c)
        endif()
        if(tool STREQUAL "hdrg-inspect" AND hdrg_has_jxl)
            hdrg_dependency_headers(${tool} hdrg::jxl)
        endif()
        # hdrg-inspect reads an AVIF with the system's libavif (the module's)
        if(tool STREQUAL "hdrg-inspect" AND HDRG_WITH_AVIF)
            target_compile_definitions(${tool} PRIVATE HDRG_WITH_AVIF)
            target_link_libraries(${tool} PRIVATE PkgConfig::HDRG_LIBAVIF)
        endif()
        hdrg_install_rpath(${tool} "${CMAKE_INSTALL_BINDIR}")
    endforeach()
endif()

# tools/ffmpeg: AVFrame and session frames (libavutil of FFmpeg 7.0 or later), where FFmpeg is
# found. Not under TSan / MSan: the uninstrumented libavutil would be all they report
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND AND NOT EMSCRIPTEN)
    pkg_check_modules(HDRG_LIBAVUTIL QUIET IMPORTED_TARGET libavutil>=59)
endif()
if(HDRG_LIBAVUTIL_FOUND AND NOT HDRG_SANITIZER MATCHES "thread|memory")
    set(HDRG_FFMPEG_DEFAULT ON)
else()
    set(HDRG_FFMPEG_DEFAULT OFF)
endif()
option(HDRG_WITH_FFMPEG "Build tools/ffmpeg (libavutil 59 or later: hdrg-ffmpeg's helper) and its tests"
       ${HDRG_FFMPEG_DEFAULT})
if(HDRG_WITH_FFMPEG)
    if(NOT HDRG_LIBAVUTIL_FOUND)
        message(FATAL_ERROR "HDRG_WITH_FFMPEG needs libavutil 59 or later (pkg-config)")
    endif()
    add_library(hdrgrading_ffmpeg STATIC tools/ffmpeg/hdrg_ffmpeg.c)
    target_include_directories(hdrgrading_ffmpeg PUBLIC "${PROJECT_SOURCE_DIR}/tools/ffmpeg")
    target_link_libraries(hdrgrading_ffmpeg PUBLIC hdrgrading PkgConfig::HDRG_LIBAVUTIL)
    hdrg_target_warnings(hdrgrading_ffmpeg)

    # hdrg-ffmpeg: a video file through FFmpeg's libraries (linked dynamically) and the binding
    pkg_check_modules(HDRG_LIBAV QUIET IMPORTED_TARGET libavcodec>=61 libavformat>=61 libswscale>=8)
    if(HDRG_LIBAV_FOUND AND HDRG_BUILD_TOOLS)
        add_executable(hdrg-ffmpeg tools/hdrg-ffmpeg.c tools/common.c)
        target_link_libraries(hdrg-ffmpeg PRIVATE hdrgrading_ffmpeg PkgConfig::HDRG_LIBAV)
        hdrg_target_warnings(hdrg-ffmpeg)
        hdrg_install_rpath(hdrg-ffmpeg "${CMAKE_INSTALL_BINDIR}")
    endif()
endif()

if(HDRG_BUILD_GSTREAMER)
    if(NOT HDRG_GST_FOUND)
        message(FATAL_ERROR "HDRG_BUILD_GSTREAMER needs GStreamer 1.20 or later (pkg-config)")
    endif()
    add_library(gsthdrgrading MODULE src/bindings/gstreamer/gsthdrgrading.c)
    target_link_libraries(gsthdrgrading PRIVATE hdrgrading PkgConfig::HDRG_GST)
    set_target_properties(gsthdrgrading PROPERTIES PREFIX "lib" C_VISIBILITY_PRESET hidden)
    # the plugin's descriptor alone: the library linked in stays inside the module
    if(APPLE)
        target_link_options(gsthdrgrading PRIVATE
            "LINKER:-exported_symbol,_gst_plugin_hdrgrading_get_desc")
    elseif(NOT WIN32)
        target_link_options(gsthdrgrading PRIVATE "LINKER:--exclude-libs,ALL")
    endif()
    hdrg_target_warnings(gsthdrgrading)
    hdrg_install_rpath(gsthdrgrading "${CMAKE_INSTALL_LIBDIR}/gstreamer-1.0")
endif()

# hdrg-gui (tools/gui): SDL3 and Dear ImGui from third_party, its media (HDRG_GUI_MEDIA) the
# platform's decoders and encoders (macOS AVFoundation / ImageIO / VideoToolbox, Windows Media
# Foundation / WIC, Linux GStreamer) or FFmpeg's (its decoders, hdrg-ffmpeg's export)
option(HDRG_BUILD_GUI "Build hdrg-gui (tools/gui: SDL3 and Dear ImGui from third_party)" OFF)
set(HDRG_GUI_MEDIA platform CACHE STRING "hdrg-gui's decoders and encoders: platform or ffmpeg")
set_property(CACHE HDRG_GUI_MEDIA PROPERTY STRINGS platform ffmpeg)
if(HDRG_GUI_MEDIA STREQUAL "ffmpeg")
    set(HDRG_GUI_FFMPEG ON)
elseif(HDRG_GUI_MEDIA STREQUAL "platform")
    set(HDRG_GUI_FFMPEG OFF)
else()
    message(FATAL_ERROR "HDRG_GUI_MEDIA: platform or ffmpeg, not ${HDRG_GUI_MEDIA}")
endif()
if(HDRG_BUILD_GUI)
    if(EMSCRIPTEN)
        message(FATAL_ERROR "HDRG_BUILD_GUI needs a native build")
    endif()
    enable_language(CXX)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    # what the window and the UI need: no audio, camera, haptics, sensors or X11's XTest
    foreach(hdrg_sdl_off IN ITEMS SDL_AUDIO SDL_CAMERA SDL_HAPTIC SDL_SENSOR SDL_X11_XTEST)
        set(${hdrg_sdl_off} OFF CACHE BOOL "" FORCE)
    endforeach()
    add_subdirectory(third_party/sdl EXCLUDE_FROM_ALL)
    set(HDRG_IMGUI "${PROJECT_SOURCE_DIR}/third_party/imgui")
    add_library(hdrg_imgui STATIC EXCLUDE_FROM_ALL
        "${HDRG_IMGUI}/imgui.cpp" "${HDRG_IMGUI}/imgui_draw.cpp" "${HDRG_IMGUI}/imgui_tables.cpp"
        "${HDRG_IMGUI}/imgui_widgets.cpp" "${HDRG_IMGUI}/backends/imgui_impl_sdl3.cpp"
        "${HDRG_IMGUI}/backends/imgui_impl_sdlgpu3.cpp")
    target_include_directories(hdrg_imgui SYSTEM PUBLIC "${HDRG_IMGUI}" "${HDRG_IMGUI}/backends")
    target_link_libraries(hdrg_imgui PUBLIC SDL3::SDL3-static)
    target_compile_features(hdrg_imgui PUBLIC cxx_std_17)

    # the C parts under the project's warnings; the UI (C++) and the Objective-C beside them
    add_library(hdrg_gui_core STATIC EXCLUDE_FROM_ALL tools/gui/grading.c tools/gui/player.c
        tools/gui/export.c)
    target_link_libraries(hdrg_gui_core PUBLIC hdrgrading SDL3::SDL3-static)
    # the codec modules from beside the program (tools/modules.h), as hdrg-cli
    target_compile_definitions(hdrg_gui_core PRIVATE
        HDRG_MODULE_SUFFIX="${CMAKE_SHARED_MODULE_SUFFIX}" HDRG_MODULE_BUILD_REL="modules"
        HDRG_MODULE_INSTALL_REL="${HDRG_MODULE_INSTALL_REL}")
    hdrg_target_warnings(hdrg_gui_core)
    add_executable(hdrg-gui tools/gui/main.cpp)
    target_link_libraries(hdrg-gui PRIVATE hdrg_gui_core hdrg_imgui)
    if(HDRG_MODULES)
        add_dependencies(hdrg-gui ${HDRG_MODULES})
    endif()
    target_include_directories(hdrg-gui PRIVATE tools/gui)
    if(APPLE)
        enable_language(OBJC)
        target_sources(hdrg-gui PRIVATE tools/gui/media_apple.m tools/gui/encoder_apple.m)
        set_source_files_properties(tools/gui/media_apple.m tools/gui/encoder_apple.m PROPERTIES
            COMPILE_OPTIONS "-fobjc-arc;-Wall;-Wextra")
        target_link_libraries(hdrg-gui PRIVATE "-framework AVFoundation" "-framework CoreMedia"
            "-framework CoreVideo" "-framework ImageIO" "-framework CoreGraphics"
            "-framework VideoToolbox" "-framework Foundation")
    elseif(WIN32)
        target_sources(hdrg_gui_core PRIVATE tools/gui/media_windows.c tools/gui/encoder_windows.c)
        target_link_libraries(hdrg_gui_core PUBLIC mfplat mfreadwrite mfuuid windowscodecs ole32
            propsys)
    elseif(NOT HDRG_GUI_FFMPEG)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(HDRG_GUI_GST REQUIRED IMPORTED_TARGET gstreamer-1.0>=1.20
            gstreamer-app-1.0 gstreamer-video-1.0)
        target_sources(hdrg_gui_core PRIVATE tools/gui/media_gst.c tools/gui/encoder_none.c)
        target_link_libraries(hdrg_gui_core PUBLIC PkgConfig::HDRG_GUI_GST)
    else()
        target_sources(hdrg_gui_core PRIVATE tools/gui/encoder_none.c)
    endif()
    if(HDRG_GUI_FFMPEG)
        if(NOT TARGET hdrg-ffmpeg)
            message(FATAL_ERROR "HDRG_GUI_MEDIA ffmpeg needs FFmpeg's libraries (hdrg-ffmpeg)")
        endif()
        target_compile_definitions(hdrg_gui_core PUBLIC HDRG_GUI_FFMPEG)
        if(NOT APPLE AND NOT WIN32)
            target_sources(hdrg_gui_core PRIVATE tools/gui/media_ffmpeg.c)
            target_link_libraries(hdrg_gui_core PUBLIC hdrgrading_ffmpeg PkgConfig::HDRG_LIBAV)
        endif()
        add_dependencies(hdrg-gui hdrg-ffmpeg)
    endif()
    # without a window: kodim23 graded into the player's pictures, and converted by the export
    if(HDRG_BUILD_TESTS)
        add_test(NAME test_gui_probe COMMAND hdrg-gui --probe "${PROJECT_SOURCE_DIR}/assets/kodim23.png")
        # side by side: twice the width, the SDR left of the brighter HDR
        add_test(NAME test_gui_probe_both COMMAND hdrg-gui --probe
            "${PROJECT_SOURCE_DIR}/assets/kodim23.png" --view both)
        add_test(NAME test_gui_export COMMAND hdrg-gui --export
            "${PROJECT_SOURCE_DIR}/assets/kodim23.png" "${CMAKE_CURRENT_BINARY_DIR}/test_gui_export.jpg")
    endif()
endif()

if(HDRG_BUILD_VAPOURSYNTH)
    add_library(hdrgrading_vs MODULE src/bindings/vapoursynth/hdrg_vapoursynth.c)
    target_include_directories(hdrgrading_vs SYSTEM PRIVATE
        "${PROJECT_SOURCE_DIR}/third_party/vapoursynth/include")
    target_link_libraries(hdrgrading_vs PRIVATE hdrgrading)
    set_target_properties(hdrgrading_vs PROPERTIES C_VISIBILITY_PRESET hidden)
    # the entry point alone: the library linked in stays inside the module
    if(APPLE)
        target_link_options(hdrgrading_vs PRIVATE "LINKER:-exported_symbol,_VapourSynthPluginInit2")
    elseif(NOT WIN32)
        target_link_options(hdrgrading_vs PRIVATE "LINKER:--exclude-libs,ALL")
    endif()
    hdrg_target_warnings(hdrgrading_vs)
    hdrg_install_rpath(hdrgrading_vs "${CMAKE_INSTALL_LIBDIR}/vapoursynth")
endif()

# Installing: the library and its headers, the CMake package
# (hdrgrading::hdrgrading) and pkg-config file, both relocatable, the tools and plugin modules, and
# the licenses (the bundled dependencies' too: their objects are in the library)
if(HDRG_INSTALL)
    include(CMakePackageConfigHelpers)
    set(HDRG_CMAKE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/hdrgrading")
    install(TARGETS hdrgrading EXPORT hdrgradingTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    install(FILES ${HDRG_PUBLIC_HEADERS} "${CMAKE_CURRENT_BINARY_DIR}/include/hdrgrading/version.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/hdrgrading")
    # the layers the plugins' projects (src/plugins) build on, static libraries of their objects
    # with their headers: hdrgrading::headers, hdrgrading::foundation, hdrgrading::gpu,
    # hdrgrading::plugin_shared and hdrg_plugin (cmake/HdrgPlugin.cmake), for a plugin built apart
    set_target_properties(hdrgrading_headers PROPERTIES EXPORT_NAME headers)
    install(TARGETS hdrgrading_headers EXPORT hdrgradingTargets)
    add_library(hdrgrading_foundation_static STATIC $<TARGET_OBJECTS:hdrgrading_foundation>)
    target_link_libraries(hdrgrading_foundation_static INTERFACE hdrgrading_headers
        Threads::Threads)
    if(NOT HDRG_COMPILER_IS_MSVC_FRONTEND)
        target_link_libraries(hdrgrading_foundation_static INTERFACE m)
    endif()
    set(hdrg_layers hdrgrading_foundation_static)
    get_target_property(hdrg_gpu_type hdrgrading_gpu TYPE)
    if(hdrg_gpu_type STREQUAL "OBJECT_LIBRARY")
        add_library(hdrgrading_gpu_static STATIC $<TARGET_OBJECTS:hdrgrading_gpu>)
        get_target_property(hdrg_gpu_definitions hdrgrading_gpu INTERFACE_COMPILE_DEFINITIONS)
        target_compile_definitions(hdrgrading_gpu_static INTERFACE ${hdrg_gpu_definitions})
        target_link_libraries(hdrgrading_gpu_static INTERFACE hdrgrading_foundation_static
            ${CMAKE_DL_LIBS})
        if(HDRG_DAWN)
            target_link_libraries(hdrgrading_gpu_static INTERFACE dawn::webgpu_dawn)
        endif()
        if(HDRG_WITH_VULKAN)
            target_link_libraries(hdrgrading_gpu_static INTERFACE Vulkan::Headers)
        endif()
        if(HDRG_WITH_METAL)
            target_link_libraries(hdrgrading_gpu_static INTERFACE "-framework Foundation"
                "-framework Metal" "-weak_framework MetalFX")
        endif()
        if(HDRG_WITH_CVPIXELBUFFER)
            target_link_libraries(hdrgrading_gpu_static INTERFACE ${HDRG_APPLE_FRAMEWORKS})
        endif()
    else()
        add_library(hdrgrading_gpu_static INTERFACE)
        target_link_libraries(hdrgrading_gpu_static INTERFACE hdrgrading_foundation_static)
    endif()
    add_library(hdrgrading_plugin_shared_static STATIC $<TARGET_OBJECTS:hdrgrading_plugin_shared>)
    target_link_libraries(hdrgrading_plugin_shared_static INTERFACE hdrgrading_foundation_static)
    list(APPEND hdrg_layers hdrgrading_gpu_static hdrgrading_plugin_shared_static)
    foreach(layer IN ITEMS foundation gpu plugin_shared)
        set_target_properties(hdrgrading_${layer}_static PROPERTIES EXPORT_NAME ${layer})
        get_target_property(hdrg_layer_type hdrgrading_${layer}_static TYPE)
        if(NOT hdrg_layer_type STREQUAL "INTERFACE_LIBRARY")
            set_target_properties(hdrgrading_${layer}_static PROPERTIES
                OUTPUT_NAME hdrgrading-${layer})
        endif()
    endforeach()
    install(TARGETS ${hdrg_layers} EXPORT hdrgradingTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
    # (not the headers no plugin builds on: the build's default allocation, the Vulkan layer's
    # own, an app's CVPixelBuffer)
    install(DIRECTORY src/foundation/include/hdrgrading/foundation src/gpu/include/hdrgrading/gpu
        src/plugins/shared/include/hdrgrading/plugins
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/hdrgrading"
        PATTERN "memory.h" EXCLUDE PATTERN "vulkan.h" EXCLUDE PATTERN "apple.h" EXCLUDE)
    install(FILES cmake/HdrgPlugin.cmake cmake/HdrgHeif.cmake DESTINATION "${HDRG_CMAKE_DIR}")
    install(EXPORT hdrgradingTargets NAMESPACE hdrgrading:: DESTINATION "${HDRG_CMAKE_DIR}")
    # a static library of the system's dependencies names them (the bundled ones are inside it)
    hdrg_builtins_have(hdrg_has_png png)
    hdrg_builtins_have(hdrg_has_gif gif)
    set(HDRG_PACKAGE_PNG OFF)
    set(HDRG_PACKAGE_GIF OFF)
    if(HDRG_WITH_SYSTEM_DEPS AND NOT BUILD_SHARED_LIBS)
        set(HDRG_PACKAGE_PNG ${hdrg_has_png})
        set(HDRG_PACKAGE_GIF ${hdrg_has_gif})
    endif()
    configure_package_config_file(cmake/hdrgradingConfig.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/hdrgradingConfig.cmake" INSTALL_DESTINATION "${HDRG_CMAKE_DIR}")
    # 0.x: a minor version breaks the API (SameMinorVersion)
    write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/hdrgradingConfigVersion.cmake"
        COMPATIBILITY SameMinorVersion)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/hdrgradingConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/hdrgradingConfigVersion.cmake" DESTINATION "${HDRG_CMAKE_DIR}")

    # pkg-config: the prefix from the file's own directory; a static library's links in Libs.private
    if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
        set(HDRG_PC_PREFIX_LINE "${CMAKE_INSTALL_PREFIX}")
        set(HDRG_PC_LIBDIR "${CMAKE_INSTALL_LIBDIR}")
    else()
        file(RELATIVE_PATH hdrg_pc_up "/${CMAKE_INSTALL_LIBDIR}/pkgconfig" "/")
        string(REGEX REPLACE "/$" "" hdrg_pc_up "${hdrg_pc_up}")
        set(HDRG_PC_PREFIX_LINE "\${pcfiledir}/${hdrg_pc_up}")
        set(HDRG_PC_LIBDIR "\${exec_prefix}/${CMAKE_INSTALL_LIBDIR}")
    endif()
    if(IS_ABSOLUTE "${CMAKE_INSTALL_INCLUDEDIR}")
        set(HDRG_PC_INCLUDEDIR "${CMAKE_INSTALL_INCLUDEDIR}")
    else()
        set(HDRG_PC_INCLUDEDIR "\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}")
    endif()
    set(HDRG_PC_REQUIRES_PRIVATE "")
    set(HDRG_PC_LIBS_PRIVATE "")
    set(HDRG_PC_CFLAGS "")
    if(BUILD_SHARED_LIBS)
        set(HDRG_PC_CFLAGS " -DHDRG_SHARED")
    else()
        if(HDRG_PACKAGE_PNG)
            list(APPEND HDRG_PC_REQUIRES_PRIVATE zlib libpng)
        endif()
        if(HDRG_WITH_SYSTEM_DEPS AND HDRG_WITH_LCMS2)
            list(APPEND HDRG_PC_REQUIRES_PRIVATE lcms2)
        endif()
        if(HDRG_PACKAGE_GIF)
            list(APPEND HDRG_PC_LIBS_PRIVATE -lgif)
        endif()
        # Dawn's library is found by the rpath (its install name, @rpath on Apple)
        if(HDRG_DAWN)
            list(APPEND HDRG_PC_LIBS_PRIVATE "$<TARGET_LINKER_FILE:dawn::webgpu_dawn>"
                "-Wl,-rpath,$<TARGET_LINKER_FILE_DIR:dawn::webgpu_dawn>")
        endif()
        if(HDRG_WITH_METAL)
            list(APPEND HDRG_PC_LIBS_PRIVATE "-framework Foundation" "-framework Metal"
                "-weak_framework MetalFX")
        endif()
        if(HDRG_WITH_CVPIXELBUFFER)
            list(APPEND HDRG_PC_LIBS_PRIVATE ${HDRG_APPLE_FRAMEWORKS})
        endif()
        list(APPEND HDRG_PC_LIBS_PRIVATE ${HDRG_CXX_RUNTIME})
        foreach(library IN LISTS CMAKE_DL_LIBS)
            list(APPEND HDRG_PC_LIBS_PRIVATE "-l${library}")
        endforeach()
        if(NOT HDRG_COMPILER_IS_MSVC_FRONTEND)
            list(APPEND HDRG_PC_LIBS_PRIVATE -lm)
        endif()
        list(APPEND HDRG_PC_LIBS_PRIVATE ${CMAKE_THREAD_LIBS_INIT})
        list(REMOVE_DUPLICATES HDRG_PC_LIBS_PRIVATE)
    endif()
    list(JOIN HDRG_PC_REQUIRES_PRIVATE " " HDRG_PC_REQUIRES_PRIVATE)
    list(JOIN HDRG_PC_LIBS_PRIVATE " " HDRG_PC_LIBS_PRIVATE)
    configure_file(cmake/hdrgrading.pc.in "${CMAKE_CURRENT_BINARY_DIR}/hdrgrading.pc.in" @ONLY)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/hdrgrading.pc"
        INPUT "${CMAKE_CURRENT_BINARY_DIR}/hdrgrading.pc.in")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/hdrgrading.pc"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")

    if(HDRG_BUILD_TOOLS AND NOT EMSCRIPTEN)
        install(TARGETS hdrg-cli hdrg-video hdrg-inspect RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
        if(TARGET hdrg-ffmpeg)
            install(TARGETS hdrg-ffmpeg RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
        endif()
    endif()
    if(TARGET gsthdrgrading)
        install(TARGETS gsthdrgrading LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/gstreamer-1.0")
    endif()
    if(TARGET hdrgrading_vs)
        install(TARGETS hdrgrading_vs LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/vapoursynth")
    endif()

    if(HDRG_MODULES)
        install(TARGETS ${HDRG_MODULES} LIBRARY DESTINATION "${HDRG_MODULE_INSTALL_DIR}")
    endif()
    # the fetched libheif and its codecs go beside the library (the HEIF module finds them there;
    # the system's are the system's)
    if(HDRG_WITH_HEIF AND HDRG_HEIF_FETCHED)
        install(DIRECTORY "${HDRG_HEIF_PREFIX}/lib/" DESTINATION "${CMAKE_INSTALL_LIBDIR}"
            USE_SOURCE_PERMISSIONS FILES_MATCHING
            PATTERN "${CMAKE_SHARED_LIBRARY_PREFIX}heif*${CMAKE_SHARED_LIBRARY_SUFFIX}*"
            PATTERN "${CMAKE_SHARED_LIBRARY_PREFIX}de265*${CMAKE_SHARED_LIBRARY_SUFFIX}*"
            PATTERN "${CMAKE_SHARED_LIBRARY_PREFIX}x265*${CMAKE_SHARED_LIBRARY_SUFFIX}*"
            PATTERN "cmake" EXCLUDE PATTERN "pkgconfig" EXCLUDE PATTERN "libheif" EXCLUDE)
        while(HDRG_HEIF_LICENSES)
            list(POP_FRONT HDRG_HEIF_LICENSES name file)
            install(FILES "${file}" DESTINATION "${CMAKE_INSTALL_DOCDIR}/licenses"
                RENAME "${name}.txt" OPTIONAL)
        endwhile()
    endif()
    install(FILES LICENSE-APACHE LICENSE-MIT NOTICE DESTINATION "${CMAKE_INSTALL_DOCDIR}")
    # the licenses of the dependencies bundled (the families built in)
    hdrg_builtins_have(hdrg_has_jpeg jpeg)
    set(hdrg_licenses "")
    if(hdrg_has_jpeg)
        list(APPEND hdrg_licenses jpegli "third_party/jpegli/LICENSE"
            libjpeg-turbo "third_party/jpegli/third_party/libjpeg-turbo/LICENSE.md")
    endif()
    if(hdrg_has_jpeg OR hdrg_has_jxl)
        list(APPEND hdrg_licenses highway "third_party/jpegli/third_party/highway/LICENSE")
    endif()
    if(hdrg_has_jxl)
        list(APPEND hdrg_licenses libjxl "third_party/libjxl/LICENSE"
            libjxl-patents "third_party/libjxl/PATENTS" brotli "third_party/brotli/LICENSE")
    endif()
    if(NOT HDRG_WITH_SYSTEM_DEPS)
        if(hdrg_has_png)
            list(APPEND hdrg_licenses zlib "third_party/zlib/LICENSE" libpng "third_party/libpng/LICENSE")
        endif()
        if(hdrg_has_gif)
            list(APPEND hdrg_licenses giflib "third_party/giflib/COPYING")
        endif()
        if(HDRG_WITH_LCMS2)
            list(APPEND hdrg_licenses lcms2 "third_party/lcms2/LICENSE")
        endif()
    endif()
    while(hdrg_licenses)
        list(POP_FRONT hdrg_licenses name file)
        install(FILES "${file}" DESTINATION "${CMAKE_INSTALL_DOCDIR}/licenses" RENAME "${name}.txt")
    endwhile()
endif()

if(HDRG_BUILD_TESTS)
    enable_testing()
    hdrg_add_unity("${CMAKE_CURRENT_SOURCE_DIR}/third_party/unity")

    foreach(test IN ITEMS
            test_common_math
            test_common_fmath
            test_common_hash
            test_common_fpenv
            test_common_thread
            test_common_pool
            test_unionfind
            test_kernels
            test_color
            test_plan
            test_render
            test_session
            test_yuv
            test_noise
            test_luminance)
        hdrg_add_test(${test} SOURCES tests/unit/${test}.c LIBRARIES ${HDRG_INTERNAL})
    endforeach()
    # the Metal layer (a device, Tint's MSL, host buffers in place, passes ended on Metal's thread),
    # on the debanding's kernel
    hdrg_builtins_have(hdrg_has_deband deband)
    if(HDRG_WITH_METAL AND hdrg_has_deband)
        hdrg_add_test(test_gpu_metal SOURCES tests/unit/test_gpu_metal.c
            LIBRARIES ${HDRG_INTERNAL})
    endif()
    # the Vulkan layer (a device of its own or the app's, Tint's SPIR-V, passes and their references)
    if(HDRG_WITH_VULKAN AND hdrg_has_deband)
        hdrg_add_test(test_gpu_vulkan SOURCES tests/unit/test_gpu_vulkan.c
            LIBRARIES ${HDRG_INTERNAL})
    endif()


    # the conformance tests: JPEG in and out (tests/test_jpeg.h writes their inputs with jpegli)
    hdrg_builtins_have(hdrg_has_jpeg jpeg)
    if(hdrg_has_jpeg)
        # the built-in plugins a test is about (HDRG_PLUGINS_EXCLUDE)
        set(hdrg_test_needs_test_interp interp)
        set(hdrg_test_needs_test_mctf mctf)
        set(hdrg_test_needs_test_grain denoise)
        set(hdrg_test_needs_test_upscale bilinear fsr nis edge)
        hdrg_builtins_have(hdrg_has_prefilters denoise deblock dering defringe deband)
        foreach(test IN ITEMS
                test_output
                test_hybrid
                test_gainmap
                test_grading
                test_gamut
                test_determinism
                test_alloc
                test_params
                test_orientation
                test_hagc
                test_pathological
                test_plugin
                test_decode
                test_interp
                test_mctf
                test_grain
                test_dynamic
                test_upscale)
            # a test of built-in plugins the build left out: none
            hdrg_builtins_have(have ${hdrg_test_needs_${test}})
            if(NOT have)
                continue()
            endif()
            hdrg_add_test(${test} SOURCES tests/conformance/${test}.c
                LIBRARIES ${HDRG_INTERNAL})
            # tests/test_jpeg.h writes their JPEG inputs with jpegli (the library has its objects)
            hdrg_dependency_headers(${test} hdrg::jpegli)
            if(HDRG_WITH_LCMS2)
                target_compile_definitions(${test} PRIVATE HDRG_WITH_LCMS2)
            endif()
        endforeach()
        # the GPU filters against their CPU filters
        if(HDRG_WITH_WEBGPU)
            target_compile_definitions(test_plugin PRIVATE HDRG_WITH_WEBGPU)
        endif()
        if(HDRG_WITH_WEBGPU AND hdrg_has_prefilters)
            hdrg_add_test(test_webgpu SOURCES tests/conformance/test_webgpu.c
                LIBRARIES ${HDRG_INTERNAL})
            # the shared device's test reads src/gpu/webgpu.h (Dawn's webgpu.h)
            if(HDRG_DAWN)
                target_link_libraries(test_webgpu PRIVATE dawn::webgpu_dawn)
            endif()
        endif()
        if(HDRG_WITH_VULKAN)
            target_compile_definitions(test_plugin PRIVATE HDRG_WITH_VULKAN)
        endif()
        # the apply pass on the GPU against the CPU's
        if(HDRG_WITH_WEBGPU OR HDRG_WITH_METAL OR HDRG_WITH_VULKAN)
            hdrg_add_test(test_gpu_apply SOURCES tests/conformance/test_gpu_apply.c
                LIBRARIES ${HDRG_INTERNAL})
            hdrg_add_test(test_gpu_convert SOURCES tests/conformance/test_gpu_convert.c
                LIBRARIES ${HDRG_INTERNAL})
            hdrg_add_test(test_gpu_analyze SOURCES tests/conformance/test_gpu_analyze.c
                LIBRARIES ${HDRG_INTERNAL})
        endif()
        # the Metal filters against their CPU filters, and MetalFX's scaler
        if(HDRG_WITH_METAL)
            target_compile_definitions(test_plugin PRIVATE HDRG_WITH_METAL)
        endif()
        if(HDRG_WITH_METAL AND hdrg_has_prefilters)
            hdrg_add_test(test_metal SOURCES tests/conformance/test_metal.c
                LIBRARIES ${HDRG_INTERNAL} ${HDRG_APPLE_FRAMEWORKS})
            if(HDRG_WITH_WEBGPU)
                target_compile_definitions(test_metal PRIVATE HDRG_WITH_WEBGPU)
            endif()
        endif()
        # the Vulkan filters against their CPU filters: test_metal.c's tests on Vulkan
        if(HDRG_WITH_VULKAN AND hdrg_has_prefilters)
            hdrg_add_test(test_vulkan SOURCES tests/conformance/test_metal.c
                LIBRARIES ${HDRG_INTERNAL})
            target_compile_definitions(test_vulkan PRIVATE TM_VULKAN)
        endif()
        # a module's GPU filters between the library's (tests/modules/gpu_copy.c: a plugin built
        # apart, on a copy of the GPU layer of its own, the objects of src/gpu and src/foundation
        # linked in): the context's device and frames shared across the binaries
        if(NOT EMSCRIPTEN AND hdrg_has_prefilters AND
           (HDRG_WITH_WEBGPU OR HDRG_WITH_METAL OR HDRG_WITH_VULKAN))
            add_library(hdrg_gpu_copy MODULE tests/modules/gpu_copy.c)
            target_link_libraries(hdrg_gpu_copy PRIVATE hdrgrading_gpu hdrgrading_foundation
                Threads::Threads ${CMAKE_DL_LIBS})
            if(NOT HDRG_COMPILER_IS_MSVC_FRONTEND)
                target_link_libraries(hdrg_gpu_copy PRIVATE m)
            endif()
            if(HDRG_WITH_METAL)
                target_link_libraries(hdrg_gpu_copy PRIVATE "-framework Foundation"
                    "-framework Metal" "-weak_framework MetalFX")
            endif()
            if(HDRG_WITH_CVPIXELBUFFER)
                target_link_libraries(hdrg_gpu_copy PRIVATE ${HDRG_APPLE_FRAMEWORKS})
            endif()
            set_target_properties(hdrg_gpu_copy PROPERTIES PREFIX "" C_VISIBILITY_PRESET hidden)
            hdrg_target_warnings(hdrg_gpu_copy)
            foreach(test IN ITEMS test_webgpu test_metal test_vulkan)
                if(TARGET ${test})
                    add_dependencies(${test} hdrg_gpu_copy)
                    target_compile_definitions(${test} PRIVATE
                        HDRG_TEST_GPU_MODULE="$<TARGET_FILE:hdrg_gpu_copy>")
                endif()
            endforeach()
        endif()
        # test_gamut writes ICC profiles with lcms2 to check the decoder; test_decode writes JPEGs
        # of a chosen subsampling and reads raw planes with jpegli (the library has their objects)
        if(HDRG_WITH_LCMS2)
            hdrg_dependency_headers(test_gamut hdrg::lcms2)
        endif()
        hdrg_dependency_headers(test_decode hdrg::jpegli)
        # the built-in JPEG XL writer, read back with the bundled libjxl's decoder
        if(hdrg_has_jxl)
            hdrg_add_test(test_jxl SOURCES tests/conformance/test_jxl.c
                LIBRARIES ${HDRG_INTERNAL})
            hdrg_dependency_headers(test_jxl hdrg::jpegli)
            hdrg_dependency_headers(test_jxl hdrg::jxl)
            target_compile_definitions(test_jxl PRIVATE JXL_STATIC_DEFINE)
        endif()
        # the codec modules (src/plugins/avif, src/plugins/heif), loaded by their paths: AVIF read
        # back with libavif and its inputs written with it; HEIF read back with libheif (and
        # ImageIO on Apple)
        if(HDRG_WITH_AVIF)
            hdrg_add_test(test_avif SOURCES tests/conformance/test_avif.c
                LIBRARIES ${HDRG_INTERNAL} PkgConfig::HDRG_LIBAVIF)
            hdrg_dependency_headers(test_avif hdrg::jpegli)
            target_compile_definitions(test_avif PRIVATE
                HDRG_TEST_AVIF_MODULE="$<TARGET_FILE:hdrgrading-avif>")
            add_dependencies(test_avif hdrgrading-avif)
        endif()
        if(HDRG_WITH_HEIF)
            hdrg_add_test(test_heif SOURCES tests/conformance/test_heif.c
                LIBRARIES ${HDRG_INTERNAL} hdrg::heif)
            hdrg_dependency_headers(test_heif hdrg::jpegli)
            target_compile_definitions(test_heif PRIVATE
                HDRG_TEST_HEIF_MODULE="$<TARGET_FILE:hdrgrading-heif>")
            if(HDRG_HEIF_FETCHED)
                set_property(TARGET test_heif APPEND PROPERTY BUILD_RPATH "${HDRG_HEIF_PREFIX}/lib")
            endif()
            add_dependencies(test_heif hdrgrading-heif)
            if(APPLE)
                target_link_libraries(test_heif PRIVATE "-framework CoreFoundation"
                    "-framework ImageIO" "-framework CoreGraphics")
            endif()
        endif()
        # the decoders over tests/fuzz/fuzz_seeds.c's corpus, each file also cut short and with a byte
        # changed (the fixtures write the corpus into the build tree first)
        if(NOT EMSCRIPTEN AND NOT WIN32)
            set(hdrg_seeds_dir "${CMAKE_CURRENT_BINARY_DIR}/seeds_corpus")
            hdrg_add_test(test_seeds SOURCES tests/conformance/test_seeds.c
                LIBRARIES ${HDRG_INTERNAL})
            # a corpus of this build's forms alone (not the files an earlier build wrote)
            add_test(NAME test_seeds_clean
                COMMAND "${CMAKE_COMMAND}" -E rm -rf "${hdrg_seeds_dir}")
            add_test(NAME test_seeds_dir
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${hdrg_seeds_dir}")
            add_test(NAME test_seeds_corpus COMMAND fuzz_seeds "${hdrg_seeds_dir}")
            set_tests_properties(test_seeds_clean PROPERTIES FIXTURES_SETUP seeds_clean)
            set_tests_properties(test_seeds_dir PROPERTIES FIXTURES_REQUIRED seeds_clean
                FIXTURES_SETUP seeds_dir)
            set_tests_properties(test_seeds_corpus PROPERTIES
                FIXTURES_REQUIRED seeds_dir FIXTURES_SETUP seeds)
            set_tests_properties(test_seeds PROPERTIES FIXTURES_REQUIRED seeds
                ENVIRONMENT "HDRG_SEEDS_DIR=${hdrg_seeds_dir}")
        endif()
        # under Valgrind threads run one at a time (what overlaps in time is not checked) and slowly
        # (test_alloc refuses a seventh of the allocations)
        if(CMAKE_TEST_LAUNCHER MATCHES "valgrind")
            if(TARGET test_interp)
                target_compile_definitions(test_interp PRIVATE HDRG_TEST_VALGRIND)
            endif()
            target_compile_definitions(test_alloc PRIVATE HDRG_TEST_VALGRIND)
        endif()
        foreach(test IN ITEMS test_pathological test_alloc test_hagc test_upscale)
            if(TARGET ${test})
                target_compile_definitions(${test}
                    PRIVATE HDRG_TEST_ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets")
            endif()
        endforeach()
        # the HAGC payload read back with hdrg-inspect's reader (tools/hagc.c)
        foreach(test IN ITEMS test_hagc test_dynamic)
            if(TARGET ${test})
                target_sources(${test} PRIVATE tools/hagc.c)
                target_include_directories(${test} PRIVATE "${PROJECT_SOURCE_DIR}/tools")
            endif()
        endforeach()

        # the browser module under node, against hdrg-cli of the same build
        if(EMSCRIPTEN AND HDRG_BUILD_TOOLS)
            add_test(NAME wasm_cli_reference
                COMMAND hdrg-cli "${PROJECT_SOURCE_DIR}/assets/kodim23.png"
                    "${CMAKE_CURRENT_BINARY_DIR}/wasm_cli_reference.jpg")
            set_tests_properties(wasm_cli_reference PROPERTIES FIXTURES_SETUP wasm_reference)
            add_test(NAME test_wasm_module
                COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR}
                    "${PROJECT_SOURCE_DIR}/tests/wasm/test_module.mjs" $<TARGET_FILE:hdrg_wasm>
                    "${PROJECT_SOURCE_DIR}/assets/kodim23.png"
                    "${CMAKE_CURRENT_BINARY_DIR}/wasm_cli_reference.jpg")
            set_tests_properties(test_wasm_module PROPERTIES FIXTURES_REQUIRED wasm_reference)
        endif()

        if(HDRG_WITH_FFMPEG)
            hdrg_add_test(test_ffmpeg SOURCES tests/ffmpeg/test_ffmpeg.c
                LIBRARIES hdrgrading_ffmpeg)
            # FFmpeg's libhdrgrading filter (src/bindings/ffmpeg/patches, an FFmpeg built with it:
            # else the test ignores it) on this build's shared library
            pkg_check_modules(HDRG_LIBAVFILTER QUIET IMPORTED_TARGET libavfilter)
            if(HDRG_LIBAVFILTER_FOUND AND BUILD_SHARED_LIBS AND NOT WIN32)
                hdrg_add_test(test_filter SOURCES tests/ffmpeg/test_filter.c
                    LIBRARIES hdrgrading PkgConfig::HDRG_LIBAVFILTER PkgConfig::HDRG_LIBAVUTIL)
                set_tests_properties(test_filter PROPERTIES ENVIRONMENT
                    "DYLD_LIBRARY_PATH=$<TARGET_FILE_DIR:hdrgrading>;LD_LIBRARY_PATH=$<TARGET_FILE_DIR:hdrgrading>")
            endif()
            if(TARGET hdrg-ffmpeg)
                hdrg_add_test(test_transcode SOURCES tests/ffmpeg/test_transcode.c
                    LIBRARIES hdrgrading hdrgrading_ffmpeg PkgConfig::HDRG_LIBAV
                    PkgConfig::HDRG_LIBAVUTIL)
                target_compile_definitions(test_transcode PRIVATE
                    HDRG_TEST_FFMPEG_TOOL="$<TARGET_FILE:hdrg-ffmpeg>")
                # under Valgrind the test leaves libaom out (FFmpeg 7.1's libaomenc writes freed
                # memory as it opens; VEX stops on libaom's NEON)
                if(CMAKE_TEST_LAUNCHER MATCHES "valgrind")
                    target_compile_definitions(test_transcode PRIVATE HDRG_TEST_VALGRIND)
                endif()
                add_dependencies(test_transcode hdrg-ffmpeg)
            endif()
        endif()

        # the GStreamer plugin in a pipeline of appsrc and appsink
        if(HDRG_BUILD_GSTREAMER)
            pkg_check_modules(HDRG_GST_APP QUIET IMPORTED_TARGET gstreamer-app-1.0>=1.20)
            if(HDRG_GST_APP_FOUND)
                hdrg_add_test(test_gstreamer SOURCES tests/gstreamer/test_gstreamer.c
                    LIBRARIES hdrgrading PkgConfig::HDRG_GST PkgConfig::HDRG_GST_APP)
                target_compile_definitions(test_gstreamer PRIVATE
                    HDRG_TEST_GST_PLUGIN="$<TARGET_FILE:gsthdrgrading>")
                add_dependencies(test_gstreamer gsthdrgrading)
            endif()
        endif()

        # the VapourSynth plugin under a host of the API's parts it calls (dlopen)
        if(HDRG_BUILD_VAPOURSYNTH AND NOT WIN32)
            hdrg_add_test(test_vapoursynth SOURCES tests/vapoursynth/test_vapoursynth.c
                LIBRARIES hdrgrading ${CMAKE_DL_LIBS})
            target_include_directories(test_vapoursynth SYSTEM PRIVATE
                "${PROJECT_SOURCE_DIR}/third_party/vapoursynth/include")
            target_compile_definitions(test_vapoursynth PRIVATE
                HDRG_TEST_VS_PLUGIN="$<TARGET_FILE:hdrgrading_vs>")
            add_dependencies(test_vapoursynth hdrgrading_vs)

            # and under VapourSynth itself, where its Python module is installed (the dev image)
            find_package(Python3 COMPONENTS Interpreter QUIET)
            if(Python3_Interpreter_FOUND)
                execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import vapoursynth"
                    RESULT_VARIABLE HDRG_VAPOURSYNTH_PYTHON OUTPUT_QUIET ERROR_QUIET)
                if(HDRG_VAPOURSYNTH_PYTHON EQUAL 0)
                    add_test(NAME test_vapoursynth_core COMMAND "${Python3_EXECUTABLE}"
                        "${PROJECT_SOURCE_DIR}/tests/vapoursynth/test_core.py"
                        "$<TARGET_FILE:hdrgrading_vs>"
                        "$<IF:$<BOOL:${HDRG_TEMPORAL_FILTER}>,4,0>"
                        "$<IF:$<BOOL:${HDRG_TEMPORAL_FILTER}>,2,0>")
                endif()
            endif()
        endif()

        # a shared library exports the HDRG_API functions alone; the installed library builds
        # and runs a program through its CMake package and its pkg-config file
        if(BUILD_SHARED_LIBS AND NOT WIN32 AND CMAKE_NM)
            add_test(NAME test_exports COMMAND "${CMAKE_COMMAND}"
                "-DLIBRARY=$<TARGET_FILE:hdrgrading>"
                "-DEXPECTED=${CMAKE_CURRENT_BINARY_DIR}/hdrgrading.exports"
                "-DNM=${CMAKE_NM}" "-DAPPLE=${APPLE}"
                -P "${PROJECT_SOURCE_DIR}/tests/install/exports.cmake")
        endif()
        if(HDRG_INSTALL AND NOT EMSCRIPTEN AND NOT WIN32 AND HDRG_SANITIZER STREQUAL "" AND
           NOT HDRG_COVERAGE AND NOT HDRG_BUILD_FUZZERS)
            if(BUILD_SHARED_LIBS OR NOT HDRG_WITH_WEBGPU)
                set(hdrg_install_dawn "")
            else()
                set(hdrg_install_dawn "${Dawn_DIR}")
            endif()
            add_test(NAME test_install COMMAND "${CMAKE_COMMAND}"
                "-DBUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}" "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
                "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/test_install" "-DCONFIG=$<CONFIG>"
                "-DGENERATOR=${CMAKE_GENERATOR}" "-DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
                "-DC_COMPILER=${CMAKE_C_COMPILER}" "-DLIBDIR=${CMAKE_INSTALL_LIBDIR}"
                "-DSHARED=${BUILD_SHARED_LIBS}" "-DOSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}"
                "-DDAWN_DIR=${hdrg_install_dawn}" "-DPKG_CONFIG=${PKG_CONFIG_EXECUTABLE}"
                -P "${PROJECT_SOURCE_DIR}/tests/install/run.cmake")
        endif()

        # hdrg-video's sidecars: HDR10+'s JSON and ST 2094-50's, frame by frame
        if(HDRG_BUILD_TOOLS AND NOT EMSCRIPTEN AND NOT WIN32)
            add_test(NAME test_video_sidecar COMMAND "${CMAKE_COMMAND}"
                "-DVIDEO=$<TARGET_FILE:hdrg-video>"
                "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/test_video_sidecar"
                -P "${PROJECT_SOURCE_DIR}/tests/tools/sidecar.cmake")
        endif()

        # test plugin modules: built like external plugins, from the public headers only
        if(NOT EMSCRIPTEN)
            foreach(plugin IN ITEMS plugin_test plugin_newer plugin_older)
                add_library(hdrg_${plugin} MODULE tests/plugins/${plugin}.c)
                target_include_directories(hdrg_${plugin} PRIVATE
                    "${PROJECT_SOURCE_DIR}/include" "${CMAKE_CURRENT_BINARY_DIR}/include")
                set_target_properties(hdrg_${plugin} PROPERTIES
                    PREFIX "" C_VISIBILITY_PRESET hidden)
                hdrg_target_warnings(hdrg_${plugin})
                add_dependencies(test_plugin hdrg_${plugin})
            endforeach()
            target_compile_definitions(test_plugin PRIVATE
                HDRG_TEST_PLUGIN="$<TARGET_FILE:hdrg_plugin_test>"
                HDRG_TEST_PLUGIN_NEWER="$<TARGET_FILE:hdrg_plugin_newer>"
                HDRG_TEST_PLUGIN_OLDER="$<TARGET_FILE:hdrg_plugin_older>")
        endif()
    endif()
endif()

# the browser module: hdrgrading-<simd128|scalar>-<st|mt|webgpu>.mjs + .wasm
if(EMSCRIPTEN)
    if(HDRG_WITH_SIMD)
        set(HDRG_WASM_FLAVOR simd128)
    else()
        set(HDRG_WASM_FLAVOR scalar)
    endif()
    if(HDRG_WASM_THREADS)
        string(APPEND HDRG_WASM_FLAVOR -mt)
    elseif(HDRG_WASM_WEBGPU)
        string(APPEND HDRG_WASM_FLAVOR -webgpu)
    else()
        string(APPEND HDRG_WASM_FLAVOR -st)
    endif()
    add_executable(hdrg_wasm src/wasm/hdrg_wasm.c)
    target_link_libraries(hdrg_wasm PRIVATE hdrgrading)
    hdrg_target_warnings(hdrg_wasm)
    set_target_properties(hdrg_wasm PROPERTIES OUTPUT_NAME "hdrgrading-${HDRG_WASM_FLAVOR}" SUFFIX ".mjs")
    target_link_options(hdrg_wasm PRIVATE
        -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createHdrgModule
        -sENVIRONMENT=web,worker,node -sFILESYSTEM=0
        -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB -sSTACK_SIZE=1MB
        "-sEXPORTED_FUNCTIONS=_malloc,_free,_hdrg_wasm_context_create,_hdrg_context_destroy,\
_hdrg_wasm_params_create,_hdrg_wasm_params_set,_hdrg_wasm_params_format,\
_hdrg_wasm_params_format_option,_hdrg_wasm_params_destroy,_hdrg_wasm_convert,\
_hdrg_wasm_output_status,_hdrg_wasm_output_data,_hdrg_wasm_output_size,_hdrg_wasm_output_free,\
_hdrg_wasm_last_tier,_hdrg_wasm_preview_create,_hdrg_wasm_preview_status,_hdrg_wasm_preview_state,\
_hdrg_wasm_preview_destroy,_hdrg_wasm_preview_shader,_hdrg_version,_hdrg_status_string,_hdrg_simd_name"
        "-sEXPORTED_RUNTIME_METHODS=HEAPU8,UTF8ToString,stringToNewUTF8")
    # the conversion waits for the GPU: a Promise in JS (await it in every build)
    if(HDRG_WASM_WEBGPU)
        target_link_options(hdrg_wasm PRIVATE -sJSPI_EXPORTS=hdrg_wasm_convert)
        set_target_properties(hdrg_wasm PROPERTIES LINKER_LANGUAGE CXX)
    endif()
endif()

if(HDRG_BUILD_FUZZERS)
    add_executable(fuzz_convert tests/fuzz/fuzz_convert.c)
    target_link_options(fuzz_convert PRIVATE -fsanitize=fuzzer)
    target_link_libraries(fuzz_convert PRIVATE hdrgrading)
    hdrg_target_warnings(fuzz_convert)
endif()

# the seed corpus (fuzz_seeds DIR): small JPEG / PNG / GIF files over the decoders'
# branches, the fuzzer's and test_seeds' inputs
hdrg_builtins_have(hdrg_has_jpeg jpeg)
if(hdrg_has_jpeg AND (HDRG_BUILD_FUZZERS OR HDRG_BUILD_TESTS) AND NOT EMSCRIPTEN)
    add_executable(fuzz_seeds tests/fuzz/fuzz_seeds.c)
    target_include_directories(fuzz_seeds PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_compile_definitions(fuzz_seeds PRIVATE ${HDRG_BUILTIN_DEFINITIONS})
    target_link_libraries(fuzz_seeds PRIVATE ${HDRG_INTERNAL})
    hdrg_dependency_headers(fuzz_seeds hdrg::jpegli)
    hdrg_target_warnings(fuzz_seeds)
    if(HDRG_WITH_LCMS2)
        target_compile_definitions(fuzz_seeds PRIVATE HDRG_WITH_LCMS2)
    endif()
endif()

# The site of docs/ (the docs, their top linking the demo and the coverage) in <build>/web, to try
# locally: cmake --build <build> --target hdrg_web. Not part of the default build; the site with the
# WASM demo is scripts/wasm_demo.sh's (build/web), GitHub Pages' is .github/workflows/pages.yml's
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_custom_target(hdrg_web
        COMMAND "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/docs/scripts/build.py" --strict
                --out "${CMAKE_CURRENT_BINARY_DIR}/web" --site
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        COMMENT "The docs' site in ${CMAKE_CURRENT_BINARY_DIR}/web"
        VERBATIM)
endif()
