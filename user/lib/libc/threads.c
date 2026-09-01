/*
 * threads.c - C11 threads implementation (§7.26)
 * Copyright (c) 2026 OpSys Project
 *
 * Backs the <threads.h> API with the kernel thread syscalls
 * (SYS_THREAD_CREATE / SYS_THREAD_YIELD / SYS_THREAD_EXIT) and
 * SYS_GET_TIME for timed waits.  Mutexes, condition variables and
 * once-flags are built on lock-free atomics; TSS is process-wide
 * until per-thread TLS lands (v1.0+).
 *
 * v0.1 limitations (user-space TLS not yet available):
 *   - thrd_current() returns a zero tid; thrd_equal() therefore only
 *     distinguishes threads created via thrd_create().
 *   - mtx_recursive owner check uses thrd_current(), so re-entrancy is
 *     only safe within a single thread of control.
 *   - tss_get/tss_set address process-wide slots, not per-thread ones.
 *   - mtx_timedlock/cnd_timedwait interpret `ts` as a RELATIVE
 *     duration; C11 specifies an absolute TIME_UTC point, but the
 *     kernel exposes no epoch mapping yet.
 
 *
 * ------------------------------------------------------------------
 * Structure (threads):
 *   thrd_create/join/exit + mtx/cnd/tss/once -> kernel syscall
 *   wrappers (ThreadCreate/ThreadExit/MutexLock/...) — thin C11 shims.
 * How it works:
 *   Each C11 call packs arguments and issues the matching syscall;
 *   mutexes/conditions map onto the kernel's blocking primitives.
 * Purpose:
 *   C11 threads.h concurrency API on top of the microkernel.
 * Caveats:
 *   thrd_sleep uses the kernel tick clock; cnd_timedwait needs a
 *   deadline in ticks.
 * ------------------------------------------------------------------
 */

#include "threads.h"
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <malloc.h>
#include "../libos/syscalls.h"

/* ====================================================================
 * Internal: per-thread completion state
 *
 * Heap-allocated so that thrd_t copies (thrd_join/thrd_detach take
 * thrd_t by value per C11) observe the same live state the wrapper
 * thread writes to.
 * ==================================================================== */

struct thrd_state {
    _Atomic int  done;   /* 0 = running, 1 = finished */
    int          result; /* thread return value (valid when done) */
    thrd_start_t func;   /* user entry (set by thrd_create) */
    void        *arg;    /* user argument (set by thrd_create) */
};

/*
 * thread_create takes void(*)(void*) but thrd_start_t is int(*)(void*);
 * this trampoline bridges the signature mismatch and records the
 * result before the thread exits.
 */
static void ThrdEntryWrapper(void *p) {
    struct thrd_state *st  = (struct thrd_state *)p;
    int                res = st->func(st->arg);
    st->result             = res;
    atomic_store(&st->done, 1);
    ThreadExit(res);
}

/* Convert a (relative) timespec to kernel ticks.  The tick rate is
 * 1 kHz (CLOCKS_PER_SEC == 1000), so 1 tick == 1 ms. */
static int TimespecToTicks(const struct timespec *ts) {
    return (int)(ts->tv_sec * 1000 + ts->tv_nsec / 1000000);
}

/* ====================================================================
 * Threads (C11 §7.26.3)
 * ==================================================================== */

int thrd_create(thrd_t *thr, thrd_start_t func, void *arg) {
    struct thrd_state *st = malloc(sizeof(*st));
    if (!st)
        return thrd_nomem;
    atomic_init(&st->done, 0);
    st->result = 0;
    st->func   = func;
    st->arg    = arg;

    int tid = ThreadCreate(ThrdEntryWrapper, st, 0);
    if (tid < 0) {
        free(st);
        return (tid == ERR_NOMEM) ? thrd_nomem : thrd_error;
    }
    thr->tid = (uint64_t)tid;
    thr->st  = st;
    return thrd_success;
}

