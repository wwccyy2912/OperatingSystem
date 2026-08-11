/*
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
#define SYS_COUNT  (SYS_CAP_REVOKE_BY_ATOM + 1)

/* System call result passed in RAX */
typedef struct {
    i64    value;   /* Result or negative error code */
} syscall_result_t;

/**
 * Initialize system call infrastructure.
 */
void syscall_init(void);

/**
 * System call entry point (called from assembly stub).
 * @param num  System call number.
 * @param arg1 - arg5  Arguments.
 * @return Result value.
 */
i64 syscall_dispatch(u64 num, u64 arg1, u64 arg2, u64 arg3,
                     u64 arg4, u64 arg5);

#endif /* KERNEL_SYSCALL_H */
