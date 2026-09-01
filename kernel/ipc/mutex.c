/*
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details: <https://www.gnu.org/licenses/>.
 *
 * mutex.c - Kernel mutexes (blocking, FIFO, non-recursive)
 * Copyright (c) 2026 OpSys Project
 *
 * Single-CPU design, protected by a global cli/sti spinlock
 * (s_mutex_lock).  A mutex has an owner and a FIFO wait queue.
 *
 * Handoff semantics: MutexUnlock() transfers ownership directly to the
 * next waiter BEFORE waking it, so the waiter never re-contends -- there
 * is no window in which a third thread could steal the lock.
 *
 * Lock discipline (hard rule): NEVER call thread_yield / sched_tick while
 * holding s_mutex_lock -- blocking with IF=0 is a guaranteed deadlock.
 * Every block path releases the spinlock first, yields, then re-acquires
 * on wakeup.
 *
 * Exit handoff: ThreadExit() calls MutexReleaseAll() so a dying owner
 * hands every held mutex to the next waiter (or frees it), guaranteeing
 * blocked waiters are never stranded. *
 * ------------------------------------------------------------------
 * Structure (mutex):
 *   mutex_t { held, owner, waiters } -> MutexLock (spin brief, then
 *   sleep on a wait list) / MutexUnlock (wake one).
 * How it works:
 *   Fast path: try-lock with cmpxchg; contention parks the caller on
 *   the kernel wait queue (scheduler yields).
 * Purpose:
 *   Kernel-internal locking that does not busy-wait under contention.
 * Caveats:
 *   Must be held for short, non-blocking critical sections; no
 *   recursive acquisition (owner not re-entrant).
 * ------------------------------------------------------------------
 */
#include <kernel/mutex.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>

static spinlock_t s_mutex_lock = SPINLOCK_INIT;
static mutex_t    s_mutex_table[MAX_MUTEXES];

/* ---- Helpers (caller holds s_mutex_lock) ---- */

static mutex_t *mutex_lookup(u32 handle) {
    if (handle == 0 || handle > MAX_MUTEXES)
        return NULL;
    mutex_t *m = &s_mutex_table[handle - 1];
    return m->in_use ? m : NULL;
}

/* Record that t now holds handle (idempotent). */
static void HeldAdd(thread_t *t, u32 handle) {
    for (u8 i = 0; i < t->held_mutex_count; i++) {
        if (t->held_mutexes[i] == handle)
            return;
    }
    if (t->held_mutex_count < MAX_HELD_MUTEXES)
        t->held_mutexes[t->held_mutex_count++] = handle;
}

/* Drop handle from t's held list. */
static void HeldRemove(thread_t *t, u32 handle) {
    for (u8 i = 0; i < t->held_mutex_count; i++) {
        if (t->held_mutexes[i] == handle) {
            t->held_mutexes[i] = t->held_mutexes[t->held_mutex_count - 1];
            t->held_mutex_count--;
            return;
        }
    }
}

/* Wake a waiter only if still blocked (sched_enqueue sets state=READY
 * unconditionally -- waking twice would double-insert into the CFS tree). */
static void MutexWake(thread_t *t) {
    if (t && t->state == THREAD_STATE_BLOCKED) {
        t->next  = NULL;
        t->state = THREAD_STATE_READY;
        SchedEnqueue(t);
    }
}

/* ------------------------------------------------------------------ */

void MutexInit(void) {
    /* Static table is BSS-zeroed: in_use=false everywhere. */
}

u32 MutexCreate(void) {
    SpinLock(&s_mutex_lock);
    for (u32 i = 0; i < MAX_MUTEXES; i++) {
        if (!s_mutex_table[i].in_use) {
            mutex_t *m    = &s_mutex_table[i];
            m->mutex_id   = i + 1;
            m->owner_tid  = -1;
            m->wait_queue = NULL;
            m->in_use     = true;
            SpinUnlock(&s_mutex_lock);
            return m->mutex_id;
        }
    }
    SpinUnlock(&s_mutex_lock);
    /* Table full: return 0 (invalid handle).  NOT (u32)ERR_NOMEM -- that
     * truncates to 0xFFFFFFFF which sign-extends to +4294967295 at the
     * syscall boundary and is indistinguishable from a valid handle. */
    return 0;
}