int thrd_equal(thrd_t thr0, thrd_t thr1) {
    return thr0.tid == thr1.tid;
}

thrd_t thrd_current(void) {
    thrd_t self;
    self.tid = 0;
    self.st  = NULL;
    return self;
}

int thrd_sleep(const struct timespec *duration, struct timespec *remaining) {
    if (!duration)
        return -2;

    int ticks = TimespecToTicks(duration);
    int start = GetTime();
    while (GetTime() - start < ticks)
        ThreadYield();

    if (remaining) {
        remaining->tv_sec  = 0;
        remaining->tv_nsec = 0;
    }
    return 0;
}

void thrd_yield(void) {
    ThreadYield();
}

_Noreturn void thrd_exit(int res) {
    /* ThreadExit() enters the kernel and never returns; the header
     * declares it plain void, so tell the optimizer the same. */
    ThreadExit(res);
    __builtin_unreachable();
}

int thrd_detach(thrd_t thr) {
    /* No kernel join handle to release; the completion state is left
     * for the wrapper to update.  The state is intentionally not
     * freed here — the spawned thread still writes to it before
     * exiting. */
    (void)thr;
    return thrd_success;
}

int thrd_join(thrd_t thr, int *res) {
    /* Without a kernel join syscall, poll the completion flag and
     * yield between checks.  Once `done` is observed, the wrapper has
     * finished writing `result` (the seq_cst store/load pair orders
     * the writes), so it is safe to read and reclaim the state. */
    if (thr.st) {
        while (!atomic_load(&thr.st->done))
            ThreadYield();
        if (res)
            *res = thr.st->result;
        free(thr.st);
    }
    return thrd_success;
}

/* ====================================================================
 * Mutexes (C11 §7.26.4)
 * ==================================================================== */

int mtx_init(mtx_t *mtx, int type) {
    atomic_init(&mtx->flag, 0);
    mtx->type      = type;
    mtx->recursion = 0;
    mtx->owner     = 0;
    return thrd_success;
}

int mtx_lock(mtx_t *mtx) {
    uint64_t self = thrd_current().tid;

    /* Recursive fast-path: same owner re-locking. */
    if ((mtx->type & mtx_recursive) && atomic_load(&mtx->flag) == 1 && mtx->owner == self) {
        mtx->recursion++;
        return thrd_success;
    }

    while (atomic_exchange(&mtx->flag, 1) == 1)
        ThreadYield();
    mtx->owner     = self;
    mtx->recursion = 1;
    return thrd_success;
}

int mtx_trylock(mtx_t *mtx) {
    uint64_t self = thrd_current().tid;

    if ((mtx->type & mtx_recursive) && atomic_load(&mtx->flag) == 1 && mtx->owner == self) {
        mtx->recursion++;
        return thrd_success;
    }
    if (atomic_exchange(&mtx->flag, 1) == 1)
        return thrd_busy;
    mtx->owner     = self;
    mtx->recursion = 1;
    return thrd_success;
}

int mtx_unlock(mtx_t *mtx) {
    if (mtx->type & mtx_recursive) {
        if (mtx->recursion > 1) {
            mtx->recursion--;
            return thrd_success;
        }
        mtx->owner     = 0;
        mtx->recursion = 0;
    }
    atomic_store(&mtx->flag, 0);
    return thrd_success;
}

int mtx_timedlock(mtx_t *mtx, const struct timespec *ts) {
    if (!ts)
        return thrd_error;

    int ticks = TimespecToTicks(ts);
    int start = GetTime();
    for (;;) {
        if (mtx_trylock(mtx) == thrd_success)
            return thrd_success;
        if (GetTime() - start >= ticks)
            return thrd_timedout;
        ThreadYield();
    }
}

void mtx_destroy(mtx_t *mtx) {
    (void)mtx;
}

/* ====================================================================
 * Condition variables (C11 §7.26.5)
 *
 * Waiters record their starting generation, release the mutex, and
 * spin until the generation advances (signal bumps it by 1, broadcast
 * by the waiter count).  Spurious wakeups are permitted by C11.
 * ==================================================================== */

