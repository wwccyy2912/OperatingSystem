/*
 * mutex.c - Kernel mutexes (blocking, FIFO, non-recursive)
 * Copyright (c) 2026 OpSys Project
 *
 * Single-CPU design, protected by a global cli/sti spinlock
 * (s_mutex_lock).  A mutex has an owner and a FIFO wait queue.
 *
 * Handoff semantics: mutex_unlock() transfers ownership directly to the
 * next waiter BEFORE waking it, so the waiter never re-contends -- there
 * is no window in which a third thread could steal the lock.
 *
 * Lock discipline (hard rule): NEVER call thread_yield / sched_tick while
 * holding s_mutex_lock -- blocking with IF=0 is a guaranteed deadlock.
 * Every block path releases the spinlock first, yields, then re-acquires
 * on wakeup.
 *
 * Exit handoff: thread_exit() calls mutex_release_all() so a dying owner
 * hands every held mutex to the next waiter (or frees it), guaranteeing
 * blocked waiters are never stranded.
 */

#include <kernel/mutex.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>

static spinlock_t s_mutex_lock = SPINLOCK_INIT;
static mutex_t    s_mutex_table[MAX_MUTEXES];

/* ---- Helpers (caller holds s_mutex_lock) ---- */

static mutex_t *mutex_lookup(u32 handle)
{
        if (handle == 0 || handle > MAX_MUTEXES)
                return NULL;
        mutex_t *m = &s_mutex_table[handle - 1];
        return m->in_use ? m : NULL;
}

/* Record that t now holds handle (idempotent). */
static void held_add(thread_t *t, u32 handle)
{
        for (u8 i = 0; i < t->held_mutex_count; i++) {
                if (t->held_mutexes[i] == handle)
                        return;
        }
        if (t->held_mutex_count < MAX_HELD_MUTEXES)
                t->held_mutexes[t->held_mutex_count++] = handle;
}

