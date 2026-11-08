/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <string.h>

#include "hdrgrading/version.h"
#include "priv.h"
#include "hdrgrading/foundation/fpenv.h"
#include "hdrgrading/foundation/memory.h"
#include "hdrgrading/foundation/thread.h"

#define HDRG_CONTEXT_MAX_WORKERS 63u /* pool threads besides the caller */

static void *hdrg_default_malloc(size_t size, void *user)
{
    (void)user;

    return HDRG_MALLOC(size);
}

static void *hdrg_default_calloc(size_t count, size_t size, void *user)
{
    (void)user;

    return HDRG_CALLOC(count, size);
}

static void *hdrg_default_realloc(void *ptr, size_t size, void *user)
{
    (void)user;

    return HDRG_REALLOC(ptr, size);
}

static void hdrg_default_free(void *ptr, void *user)
{
    (void)user;
    HDRG_FREE(ptr);
}

/* pool threads belong to the library: pin the FP environment once, never restore */
static void hdrg_worker_init(uint32_t worker_index, void *user)
{
    hdrg_fpenv_t env;

    (void)worker_index;
    (void)user;

    hdrg_fpenv_enter(&env);
}

extern void *hdrg_mem_alloc(const hdrg_allocator_t *alloc, size_t size)
{
    if (size == 0) {
        size = 1;
    }

    return alloc->malloc_fn(size, alloc->user);
}

extern void *hdrg_mem_calloc(const hdrg_allocator_t *alloc, size_t count, size_t size)
{
    void *ptr;

    if (size != 0 && count > SIZE_MAX / size) {
        return NULL;
    }

    if (count == 0 || size == 0) {
        count = 1;
        size = 1;
    }

    if (alloc->calloc_fn != NULL) {
        return alloc->calloc_fn(count, size, alloc->user);
    }

    ptr = hdrg_mem_alloc(alloc, count * size);
    if (ptr != NULL) {
        memset(ptr, 0, count * size);
    }

    return ptr;
}

extern void *hdrg_mem_realloc(const hdrg_allocator_t *alloc, void *ptr, size_t size)
{
    /* as hdrg_mem_alloc: never 0 (a C realloc may free the block and return NULL for it) */
    return alloc->realloc_fn(ptr, (size == 0) ? 1u : size, alloc->user);
}

extern void hdrg_mem_free(const hdrg_allocator_t *alloc, void *ptr)
{
    if (ptr != NULL) {
        alloc->free_fn(ptr, alloc->user);
    }
}

extern void *hdrg_mem_aligned_alloc(const hdrg_allocator_t *alloc, size_t size, size_t align)
{
    uintptr_t start, aligned;
    void *raw;

    if (align < sizeof(void *) || (align & (align - 1u)) != 0) {
        return NULL;
    }

    if (alloc->malloc_fn == hdrg_default_malloc) {
        return HDRG_ALIGNED_ALLOC(align, (size == 0) ? 1u : size);
    }

    if (size > SIZE_MAX - align - sizeof(void *)) {
        return NULL;
    }

    raw = hdrg_mem_alloc(alloc, size + align + sizeof(void *));
    if (raw == NULL) {
        return NULL;
    }

    start = (uintptr_t)raw + sizeof(void *);
    aligned = (start + align - 1u) & ~(uintptr_t)(align - 1u);
    memcpy((void *)(aligned - sizeof(void *)), &raw, sizeof(raw));

    return (void *)aligned;
}

extern void hdrg_mem_aligned_free(const hdrg_allocator_t *alloc, void *ptr)
{
    void *raw;

    if (ptr == NULL) {
        return;
    }

    if (alloc->malloc_fn == hdrg_default_malloc) {
        HDRG_ALIGNED_FREE(ptr);

        return;
    }

    memcpy(&raw, (uint8_t *)ptr - sizeof(void *), sizeof(raw));
    hdrg_mem_free(alloc, raw);
}

