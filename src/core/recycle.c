/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "recycle.h"

#include <stddef.h>
#include <string.h>

/*
 * Sizes that follow the content (a component count, say) still reuse blocks:
 * every small block is of one class, and a request takes a free block of up
 * to 2^REACH times its size before it takes a new one
 */
#define HDRG_RECYCLE_MIN_CLASS 8u /* 256 bytes */
#define HDRG_RECYCLE_REACH     2u

/* in front of each block: the payload keeps the backing allocator's alignment */
typedef union {
    max_align_t align;
    struct {
        void *next; /* in a free list */
        uint8_t cls;
    } info;
} hdrg_recycle_header_t;

/* the smallest class of 2^k bytes that holds size after the header; HDRG_RECYCLE_CLASSES = none */
static inline uint8_t hdrg_recycle_class(size_t size)
{
    const uint8_t limit = (uint8_t)HDRG_MIN(HDRG_RECYCLE_CLASSES, sizeof(size_t) * 8u - 1u);
    uint8_t k;

    if (size > ((size_t)1 << (limit - 1u))) {
        return HDRG_RECYCLE_CLASSES;
    }

    k = HDRG_RECYCLE_MIN_CLASS;
    while (k < limit && ((size_t)1 << k) < size + sizeof(hdrg_recycle_header_t)) {
        k++;
    }

    return (k < limit) ? k : HDRG_RECYCLE_CLASSES;
}

static void *hdrg_recycle_malloc(size_t size, void *user)
{
    hdrg_recycle_t *recycle = (hdrg_recycle_t *)user;
    hdrg_recycle_header_t *block;
    uint8_t cls, k;

    cls = hdrg_recycle_class(size);
    if (cls == HDRG_RECYCLE_CLASSES) {
        return NULL;
    }

    hdrg_mutex_lock(&recycle->lock);

    block = NULL;
    for (k = cls; block == NULL && k < HDRG_RECYCLE_CLASSES && k <= cls + HDRG_RECYCLE_REACH; k++) {
        block = (hdrg_recycle_header_t *)recycle->lists[k];
    }

    if (block != NULL) {
        cls = (uint8_t)(k - 1u);
        recycle->lists[cls] = block->info.next;
    } else {
        block = (hdrg_recycle_header_t *)recycle->backing->malloc_fn((size_t)1 << cls,
                                                                     recycle->backing->user);
    }

    hdrg_mutex_unlock(&recycle->lock);

    if (block == NULL) {
        return NULL;
    }

    block->info.cls = cls;

    return block + 1;
}

static void hdrg_recycle_free(void *ptr, void *user)
{
    hdrg_recycle_t *recycle = (hdrg_recycle_t *)user;
    hdrg_recycle_header_t *block;

    if (ptr == NULL) {
        return;
    }

    block = (hdrg_recycle_header_t *)ptr - 1;

    hdrg_mutex_lock(&recycle->lock);
    block->info.next = recycle->lists[block->info.cls];
    recycle->lists[block->info.cls] = block;
    hdrg_mutex_unlock(&recycle->lock);
}

static void *hdrg_recycle_realloc(void *ptr, size_t size, void *user)
{
    hdrg_recycle_header_t *block;
    size_t capacity;
    void *grown;

    if (ptr == NULL) {
        return hdrg_recycle_malloc(size, user);
    }

    block = (hdrg_recycle_header_t *)ptr - 1;
    capacity = ((size_t)1 << block->info.cls) - sizeof(hdrg_recycle_header_t);

    if (size <= capacity) {
        return ptr;
    }

    grown = hdrg_recycle_malloc(size, user);
    if (grown == NULL) {
        return NULL;
    }

    memcpy(grown, ptr, capacity);
    hdrg_recycle_free(ptr, user);

    return grown;
}

extern bool hdrg_recycle_init(hdrg_recycle_t *recycle, const hdrg_allocator_t *backing)
{
    memset(recycle, 0, sizeof(*recycle));

    if (!hdrg_mutex_init(&recycle->lock)) {
        return false;
    }

    recycle->backing = backing;
    recycle->alloc.malloc_fn = hdrg_recycle_malloc;
    recycle->alloc.realloc_fn = hdrg_recycle_realloc;
    recycle->alloc.free_fn = hdrg_recycle_free;
    recycle->alloc.user = recycle;

    return true;
}

extern void hdrg_recycle_destroy(hdrg_recycle_t *recycle)
{
    hdrg_recycle_header_t *block, *next;
    uint8_t k;

    for (k = 0; k < HDRG_RECYCLE_CLASSES; k++) {
        for (block = (hdrg_recycle_header_t *)recycle->lists[k]; block != NULL; block = next) {
            next = (hdrg_recycle_header_t *)block->info.next;
            recycle->backing->free_fn(block, recycle->backing->user);
        }

        recycle->lists[k] = NULL;
    }

    hdrg_mutex_destroy(&recycle->lock);
}
