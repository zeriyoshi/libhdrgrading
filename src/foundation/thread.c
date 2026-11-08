/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

/* sched_getaffinity / CPU_COUNT (must precede every system header) */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "hdrgrading/foundation/thread.h"

#include <stddef.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sched.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <pthread/qos.h>
#endif
#endif

/*
 * the least stack of a thread (POSIX): musl's default (128 KiB) and macOS's
 * (512 KiB) raised to it, glibc's (8 MiB) kept
 */
#define HDRG_THREAD_STACK ((size_t)1u << 20)

#if defined(_WIN32)
_Static_assert(sizeof(SRWLOCK) == sizeof(void *), "SRWLOCK must fit hdrg_mutex_t");
_Static_assert(sizeof(CONDITION_VARIABLE) == sizeof(void *),
               "CONDITION_VARIABLE must fit hdrg_cond_t");
_Static_assert(sizeof(HANDLE) == sizeof(void *), "HANDLE must fit hdrg_thread_t");
#endif

#if defined(_WIN32)
static DWORD WINAPI hdrg_thread_entry(LPVOID param)
{
    hdrg_thread_t *thread = (hdrg_thread_t *)param;

    thread->fn(thread->arg);

    return 0;
}
#else
static void *hdrg_thread_entry(void *param)
{
    hdrg_thread_t *thread = (hdrg_thread_t *)param;

    thread->fn(thread->arg);

    return NULL;
}
#endif

extern bool hdrg_mutex_init(hdrg_mutex_t *mutex)
{
#if defined(_WIN32)
    InitializeSRWLock((PSRWLOCK)&mutex->opaque);

    return true;
#else
    return pthread_mutex_init(&mutex->handle, NULL) == 0;
#endif
}

extern void hdrg_mutex_destroy(hdrg_mutex_t *mutex)
{
#if defined(_WIN32)
    (void)mutex;
#else
    (void)pthread_mutex_destroy(&mutex->handle);
#endif
}

extern void hdrg_mutex_lock(hdrg_mutex_t *mutex)
{
#if defined(_WIN32)
    AcquireSRWLockExclusive((PSRWLOCK)&mutex->opaque);
#else
    (void)pthread_mutex_lock(&mutex->handle);
#endif
}

extern void hdrg_mutex_unlock(hdrg_mutex_t *mutex)
{
#if defined(_WIN32)
    ReleaseSRWLockExclusive((PSRWLOCK)&mutex->opaque);
#else
    (void)pthread_mutex_unlock(&mutex->handle);
#endif
}

extern bool hdrg_cond_init(hdrg_cond_t *cond)
{
#if defined(_WIN32)
    InitializeConditionVariable((PCONDITION_VARIABLE)&cond->opaque);

    return true;
#else
    return pthread_cond_init(&cond->handle, NULL) == 0;
#endif
}

extern void hdrg_cond_destroy(hdrg_cond_t *cond)
{
#if defined(_WIN32)
    (void)cond;
#else
    (void)pthread_cond_destroy(&cond->handle);
#endif
}

extern void hdrg_cond_wait(hdrg_cond_t *cond, hdrg_mutex_t *mutex)
{
#if defined(_WIN32)
    (void)SleepConditionVariableSRW((PCONDITION_VARIABLE)&cond->opaque, (PSRWLOCK)&mutex->opaque,
                                    INFINITE, 0);
#else
    (void)pthread_cond_wait(&cond->handle, &mutex->handle);
#endif
}

extern void hdrg_cond_signal(hdrg_cond_t *cond)
{
#if defined(_WIN32)
    WakeConditionVariable((PCONDITION_VARIABLE)&cond->opaque);
#else
    (void)pthread_cond_signal(&cond->handle);
#endif
}

extern void hdrg_cond_broadcast(hdrg_cond_t *cond)
{
#if defined(_WIN32)
    WakeAllConditionVariable((PCONDITION_VARIABLE)&cond->opaque);
#else
    (void)pthread_cond_broadcast(&cond->handle);
#endif
}

extern bool hdrg_thread_create(hdrg_thread_t *thread, hdrg_thread_fn_t fn, void *arg)
{
#if !defined(_WIN32)
    pthread_attr_t attr;
    size_t stack;
    bool created;
#endif

    thread->fn = fn;
    thread->arg = arg;

#if defined(_WIN32)
    thread->handle = (void *)CreateThread(NULL, 0, hdrg_thread_entry, thread, 0, NULL);

    return thread->handle != NULL;
#else
    if (pthread_attr_init(&attr) != 0) {
        return false;
    }

    if (pthread_attr_getstacksize(&attr, &stack) == 0 && stack < HDRG_THREAD_STACK) {
        (void)pthread_attr_setstacksize(&attr, HDRG_THREAD_STACK);
    }

    created = pthread_create(&thread->handle, &attr, hdrg_thread_entry, thread) == 0;
    (void)pthread_attr_destroy(&attr);

    return created;
#endif
}

extern void hdrg_thread_join(hdrg_thread_t *thread)
{
#if defined(_WIN32)
    (void)WaitForSingleObject((HANDLE)thread->handle, INFINITE);
    (void)CloseHandle((HANDLE)thread->handle);
    thread->handle = NULL;
#else
    (void)pthread_join(thread->handle, NULL);
#endif
}

extern void hdrg_thread_yield(void)
{
#if defined(_WIN32)
    (void)SwitchToThread();
#else
    (void)sched_yield();
#endif
}

extern void hdrg_thread_set_worker_qos(void)
{
#if defined(__APPLE__)
    (void)pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#endif
}

extern uint32_t hdrg_thread_cpu_count(void)
{
#if defined(__linux__) && defined(CPU_COUNT)
    cpu_set_t set;
#endif
    uint32_t count;
#if !defined(_WIN32)
    int64_t online;
#endif

#if defined(_WIN32)
    count = (uint32_t)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
#else
    online = (int64_t)sysconf(_SC_NPROCESSORS_ONLN);
    count = (online > 0 && online <= (int64_t)UINT32_MAX) ? (uint32_t)online : 1u;
#if defined(__linux__) && defined(CPU_COUNT)
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0 && CPU_COUNT(&set) > 0) {
        count = (uint32_t)CPU_COUNT(&set);
    }
#endif
#endif

    return count > 0 ? count : 1u;
}
