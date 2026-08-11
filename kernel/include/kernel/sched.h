/*
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
void sched_init(void);

/**
 * Add a thread to the ready queue.
 * @param t  Thread to enqueue.
 */
void sched_enqueue(thread_t *t);

/**
 * Remove a thread from the ready queue.
 * @param t  Thread to dequeue.
 */
void sched_dequeue(thread_t *t);

/**
 * Timer tick — advance the time base (s_ticks), wake expired sleepers,
 * then reschedule.  Called ONLY from the PIT IRQ0 handler.
 */
void sched_tick(void);

/**
 * Pure reschedule from a blocking/yield path (join, wait, sleep, exit,
 * yield).  Performs the CFS context switch WITHOUT advancing the clock,
 * so the tick rate tracks the PIT (100 Hz) instead of system load.
 */
void sched_reschedule(void);

/**
 * Perform a direct context switch to a specific thread.
 * @param t  Target thread.
 */
void sched_switch_to(thread_t *t);

/**
 * Set the current thread for the current CPU WITHOUT performing a
 * context switch.  Used during bootstrap to register the thread that
 * is about to enter user mode via raw IRETQ (bypassing the scheduler).
 */
void sched_set_current(thread_t *t);

/**
 * Get the currently running thread for the current CPU.
 */
thread_t *sched_get_current(void);

/**
 * Get total number of created threads.
 */
u32 sched_get_thread_count(void);

/**
 * Get the monotonic tick count (incremented each timer interrupt).
 */
u64 sched_get_ticks(void);

/**
 * Block the current thread for the given number of ticks.
 * The thread is re-enqueued after the timeout expires.
 * Sleepers live on a sorted sleep list keyed by absolute wake tick,
 * so per-tick wakeup is amortized O(1) instead of O(MAX_THREADS).
 * @param ticks  Number of ticks to sleep (0 is clamped to 1).
 */
void sched_sleep(u64 ticks);

/**
 * Unlink a blocked sleeper from the sleep list without waking it.
 * Must be called with interrupts disabled (same requirement as
 * sched_sleep).  No-op for threads that are not sleeping.
 * @param t  Thread to unsleep.
 */
void sched_unsleep(thread_t *t);

#endif /* KERNEL_SCHED_H */
