/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_TOOLS_MODULES_H
#define HDRG_TOOLS_MODULES_H

/*
 * The codec modules (src/plugins/avif, src/plugins/heif) a tool loads from beside itself: a build
 * tree's (HDRG_MODULE_BUILD_REL from the program's directory) or an install's
 * (HDRG_MODULE_INSTALL_REL), by their names alone. The library itself searches nothing. On
 * Linux the tool defines _POSIX_C_SOURCE (readlink) before its first include.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

#include "hdrgrading/plugin.h"

#ifndef HDRG_MODULE_SUFFIX
#define HDRG_MODULE_SUFFIX ".so"
#endif
#ifndef HDRG_MODULE_BUILD_REL
#define HDRG_MODULE_BUILD_REL "modules"
#endif
#ifndef HDRG_MODULE_INSTALL_REL
#define HDRG_MODULE_INSTALL_REL "../lib/hdrgrading"
#endif

#define HDRG_TOOL_PATH 4096u

/* the running program's path into path (cap bytes) */
#if defined(_WIN32)
static inline bool hdrg_tool_exe(char *path, size_t cap)
{
    size_t length;

    length = (size_t)GetModuleFileNameA(NULL, path, (DWORD)cap);

    return length > 0 && length < cap;
}
#elif defined(__APPLE__)
static inline bool hdrg_tool_exe(char *path, size_t cap)
{
    uint32_t size;

    size = (uint32_t)cap;

    return _NSGetExecutablePath(path, &size) == 0;
}
#elif defined(__linux__)
static inline bool hdrg_tool_exe(char *path, size_t cap)
{
    long got;

    got = (long)readlink("/proc/self/exe", path, cap - 1u);
    if (got <= 0) {
        return false;
    }

    path[got] = '\0';

    return true;
}
#else
static inline bool hdrg_tool_exe(char *path, size_t cap)
{
    (void)path;
    (void)cap;

    return false;
}
#endif

/* the running program's directory into dir (cap bytes, with its trailing separator) */
static inline bool hdrg_tool_dir(char *dir, size_t cap)
{
    char *slash, *back;

    if (cap < 2u || !hdrg_tool_exe(dir, cap)) {
        return false;
    }

    slash = strrchr(dir, '/');
    back = strrchr(dir, '\\');
    slash = (back != NULL && (slash == NULL || back > slash)) ? back : slash;

    if (slash == NULL) {
        return false;
    }

    slash[1] = '\0';

    return true;
}

/*
 * every codec module there is beside the program onto ctx (each from the first place it is
 * in); a module there failing to load is reported on stderr
 */
static inline void hdrg_tool_load_modules(hdrg_context_t *ctx)
{
    static const char *const names[] = {"hdrgrading-avif", "hdrgrading-heif"},
                             *const places[] = {HDRG_MODULE_BUILD_REL, HDRG_MODULE_INSTALL_REL};
    FILE *fp;
    hdrg_status_t status;
    int written;
    size_t n, p;
    char dir[HDRG_TOOL_PATH], path[HDRG_TOOL_PATH];

    if (!hdrg_tool_dir(dir, sizeof(dir))) {
        return;
    }

    for (n = 0; n < sizeof(names) / sizeof(names[0]); n++) {
        for (p = 0; p < sizeof(places) / sizeof(places[0]); p++) {
            written = snprintf(path, sizeof(path), "%s%s/%s%s", dir, places[p], names[n],
                               HDRG_MODULE_SUFFIX);
            if (written <= 0 || (size_t)written >= sizeof(path)) {
                continue;
            }

            fp = fopen(path, "rb");
            if (fp == NULL) {
                continue;
            }

            (void)fclose(fp);

            status = hdrg_context_load_plugin(ctx, path);
            if (status != HDRG_OK) {
                fprintf(stderr, "module %s: %s (%s)\n", path, hdrg_context_error(ctx),
                        hdrg_status_string(status));
            }

            break;
        }
    }
}

#endif /* HDRG_TOOLS_MODULES_H */