error_t MutexLock(u32 handle) {
    SpinLock(&s_mutex_lock);
    mutex_t *m = mutex_lookup(handle);
    if (!m) {
        SpinUnlock(&s_mutex_lock);
        return ERR_NOENT;
    }

    thread_t *cur = thread_current();

    /* Non-recursive: re-lock by the owner is rejected */
    if (m->owner_tid == cur->tid) {
        SpinUnlock(&s_mutex_lock);
        return ERR_BUSY;
    }

    /* Free -- acquire immediately */
    if (m->owner_tid < 0) {
        m->owner_tid = cur->tid;
        HeldAdd(cur, handle);
        SpinUnlock(&s_mutex_lock);
        return OK;
    }

    /* Contended -- FIFO enqueue and block */
    cur->next     = NULL;
    thread_t **pp = &m->wait_queue;
    while (*pp)
        pp = &(*pp)->next;
    *pp = cur;

    /* Lock held (IF=0): state change + ready-tree dequeue are atomic
     * w.r.t. the timer IRQ.  A BLOCKED thread must not stay in the
     * ready tree or Reschedule() can force-pick it as `next`. */
    cur->state = THREAD_STATE_BLOCKED;
    SchedDequeue(cur);
    SpinUnlock(&s_mutex_lock);
    ThreadYield(); /* returns when we are woken */
    SpinLock(&s_mutex_lock);

    /* Woken: either handed off (owner == cur) or the mutex was
     * destroyed / slot reused (lookup fails or owner differs). */
    m = mutex_lookup(handle);
    if (m && m->owner_tid == cur->tid) {
        HeldAdd(cur, handle);
        SpinUnlock(&s_mutex_lock);
        return OK;
    }
    SpinUnlock(&s_mutex_lock);
    return ERR_NOENT;
}

error_t MutexUnlock(u32 handle) {
    SpinLock(&s_mutex_lock);
    mutex_t *m = mutex_lookup(handle);
    if (!m) {
        SpinUnlock(&s_mutex_lock);
        return ERR_NOENT;
    }

    thread_t *cur = thread_current();
    if (m->owner_tid != cur->tid) {
        SpinUnlock(&s_mutex_lock);
        return ERR_DENIED;
    }

    HeldRemove(cur, handle);

    /* Hand ownership directly to the next FIFO waiter, or free */
    thread_t *w = m->wait_queue;
    if (w) {
        m->wait_queue = w->next;
        m->owner_tid  = w->tid;
        MutexWake(w);
    } else {
        m->owner_tid = -1;
    }

    SpinUnlock(&s_mutex_lock);
    return OK;
}

void MutexDestroy(u32 handle) {
    SpinLock(&s_mutex_lock);
    mutex_t *m = mutex_lookup(handle);
    if (!m) {
        SpinUnlock(&s_mutex_lock);
        return;
    }

    /* Drop from the owner's held list so exit handoff skips it */
    if (m->owner_tid >= 0) {
        thread_t *owner = thread_get(m->owner_tid);
        if (owner)
            HeldRemove(owner, handle);
    }

    /* Wake every waiter -- they observe the freed slot / new owner and
     * return ERR_NOENT */
    thread_t *w = m->wait_queue;
    while (w) {
        thread_t *n = w->next;
        w->next     = NULL;
        MutexWake(w);
        w = n;
    }
    m->wait_queue = NULL;
    m->owner_tid  = -1;
    m->in_use     = false;

    SpinUnlock(&s_mutex_lock);
}

void MutexReleaseAll(thread_t *t) {
    if (!t)
        return;

    SpinLock(&s_mutex_lock);
    while (t->held_mutex_count > 0) {
        u32      handle = t->held_mutexes[t->held_mutex_count - 1];
        mutex_t *m      = mutex_lookup(handle);
        if (m && m->owner_tid == t->tid) {
            thread_t *w = m->wait_queue;
            if (w) {
                m->wait_queue = w->next;
                m->owner_tid  = w->tid;
                MutexWake(w);
            } else {
                m->owner_tid = -1;
            }
        }
        t->held_mutex_count--;
    }
    SpinUnlock(&s_mutex_lock);
}

/*
 * Remove t from any mutex FIFO wait queue it is blocked on (called
 * from the signal kill path when force-terminating a blocked thread).
 * A stale queue entry would later hand the mutex to an exited thread;
 * the woken MutexLock() sees owner != cur and returns ERR_NOENT.
 * Caller must NOT hold s_mutex_lock.
 */
void MutexAbortWait(thread_t *t) {
    if (!t)
        return;

    SpinLock(&s_mutex_lock);
    for (u32 i = 0; i < MAX_MUTEXES; i++) {
        mutex_t *m = &s_mutex_table[i];
        if (!m->in_use)
            continue;
        thread_t **pp = &m->wait_queue;
        while (*pp) {
            if (*pp == t) {
                *pp     = t->next;
                t->next = NULL;
                break;
            }
            pp = &(*pp)->next;
        }
    }
    SpinUnlock(&s_mutex_lock);
}
