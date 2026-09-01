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
 * sched.h - Scheduler
 * Copyright (c) 2026 OpSys Project
 */

#ifndef KERNEL_SCHED_H
#define KERNEL_SCHED_H

#include <kernel/types.h>
#include <kernel/thread.h>

/**
 * Initialize the scheduler.
 */
void SchedInit(void);

/**
 * Add a thread to the ready queue.
 * @param t  Thread to enqueue.
 */
void SchedEnqueue(thread_t *t);

/**
 * Remove a thread from the ready queue.
 * @param t  Thread to dequeue.
 */
void SchedDequeue(thread_t *t);

/**
 * Timer tick — advance the time base (s_ticks), wake expired sleepers,
 * then reschedule.  Called ONLY from the PIT IRQ0 handler.
 */
void SchedTick(void);

/**
 * Pure reschedule from a blocking/yield path (join, wait, sleep, exit,
 * yield).  Performs the CFS context switch WITHOUT advancing the clock,
 * so the tick rate tracks the PIT (100 Hz) instead of system load.
 */
void SchedReschedule(void);

/**
 * Perform a direct context switch to a specific thread.
 * @param t  Target thread.
 */
void SchedSwitchTo(thread_t *t);

/**
 * Set the current thread for the current CPU WITHOUT performing a
 * context switch.  Used during bootstrap to register the thread that
 * is about to enter user mode via raw IRETQ (bypassing the scheduler).
 */
void SchedSetCurrent(thread_t *t);

/**
 * Get the currently running thread for the current CPU.
 */
thread_t *sched_get_current(void);

/**
 * Get total number of created threads.
 */
u32 SchedGetThreadCount(void);

/**
 * Get the monotonic tick count (incremented each timer interrupt).
 */
u64 SchedGetTicks(void);

/**
 * Block the current thread for the given number of ticks.
 * The thread is re-enqueued after the timeout expires.
 * Sleepers live on a sorted sleep list keyed by absolute wake tick,
 * so per-tick wakeup is amortized O(1) instead of O(MAX_THREADS).
 * @param ticks  Number of ticks to sleep (0 is clamped to 1).
 */
void SchedSleep(u64 ticks);

/**
 * Unlink a blocked sleeper from the sleep list without waking it.
 * Must be called with interrupts disabled (same requirement as
 * sched_sleep).  No-op for threads that are not sleeping.
 * @param t  Thread to unsleep.
 */
void SchedUnsleep(thread_t *t);

#endif /* KERNEL_SCHED_H */
