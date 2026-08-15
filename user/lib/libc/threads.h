/*
 * threads.h - C11 threads (§7.26)
 * Copyright (c) 2026 OpSys Project
 *
 * User-space C11 <threads.h> backed by the kernel thread syscalls
 * (thread_create / thread_yield / thread_exit) and get_time() for
 * timed waits.  Mutexes, condition variables and once-flags are built
 * on lock-free atomics; TSS is process-wide until per-thread TLS lands
 * (v1.0+).
 *
 * v0.1 limitations (user-space TLS not yet available):
 *   - thrd_current() returns a zero tid; thrd_equal() therefore only
 *     distinguishes threads created via thrd_create().
 *   - mtx_recursive owner check uses thrd_current(), so re-entrancy is
 *     only safe within a single thread of control.
 *   - tss_get/tss_set address process-wide slots, not per-thread ones.
 */

#ifndef LIBC_THREADS_H
#define LIBC_THREADS_H

#include <stdatomic.h>
#include <stdint.h>
#include <time.h>      /* struct timespec */

/* ====================================================================
 * Macros (C11 §7.26.1)
 * ==================================================================== */

#ifndef thread_local
#define thread_local        _Thread_local
#endif

#define ONCE_FLAG_INIT      { 0 }
#define TSS_DTOR_ITERATIONS 4

/* ====================================================================
 * Types (C11 §7.26.1)
 * ==================================================================== */

/*
 * thrd_t holds the kernel thread id plus a pointer to the shared
 * completion state.  The indirection is required because thrd_join()
 * and thrd_detach() take thrd_t by value per C11 — a copy must still
 * observe the done/result fields the spawned thread writes.
 */
struct thrd_state;
typedef struct {
    uint64_t            tid;   /* kernel thread id */
    struct thrd_state  *st;    /* heap-allocated done/result state */
} thrd_t;

typedef int (*thrd_start_t)(void *);

/* Mutex: spinlock with optional recursion. */
typedef struct {
    _Atomic int flag;       /* 0 = unlocked, 1 = locked */
    int          type;      /* mtx_plain | mtx_timed | mtx_try | mtx_recursive */
    int          recursion; /* re-lock count (recursive mutexes) */
    uint64_t     owner;     /* owning thread tid (recursive mutexes) */
} mtx_t;

/* Condition variable: counter-based, woken via thread_yield polling. */
typedef struct {
    _Atomic int waiters;    /* threads blocked in cnd_wait */
    _Atomic int generation; /* bumped to release waiters */
} cnd_t;

typedef struct {
    _Atomic int state;      /* 0 = not called, 1 = in progress, 2 = done */
} once_flag;

typedef int tss_t;
typedef void (*tss_dtor_t)(void *);

/* ====================================================================
 * Enumeration constants (C11 §7.26.2)
 * ==================================================================== */

enum {
    thrd_success  = 0,
    thrd_busy     = 1,
    thrd_error    = 2,
    thrd_nomem    = 3,
    thrd_timedout = 4
};

enum {
    mtx_plain     = 0,
    mtx_timed     = 1,
    mtx_try       = 2,
    mtx_recursive = 4
};

/* ====================================================================
 * Threads (C11 §7.26.3)
 * ==================================================================== */

int    thrd_create(thrd_t *thr, thrd_start_t func, void *arg);
int    thrd_equal(thrd_t thr0, thrd_t thr1);
thrd_t thrd_current(void);
int    thrd_sleep(const struct timespec *duration,
                  struct timespec *remaining);
void   thrd_yield(void);
_Noreturn void thrd_exit(int res);
int    thrd_detach(thrd_t thr);
int    thrd_join(thrd_t thr, int *res);

/* ====================================================================
 * Mutexes (C11 §7.26.4)
 * ==================================================================== */

int  mtx_init(mtx_t *mtx, int type);
int  mtx_lock(mtx_t *mtx);
int  mtx_timedlock(mtx_t *mtx, const struct timespec *ts);
int  mtx_trylock(mtx_t *mtx);
int  mtx_unlock(mtx_t *mtx);
void mtx_destroy(mtx_t *mtx);

/* ====================================================================
 * Condition variables (C11 §7.26.5)
 * ==================================================================== */

int  cnd_init(cnd_t *cnd);
int  cnd_signal(cnd_t *cnd);
int  cnd_broadcast(cnd_t *cnd);
int  cnd_wait(cnd_t *cnd, mtx_t *mtx);
int  cnd_timedwait(cnd_t *cnd, mtx_t *mtx, const struct timespec *ts);
void cnd_destroy(cnd_t *cnd);

/* ====================================================================
 * Once (C11 §7.26.6)
 * ==================================================================== */

void call_once(once_flag *flag, void (*func)(void));

/* ====================================================================
 * Thread-specific storage (C11 §7.26.6)
 * ==================================================================== */

int   tss_create(tss_t *key, tss_dtor_t dtor);
void *tss_get(tss_t key);
int   tss_set(tss_t key, void *val);
void  tss_delete(tss_t key);

#endif /* LIBC_THREADS_H */
