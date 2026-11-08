# SPDX-License-Identifier: Apache-2.0 OR MIT
# Builds the dependencies of what the library has from the third_party/ submodules (no prebuilt
# step): lcms2 for the core's ICC (HDRG_WITH_LCMS2), and each codec family's (jpeg: jpegli and
# Highway, png: libpng and zlib, gif: giflib, jxl: libjxl, brotli and Highway) where it is built in
# (HDRG_PLUGINS_EXCLUDE). Targets: hdrg::lcms2, hdrg::jpegli, hdrg::png, hdrg::gif, hdrg::jxl. The
# bundled ones are object libraries whose objects go into libhdrgrading itself
# (HDRG_BUNDLED_OBJECTS): the installed library is one file, and a shared one exports none of their
# symbols. HDRG_DEPS_LIBRARIES are what the library links instead (the system ones,
# HDRG_WITH_SYSTEM_DEPS); HDRG_BUNDLED_CXX: some of the bundled are C++.
include_guard(GLOBAL)

set(HDRG_TP_DIR "${PROJECT_SOURCE_DIR}/third_party")
set(HDRG_DEPS_BINARY_DIR "${PROJECT_BINARY_DIR}/hdrg_deps")

function(hdrg_require_dependency name marker)
    if(NOT EXISTS "${HDRG_TP_DIR}/${name}/${marker}")
        message(FATAL_ERROR "third_party/${name} is missing. Run scripts/fetch_deps.sh "
            "(git submodule update --init plus the nested jpegli dependencies).")
    endif()
endfunction()

function(hdrg_link_libm target)
    if(NOT HDRG_COMPILER_IS_MSVC_FRONTEND AND NOT EMSCRIPTEN)
        target_link_libraries(${target} PUBLIC m)
    endif()
endfunction()

hdrg_builtins_have(hdrg_deps_jpeg jpeg)
hdrg_builtins_have(hdrg_deps_png png)
hdrg_builtins_have(hdrg_deps_gif gif)
hdrg_builtins_have(hdrg_deps_jxl jxl)
set(HDRG_BUNDLED_OBJECTS "")
set(HDRG_DEPS_LIBRARIES "")
set(HDRG_BUNDLED_CXX OFF)
set(hdrg_c_deps "")
if(hdrg_deps_jpeg OR hdrg_deps_jxl)
    set(HDRG_BUNDLED_CXX ON)
endif()

if(HDRG_WITH_SYSTEM_DEPS)
    # the library links them; lcms2 by its path (an exported static library names it as is)
    if(hdrg_deps_png)
        find_package(ZLIB REQUIRED)
        find_package(PNG REQUIRED)
        add_library(hdrg::png ALIAS PNG::PNG)
        list(APPEND HDRG_DEPS_LIBRARIES ZLIB::ZLIB PNG::PNG)
    endif()
    if(hdrg_deps_gif)
        find_package(GIF REQUIRED)
        add_library(hdrg::gif ALIAS GIF::GIF)
        list(APPEND HDRG_DEPS_LIBRARIES GIF::GIF)
    endif()
    if(HDRG_WITH_LCMS2)
        find_library(HDRG_LCMS2_LIBRARY NAMES lcms2 REQUIRED)
        find_path(HDRG_LCMS2_INCLUDE_DIR NAMES lcms2.h REQUIRED)
        add_library(hdrg_lcms2 INTERFACE)
        target_include_directories(hdrg_lcms2 SYSTEM INTERFACE "${HDRG_LCMS2_INCLUDE_DIR}")
        add_library(hdrg::lcms2 ALIAS hdrg_lcms2)
        list(APPEND HDRG_DEPS_LIBRARIES "${HDRG_LCMS2_LIBRARY}")
    endif()
endif()

