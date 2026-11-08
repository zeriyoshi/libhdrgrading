/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_CORE_RECYCLE_H
#define HDRG_CORE_RECYCLE_H

#include <stdbool.h>
#include <stdint.h>

#include "../priv.h"
#include "hdrgrading/foundation/thread.h"

#define HDRG_RECYCLE_CLASSES 48u /* blocks of 2^k bytes, k < 48 */

/*
 * An allocator that keeps the blocks freed to it for reuse: a block is rounded up to a power of two
 * (at least 256 bytes) and goes back to the list of its size when freed; a request takes a free
 * block of up to four times its size first. Frames of one size so take
 * blocks from the backing allocator only while the sequence warms up. Safe
 * from several threads.
 */
typedef struct {
    hdrg_allocator_t alloc; /* the recycling allocator; its user is this */
    const hdrg_allocator_t *backing;
    void *lists[HDRG_RECYCLE_CLASSES];
    hdrg_mutex_t lock;
} hdrg_recycle_t;

/* recycle must stay where it is while alloc is used */
extern bool hdrg_recycle_init(hdrg_recycle_t *recycle, const hdrg_allocator_t *backing);
/* every block taken from it must have been freed back first */
extern void hdrg_recycle_destroy(hdrg_recycle_t *recycle);

#endif /* HDRG_CORE_RECYCLE_H */
