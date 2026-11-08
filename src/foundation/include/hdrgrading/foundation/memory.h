/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_FOUNDATION_MEMORY_H
#define HDRG_FOUNDATION_MEMORY_H

/*
 * The build's default allocation: the allocator of a
 * context created without one (hdrg_context_desc_t.allocator) and what has no context (the browser
 * module's glue). Everything else allocates through a context's allocator. A build replaces any of
 * the macros by HDRG_MEMORY_HEADER (the CMake option of the same name): a header defining them and
 * declaring their functions. HDRG_ALIGNED_ALLOC's align is a power of two, its size any.
 */
#ifdef HDRG_MEMORY_HEADER
#include HDRG_MEMORY_HEADER
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#if defined(_WIN32) && !defined(HDRG_ALIGNED_ALLOC)
#include <malloc.h>
#endif

#ifndef HDRG_MALLOC
#define HDRG_MALLOC(size) malloc(size)
#endif
#ifndef HDRG_CALLOC
#define HDRG_CALLOC(count, size) calloc((count), (size))
#endif
#ifndef HDRG_REALLOC
#define HDRG_REALLOC(ptr, size) realloc((ptr), (size))
#endif
#ifndef HDRG_FREE
#define HDRG_FREE(ptr) free(ptr)
#endif

/* C11's aligned_alloc takes a size of a multiple of the alignment (Apple's insists); MSVC has none
 */
#ifndef HDRG_ALIGNED_ALLOC
#if defined(_WIN32)
#define HDRG_ALIGNED_ALLOC(align, size) _aligned_malloc((size), (align))
#define HDRG_ALIGNED_FREE(ptr)          _aligned_free(ptr)
#else
#define HDRG_ALIGNED_ALLOC(align, size) hdrg_memory_aligned_alloc((align), (size))
#define HDRG_ALIGNED_FREE(ptr)          free(ptr)

static inline void *hdrg_memory_aligned_alloc(size_t align, size_t size)
{
    if (size > SIZE_MAX - (align - 1u)) {
        return NULL;
    }

    return aligned_alloc(align, (size + align - 1u) & ~(align - 1u));
}
#endif
#endif

#endif /* HDRG_FOUNDATION_MEMORY_H */