if(NOT HDRG_WITH_SYSTEM_DEPS AND hdrg_deps_png)
    hdrg_require_dependency(zlib zlib.h)
    hdrg_require_dependency(libpng png.h)

    # zlib: only the inflate/deflate core (the gz* file API is not used)
    add_library(hdrg_zlib OBJECT EXCLUDE_FROM_ALL
        "${HDRG_TP_DIR}/zlib/adler32.c"
        "${HDRG_TP_DIR}/zlib/compress.c"
        "${HDRG_TP_DIR}/zlib/crc32.c"
        "${HDRG_TP_DIR}/zlib/deflate.c"
        "${HDRG_TP_DIR}/zlib/infback.c"
        "${HDRG_TP_DIR}/zlib/inffast.c"
        "${HDRG_TP_DIR}/zlib/inflate.c"
        "${HDRG_TP_DIR}/zlib/inftrees.c"
        "${HDRG_TP_DIR}/zlib/trees.c"
        "${HDRG_TP_DIR}/zlib/uncompr.c"
        "${HDRG_TP_DIR}/zlib/zutil.c")
    target_include_directories(hdrg_zlib SYSTEM PUBLIC "${HDRG_TP_DIR}/zlib")
    add_library(hdrg::zlib ALIAS hdrg_zlib)

    # libpng: prebuilt pnglibconf.h, architecture-specific filter code disabled
    configure_file("${HDRG_TP_DIR}/libpng/scripts/pnglibconf.h.prebuilt"
        "${HDRG_DEPS_BINARY_DIR}/libpng/pnglibconf.h" COPYONLY)
    add_library(hdrg_png OBJECT EXCLUDE_FROM_ALL
        "${HDRG_TP_DIR}/libpng/png.c"
        "${HDRG_TP_DIR}/libpng/pngerror.c"
        "${HDRG_TP_DIR}/libpng/pngget.c"
        "${HDRG_TP_DIR}/libpng/pngmem.c"
        "${HDRG_TP_DIR}/libpng/pngpread.c"
        "${HDRG_TP_DIR}/libpng/pngread.c"
        "${HDRG_TP_DIR}/libpng/pngrio.c"
        "${HDRG_TP_DIR}/libpng/pngrtran.c"
        "${HDRG_TP_DIR}/libpng/pngrutil.c"
        "${HDRG_TP_DIR}/libpng/pngset.c"
        "${HDRG_TP_DIR}/libpng/pngtrans.c"
        "${HDRG_TP_DIR}/libpng/pngwio.c"
        "${HDRG_TP_DIR}/libpng/pngwrite.c"
        "${HDRG_TP_DIR}/libpng/pngwtran.c"
        "${HDRG_TP_DIR}/libpng/pngwutil.c")
    target_include_directories(hdrg_png SYSTEM PUBLIC
        "${HDRG_TP_DIR}/libpng" "${HDRG_DEPS_BINARY_DIR}/libpng")
    target_compile_definitions(hdrg_png PRIVATE PNG_ARM_NEON_OPT=0)
    target_link_libraries(hdrg_png PUBLIC hdrg_zlib)
    hdrg_link_libm(hdrg_png)
    add_library(hdrg::png ALIAS hdrg_png)
    list(APPEND hdrg_c_deps hdrg_zlib hdrg_png)
    list(APPEND HDRG_BUNDLED_OBJECTS $<TARGET_OBJECTS:hdrg_zlib> $<TARGET_OBJECTS:hdrg_png>)
endif()

if(NOT HDRG_WITH_SYSTEM_DEPS AND hdrg_deps_gif)
    hdrg_require_dependency(giflib gif_lib.h)
    # the decoder only: the library reads GIF and never writes it (the encoder, the font and the
    # quantizer are left out; gif_font.c needs POSIX's strtok_r, which Windows does not have)
    add_library(hdrg_gif OBJECT EXCLUDE_FROM_ALL
        "${HDRG_TP_DIR}/giflib/dgif_lib.c"
        "${HDRG_TP_DIR}/giflib/gif_err.c"
        "${HDRG_TP_DIR}/giflib/gifalloc.c"
        "${HDRG_TP_DIR}/giflib/openbsd-reallocarray.c")
    target_include_directories(hdrg_gif SYSTEM PUBLIC "${HDRG_TP_DIR}/giflib")
    add_library(hdrg::gif ALIAS hdrg_gif)
    list(APPEND hdrg_c_deps hdrg_gif)
    list(APPEND HDRG_BUNDLED_OBJECTS $<TARGET_OBJECTS:hdrg_gif>)
endif()

if(NOT HDRG_WITH_SYSTEM_DEPS AND HDRG_WITH_LCMS2)
    hdrg_require_dependency(lcms2 include/lcms2.h)
    file(GLOB hdrg_lcms2_sources CONFIGURE_DEPENDS "${HDRG_TP_DIR}/lcms2/src/*.c")
    add_library(hdrg_lcms2 OBJECT EXCLUDE_FROM_ALL ${hdrg_lcms2_sources})
    target_include_directories(hdrg_lcms2 SYSTEM PUBLIC "${HDRG_TP_DIR}/lcms2/include")
    hdrg_link_libm(hdrg_lcms2)
    add_library(hdrg::lcms2 ALIAS hdrg_lcms2)
    list(APPEND hdrg_c_deps hdrg_lcms2)
    list(APPEND HDRG_BUNDLED_OBJECTS $<TARGET_OBJECTS:hdrg_lcms2>)
endif()
# Upstreams build as gnu99/gnu11; strict ISO mode hides POSIX declarations (fdopen) on glibc.
if(hdrg_c_deps)
    set_target_properties(${hdrg_c_deps} PROPERTIES C_EXTENSIONS ON)
