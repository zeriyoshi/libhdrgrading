# SPDX-License-Identifier: Apache-2.0 OR MIT
# HEIF (HDRG_WITH_HEIF, opt-in): libheif with its HEVC codecs for the HEIF writer module
# (src/plugins/heif, never linked into the library). libheif and the codecs are not bundled: the
# system's libheif (pkg-config) when found, else built at build time from pinned tags (libde265's
# decoder: the tests read the files back; x265's 10-bit encoder; libheif with them built in) as
# shared libraries under the build tree, linked dynamically (libheif and libde265 are LGPL-3.0;
# x265 is GPL-2.0: a module with them fetched is under the GPL's terms). Target: hdrg::heif.
include_guard(GLOBAL)

set(HDRG_HEIF_LIBHEIF_TAG v1.23.7 CACHE STRING "libheif's tag HDRG_WITH_HEIF builds when no system one")
set(HDRG_HEIF_LIBDE265_TAG v1.1.3 CACHE STRING "libde265's tag ...")
set(HDRG_HEIF_X265_TAG 4.2 CACHE STRING "x265's tag ...")

if(EMSCRIPTEN)
    message(FATAL_ERROR "HDRG_WITH_HEIF: no WASM build (libheif's codecs)")
endif()
if(HDRG_SANITIZER STREQUAL "memory")
    message(FATAL_ERROR "HDRG_WITH_HEIF: MSan needs every library instrumented (libheif's are not)")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(HDRG_SYSTEM_HEIF QUIET IMPORTED_TARGET GLOBAL libheif>=1.17)

if(HDRG_SYSTEM_HEIF_FOUND)
    message(STATUS "HDRG_WITH_HEIF: the system's libheif ${HDRG_SYSTEM_HEIF_VERSION}")
    add_library(hdrg::heif ALIAS PkgConfig::HDRG_SYSTEM_HEIF)
    set(HDRG_HEIF_FETCHED OFF)
    return()
endif()

message(STATUS "HDRG_WITH_HEIF: no system libheif; libde265 ${HDRG_HEIF_LIBDE265_TAG}, "
    "x265 ${HDRG_HEIF_X265_TAG} and libheif ${HDRG_HEIF_LIBHEIF_TAG} are fetched at build time")
include(ExternalProject)
set(HDRG_HEIF_FETCHED ON)
set(HDRG_HEIF_PREFIX "${PROJECT_BINARY_DIR}/hdrg_deps/heif")
file(MAKE_DIRECTORY "${HDRG_HEIF_PREFIX}/include" "${HDRG_HEIF_PREFIX}/lib")

# shared, Release, installed into one prefix whose libraries find each other (@rpath / $ORIGIN:
# through the initial cache, which no build tool expands)
if(APPLE)
    set(hdrg_heif_rpath "-DCMAKE_INSTALL_NAME_DIR:STRING=@rpath"
        "-DCMAKE_INSTALL_RPATH:STRING=@loader_path")
else()
    set(hdrg_heif_rpath "-DCMAKE_INSTALL_RPATH:STRING=$ORIGIN")
endif()
set(hdrg_heif_common
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_INSTALL_PREFIX=${HDRG_HEIF_PREFIX}
    -DCMAKE_INSTALL_LIBDIR=lib
    -DCMAKE_PREFIX_PATH=${HDRG_HEIF_PREFIX}
    -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
    -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
    -DBUILD_SHARED_LIBS=ON)
if(CMAKE_OSX_ARCHITECTURES)
    list(APPEND hdrg_heif_common "-DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}")
endif()

ExternalProject_Add(hdrg_libde265_ep
    GIT_REPOSITORY https://github.com/strukturag/libde265.git
    GIT_TAG ${HDRG_HEIF_LIBDE265_TAG}
    GIT_SHALLOW ON
    UPDATE_COMMAND ""
    CMAKE_CACHE_ARGS ${hdrg_heif_rpath}
    CMAKE_ARGS ${hdrg_heif_common} -DENABLE_SDL=OFF -DENABLE_DECODER=OFF -DENABLE_ENCODER=OFF
    EXCLUDE_FROM_ALL ON)

# x265's 10-bit library alone (Main10): libheif asks it for the bit depth of each image
find_program(HDRG_HEIF_NASM NAMES nasm)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$" AND NOT CMAKE_OSX_ARCHITECTURES MATCHES "arm64"
   AND NOT HDRG_HEIF_NASM)
    set(hdrg_x265_asm -DENABLE_ASSEMBLY=OFF)