extern uint8_t *hdrg_outbuf_alloc(const hdrg_allocator_t *alloc, size_t size)
{
    hdrg_outbuf_header_t *header;

    if (size > SIZE_MAX - sizeof(*header)) {
        return NULL;
    }

    header = (hdrg_outbuf_header_t *)hdrg_mem_alloc(alloc, sizeof(*header) + size);
    if (header == NULL) {
        return NULL;
    }

    header->free_fn = alloc->free_fn;
    header->user = alloc->user;
    header->magic = HDRG_OUTBUF_MAGIC;
    header->pad = 0;

    return (uint8_t *)(header + 1);
}

extern uint8_t *hdrg_outbuf_adopt(const hdrg_allocator_t *alloc, void *block)
{
    hdrg_outbuf_header_t *header = (hdrg_outbuf_header_t *)block;

    header->free_fn = alloc->free_fn;
    header->user = alloc->user;
    header->magic = HDRG_OUTBUF_MAGIC;
    header->pad = 0;

    return (uint8_t *)(header + 1);
}

extern void hdrg_context_exec(hdrg_context_t *ctx, hdrg_exec_t *out)
{
    uint32_t threads;

    out->pool = NULL;
    out->max_workers = 1;

    if (ctx->threads == 1) {
        return;
    }

    /* the pool of the context's threads (0: one a core), the calling thread one of them */
    if (ctx->pool == NULL && !ctx->pool_failed) {
        threads = (ctx->threads == 0) ? hdrg_thread_cpu_count() : ctx->threads;
        if (threads <= 1 || hdrg_pool_create(&ctx->pool, &ctx->alloc,
                                             HDRG_MIN(threads - 1u, HDRG_CONTEXT_MAX_WORKERS),
                                             hdrg_worker_init, NULL) != HDRG_OK) {
            ctx->pool = NULL;
            ctx->pool_failed = true;
        }
    }

    out->pool = ctx->pool;
    out->max_workers = (ctx->threads == 0) ? UINT32_MAX : ctx->threads;
}

extern const char *hdrg_status_string(hdrg_status_t s)
{
    switch (s) {
    case HDRG_OK:
        return "ok";
    case HDRG_ERR_INVALID_ARG:
        return "invalid argument";
    case HDRG_ERR_UNSUPPORTED_FORMAT:
        return "unsupported format";
    case HDRG_ERR_DECODE:
        return "decode error";
    case HDRG_ERR_ENCODE:
        return "encode error";
    case HDRG_ERR_NOMEM:
        return "out of memory";
    case HDRG_ERR_ICC_UNSUPPORTED:
        return "unsupported icc profile";
    case HDRG_ERR_INTERNAL:
        return "internal error";
    case HDRG_ERR_INVALID_STATE:
        return "invalid state";
    case HDRG_ERR_UNSUPPORTED:
        return "unsupported";
    case HDRG_ERR_NOT_FOUND:
        return "not found";
    case HDRG_ERR_PLUGIN:
        return "plugin error";
    case HDRG_ERR_AGAIN:
        return "try again";
    case HDRG_EOF:
        return "end of stream";
    case HDRG_ERR_LIMIT:
        return "image too large";
    default:
        return "unknown status";
    }
}

extern const char *hdrg_version(void)
{
    return HDRG_VERSION_STRING;
}

extern void hdrg_context_desc_init(hdrg_context_desc_t *desc)
{
    if (desc == NULL) {
        return;
    }

    memset(desc, 0, sizeof(*desc));
    desc->struct_size = (uint32_t)sizeof(*desc);
}

