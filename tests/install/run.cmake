# SPDX-License-Identifier: Apache-2.0 OR MIT
# test_install: installs the build into WORK_DIR/prefix and builds tests/install/consumer.c from it,
# through the CMake package (tests/install/CMakeLists.txt) and through pkg-config (the compiler
# with pkg-config's flags, --static for a static library), and runs both programs; and builds a
# plugin's project (src/plugins/deband) apart against the package, into a loadable module.
#
#   cmake -DBUILD_DIR= -DSOURCE_DIR= -DWORK_DIR= -DCONFIG= -DGENERATOR= -DMAKE_PROGRAM=
#         -DC_COMPILER= -DLIBDIR= -DSHARED= -DOSX_ARCHITECTURES= -DDAWN_DIR= -DPKG_CONFIG=
#         -P tests/install/run.cmake
cmake_minimum_required(VERSION 3.29)

function(run_step name)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${name} failed (${result}):\n${output}")
    endif()
    message(STATUS "${name}: ok")
endfunction()

set(prefix "${WORK_DIR}/prefix")
file(REMOVE_RECURSE "${WORK_DIR}")
run_step(install "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${prefix}" --config "${CONFIG}")

# the CMake package
set(configure "${CMAKE_COMMAND}" -S "${SOURCE_DIR}/tests/install" -B "${WORK_DIR}/cmake"
    -G "${GENERATOR}" "-DCMAKE_BUILD_TYPE=${CONFIG}" "-DCMAKE_C_COMPILER=${C_COMPILER}"
    "-DCMAKE_PREFIX_PATH=${prefix}")
if(MAKE_PROGRAM)
    list(APPEND configure "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}")
endif()
if(OSX_ARCHITECTURES)
    list(APPEND configure "-DCMAKE_OSX_ARCHITECTURES=${OSX_ARCHITECTURES}")
endif()
if(DAWN_DIR)
    list(APPEND configure "-DDawn_DIR=${DAWN_DIR}")
endif()
run_step("configure (CMake package)" ${configure})
run_step("build (CMake package)" "${CMAKE_COMMAND}" --build "${WORK_DIR}/cmake" --config "${CONFIG}")
run_step("run (CMake package)" "${WORK_DIR}/cmake/consumer")

# a plugin's project built apart against the installed package (its GPU parts too): a module
string(REPLACE "/tests/install" "/src/plugins/deband" plugin_configure "${configure}")
string(REPLACE "${WORK_DIR}/cmake" "${WORK_DIR}/plugin" plugin_configure "${plugin_configure}")
run_step("configure (plugin project)" ${plugin_configure})
run_step("build (plugin project)" "${CMAKE_COMMAND}" --build "${WORK_DIR}/plugin" --config "${CONFIG}")
file(GLOB_RECURSE plugin_module "${WORK_DIR}/plugin/hdrgrading-deband.*")
if(NOT plugin_module)
    message(FATAL_ERROR "the plugin project built no module hdrgrading-deband")
endif()
message(STATUS "module (plugin project): ${plugin_module}")

# pkg-config: its flags straight to the compiler
if(NOT PKG_CONFIG)
    message(STATUS "pkg-config: not found, skipped")
    return()
endif()
set(ENV{PKG_CONFIG_PATH} "${prefix}/${LIBDIR}/pkgconfig")
unset(ENV{PKG_CONFIG_LIBDIR})
set(static "")
if(NOT SHARED)
    set(static --static)
endif()
execute_process(COMMAND "${PKG_CONFIG}" --cflags --libs ${static} hdrgrading
    RESULT_VARIABLE result OUTPUT_VARIABLE flags ERROR_VARIABLE error OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "pkg-config failed:\n${error}")
endif()
separate_arguments(flags UNIX_COMMAND "${flags}")
set(extra "")
foreach(arch IN LISTS OSX_ARCHITECTURES)
    list(APPEND extra -arch "${arch}")
endforeach()
if(SHARED)
    list(APPEND extra "-Wl,-rpath,${prefix}/${LIBDIR}")
endif()
run_step("build (pkg-config)" "${C_COMPILER}" ${extra} "${SOURCE_DIR}/tests/install/consumer.c"
    -o "${WORK_DIR}/consumer-pc" ${flags})
run_step("run (pkg-config)" "${WORK_DIR}/consumer-pc")
