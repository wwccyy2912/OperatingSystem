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
 * syscall.h - System call definitions
 * Copyright (c) 2026 OpSys Project
 *
 * All syscalls go through a single entry point.
 * Numbers are defined in syscall_numbers.h (shared with user-space).
 */

#ifndef KERNEL_SYSCALL_H
#define KERNEL_SYSCALL_H

#include <kernel/types.h>
#include <kernel/syscall_numbers.h>

/*
 * Sentinel: total size of the dispatch table.
 * Covers every *defined* number (including reserved but not-yet-
 * implemented slots, which stay NULL in the table and are rejected
 * with ERR_INVAL).  Must equal last defined SYS_ + 1.
 */
#define SYS_COUNT (SYS_PCI_CFG_WRITE + 1)

/* Compile-time guard: SYS_PCI_CFG_WRITE (72) is the highest syscall
 * number; if a higher one is ever added, update SYS_COUNT to match. */
_Static_assert(SYS_PCI_CFG_WRITE + 1 == SYS_COUNT, "SYS_COUNT drift");

/* System call result passed in RAX */
typedef struct {
    i64 value; /* Result or negative error code */
} syscall_result_t;

/**
 * Initialize system call infrastructure.
 */
void SyscallInit(void);

/**
 * System call entry point (called from assembly stub).
 * @param num  System call number.
 * @param arg1 - arg5  Arguments.
 * @return Result value.
 */
i64 syscall_dispatch(u64 num, u64 arg1, u64 arg2, u64 arg3, u64 arg4, u64 arg5);

#endif /* KERNEL_SYSCALL_H */
