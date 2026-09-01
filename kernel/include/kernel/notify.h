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
 * notify.h - Async notification (seL4-style signal/wait)
 * Copyright (c) 2026 OpSys Project
 *
 * Per-thread asynchronous notification bitmasks.  Notify() ORs
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
error_t Notify(tid_t target_tid, u32 mask);

/**
 * Wait for (or poll) a notification bitmask.
 * @param mask  Bits to wait for.  mask == 0 polls: returns immediately.
 * @return The consumed notification word (previous pending_signals value).
 */
u32 WaitNotification(u32 mask);

#endif /* KERNEL_NOTIFY_H */
