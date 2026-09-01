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
 * spinlock.h - User-space spinlock (kernel mutex fast-path)
 * Copyright (c) 2026 OpSys Project
 *
 * P0 of docs/kernel_roadmap.md: a user-space lock with zero syscalls on
 * the uncontended path (single atomic exchange vs. 2 syscalls through
 * SYS_MUTEX_LOCK/UNLOCK).  The kernel mutex remains for long-held locks
 * that should block-and-wake (slow path).
 *
 * Design notes:
 *  - Uncontended lock = one `lock xchg` (__atomic_exchange_n) — no
 *    context switch, no syscall.
 *  - Contended lock = yield to the scheduler instead of busy-spinning.
 *    This kernel is single-CPU; a pure spin would deadlock the holder.
 *  - NOT reentrant (same as the kernel mutex it replaces).
 *  - Limitation: on single-CPU preemptive scheduling, a low-priority
 *    holder can be starved by a high-priority spinner (priority
 *    inversion).  All current user threads run at equal priority, so
 *    the yield loop is fair in practice.
 */

#ifndef LIBOS_SPINLOCK_H
#define LIBOS_SPINLOCK_H

#include <libos/syscalls.h> /* ThreadYield() */

typedef volatile int user_spinlock_t;

/* Try to acquire the lock.  Returns 1 on success, 0 if already held. */
static inline int SpinTrylock(user_spinlock_t *l) {
    return __atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE) == 0;
}

/* Acquire the lock.  Uncontended: one atomic exchange, zero syscalls.
 * Contended: yield to the scheduler so the holder can make progress. */
static inline void SpinLock(user_spinlock_t *l) {
    while (!SpinTrylock(l))
        ThreadYield();
}

/* Release the lock. */
static inline void SpinUnlock(user_spinlock_t *l) {
    __atomic_store_n(l, 0, __ATOMIC_RELEASE);
}

#endif /* LIBOS_SPINLOCK_H */