endif()

# Highway (jpegli's own pin) for jpegli and libjxl
set(HDRG_JPEGLI_DIR "${HDRG_TP_DIR}/jpegli")
if(hdrg_deps_jpeg OR hdrg_deps_jxl)
    hdrg_require_dependency(jpegli third_party/highway/CMakeLists.txt)
    block()
        set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
        set(BUILD_SHARED_LIBS OFF)
        set(HWY_FORCE_STATIC_LIBS ON CACHE INTERNAL "")
        set(HWY_ENABLE_CONTRIB OFF CACHE INTERNAL "")
        set(HWY_ENABLE_EXAMPLES OFF CACHE INTERNAL "")
        set(HWY_ENABLE_INSTALL OFF CACHE INTERNAL "")
        set(HWY_ENABLE_TESTS OFF CACHE INTERNAL "")
        add_subdirectory("${HDRG_JPEGLI_DIR}/third_party/highway" "${HDRG_DEPS_BINARY_DIR}/highway"
            EXCLUDE_FROM_ALL SYSTEM)
    endblock()
    # jpegli's default target set (its CMakeLists.txt JPEGLI_HWY_TARGETS_OFF_BY_DEFAULT)
    target_compile_definitions(hwy PUBLIC
        "HWY_DISABLED_TARGETS=(HWY_AVX3|HWY_AVX3_SPR|HWY_AVX3_ZEN4|HWY_RVV|HWY_SSSE3|HWY_SVE_256)")
    # a static library of its own CMake: its objects go in as the others' do
    list(APPEND HDRG_BUNDLED_OBJECTS $<TARGET_OBJECTS:hwy>)
endif()

# jpegli is always the bundled, commit-pinned build. Only its library sources are compiled (from
# jpegli's own source lists); jpegli's top-level CMake would pull in lcms/skcms, CTest and tools.
if(hdrg_deps_jpeg)
    hdrg_require_dependency(jpegli lib/jpegli/encode.h)
    hdrg_require_dependency(jpegli third_party/libjpeg-turbo/jconfig.h.in)
    include("${HDRG_JPEGLI_DIR}/lib/jpegli_lists.cmake")
    set(hdrg_jpegli_sources ${JPEGLI_INTERNAL_JPEGLI_SOURCES} ${JPEGLI_INTERNAL_JPEGLI_WRAPPER_SOURCES})
    list(TRANSFORM hdrg_jpegli_sources PREPEND "${HDRG_JPEGLI_DIR}/lib/")

    set(hdrg_jpegli_include "${HDRG_DEPS_BINARY_DIR}/jpegli/include")
    set(JPEG_LIB_VERSION 62)
    set(BITS_IN_JSAMPLE 8)
    set(MEM_SRCDST_SUPPORTED 1)
    configure_file("${HDRG_JPEGLI_DIR}/third_party/libjpeg-turbo/jconfig.h.in"
        "${hdrg_jpegli_include}/jconfig.h")
    foreach(header IN ITEMS jpeglib.h jmorecfg.h jerror.h)
        configure_file("${HDRG_JPEGLI_DIR}/third_party/libjpeg-turbo/${header}"
            "${hdrg_jpegli_include}/${header}" COPYONLY)
    endforeach()

    find_package(Threads REQUIRED)
    add_library(hdrg_jpegli OBJECT EXCLUDE_FROM_ALL ${hdrg_jpegli_sources})
    target_compile_features(hdrg_jpegli PRIVATE cxx_std_17)
    target_include_directories(hdrg_jpegli PRIVATE "${HDRG_JPEGLI_DIR}")
    target_include_directories(hdrg_jpegli SYSTEM PUBLIC "${hdrg_jpegli_include}")
    # The native jpegli headers include <cstddef>/<cstdio> even for C consumers; jpegli_compat (beside
    # the JPEG plugin) maps them to the C headers.
    target_include_directories(hdrg_jpegli SYSTEM INTERFACE
        "${HDRG_JPEGLI_DIR}" "${PROJECT_SOURCE_DIR}/src/plugins/jpeg/jpegli_compat")
    target_link_libraries(hdrg_jpegli PRIVATE hwy Threads::Threads)
    add_library(hdrg::jpegli ALIAS hdrg_jpegli)
    list(APPEND HDRG_BUNDLED_OBJECTS $<TARGET_OBJECTS:hdrg_jpegli>)
endif()

