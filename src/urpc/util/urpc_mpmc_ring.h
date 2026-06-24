/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: urpc bounded MPMC lock-free ring buffer
 *              Vyukov-style: unbounded monotonic 64-bit counters,
 *              cache-line-padded, value-semantic element storage.
 * Create: 2026-06-16
 */

#ifndef URPC_MPMC_RING_H
#define URPC_MPMC_RING_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <sched.h>

#if defined(__x86_64__)
#include <x86intrin.h>
#define URPC_MPMC_PAUSE() _mm_pause()
#elif defined(__aarch64__)
#define URPC_MPMC_PAUSE() __asm__ volatile("yield" ::: "memory")
#else
#define URPC_MPMC_PAUSE() ((void)0)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */

#ifndef URPC_MPMC_CACHE_LINE
#define URPC_MPMC_CACHE_LINE 64
#endif

#ifndef URPC_MPMC_SPIN_THRESHOLD
#define URPC_MPMC_SPIN_THRESHOLD 1024
#endif

/* --------------------------------------------------------------------------
 * Structure declaration macro
 * -------------------------------------------------------------------------- */

/*
 * Declare a bounded MPMC ring struct with a fixed capacity.
 *
 * Usage:
 *   URPC_MPMC_RING_DECLARE(my_ring, my_elem_t, 64);
 *
 *   my_ring_t ring;
 *   urpc_mpmc_ring_init(&ring);
 *
 * NOTES:
 *   - capacity MUST be a power of 2.
 *   - The ring buffer is embedded in the struct (no allocation needed).
 *   - Each hot counter is padded to a full cache line (64 bytes on
 *     x86_64/AArch64). Without this padding, two counters would share
 *     the same cache line — when core 0 writes prod_tail and core 1
 *     reads cons_head on the same line, the cache-coherency protocol
 *     bounces the line between cores, adding 50-200 cycles of invisible
 *     latency (false sharing). Padding eliminates this entirely at the
 *     cost of 56 bytes per counter (280 bytes total) — negligible for
 *     a ~500 KB ring.
 */
#define URPC_MPMC_RING_DECLARE(_name, _type, _capacity)           \
    typedef struct _name {                                          \
        volatile uint64_t prod_head;                                \
        uint8_t _rsvd0[URPC_MPMC_CACHE_LINE - sizeof(uint64_t)];   \
        volatile uint64_t prod_tail;                                \
        uint8_t _rsvd1[URPC_MPMC_CACHE_LINE - sizeof(uint64_t)];   \
        volatile uint64_t cons_head;                                \
        uint8_t _rsvd2[URPC_MPMC_CACHE_LINE - sizeof(uint64_t)];   \
        volatile uint64_t cons_tail;                                \
        uint8_t _rsvd3[URPC_MPMC_CACHE_LINE - sizeof(uint64_t)];   \
        uint32_t capacity;                                          \
        uint32_t mask;                                              \
        uint8_t _rsvd4[URPC_MPMC_CACHE_LINE - 2 * sizeof(uint32_t)]; \
        _type buf[(_capacity)];                                     \
    } _name##_t

/* --------------------------------------------------------------------------
 * Init / Reset
 * -------------------------------------------------------------------------- */

#define urpc_mpmc_ring_init(_ring) do {                                    \
    (_ring)->prod_head = 0;                                                \
    (_ring)->prod_tail = 0;                                                \
    (_ring)->cons_head = 0;                                                \
    (_ring)->cons_tail = 0;                                                \
    (_ring)->capacity = (uint32_t)(sizeof((_ring)->buf) / sizeof((_ring)->buf[0])); \
    (_ring)->mask = (_ring)->capacity - 1;                                 \
} while (0)

#define urpc_mpmc_ring_reset(_ring) do {                                   \
    urpc_mpmc_ring_init(_ring);                                            \
    memset((_ring)->buf, 0, sizeof((_ring)->buf));                         \
} while (0)

/* --------------------------------------------------------------------------
 * Spin-wait helper (internal)
 * -------------------------------------------------------------------------- */

#define URPC_MPMC_WAIT_TAIL(_tail, _head) do {                             \
    uint32_t _spin = 0;                                                    \
    while (__atomic_load_n((_tail), __ATOMIC_ACQUIRE) != (_head)) {        \
        if (++_spin > URPC_MPMC_SPIN_THRESHOLD) {                          \
            sched_yield();                                                 \
            _spin = 0;                                                     \
        }                                                                  \
        URPC_MPMC_PAUSE();                                                 \
    }                                                                      \
} while (0)

/* --------------------------------------------------------------------------
 * Enqueue
 * -------------------------------------------------------------------------- */