else()
    set(hdrg_x265_asm "")
endif()
ExternalProject_Add(hdrg_x265_ep
    GIT_REPOSITORY https://bitbucket.org/multicoreware/x265_git.git
    GIT_TAG ${HDRG_HEIF_X265_TAG}
    GIT_SHALLOW ON
    UPDATE_COMMAND ""
    SOURCE_SUBDIR source
    CMAKE_CACHE_ARGS ${hdrg_heif_rpath}
    CMAKE_ARGS ${hdrg_heif_common} -DHIGH_BIT_DEPTH=ON -DENABLE_SHARED=ON -DENABLE_CLI=OFF
        -DENABLE_HDR10_PLUS=OFF ${hdrg_x265_asm}
    EXCLUDE_FROM_ALL ON)

ExternalProject_Add(hdrg_libheif_ep
    GIT_REPOSITORY https://github.com/strukturag/libheif.git
    GIT_TAG ${HDRG_HEIF_LIBHEIF_TAG}
    GIT_SHALLOW ON
    UPDATE_COMMAND ""
    DEPENDS hdrg_libde265_ep hdrg_x265_ep
    CMAKE_COMMAND ${CMAKE_COMMAND} -E env "PKG_CONFIG_PATH=${HDRG_HEIF_PREFIX}/lib/pkgconfig"
        ${CMAKE_COMMAND}
    CMAKE_CACHE_ARGS ${hdrg_heif_rpath}
    CMAKE_ARGS ${hdrg_heif_common}
        -DWITH_LIBDE265=ON -DWITH_X265=ON -DENABLE_PLUGIN_LOADING=OFF
        -DWITH_LIBDE265_PLUGIN=OFF -DWITH_X265_PLUGIN=OFF
        -DWITH_AOM_DECODER=OFF -DWITH_AOM_ENCODER=OFF -DWITH_DAV1D=OFF -DWITH_RAV1E=OFF
        -DWITH_SvtEnc=OFF -DWITH_X264=OFF -DWITH_OpenH264_DECODER=OFF -DWITH_KVAZAAR=OFF
        -DWITH_JPEG_DECODER=OFF -DWITH_JPEG_ENCODER=OFF -DWITH_OpenJPEG_DECODER=OFF
        -DWITH_OpenJPEG_ENCODER=OFF -DWITH_OPENJPH_ENCODER=OFF -DWITH_FFMPEG_DECODER=OFF
        -DWITH_VVDEC=OFF -DWITH_VVENC=OFF -DWITH_UVG266=OFF -DWITH_LIBSHARPYUV=OFF
        -DWITH_EXAMPLES=OFF -DWITH_GDK_PIXBUF=OFF -DBUILD_TESTING=OFF
        -DWITH_HEADER_COMPRESSION=OFF -DWITH_UNCOMPRESSED_CODEC=OFF
    # Ninja needs to know where the library comes from
    INSTALL_BYPRODUCTS
        "${HDRG_HEIF_PREFIX}/lib/${CMAKE_SHARED_LIBRARY_PREFIX}heif${CMAKE_SHARED_LIBRARY_SUFFIX}"
    EXCLUDE_FROM_ALL ON)

add_library(hdrg_heif SHARED IMPORTED GLOBAL)
set_target_properties(hdrg_heif PROPERTIES
    IMPORTED_LOCATION
        "${HDRG_HEIF_PREFIX}/lib/${CMAKE_SHARED_LIBRARY_PREFIX}heif${CMAKE_SHARED_LIBRARY_SUFFIX}"
    INTERFACE_INCLUDE_DIRECTORIES "${HDRG_HEIF_PREFIX}/include")
add_dependencies(hdrg_heif hdrg_libheif_ep)
add_library(hdrg::heif ALIAS hdrg_heif)
ExternalProject_Get_Property(hdrg_libheif_ep SOURCE_DIR)
set(HDRG_HEIF_LICENSES libheif "${SOURCE_DIR}/COPYING")
ExternalProject_Get_Property(hdrg_libde265_ep SOURCE_DIR)
list(APPEND HDRG_HEIF_LICENSES libde265 "${SOURCE_DIR}/COPYING")
ExternalProject_Get_Property(hdrg_x265_ep SOURCE_DIR)
list(APPEND HDRG_HEIF_LICENSES x265 "${SOURCE_DIR}/COPYING")