int cnd_init(cnd_t *cnd) {
    atomic_init(&cnd->waiters, 0);
    atomic_init(&cnd->generation, 0);
    return thrd_success;
}

int cnd_signal(cnd_t *cnd) {
    if (atomic_load(&cnd->waiters) > 0)
        atomic_fetch_add(&cnd->generation, 1);
    return thrd_success;
}

int cnd_broadcast(cnd_t *cnd) {
    int w = atomic_load(&cnd->waiters);
    if (w > 0)
        atomic_fetch_add(&cnd->generation, w);
    return thrd_success;
}

int cnd_wait(cnd_t *cnd, mtx_t *mtx) {
    atomic_fetch_add(&cnd->waiters, 1);
    int gen = atomic_load(&cnd->generation);
    mtx_unlock(mtx);
    while (atomic_load(&cnd->generation) == gen)
        ThreadYield();
    atomic_fetch_sub(&cnd->waiters, 1);
    return mtx_lock(mtx);
}

int cnd_timedwait(cnd_t *cnd, mtx_t *mtx, const struct timespec *ts) {
    if (!ts)
        return thrd_error;

    int ticks = TimespecToTicks(ts);
    int start = GetTime();
    int rc    = thrd_success;

    atomic_fetch_add(&cnd->waiters, 1);
    int gen = atomic_load(&cnd->generation);
    mtx_unlock(mtx);
    while (atomic_load(&cnd->generation) == gen) {
        if (GetTime() - start >= ticks) {
            rc = thrd_timedout;
            break;
        }
        ThreadYield();
    }
    atomic_fetch_sub(&cnd->waiters, 1);
    mtx_lock(mtx);
    return rc;
}

void cnd_destroy(cnd_t *cnd) {
    (void)cnd;
}

/* ====================================================================
 * Once (C11 §7.26.6)
 * ==================================================================== */

void call_once(once_flag *flag, void (*func)(void)) {
    int expected = 0;
    /* Transition 0 -> 1: the winner runs func. */
    if (atomic_compare_exchange_strong(&flag->state, &expected, 1)) {
        func();
        atomic_store(&flag->state, 2);
        return;
    }
    /* Losers (state 1 or 2) wait until init completes.  If state is
     * already 2 the loop body never runs. */
    while (atomic_load(&flag->state) != 2)
        ThreadYield();
}

/* ====================================================================
 * Thread-specific storage (C11 §7.26.6)
 *
 * v0.1: slots are process-wide (not per-thread) until TLS support
 * lands.  tss_t is the slot index into a fixed-size array.
 * ==================================================================== */

#define TSS_SLOTS 64

struct tss_slot {
    _Atomic int used;
    tss_dtor_t  dtor;
    void       *value;
};

static struct tss_slot g_tss[TSS_SLOTS];

int tss_create(tss_t *key, tss_dtor_t dtor) {
    for (int i = 0; i < TSS_SLOTS; i++) {
        int expected = 0;
        if (atomic_compare_exchange_strong(&g_tss[i].used, &expected, 1)) {
            g_tss[i].dtor  = dtor;
            g_tss[i].value = NULL;
            *key           = (tss_t)i;
            return thrd_success;
        }
    }
    return thrd_nomem;
}

void *tss_get(tss_t key) {
    if (key < 0 || key >= TSS_SLOTS)
        return NULL;
    return g_tss[key].value;
}

int tss_set(tss_t key, void *val) {
    if (key < 0 || key >= TSS_SLOTS)
        return thrd_error;
    g_tss[key].value = val;
    return thrd_success;
}

void tss_delete(tss_t key) {
    if (key < 0 || key >= TSS_SLOTS)
        return;
    g_tss[key].used  = 0;
    g_tss[key].dtor  = NULL;
    g_tss[key].value = NULL;
}
