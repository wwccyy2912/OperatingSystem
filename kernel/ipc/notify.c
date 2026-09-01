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
 * notify.c - Async notification (seL4-style signal/wait)
 * Copyright (c) 2026 OpSys Project
 *
 * Per-thread async notification bitmasks.  Notify() ORs bits into a
 * target thread's pending_signals; WaitNotification() blocks the
 * caller until matching bits arrive.  Wakeups reuse the exact same
 * mechanism as IPC (deliver_to_waiter in ipc.c): state is set to
 * THREAD_STATE_READY + SchedEnqueue().
 *
 * v0.2: Single-CPU, cli/sti spinlock (s_notify_lock) protects the
 * pending/wait bitmasks.  Notify() is also called from IRQ context
 * (irq_handle); the lock restores the saved IF flag, so interrupt
 * context (IF=0 on entry) stays masked and syscall context resumes
 * with IF re-enabled -- no interrupt is ever spuriously enabled.
 *
 * Lock discipline: never call thread_yield/sched_sleep while holding
 * the lock (blocking with IF=0 deadlocks).  WaitNotification() drops
 * the lock before thread_yield and re-acquires on wakeup. *
 * ------------------------------------------------------------------
 * Structure (notify):
 *   NotifySet/NotifyClear/NotifyWait on per-thread notification bits
 *   (interrupts and IPC completions post bits).
 * How it works:
 *   A thread waits on a mask of bits; set bits wake it; cleared bits
 *   re-arm the wait.
 * Purpose:
 *   Asynchronous wakeup primitive for IRQ and IPC events.
 * Caveats:
 *   Bits are edge-ish latched: must clear before re-waiting to avoid
 *   missed/lost wakeups.
 * ------------------------------------------------------------------
 */
#include <kernel/notify.h>
#include <kernel/thread.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>

static spinlock_t s_notify_lock = SPINLOCK_INIT;

/* Wake a thread only if it is still blocked (SchedEnqueue() sets
 * state=READY unconditionally -- waking a READY thread double-inserts
 * it into the CFS tree). */
static void NotifyWake(thread_t *t) {
    if (t && t->state == THREAD_STATE_BLOCKED) {
        t->state = THREAD_STATE_READY;
        SchedEnqueue(t);
    }
}

/*
 * Send an async notification to a target thread.
 * The signal bits are ORed into pending_signals.  If the target is
 * currently blocked on a notification (wait_mask != 0) and the pending
 * bits now match its wait mask, it is woken (state READY + enqueue).
 */
error_t Notify(tid_t target_tid, u32 mask) {
    SpinLock(&s_notify_lock);

    thread_t *target = thread_get(target_tid);
    if (!target) {
        SpinUnlock(&s_notify_lock);
        return ERR_NOENT;
    }

    target->pending_signals |= mask;

    /* Wake the target only when matched bits are non-zero */
    if (target->wait_mask != 0 && (target->pending_signals & target->wait_mask) != 0) {
        NotifyWake(target);
    }

    SpinUnlock(&s_notify_lock);
    return OK;
}

/*
 * Wait for (or poll) a notification bitmask.
 * If signals already match (or mask == 0 for poll semantics), consume
 * pending_signals immediately and return the previous word.  Otherwise
 * block the current thread (THREAD_STATE_BLOCKED + thread_yield, the
 * same pattern ipc.c uses for synchronous port recv); on wakeup the
 * consumed word is returned.
 */
u32 WaitNotification(u32 mask) {
    thread_t *cur = thread_current();
    if (!cur)
        return 0;

    SpinLock(&s_notify_lock);

    /* Fast path: poll (mask == 0) or signals already pending */
    if (mask == 0 || (cur->pending_signals & mask) != 0) {
        u32 val              = cur->pending_signals;
        cur->pending_signals = 0;
        SpinUnlock(&s_notify_lock);
        return val;
    }

    /* Block until Notify() delivers a matching signal.
     *
     * Critical section (lock held, IF=0): wait_mask, state and the
     * ready-tree dequeue must be atomic w.r.t. the timer IRQ.  If a
     * BLOCKED thread stayed in the ready tree, Reschedule()'s
     * pick_next() (which does NOT check state) could force-pick it as
     * `next`, mark it RUNNING and switch into it -- a spurious wakeup
     * with no signals pending.  Mirror SchedSleep()'s dequeue. */
    cur->wait_mask = mask;
    cur->state     = THREAD_STATE_BLOCKED;
    SchedDequeue(cur);
    SpinUnlock(&s_notify_lock);
    ThreadYield();

    /* Woken by Notify(): clear the wait and consume the word */
    SpinLock(&s_notify_lock);
    cur->wait_mask       = 0;
    u32 val              = cur->pending_signals;
    cur->pending_signals = 0;
    SpinUnlock(&s_notify_lock);
    return val;
}
