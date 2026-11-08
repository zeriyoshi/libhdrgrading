/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "hdrgrading/foundation/clock.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

#if defined(_WIN32)
extern uint64_t hdrg_clock_ns(void)
{
    LARGE_INTEGER count, rate;

    QueryPerformanceCounter(&count);
    QueryPerformanceFrequency(&rate);

    return (uint64_t)((double)count.QuadPart * 1e9 / (double)rate.QuadPart);
}
#else
extern uint64_t hdrg_clock_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }

    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}
#endif
