# SPDX-License-Identifier: Apache-2.0 OR MIT
# The built-in plugins: each a CMake project of its own (src/plugins/<name>, cmake/HdrgPlugin.cmake)
# linked into the library from the start, its entry point registered by every context in this
# order (the filters, then the codecs: their readers probed before the app's). shared: what
# several of them build on. All are built in by default; HDRG_PLUGINS_EXCLUDE (resolved at the top:
# HDRG_PLUGINS_EXCLUDED) leaves optional ones out (the library runs without them: their defaults
# off, an explicit use HDRG_ERR_UNSUPPORTED, a file no reader takes HDRG_ERR_UNSUPPORTED_FORMAT);
# polyphase (the SCALE stage's Lanczos) always stays. The temporal features (HDRG_TEMPORAL_*) are
# classes of their families: stats (the statistics across a session's frames, a family of its
# own), interp's hdrg.interp.mc and edge's hdrg.scale.temporal
set(HDRG_PLUGINS_BUILTIN ON)

add_subdirectory(shared)
foreach(plugin IN ITEMS bilinear polyphase denoise deblock dering defringe deband mctf interp fsr nis
                        edge predict)
    if(NOT plugin IN_LIST HDRG_PLUGINS_EXCLUDED)
        add_subdirectory(${plugin})
    endif()
endforeach()
if(HDRG_TEMPORAL_FILTER)
    add_subdirectory(stats)
endif()
if(HDRG_WITH_METAL)
    add_subdirectory(metalfx)
endif()
# the codecs from here on (a host for reading alone registers these: hdrg_builtin_codecs_first)
get_property(hdrg_filters GLOBAL PROPERTY HDRG_BUILTIN_PLUGINS)
list(LENGTH hdrg_filters hdrg_filter_count)
set_property(GLOBAL PROPERTY HDRG_BUILTIN_CODECS_FIRST ${hdrg_filter_count})
foreach(plugin IN ITEMS jpeg png gif jxl)
    if(NOT plugin IN_LIST HDRG_PLUGINS_EXCLUDED)
        add_subdirectory(${plugin})
    endif()
endforeach()
# the codec modules: loadable, never built in
if(HDRG_WITH_AVIF)
    add_subdirectory(avif)
endif()
if(HDRG_WITH_HEIF)
    add_subdirectory(heif)
endif()
