/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_FOUNDATION_THREAD_H
#define HDRG_FOUNDATION_THREAD_H

#include <stdbool.h>
#include <stdint.h>

#if !defined(_WIN32)
#include <pthread.h>
#endif

/*
 * Thin threading layer: mutex / condvar / thread over pthread or
 * Win32 (SRWLOCK + CONDITION_VARIABLE + CreateThread), and 32-bit atomics over C11
 * <stdatomic.h>. <threads.h> is not used (the macOS SDK lacks it). MSVC's C front
 * end without C11 atomics falls back to Interlocked*; clang-cl uses <stdatomic.h>.
 */
#if defined(_MSC_VER) && !defined(__clang__)
#define HDRG_ATOMIC_INTERLOCKED 1
#include <intrin.h>
#define HDRG_THREAD_LOCAL __declspec(thread)
#else
#define HDRG_ATOMIC_INTERLOCKED 0
#include <stdatomic.h>
#define HDRG_THREAD_LOCAL _Thread_local
#endif

#if HDRG_ATOMIC_INTERLOCKED
typedef struct {
    volatile long value;
} hdrg_atomic_u32_t;
#else
typedef struct {
    _Atomic uint32_t value;
} hdrg_atomic_u32_t;
#endif

/* The Win32 objects are pointer-sized; thread.c checks the sizes. */
#if defined(_WIN32)
typedef struct {
    void *opaque;
} hdrg_mutex_t;

typedef struct {
    void *opaque;
} hdrg_cond_t;
#else
typedef struct {
    pthread_mutex_t handle;
} hdrg_mutex_t;

typedef struct {
    pthread_cond_t handle;
} hdrg_cond_t;
#endif

typedef void (*hdrg_thread_fn_t)(void *arg);

/* The new thread reads fn / arg through this object: it must stay in place until joined. */
typedef struct {
#if defined(_WIN32)
    void *handle;
#else
    pthread_t handle;
#endif
    hdrg_thread_fn_t fn;
    void *arg;
} hdrg_thread_t;

/* Not atomic itself: only before the object is shared. */
static inline void hdrg_atomic_u32_init(hdrg_atomic_u32_t *a, uint32_t value)
{
#if HDRG_ATOMIC_INTERLOCKED
    a->value = (long)value;
#else
    atomic_init(&a->value, value);
#endif
}

static inline uint32_t hdrg_atomic_u32_load_relaxed(hdrg_atomic_u32_t *a)
{
#if HDRG_ATOMIC_INTERLOCKED
    return (uint32_t)a->value;
#else
    return atomic_load_explicit(&a->value, memory_order_relaxed);
#endif
}

static inline uint32_t hdrg_atomic_u32_load_acquire(hdrg_atomic_u32_t *a)
{
#if HDRG_ATOMIC_INTERLOCKED
    return (uint32_t)_InterlockedOr(&a->value, 0);
#else
    return atomic_load_explicit(&a->value, memory_order_acquire);
#endif
}

static inline void hdrg_atomic_u32_store_relaxed(hdrg_atomic_u32_t *a, uint32_t value)
{
#if HDRG_ATOMIC_INTERLOCKED
    a->value = (long)value;
#else
    atomic_store_explicit(&a->value, value, memory_order_relaxed);
#endif
}

static inline void hdrg_atomic_u32_store_release(hdrg_atomic_u32_t *a, uint32_t value)
{
#if HDRG_ATOMIC_INTERLOCKED
    (void)_InterlockedExchange(&a->value, (long)value);
#else
    atomic_store_explicit(&a->value, value, memory_order_release);
#endif
}

/* Returns the previous value. Relaxed: counters and task claims only. */
static inline uint32_t hdrg_atomic_u32_fetch_add_relaxed(hdrg_atomic_u32_t *a, uint32_t value)
{
#if HDRG_ATOMIC_INTERLOCKED
    return (uint32_t)_InterlockedExchangeAdd(&a->value, (long)value);
#else
    return atomic_fetch_add_explicit(&a->value, value, memory_order_relaxed);
#endif
}

static inline uint32_t hdrg_atomic_u32_fetch_add_acq_rel(hdrg_atomic_u32_t *a, uint32_t value)
{
#if HDRG_ATOMIC_INTERLOCKED
    return (uint32_t)_InterlockedExchangeAdd(&a->value, (long)value);
#else
    return atomic_fetch_add_explicit(&a->value, value, memory_order_acq_rel);
#endif
}

/* Strong CAS (acq_rel, acquire on failure). On failure *expected receives the current value. */
static inline bool hdrg_atomic_u32_cas_acq_rel(hdrg_atomic_u32_t *a, uint32_t *expected,
                                               uint32_t desired)
{
#if HDRG_ATOMIC_INTERLOCKED
    long previous;

    previous = _InterlockedCompareExchange(&a->value, (long)desired, (long)*expected);
    if ((uint32_t)previous == *expected) {
        return true;
    }

    *expected = (uint32_t)previous;

    return false;
#else
    return atomic_compare_exchange_strong_explicit(&a->value, expected, desired,
                                                   memory_order_acq_rel, memory_order_acquire);
#endif
}

extern bool hdrg_mutex_init(hdrg_mutex_t *mutex);
extern void hdrg_mutex_destroy(hdrg_mutex_t *mutex);
extern void hdrg_mutex_lock(hdrg_mutex_t *mutex);
extern void hdrg_mutex_unlock(hdrg_mutex_t *mutex);

extern bool hdrg_cond_init(hdrg_cond_t *cond);
extern void hdrg_cond_destroy(hdrg_cond_t *cond);
/* May wake spuriously: always wait in a loop over the predicate. */
extern void hdrg_cond_wait(hdrg_cond_t *cond, hdrg_mutex_t *mutex);
extern void hdrg_cond_signal(hdrg_cond_t *cond);
extern void hdrg_cond_broadcast(hdrg_cond_t *cond);

extern bool hdrg_thread_create(hdrg_thread_t *thread, hdrg_thread_fn_t fn, void *arg);
extern void hdrg_thread_join(hdrg_thread_t *thread);
/* the rest of the calling thread's time slice to the others (a short wait's spin) */
extern void hdrg_thread_yield(void);
/* QoS for compute workers: USER_INITIATED on Apple platforms, no-op elsewhere. */
extern void hdrg_thread_set_worker_qos(void);

/* Logical CPUs usable by this process (Linux: the affinity mask), at least 1. */
extern uint32_t hdrg_thread_cpu_count(void);

#endif /* HDRG_FOUNDATION_THREAD_H */