/* Drop handle from t's held list. */
static void held_remove(thread_t *t, u32 handle)
{
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
static void mutex_wake(thread_t *t)
{
        if (t && t->state == THREAD_STATE_BLOCKED) {
                t->next = NULL;
                t->state = THREAD_STATE_READY;
                sched_enqueue(t);
        }
}

/* ------------------------------------------------------------------ */

void mutex_init(void)
{
        /* Static table is BSS-zeroed: in_use=false everywhere. */
}

u32 mutex_create(void)
{
        spin_lock(&s_mutex_lock);
        for (u32 i = 0; i < MAX_MUTEXES; i++) {
                if (!s_mutex_table[i].in_use) {
                        mutex_t *m = &s_mutex_table[i];
                        m->mutex_id   = i + 1;
                        m->owner_tid  = -1;
                        m->wait_queue = NULL;
                        m->in_use     = true;
                        spin_unlock(&s_mutex_lock);
                        return m->mutex_id;
                }
        }
        spin_unlock(&s_mutex_lock);
        /* Table full: return 0 (invalid handle).  NOT (u32)ERR_NOMEM -- that
     * truncates to 0xFFFFFFFF which sign-extends to +4294967295 at the
     * syscall boundary and is indistinguishable from a valid handle. */
        return 0;
}

error_t mutex_lock(u32 handle)
{
        spin_lock(&s_mutex_lock);
        mutex_t *m = mutex_lookup(handle);
        if (!m) {
                spin_unlock(&s_mutex_lock);
                return ERR_NOENT;
        }

        thread_t *cur = thread_current();

        /* Non-recursive: re-lock by the owner is rejected */
        if (m->owner_tid == cur->tid) {
                spin_unlock(&s_mutex_lock);
                return ERR_BUSY;
        }

        /* Free -- acquire immediately */
        if (m->owner_tid < 0) {
                m->owner_tid = cur->tid;
                held_add(cur, handle);
                spin_unlock(&s_mutex_lock);
                return OK;
        }

        /* Contended -- FIFO enqueue and block */
        cur->next = NULL;
        thread_t **pp = &m->wait_queue;
        while (*pp)
                pp = &(*pp)->next;
        *pp = cur;

        /* Lock held (IF=0): state change + ready-tree dequeue are atomic
     * w.r.t. the timer IRQ.  A BLOCKED thread must not stay in the
     * ready tree or reschedule() can force-pick it as `next`. */
        cur->state = THREAD_STATE_BLOCKED;
        sched_dequeue(cur);
        spin_unlock(&s_mutex_lock);
        thread_yield();          /* returns when we are woken */
        spin_lock(&s_mutex_lock);

        /* Woken: either handed off (owner == cur) or the mutex was
     * destroyed / slot reused (lookup fails or owner differs). */
        m = mutex_lookup(handle);
        if (m && m->owner_tid == cur->tid) {
                held_add(cur, handle);
                spin_unlock(&s_mutex_lock);
                return OK;
        }
        spin_unlock(&s_mutex_lock);
        return ERR_NOENT;
}

error_t mutex_unlock(u32 handle)
{
        spin_lock(&s_mutex_lock);
        mutex_t *m = mutex_lookup(handle);
        if (!m) {
                spin_unlock(&s_mutex_lock);
                return ERR_NOENT;
        }

        thread_t *cur = thread_current();
        if (m->owner_tid != cur->tid) {
                spin_unlock(&s_mutex_lock);
                return ERR_DENIED;
        }

        held_remove(cur, handle);

        /* Hand ownership directly to the next FIFO waiter, or free */
        thread_t *w = m->wait_queue;
        if (w) {
                m->wait_queue = w->next;
                m->owner_tid  = w->tid;
                mutex_wake(w);
        } else {
                m->owner_tid = -1;
        }

        spin_unlock(&s_mutex_lock);
        return OK;
}

void mutex_destroy(u32 handle)
{
        spin_lock(&s_mutex_lock);
        mutex_t *m = mutex_lookup(handle);
        if (!m) {
                spin_unlock(&s_mutex_lock);
                return;
        }

        /* Drop from the owner's held list so exit handoff skips it */
        if (m->owner_tid >= 0) {
                thread_t *owner = thread_get(m->owner_tid);
                if (owner)
                        held_remove(owner, handle);
        }

        /* Wake every waiter -- they observe the freed slot / new owner and
     * return ERR_NOENT */
        thread_t *w = m->wait_queue;
        while (w) {
                thread_t *n = w->next;
                w->next = NULL;
                mutex_wake(w);
                w = n;
        }
        m->wait_queue = NULL;
        m->owner_tid  = -1;
        m->in_use     = false;

        spin_unlock(&s_mutex_lock);
}

void mutex_release_all(thread_t *t)
{
        if (!t)
                return;

        spin_lock(&s_mutex_lock);
        while (t->held_mutex_count > 0) {
                u32 handle = t->held_mutexes[t->held_mutex_count - 1];
                mutex_t *m = mutex_lookup(handle);
                if (m && m->owner_tid == t->tid) {
                        thread_t *w = m->wait_queue;
                        if (w) {
                                m->wait_queue = w->next;
                                m->owner_tid  = w->tid;
                                mutex_wake(w);
                        } else {
                                m->owner_tid = -1;
                        }
                }
                t->held_mutex_count--;
        }
        spin_unlock(&s_mutex_lock);
}

/*
 * Remove t from any mutex FIFO wait queue it is blocked on (called
 * from the signal kill path when force-terminating a blocked thread).
 * A stale queue entry would later hand the mutex to an exited thread;
 * the woken mutex_lock() sees owner != cur and returns ERR_NOENT.
 * Caller must NOT hold s_mutex_lock.
 */
void mutex_abort_wait(thread_t *t)
{
        if (!t)
                return;

        spin_lock(&s_mutex_lock);
        for (u32 i = 0; i < MAX_MUTEXES; i++) {
                mutex_t *m = &s_mutex_table[i];
                if (!m->in_use)
                        continue;
                thread_t **pp = &m->wait_queue;
                while (*pp) {
                        if (*pp == t) {
                                *pp = t->next;
                                t->next = NULL;
                                break;
                        }
                        pp = &(*pp)->next;
                }
        }
        spin_unlock(&s_mutex_lock);
}
