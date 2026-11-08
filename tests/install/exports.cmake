# SPDX-License-Identifier: Apache-2.0 OR MIT
# test_exports: a shared libhdrgrading exports exactly the HDRG_API functions of the public headers
# (EXPECTED, the list CMakeLists.txt reads from them), nothing of the library's internals nor of the
# bundled dependencies.
#
#   cmake -DLIBRARY= -DEXPECTED= -DNM= -DAPPLE= -P tests/install/exports.cmake
cmake_minimum_required(VERSION 3.29)

if(APPLE)
    set(arguments -g -U -j "${LIBRARY}")
else()
    set(arguments -D -g --defined-only "${LIBRARY}")
endif()
execute_process(COMMAND "${NM}" ${arguments} RESULT_VARIABLE result OUTPUT_VARIABLE output
    ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "${NM} failed:\n${error}")
endif()

string(REPLACE "\n" ";" lines "${output}")
set(exported "")
foreach(line IN LISTS lines)
    if(NOT line MATCHES "([^ ]+)$")
        continue()
    endif()
    set(symbol "${CMAKE_MATCH_1}")
    if(APPLE)
        string(REGEX REPLACE "^_" "" symbol "${symbol}")
    endif()
    # the linker's own (ELF), not the library's
    if(symbol MATCHES "^(_init|_fini|_edata|_end|__bss_start)$")
        continue()
    endif()
    list(APPEND exported "${symbol}")
endforeach()

file(STRINGS "${EXPECTED}" expected)
list(SORT exported)
list(REMOVE_DUPLICATES exported)
list(SORT expected)
set(missing "")
foreach(symbol IN LISTS expected)
    if(NOT symbol IN_LIST exported)
        list(APPEND missing "${symbol}")
    endif()
endforeach()
set(extra "")
foreach(symbol IN LISTS exported)
    if(NOT symbol IN_LIST expected)
        list(APPEND extra "${symbol}")
    endif()
endforeach()
list(LENGTH expected expected_count)
if(missing OR extra)
    list(JOIN missing "\n  " missing)
    list(JOIN extra "\n  " extra)
    message(FATAL_ERROR "${LIBRARY}: the exports differ from the ${expected_count} HDRG_API "
        "functions\nmissing:\n  ${missing}\nextra:\n  ${extra}")
endif()
message(STATUS "${LIBRARY}: exports the ${expected_count} HDRG_API functions alone")
