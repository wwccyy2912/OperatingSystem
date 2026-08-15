/*
 * stdatomic.h - Atomic operations (C11 §7.17)
 * Copyright (c) 2026 OpSys Project
 *
 * Implemented entirely via GCC __atomic builtins — no .c file needed.
 * The builtins emit lock-prefixed instructions on x86 for sizes <= 8
 * and call libatomic helpers for larger sizes (not used in v0.1).
 */

#ifndef LIBC_STDATOMIC_H
#define LIBC_STDATOMIC_H

#include <stddef.h>
#include <stdint.h>

/* ====================================================================
 * Memory ordering (C11 §7.17.3)
 * ==================================================================== */

typedef enum {
        memory_order_relaxed = __ATOMIC_RELAXED,
        memory_order_consume = __ATOMIC_CONSUME,
        memory_order_acquire = __ATOMIC_ACQUIRE,
        memory_order_release = __ATOMIC_RELEASE,
        memory_order_acq_rel = __ATOMIC_ACQ_REL,
        memory_order_seq_cst = __ATOMIC_SEQ_CST
} memory_order;

/* ====================================================================
 * Atomic type aliases (C11 §7.17.6)
 * ==================================================================== */

#define _Atomic_type(T)  T _Atomic

typedef _Atomic(_Bool)              atomic_bool;
typedef _Atomic(char)               atomic_char;
typedef _Atomic(signed char)        atomic_schar;
typedef _Atomic(unsigned char)      atomic_uchar;
typedef _Atomic(short)              atomic_short;
typedef _Atomic(unsigned short)     atomic_ushort;
typedef _Atomic(int)                atomic_int;
typedef _Atomic(unsigned int)       atomic_uint;
typedef _Atomic(long)               atomic_long;
typedef _Atomic(unsigned long)      atomic_ulong;
typedef _Atomic(long long)          atomic_llong;
typedef _Atomic(unsigned long long) atomic_ullong;
typedef _Atomic(wchar_t)            atomic_wchar_t;
typedef _Atomic(int_least8_t)       atomic_int_least8_t;
typedef _Atomic(uint_least8_t)      atomic_uint_least8_t;
typedef _Atomic(int_least16_t)      atomic_int_least16_t;
typedef _Atomic(uint_least16_t)     atomic_uint_least16_t;
typedef _Atomic(int_least32_t)      atomic_int_least32_t;
typedef _Atomic(uint_least32_t)     atomic_uint_least32_t;
typedef _Atomic(int_least64_t)      atomic_int_least64_t;
typedef _Atomic(uint_least64_t)     atomic_uint_least64_t;
typedef _Atomic(int_fast8_t)        atomic_int_fast8_t;
typedef _Atomic(uint_fast8_t)       atomic_uint_fast8_t;
typedef _Atomic(int_fast16_t)       atomic_int_fast16_t;
typedef _Atomic(uint_fast16_t)      atomic_uint_fast16_t;
typedef _Atomic(int_fast32_t)       atomic_int_fast32_t;
typedef _Atomic(uint_fast32_t)      atomic_uint_fast32_t;
typedef _Atomic(int_fast64_t)       atomic_int_fast64_t;
typedef _Atomic(uint_fast64_t)      atomic_uint_fast64_t;
typedef _Atomic(intptr_t)           atomic_intptr_t;
typedef _Atomic(uintptr_t)          atomic_uintptr_t;
typedef _Atomic(size_t)             atomic_size_t;
typedef _Atomic(ptrdiff_t)          atomic_ptrdiff_t;
typedef _Atomic(intmax_t)           atomic_intmax_t;
typedef _Atomic(uintmax_t)          atomic_uintmax_t;

/* ====================================================================
 * Atomic flag (C11 §7.17.8)
 * ==================================================================== */

typedef struct {
        _Atomic _Bool value;
} atomic_flag;

#define ATOMIC_FLAG_INIT  { 0 }

#define atomic_flag_test_and_set(p) \
        __atomic_test_and_set(&(p)->value, __ATOMIC_SEQ_CST)
