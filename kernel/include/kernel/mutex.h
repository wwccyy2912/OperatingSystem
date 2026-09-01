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
 * mutex.h - Kernel mutexes (blocking, FIFO, non-recursive)
 * Copyright (c) 2026 OpSys Project
 *
 * A kernel mutex is a blocking lock with an owner and a FIFO wait
 * queue.  Unlike a spinlock it never busy-waits: a contending thread
 * blocks in the scheduler until the owner unlocks or the mutex is
 * destroyed.  Ownership is handed off directly to the next waiter on
 * unlock (no wake-then-recontend race).
 *
 * Handles are u32 values 1..MAX_MUTEXES (0 = invalid), allocated from
 * a static table like IPC ports.
 *
 * Lock discipline (hard rule): NEVER call thread_yield /
 * sched_reschedule / sched_sleep while holding the global mutex
 * spinlock -- blocking with IF=0 is a guaranteed deadlock.  Every block
 * path releases the spinlock first, then yields, then re-acquires on
 * wakeup.
 */

#ifndef KERNEL_MUTEX_H
#define KERNEL_MUTEX_H

#include <kernel/types.h>
#include <kernel/thread.h>

#define MAX_MUTEXES 256

/* Kernel mutex object (static table entry) */
typedef struct mutex {
    u32       mutex_id;   /* Handle (index + 1) */
    tid_t     owner_tid;  /* Owning thread, -1 when free */
    thread_t *wait_queue; /* FIFO queue of blocked waiters */
    bool      in_use;
} mutex_t;

/**
 * Initialize the mutex subsystem.
 */
void MutexInit(void);

/**
 * Create a new mutex.
 * @return Handle (>= 1), or 0 when the table is full (0 is an invalid
 *         handle -- mutex_lookup rejects it -- so it is a safe sentinel).
 */
u32 MutexCreate(void);

/**
 * Acquire a mutex, blocking until it is free.
 * @param handle  Mutex handle.
 * @return OK, ERR_NOENT (bad/stale handle), ERR_BUSY (already owner).
 */
error_t MutexLock(u32 handle);

/**
 * Release a mutex, handing ownership to the next FIFO waiter.
 * @param handle  Mutex handle.
 * @return OK, ERR_NOENT (bad handle), ERR_DENIED (not the owner).
 */
error_t MutexUnlock(u32 handle);

/**
 * Destroy a mutex: wake every waiter with ERR_NOENT and free the slot.
 * @param handle  Mutex handle.
 */
void MutexDestroy(u32 handle);

/**
 * Release every mutex held by thread t (hand off to next waiter, or
 * free if none).  Called from thread_exit so a dying owner never
 * strands blocked waiters.
 * @param t  Thread that is exiting.
 */
void MutexReleaseAll(thread_t *t);

/**
 * Remove t from any mutex FIFO wait queue it is blocked on (called
 * from the signal kill path when force-terminating a blocked thread).
 * A stale queue entry would later hand the mutex to an exited thread.
 * The woken MutexLock() sees owner != cur and returns ERR_NOENT.
 * Caller must NOT hold s_mutex_lock.
 * @param t  Thread possibly blocked on a mutex (no-op otherwise).
 */
void MutexAbortWait(thread_t *t);

#endif /* KERNEL_MUTEX_H */