# JPEG XL (the jxl family): the bundled libjxl (its tag pinned, write-only in the library: the
# encoder, the decoder for the tests and hdrg-inspect, its CMS over the bundled lcms2) and brotli
# (its boxes' compression). Only their library sources are compiled (libjxl's own source lists):
# libjxl's top-level CMake would pull in its own third_party (skcms, sjpeg, googletest) and tools.
# Highway is jpegli's. Their objects go into the library as jpegli's do.
if(hdrg_deps_jxl)
    hdrg_require_dependency(libjxl lib/jxl_lists.cmake)
    hdrg_require_dependency(brotli c/include/brotli/encode.h)

    set(HDRG_BROTLI_DIR "${HDRG_TP_DIR}/brotli")
    file(GLOB hdrg_brotli_sources "${HDRG_BROTLI_DIR}/c/common/*.c" "${HDRG_BROTLI_DIR}/c/enc/*.c"
        "${HDRG_BROTLI_DIR}/c/dec/*.c")
    add_library(hdrg_brotli OBJECT EXCLUDE_FROM_ALL ${hdrg_brotli_sources})
    target_include_directories(hdrg_brotli SYSTEM PUBLIC "${HDRG_BROTLI_DIR}/c/include")
    hdrg_link_libm(hdrg_brotli)

    set(HDRG_JXL_DIR "${HDRG_TP_DIR}/libjxl/lib")
    include("${HDRG_JXL_DIR}/jxl_lists.cmake")
    # the gain map bundle (the jhgm box) is of its extras, on the library's own parts alone
    set(hdrg_jxl_sources ${JPEGXL_INTERNAL_DEC_SOURCES} ${JPEGXL_INTERNAL_DEC_BOX_SOURCES}
        ${JPEGXL_INTERNAL_ENC_SOURCES} ${JPEGXL_INTERNAL_CMS_SOURCES} extras/gain_map.cc)
    list(FILTER hdrg_jxl_sources INCLUDE REGEX "\\.cc$")
    list(TRANSFORM hdrg_jxl_sources PREPEND "${HDRG_JXL_DIR}/")

    # the headers libjxl's CMake generates: its version, and no exports (a static build inside
    # the library, its symbols hidden)
    set(hdrg_jxl_include "${HDRG_DEPS_BINARY_DIR}/libjxl/include")
    file(STRINGS "${HDRG_JXL_DIR}/CMakeLists.txt" hdrg_jxl_versions
        REGEX "^set\\(JPEGXL_(MAJOR|MINOR|PATCH)_VERSION [0-9]+\\)")
    foreach(line IN LISTS hdrg_jxl_versions)
        string(REGEX REPLACE "^set\\((JPEGXL_[A-Z]+_VERSION) ([0-9]+)\\)$" "\\1;\\2" pair "${line}")
        list(GET pair 0 name)
        list(GET pair 1 value)
        set(${name} ${value})
    endforeach()
    configure_file("${HDRG_JXL_DIR}/jxl/version.h.in" "${hdrg_jxl_include}/jxl/version.h")
    foreach(prefix IN ITEMS JXL JXL_CMS JXL_THREADS)
        string(TOLOWER "${prefix}" lower)
        file(CONFIGURE OUTPUT "${hdrg_jxl_include}/jxl/${lower}_export.h" CONTENT
            "/* generated: libjxl's ${prefix} exports, none (bundled in libhdrgrading) */\n#ifndef ${prefix}_EXPORT_H\n#define ${prefix}_EXPORT_H\n#define ${prefix}_EXPORT\n#define ${prefix}_NO_EXPORT\n#define ${prefix}_DEPRECATED\n#endif\n")
    endforeach()

    add_library(hdrg_jxl OBJECT EXCLUDE_FROM_ALL ${hdrg_jxl_sources})
    target_compile_features(hdrg_jxl PRIVATE cxx_std_17)
    target_compile_definitions(hdrg_jxl PRIVATE JXL_INTERNAL_LIBRARY_BUILD
        JPEGXL_ENABLE_TRANSCODE_JPEG=0 JPEGXL_ENABLE_BOXES=1 JPEGXL_ENABLE_SKCMS=0)
    target_compile_definitions(hdrg_jxl PUBLIC JXL_STATIC_DEFINE JXL_CMS_STATIC_DEFINE)
    target_include_directories(hdrg_jxl PRIVATE "${HDRG_TP_DIR}/libjxl")
    target_include_directories(hdrg_jxl SYSTEM PUBLIC "${hdrg_jxl_include}"
        "${HDRG_JXL_DIR}/include")
    target_link_libraries(hdrg_jxl PRIVATE hwy hdrg_brotli hdrg::lcms2 Threads::Threads)
    add_library(hdrg::jxl ALIAS hdrg_jxl)
    list(APPEND HDRG_BUNDLED_OBJECTS $<TARGET_OBJECTS:hdrg_jxl> $<TARGET_OBJECTS:hdrg_brotli>)
endif()
