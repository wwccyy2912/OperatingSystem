/*
 * notify.h - Async notification (seL4-style signal/wait)
 * Copyright (c) 2026 OpSys Project
 *
 * Per-thread asynchronous notification bitmasks.  notify() ORs
 * signal bits into a target thread's pending set; a waiting thread
 * is woken only when the pending bits match its wait mask.
 */

#ifndef KERNEL_NOTIFY_H
#define KERNEL_NOTIFY_H

#include <kernel/types.h>

/**
 * Send an async notification to a target thread.
 * @param target_tid  Target thread ID.
 * @param mask        Signal bits to OR into the target's pending set.
 * @return OK, or ERR_NOENT if the target thread does not exist.
 */
error_t notify(tid_t target_tid, u32 mask);

/**
 * Wait for (or poll) a notification bitmask.
 * @param mask  Bits to wait for.  mask == 0 polls: returns immediately.
 * @return The consumed notification word (previous pending_signals value).
 */
u32 wait_notification(u32 mask);

#endif /* KERNEL_NOTIFY_H */
