/*
 * proc_info.h - Process table entry exposed to user space
 * Copyright (c) 2026 OpSys Project
 *
 * One process-table record as returned by SYS_PROCESS_LIST.
 * Fixed 84-byte layout.  User-space mirrors this struct verbatim
 * (user/lib/libos/syscalls.h); both sides must stay in sync.
 */

#ifndef KERNEL_PROC_INFO_H
#define KERNEL_PROC_INFO_H

#include <kernel/types.h>

typedef struct {
    i32  pid;           /* process ID */
    u32  state;         /* proc_state_t value (CREATED=0 .. FINISHED=4) */
    u32  thread_count;  /* live threads */
    i32  exit_code;     /* exit code (valid when state == ZOMBIE) */
    u32  main_tid;      /* main thread ID */
    char name[64];      /* NUL-terminated process name */
} proc_info_t;

#endif /* KERNEL_PROC_INFO_H */
