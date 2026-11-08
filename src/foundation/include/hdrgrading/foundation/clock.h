/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_FOUNDATION_CLOCK_H
#define HDRG_FOUNDATION_CLOCK_H

#include <stdint.h>

/*
 * A monotonic clock's nanoseconds from an origin of its own: for reports of
 * time (a session's latency), never for what the library computes
 */
extern uint64_t hdrg_clock_ns(void);

#endif /* HDRG_FOUNDATION_CLOCK_H */