#define atomic_flag_test_and_set_explicit(p, mo) \
        __atomic_test_and_set(&(p)->value, (mo))
#define atomic_flag_clear(p) \
        __atomic_clear(&(p)->value, __ATOMIC_SEQ_CST)
#define atomic_flag_clear_explicit(p, mo) \
        __atomic_clear(&(p)->value, (mo))

/* ====================================================================
 * Lock-free property (C11 §7.17.5)
 * ==================================================================== */

#define ATOMIC_BOOL_LOCK_FREE       2
#define ATOMIC_CHAR_LOCK_FREE       2
#define ATOMIC_CHAR16_T_LOCK_FREE   2
#define ATOMIC_CHAR32_T_LOCK_FREE   2
#define ATOMIC_WCHAR_T_LOCK_FREE    2
#define ATOMIC_SHORT_LOCK_FREE      2
#define ATOMIC_INT_LOCK_FREE        2
#define ATOMIC_LONG_LOCK_FREE       2
#define ATOMIC_LLONG_LOCK_FREE      2
#define ATOMIC_POINTER_LOCK_FREE    2

/* ====================================================================
 * Generic atomic operations (C11 §7.17.7)
 * ==================================================================== */

#define atomic_init(p, v)   __atomic_store_n(p, v, __ATOMIC_RELAXED)
#define atomic_load(p)      __atomic_load_n(p, __ATOMIC_SEQ_CST)
#define atomic_store(p, v)  __atomic_store_n(p, v, __ATOMIC_SEQ_CST)
#define atomic_exchange(p, v) \
        __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST)
#define atomic_load_explicit(p, mo)   __atomic_load_n(p, mo)
#define atomic_store_explicit(p, v, mo) __atomic_store_n(p, v, mo)
#define atomic_exchange_explicit(p, v, mo) __atomic_exchange_n(p, v, mo)

#define atomic_compare_exchange_strong(p, expected, desired) \
        __atomic_compare_exchange_n(p, expected, desired, 0, \
                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
#define atomic_compare_exchange_weak(p, expected, desired) \
        __atomic_compare_exchange_n(p, expected, desired, 1, \
                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
#define atomic_compare_exchange_strong_explicit(p, expected, desired, succ, fail) \
        __atomic_compare_exchange_n(p, expected, desired, 0, succ, fail)
#define atomic_compare_exchange_weak_explicit(p, expected, desired, succ, fail) \
        __atomic_compare_exchange_n(p, expected, desired, 1, succ, fail)

#define atomic_fetch_add(p, v) \
        __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST)
#define atomic_fetch_sub(p, v) \
        __atomic_fetch_sub(p, v, __ATOMIC_SEQ_CST)
#define atomic_fetch_or(p, v)  \
        __atomic_fetch_or(p, v, __ATOMIC_SEQ_CST)
#define atomic_fetch_xor(p, v) \
        __atomic_fetch_xor(p, v, __ATOMIC_SEQ_CST)
#define atomic_fetch_and(p, v) \
        __atomic_fetch_and(p, v, __ATOMIC_SEQ_CST)

#define atomic_fetch_add_explicit(p, v, mo) __atomic_fetch_add(p, v, mo)
#define atomic_fetch_sub_explicit(p, v, mo) __atomic_fetch_sub(p, v, mo)
#define atomic_fetch_or_explicit(p, v, mo)  __atomic_fetch_or(p, v, mo)
#define atomic_fetch_xor_explicit(p, v, mo) __atomic_fetch_xor(p, v, mo)
#define atomic_fetch_and_explicit(p, v, mo) __atomic_fetch_and(p, v, mo)

/* ====================================================================
 * Fences (C11 §7.17.4)
 * ==================================================================== */

#define atomic_thread_fence(mo)   __atomic_thread_fence(mo)
#define atomic_signal_fence(mo)   __atomic_signal_fence(mo)

/* ====================================================================
 * Convenience: atomic_var_init (TS 18037 / deprecated in C11)
 * ==================================================================== */

#define ATOMIC_VAR_INIT(v)  (v)

#endif /* LIBC_STDATOMIC_H */