/*
 * Try enqueue — returns false if full.
 * elem_ptr: pointer to the element to copy into the ring.
 */
#define urpc_mpmc_try_enqueue(_ring, _elem_ptr) ({                         \
    typeof(_ring) _r = (_ring);                                            \
    typeof(_elem_ptr) _e = (_elem_ptr);                                    \
    bool _ok = false;                                                      \
    uint64_t _head;                                                        \
    uint64_t _tail_snap;                                                   \
    _head = __atomic_load_n(&(_r)->prod_head, __ATOMIC_RELAXED);           \
    do {                                                                    \
        _tail_snap = __atomic_load_n(&(_r)->cons_tail, __ATOMIC_ACQUIRE);  \
        if (_head - _tail_snap >= (_r)->capacity) {                         \
            break;  /* full */                                              \
        }                                                                   \
    } while (!__atomic_compare_exchange_n(                                  \
        &(_r)->prod_head, &_head, _head + 1,                               \
        false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED));                       \
    if (_head - _tail_snap < (_r)->capacity) {                              \
        memcpy(&(_r)->buf[_head & (_r)->mask], _e, sizeof(*_e));            \
        URPC_MPMC_WAIT_TAIL(&(_r)->prod_tail, _head);                      \
        __atomic_store_n(&(_r)->prod_tail, _head + 1, __ATOMIC_RELEASE);   \
        _ok = true;                                                         \
    }                                                                       \
    _ok;                                                                    \
})

/*
 * Blocking enqueue — spins until a slot is available.
 */
#define urpc_mpmc_enqueue(_ring, _elem_ptr) do {                           \
    while (!urpc_mpmc_try_enqueue((_ring), (_elem_ptr)))                   \
        ;                                                                  \
} while (0)

/* --------------------------------------------------------------------------
 * Dequeue
 * -------------------------------------------------------------------------- */

/*
 * Try dequeue — returns false if empty.
 * out_ptr: pointer to the output buffer.
 */
#define urpc_mpmc_try_dequeue(_ring, _out_ptr) ({                          \
    typeof(_ring) _r = (_ring);                                            \
    typeof(_out_ptr) _o = (_out_ptr);                                      \
    bool _ok = false;                                                      \
    uint64_t _head;                                                        \
    _head = __atomic_load_n(&(_r)->cons_head, __ATOMIC_RELAXED);           \
    do {                                                                    \
        if (_head >= __atomic_load_n(&(_r)->prod_tail, __ATOMIC_ACQUIRE)) { \
            break;  /* empty */                                             \
        }                                                                   \
    } while (!__atomic_compare_exchange_n(                                  \
        &(_r)->cons_head, &_head, _head + 1,                               \
        false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED));                       \
    if (_head < __atomic_load_n(&(_r)->prod_tail, __ATOMIC_ACQUIRE)) {      \
        memcpy(_o, &(_r)->buf[_head & (_r)->mask], sizeof(*_o));            \
        URPC_MPMC_WAIT_TAIL(&(_r)->cons_tail, _head);                      \
        __atomic_store_n(&(_r)->cons_tail, _head + 1, __ATOMIC_RELEASE);   \
        _ok = true;                                                         \
    }                                                                       \
    _ok;                                                                    \
})

/* --------------------------------------------------------------------------
 * Query (approximate under concurrency)
 * -------------------------------------------------------------------------- */

#define urpc_mpmc_is_empty(_ring) ({                                       \
    typeof(_ring) _r = (_ring);                                            \
    __atomic_load_n(&(_r)->cons_tail, __ATOMIC_ACQUIRE) >=                 \
    __atomic_load_n(&(_r)->prod_tail, __ATOMIC_ACQUIRE);                   \
})

#define urpc_mpmc_is_full(_ring) ({                                        \
    typeof(_ring) _r = (_ring);                                            \
    __atomic_load_n(&(_r)->prod_head, __ATOMIC_ACQUIRE) -                  \
    __atomic_load_n(&(_r)->cons_tail, __ATOMIC_ACQUIRE) >=                 \
    (_r)->capacity;                                                         \
})

#define urpc_mpmc_count(_ring) ({                                          \
    typeof(_ring) _r = (_ring);                                            \
    uint64_t _pt = __atomic_load_n(&(_r)->prod_tail, __ATOMIC_ACQUIRE);    \
    uint64_t _ct = __atomic_load_n(&(_r)->cons_tail, __ATOMIC_ACQUIRE);    \
    (uint32_t)(_pt >= _ct ? _pt - _ct : 0);                                \
})

#define urpc_mpmc_capacity(_ring) ((_ring)->capacity)

#ifdef __cplusplus
}
#endif

#endif /* URPC_MPMC_RING_H */
