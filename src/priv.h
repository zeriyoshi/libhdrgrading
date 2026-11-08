/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_PRIV_H
#define HDRG_PRIV_H

#include <stddef.h>
#include <stdint.h>

#include "core/exec.h"
#include "hdrgrading/hdrgrading.h"
#include "kernels/kernels.h"
#include "host/host.h"

#define HDRG_MIN(a, b)        ((a) < (b) ? (a) : (b))
#define HDRG_MAX(a, b)        ((a) > (b) ? (a) : (b))
#define HDRG_CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))

#define HDRG_OUTBUF_MAGIC       UINT64_C(0x48445247424C4F42)
#define HDRG_MAX_DIMENSION      65535u /* largest image side (16 bits) */
#define HDRG_JPEG_MAX_DIMENSION 65500u /* ... a JPEG's (jpegli's JPEG_MAX_DIMENSION) */

/* tests (hdrg_context_t.gpu_frames): where a session's GPU work keeps its frames */
#define HDRG_GPU_FRAMES_CHOSEN 0u /* the session's choice */
#define HDRG_GPU_FRAMES_HOST   1u /* read back to the host */
#define HDRG_GPU_FRAMES_GPU    2u /* on the GPU whenever they can be */
/*
 * the most pixels a context takes, whatever max_pixels asks: an image of 16 bytes a
 * pixel still fits size_t (2^28 - 1 where it is 32 bits, below the default 2^28; no cap on
 * 64 bits)
 */
#define HDRG_MAX_PIXELS_CAP ((uint64_t)(SIZE_MAX / 16u))

/* the optional built-in plugins the build has (src/plugins/CMakeLists.txt, HDRG_PLUGINS_EXCLUDE) */
#if !defined(HDRG_BUILTIN_DEBLOCK) || !defined(HDRG_BUILTIN_DERING) ||                             \
    !defined(HDRG_BUILTIN_DEFRINGE) || !defined(HDRG_BUILTIN_DEBAND) ||                            \
    !defined(HDRG_BUILTIN_MCTF) || !defined(HDRG_BUILTIN_DENOISE) || !defined(HDRG_BUILTIN_EDGE)
#error "the HDRG_BUILTIN_* definitions of the built-in plugins are missing"
#endif

struct hdrg_context {
    hdrg_allocator_t alloc;
    hdrg_plugin_host_t host; /* registered plugins and the filters added to stages */
    hdrg_stats_t last_stats;
    hdrg_pool_t *pool;             /* created on the first multi-threaded call */
    const hdrg_kernels_t *kernels; /* the SIMD tier, chosen at creation (HDRG_SIMD_MAX caps it) */
    uint64_t max_pixels;           /* the descriptor's (0 resolved) */
    uint32_t threads;              /* the descriptor's thread_count */
    uint8_t gpu;                   /* the descriptor's HDRG_GPU_* */
    uint8_t gpu_analysis;          /* ... and HDRG_GPU_ANALYSIS_* */
    uint32_t gpu_kind;  /* tests: the HDRG_DEVICE_* of a session's GPU work (0: Metal, WebGPU) */
    uint8_t gpu_frames; /* tests: a session's frames on the GPU (HDRG_GPU_FRAMES_*) */
    bool has_stats;
    bool pool_failed; /* threads unavailable: stay inline */
};

/*
 * Output buffers returned to the caller are prefixed with a hidden header
 * carrying the free hook, so hdrg_free() works after the originating context
 * is destroyed. The header is 32 bytes to keep the payload 16-byte aligned.
 */
typedef struct {
    void (*free_fn)(void *ptr, void *user);
    void *user;
    uint64_t magic;
    uint64_t pad;
} hdrg_outbuf_header_t;

extern void *hdrg_mem_alloc(const hdrg_allocator_t *alloc, size_t size);
extern void *hdrg_mem_calloc(const hdrg_allocator_t *alloc, size_t count, size_t size);
extern void *hdrg_mem_realloc(const hdrg_allocator_t *alloc, void *ptr, size_t size);
extern void hdrg_mem_free(const hdrg_allocator_t *alloc, void *ptr);
/*
 * A block at a multiple of align (a power of two): the build's HDRG_ALIGNED_ALLOC for the default
 * allocator, else carved from malloc_fn's block (the start kept just before it). Freed by
 * hdrg_mem_aligned_free alone
 */
extern void *hdrg_mem_aligned_alloc(const hdrg_allocator_t *alloc, size_t size, size_t align);
extern void hdrg_mem_aligned_free(const hdrg_allocator_t *alloc, void *ptr);

extern uint8_t *hdrg_outbuf_alloc(const hdrg_allocator_t *alloc, size_t size);
/*
 * a block of alloc's (hdrg_mem_alloc / realloc) that keeps sizeof(hdrg_outbuf_header_t) bytes
 * before its data made an output buffer in place (no copy): the data's pointer
 */
extern uint8_t *hdrg_outbuf_adopt(const hdrg_allocator_t *alloc, void *block);

/*
 * Execution for one call: the context's pool (created on first use) and up
 * to the context's thread count of workers, the caller included (0 = all,
 * 1 = inline).
 */
extern void hdrg_context_exec(hdrg_context_t *ctx, hdrg_exec_t *out);

/* convert.c: defaults + struct_size gating + range validation */
extern bool hdrg_params_resolve(const hdrg_params_t *params, hdrg_params_t *out);
/* encode / caps (NULL = the defaults) checked and copied into out; false = out of range */
extern bool hdrg_encode_resolve(const hdrg_encode_params_t *encode, hdrg_encode_params_t *out);
extern bool hdrg_display_resolve(const hdrg_display_caps_t *caps, hdrg_display_caps_t *out);

#endif /* HDRG_PRIV_H */
