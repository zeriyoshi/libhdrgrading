/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif !defined(__EMSCRIPTEN__)
#include <dlfcn.h>
#endif

#include "../priv.h"
#include "host.h"

#define HDRG_SYMBOL_VERSION "hdrg_plugin_abi_version"
#define HDRG_SYMBOL_INIT    "hdrg_plugin_init"

typedef uint32_t(HDRG_PLUGIN_CALL *hdrg_abi_version_fn_t)(void);

#if defined(__EMSCRIPTEN__)
static hdrg_status_t hdrg_platform_open(const hdrg_allocator_t *alloc, const char *path,
                                        void **module, hdrg_abi_version_fn_t *version,
                                        hdrg_plugin_init_fn_t *init)
{
    (void)alloc;
    (void)path;
    (void)module;
    (void)version;
    (void)init;

    return HDRG_ERR_UNSUPPORTED;
}

static void hdrg_platform_close(void *module)
{
    (void)module;
}
#elif defined(_WIN32)
static hdrg_status_t hdrg_platform_open(const hdrg_allocator_t *alloc, const char *path,
                                        void **module, hdrg_abi_version_fn_t *version,
                                        hdrg_plugin_init_fn_t *init)
{
    HMODULE handle;
    FARPROC version_sym, init_sym;
    wchar_t *wide;
    int32_t wide_len;

    wide_len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (wide_len <= 0) {
        return HDRG_ERR_NOT_FOUND;
    }

    wide = (wchar_t *)hdrg_mem_alloc(alloc, (size_t)wide_len * sizeof(wchar_t));
    if (wide == NULL) {
        return HDRG_ERR_NOMEM;
    }

    (void)MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, wide_len);

    /* an absolute path also resolves the module's own dependencies next to it */
    handle = LoadLibraryExW(wide, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);

    hdrg_mem_free(alloc, wide);

    if (handle == NULL) {
        return HDRG_ERR_NOT_FOUND;
    }

    version_sym = GetProcAddress(handle, HDRG_SYMBOL_VERSION);
    init_sym = GetProcAddress(handle, HDRG_SYMBOL_INIT);
    if (version_sym == NULL || init_sym == NULL) {
        FreeLibrary(handle);

        return HDRG_ERR_UNSUPPORTED;
    }

    *version = (hdrg_abi_version_fn_t)(void (*)(void))version_sym;
    *init = (hdrg_plugin_init_fn_t)(void (*)(void))init_sym;
    *module = (void *)handle;

    return HDRG_OK;
}

static void hdrg_platform_close(void *module)
{
    FreeLibrary((HMODULE)module);
}
#else
static hdrg_status_t hdrg_platform_open(const hdrg_allocator_t *alloc, const char *path,
                                        void **module, hdrg_abi_version_fn_t *version,
                                        hdrg_plugin_init_fn_t *init)
{
    void *handle, *version_sym, *init_sym;

    (void)alloc;

    handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL) {
        return HDRG_ERR_NOT_FOUND;
    }

    version_sym = dlsym(handle, HDRG_SYMBOL_VERSION);
    init_sym = dlsym(handle, HDRG_SYMBOL_INIT);
    if (version_sym == NULL || init_sym == NULL) {
        dlclose(handle);

        return HDRG_ERR_UNSUPPORTED;
    }

    /* POSIX: object and function pointers share one representation */
    memcpy(version, &version_sym, sizeof(*version));
    memcpy(init, &init_sym, sizeof(*init));
    *module = handle;

    return HDRG_OK;
}

static void hdrg_platform_close(void *module)
{
    dlclose(module);
}
#endif

extern hdrg_status_t hdrg_module_open(const hdrg_allocator_t *alloc, const char *path,
                                      void **module, uint32_t *abi_version,
                                      hdrg_plugin_init_fn_t *init)
{
    hdrg_abi_version_fn_t version;
    hdrg_status_t status;

    *module = NULL;
    *abi_version = 0;
    *init = NULL;

    status = hdrg_platform_open(alloc, path, module, &version, init);
    if (status != HDRG_OK) {
        return status;
    }

    *abi_version = version();

    return HDRG_OK;
}

extern void hdrg_module_close(void *module)
{
    if (module != NULL) {
        hdrg_platform_close(module);
    }
}