extern hdrg_status_t hdrg_context_create(const hdrg_context_desc_t *desc, hdrg_context_t **ctx)
{
    const hdrg_allocator_t *alloc;
    const hdrg_kernels_t *capped;
    hdrg_context_desc_t defaults;
    hdrg_allocator_t effective;
    hdrg_context_t *created;
    hdrg_status_t status;
    uint32_t i;
    uint8_t env;

    if (ctx == NULL ||
        (desc != NULL && (desc->struct_size != sizeof(*desc) || desc->gpu > HDRG_GPU_FILTERS ||
                          desc->gpu_analysis > HDRG_GPU_ANALYSIS_FAST))) {
        return HDRG_ERR_INVALID_ARG;
    }

    hdrg_context_desc_init(&defaults);
    desc = (desc != NULL) ? desc : &defaults;
    alloc = desc->allocator;

    *ctx = NULL;

    if (alloc != NULL) {
        if (alloc->malloc_fn == NULL || alloc->realloc_fn == NULL || alloc->free_fn == NULL) {
            return HDRG_ERR_INVALID_ARG;
        }

        effective = *alloc;
    } else {
        effective.malloc_fn = hdrg_default_malloc;
        effective.calloc_fn = hdrg_default_calloc;
        effective.realloc_fn = hdrg_default_realloc;
        effective.free_fn = hdrg_default_free;
        effective.user = NULL;
    }

    created = (hdrg_context_t *)hdrg_mem_calloc(&effective, 1, sizeof(*created));
    if (created == NULL) {
        return HDRG_ERR_NOMEM;
    }

    created->alloc = effective;
    created->threads = desc->thread_count;
    created->gpu = desc->gpu;
    created->gpu_analysis = desc->gpu_analysis;
    created->max_pixels = (desc->max_pixels != 0) ? desc->max_pixels : HDRG_DEFAULT_MAX_PIXELS;
    created->max_pixels = HDRG_MIN(created->max_pixels, HDRG_MAX_PIXELS_CAP);

    /*
     * the descriptor's tier, or the environment's: with both, the lower one
     * (the tiers of one architecture's chain ascend)
     */
    env = hdrg_kernels_parse(getenv("HDRG_SIMD_MAX"));
    created->kernels = hdrg_kernels_select((desc->simd_max != 0) ? desc->simd_max : env);

    if (env != 0 && desc->simd_max != 0) {
        capped = hdrg_kernels_select(env);
        created->kernels = (capped->tier < created->kernels->tier) ? capped : created->kernels;
    }

    hdrg_host_init(&created->host, &created->alloc, created);

    /*
     * the built-in plugins (src/plugins: the filters, then the codecs, before any of the app's:
     * their readers are probed first) go through the same plugin ABI as external ones
     */
    for (i = 0; i < hdrg_builtin_plugin_count; i++) {
        status = hdrg_host_register(&created->host, hdrg_builtin_plugins[i], NULL,
                                    HDRG_PLUGIN_ABI_VERSION, true);
        if (status != HDRG_OK) {
            hdrg_context_destroy(created);

            return (status == HDRG_ERR_NOMEM) ? HDRG_ERR_NOMEM : HDRG_ERR_INTERNAL;
        }
    }

    *ctx = created;

    return HDRG_OK;
}

extern void hdrg_context_destroy(hdrg_context_t *ctx)
{
    hdrg_allocator_t alloc;

    if (ctx == NULL) {
        return;
    }

    alloc = ctx->alloc;

    hdrg_host_destroy(&ctx->host);
    hdrg_pool_destroy(ctx->pool);
    hdrg_mem_free(&alloc, ctx);
}

extern void hdrg_free(uint8_t *buffer)
{
    hdrg_outbuf_header_t *header;

    if (buffer == NULL) {
        return;
    }

    header = (hdrg_outbuf_header_t *)buffer - 1;
    if (header->magic != HDRG_OUTBUF_MAGIC) {
        return;
    }

    header->magic = 0;

    header->free_fn(header, header->user);
}

extern hdrg_status_t hdrg_get_last_stats(const hdrg_context_t *ctx, hdrg_stats_t *out)
{
    if (ctx == NULL || out == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    if (!ctx->has_stats) {
        return HDRG_ERR_INVALID_STATE;
    }

    *out = ctx->last_stats;

    return HDRG_OK;
}
