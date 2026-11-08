/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_TEST_GPU_MODULE_H
#define HDRG_TEST_GPU_MODULE_H

#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "hdrgrading/plugin.h"

/*
 * tests/modules/gpu_copy.c held open by a test (the library's load of it the same image): its
 * counts of the jobs given their input, and their output, on the device (hdrg_test_gpu_jobs)
 */
typedef void(HDRG_PLUGIN_CALL *hdrg_test_gpu_jobs_fn_t)(uint32_t *in, uint32_t *out);

static inline void *hdrg_test_gpu_module_open(const char *path)
{
#if defined(_WIN32)
    return (void *)LoadLibraryA(path);
#else
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}

/* *in and *out 0 when the module or its function is not to be had */
static inline void hdrg_test_gpu_module_jobs(void *module, uint32_t *in, uint32_t *out)
{
    hdrg_test_gpu_jobs_fn_t fn;

    *in = 0;
    *out = 0;

    if (module == NULL) {
        return;
    }

#if defined(_WIN32)
    fn = (hdrg_test_gpu_jobs_fn_t)(void (*)(void))GetProcAddress((HMODULE)module,
                                                                 "hdrg_test_gpu_jobs");
#else
    *(void **)&fn = dlsym(module, "hdrg_test_gpu_jobs");
#endif

    if (fn != NULL) {
        fn(in, out);
    }
}

static inline void hdrg_test_gpu_module_close(void *module)
{
    if (module == NULL) {
        return;
    }

#if defined(_WIN32)
    (void)FreeLibrary((HMODULE)module);
#else
    (void)dlclose(module);
#endif
}

#endif /* HDRG_TEST_GPU_MODULE_H */
