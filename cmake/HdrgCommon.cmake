# SPDX-License-Identifier: Apache-2.0 OR MIT
# Build helpers shared by every libhdrgrading target (modeled on zendriver's ZenCommon.cmake).
include_guard(GLOBAL)

# errors in the project's own build, not where another project builds it as a part
option(HDRG_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ${PROJECT_IS_TOP_LEVEL})
option(HDRG_COVERAGE "Instrument the project's own targets for Clang source-based coverage" OFF)
option(HDRG_BUILD_FUZZERS "Build libFuzzer targets (instruments all targets, third_party too)" OFF)
set(HDRG_SANITIZER "" CACHE STRING "Sanitizer build: empty, address-undefined, thread or memory")
set_property(CACHE HDRG_SANITIZER PROPERTY STRINGS "" address-undefined thread memory)

if(CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    set(HDRG_COMPILER_IS_MSVC_FRONTEND TRUE)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    set(HDRG_COMPILER_IS_MSVC_FRONTEND FALSE)
endif()
if(HDRG_COMPILER_IS_MSVC_FRONTEND AND CMAKE_C_COMPILER_ID STREQUAL "Clang")
    set(HDRG_COMPILER_IS_CLANG_CL TRUE)
else()
    set(HDRG_COMPILER_IS_CLANG_CL FALSE)
endif()

# 32-bit x86 with GCC or Clang (Linux's i686 / i586 targets): float and double in SSE2, not the
# x87's extended precision, for every target (third_party too): the results are the IEEE
# operations', as on x86-64 and with MSVC / clang-cl (/arch:SSE2 by default). Such a
# build needs an SSE2 CPU.
include(CheckCSourceCompiles)
check_c_source_compiles("
#if !defined(__i386__) || defined(__SSE2_MATH__)
#error no x87 math
#endif
int main(void) { return 0; }" HDRG_TARGET_X87_MATH)
if(HDRG_TARGET_X87_MATH)
    add_compile_options(-msse2 -mfpmath=sse)
endif()

# musl (Alpine): a thread's default stack is 128 KiB. The programs here (tests, tools) take glibc's
# 8 MiB, from PT_GNU_STACK (-z stack-size), for the threads they do not create themselves: on arm64,
# lavapipe's LLVM overflows 128 KiB compiling a shader in its own threads. The library's threads ask
# for theirs (src/foundation/thread.c).
check_c_source_compiles("
#include <stdio.h>
#if !defined(__linux__) || defined(__GLIBC__) || defined(__ANDROID__)
#error not musl
#endif
int main(void) { return 0; }" HDRG_TARGET_MUSL)
if(HDRG_TARGET_MUSL)
    string(APPEND CMAKE_EXE_LINKER_FLAGS " -Wl,-z,stack-size=8388608")
endif()

function(hdrg_apply_sanitizers)
    if(HDRG_BUILD_FUZZERS)
        if(HDRG_COMPILER_IS_MSVC_FRONTEND OR NOT CMAKE_C_COMPILER_ID MATCHES "Clang" OR
           NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            message(FATAL_ERROR "HDRG_BUILD_FUZZERS needs Clang (libFuzzer) for C and C++, but the "
                "compilers are ${CMAKE_C_COMPILER_ID} / ${CMAKE_CXX_COMPILER_ID}")
        endif()
        # the sanitizer's runtime also defines the coverage callbacks, which fuzz_seeds (no
        # libFuzzer) needs
        if(HDRG_SANITIZER STREQUAL "")
            message(FATAL_ERROR "HDRG_BUILD_FUZZERS needs HDRG_SANITIZER (the fuzz preset uses "
                "address-undefined)")
        endif()
        # libFuzzer's feedback is the coverage of every object the harness runs: the library and
        # the decoders it calls, not the harness alone
        add_compile_options(-fsanitize=fuzzer-no-link)
    endif()
    if(HDRG_SANITIZER STREQUAL "")
        return()
    endif()
    if(HDRG_COMPILER_IS_MSVC_FRONTEND)
        message(FATAL_ERROR "HDRG_SANITIZER needs a GNU-style compiler driver (GCC or Clang)")
    endif()
    if(HDRG_SANITIZER STREQUAL "address-undefined")
        set(flags -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    elseif(HDRG_SANITIZER STREQUAL "thread")
        set(flags -fsanitize=thread -fno-omit-frame-pointer)
    elseif(HDRG_SANITIZER STREQUAL "memory")
        if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
            message(FATAL_ERROR "HDRG_SANITIZER=memory needs Clang (Linux), but the C compiler is "
                "${CMAKE_C_COMPILER_ID} (${CMAKE_C_COMPILER})")
        endif()
        set(flags -fsanitize=memory -fsanitize-memory-track-origins -fno-omit-frame-pointer)
    else()
        message(FATAL_ERROR "Unknown HDRG_SANITIZER value: ${HDRG_SANITIZER}")
    endif()
    add_compile_options(${flags})
    add_link_options(${flags})
endfunction()

function(hdrg_set_coverage_profiles directory profile_dir)
    get_property(tests DIRECTORY "${directory}" PROPERTY TESTS)
    foreach(test IN LISTS tests)
        set_property(TEST ${test} DIRECTORY "${directory}" APPEND PROPERTY ENVIRONMENT_MODIFICATION
            "LLVM_PROFILE_FILE=set:${profile_dir}/${test}-%p-%m.profraw")
    endforeach()
    get_property(subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(subdirectory IN LISTS subdirectories)
        hdrg_set_coverage_profiles("${subdirectory}" "${profile_dir}")
    endforeach()
endfunction()

function(hdrg_finish_coverage)
    hdrg_set_coverage_profiles("${CMAKE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/hdrg-coverage/profiles")
endfunction()

function(hdrg_target_coverage target)
    if(NOT HDRG_COVERAGE)
        return()
    endif()
    get_target_property(target_type ${target} TYPE)
    if(target_type STREQUAL "INTERFACE_LIBRARY")
        return()
    endif()
    if(target_type STREQUAL "STATIC_LIBRARY" OR target_type STREQUAL "OBJECT_LIBRARY")
        set(scope INTERFACE)
    else()
        set(scope PRIVATE)
    endif()
    # clang-cl: the objects' profile runtime given to link.exe by its file (it adds none itself)
    if(HDRG_COMPILER_IS_CLANG_CL)
        target_compile_options(${target} PRIVATE
            /clang:-fprofile-instr-generate /clang:-fcoverage-mapping)
        target_link_libraries(${target} ${scope} "${HDRG_COVERAGE_RUNTIME}")
    else()
        target_compile_options(${target} PRIVATE -fprofile-instr-generate -fcoverage-mapping)
        target_link_options(${target} ${scope} -fprofile-instr-generate)
    endif()
endfunction()

# Warnings (and coverage) for the project's own targets, with the determinism policy
# (hdrg_target_determinism, cmake/HdrgPlugin.cmake). third_party targets never get these.
function(hdrg_target_warnings target)
    hdrg_target_coverage(${target})
    hdrg_target_determinism(${target})
    if(HDRG_COMPILER_IS_CLANG_CL)
        target_compile_options(${target} PRIVATE /W4 -Wextra -Wpedantic -Wconversion -Wshadow)
        if(HDRG_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    elseif(HDRG_COMPILER_IS_MSVC_FRONTEND)
        target_compile_options(${target} PRIVATE /W4)
        if(HDRG_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
        if(HDRG_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

function(hdrg_add_unity unity_dir)
    if(TARGET unity)
        return()
    endif()
    add_library(unity STATIC EXCLUDE_FROM_ALL "${unity_dir}/src/unity.c")
    target_include_directories(unity SYSTEM PUBLIC "${unity_dir}/src")
    target_compile_definitions(unity PUBLIC UNITY_SUPPORT_64)
endfunction()

# where an installed target finds its libraries: a shared libhdrgrading in the library directory
# relative to its own (dir, relative to the prefix), and Dawn's library by its directory (where
# Dawn is inside the source tree, as scripts/build_dawn.sh puts it, INSTALL_RPATH_USE_LINK_PATH
# leaves it out)
function(hdrg_install_rpath target dir)
    if(WIN32)
        return()
    endif()
    set(rpath "")
    if(BUILD_SHARED_LIBS AND NOT target STREQUAL "hdrgrading")
        if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
            set(rpath "${CMAKE_INSTALL_LIBDIR}")
        else()
            file(RELATIVE_PATH relative "/${dir}" "/${CMAKE_INSTALL_LIBDIR}")
            if(APPLE)
                set(rpath "@loader_path")
            else()
                set(rpath "$ORIGIN")
            endif()
            if(NOT relative STREQUAL "")
                string(APPEND rpath "/${relative}")
            endif()
        endif()
    endif()
    if(HDRG_DAWN)
        list(APPEND rpath "$<TARGET_LINKER_FILE_DIR:dawn::webgpu_dawn>")
    endif()
    set_target_properties(${target} PROPERTIES INSTALL_RPATH "${rpath}")
endfunction()

# a bundled dependency's headers alone: its objects are in libhdrgrading (cmake/HdrgDeps.cmake)
function(hdrg_dependency_headers target dependency)
    target_include_directories(${target} SYSTEM PRIVATE
        "$<TARGET_PROPERTY:${dependency},INTERFACE_INCLUDE_DIRECTORIES>")
endfunction()

function(hdrg_add_test name)
    cmake_parse_arguments(ARG "" "" "SOURCES;LIBRARIES" ${ARGN})
    add_executable(${name} ${ARG_SOURCES})
    target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(${name} PRIVATE unity ${ARG_LIBRARIES})
    # the built-in plugins the library has (src/plugins/CMakeLists.txt)
    target_compile_definitions(${name} PRIVATE ${HDRG_BUILTIN_DEFINITIONS})
    if(NOT HDRG_COMPILER_IS_MSVC_FRONTEND)
        target_link_libraries(${name} PRIVATE m)
    endif()
    hdrg_target_warnings(${name})
    hdrg_wasm_node_program(${name})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

if(HDRG_COVERAGE)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "HDRG_COVERAGE needs Clang (its GNU-style driver, or clang-cl on Windows), "
            "but the C compiler is ${CMAKE_C_COMPILER_ID} (${CMAKE_C_COMPILER}). Configure with "
            "-DCMAKE_C_COMPILER=clang (the coverage preset) or clang-cl (coverage-msvc).")
    endif()
    # clang-cl's profile runtime for the target (-m32 in CMAKE_C_FLAGS: x86): clang_rt.profile-<arch>.lib
    # in the resource directory's lib/windows, or clang_rt.profile.lib in a per-target one
    if(HDRG_COMPILER_IS_CLANG_CL AND NOT HDRG_COVERAGE_RUNTIME)
        separate_arguments(hdrg_coverage_flags WINDOWS_COMMAND "${CMAKE_C_FLAGS}")
        execute_process(
            COMMAND "${CMAKE_C_COMPILER}" ${hdrg_coverage_flags} /clang:-print-runtime-dir
            OUTPUT_VARIABLE hdrg_coverage_dir OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE hdrg_coverage_result)
        string(TOLOWER "${CMAKE_C_COMPILER_ARCHITECTURE_ID}" hdrg_coverage_arch)
        if(hdrg_coverage_arch STREQUAL "x64")
            set(hdrg_coverage_arch x86_64)
        elseif(hdrg_coverage_arch STREQUAL "x86")
            set(hdrg_coverage_arch i386)
        elseif(hdrg_coverage_arch STREQUAL "arm64")
            set(hdrg_coverage_arch aarch64)
        endif()
        find_file(HDRG_COVERAGE_RUNTIME
            NAMES "clang_rt.profile-${hdrg_coverage_arch}.lib" clang_rt.profile.lib
            PATHS "${hdrg_coverage_dir}" NO_DEFAULT_PATH NO_CACHE)
        if(NOT hdrg_coverage_result EQUAL 0 OR NOT HDRG_COVERAGE_RUNTIME)
            message(FATAL_ERROR "HDRG_COVERAGE: clang-cl's profile runtime for "
                "${CMAKE_C_COMPILER_ARCHITECTURE_ID} is not in \"${hdrg_coverage_dir}\" "
                "(${CMAKE_C_COMPILER})")
        endif()
        message(STATUS "HDRG_COVERAGE: profile runtime ${HDRG_COVERAGE_RUNTIME}")
    endif()
    get_property(hdrg_coverage_scheduled GLOBAL PROPERTY HDRG_COVERAGE_SCHEDULED)
    if(NOT hdrg_coverage_scheduled)
        set_property(GLOBAL PROPERTY HDRG_COVERAGE_SCHEDULED TRUE)
        cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL hdrg_finish_coverage)
    endif()
endif()
