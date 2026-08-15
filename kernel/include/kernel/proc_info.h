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
    i32  pid;          /* process ID */
    u32  state;        /* proc_state_t value (CREATED=0 .. FINISHED=4) */
    u32  thread_count; /* live threads */
    i32  exit_code;    /* exit code (valid when state == ZOMBIE) */
    u32  main_tid;     /* main thread ID */
    char name[64];     /* NUL-terminated process name */
} proc_info_t;

/*
 * proc_ident_t - Kernel-issued per-process identity record
 * (Unit 1: app identity moves from a forgeable self-reported u32
 * app_id_hash to a kernel-issued 128-bit UUID + subject_id).
 *
 * Returned by SYS_PROC_INFO_BY_SUBJECT.  Fixed layout.  User-space
 * mirrors this struct verbatim (user/lib/libos/syscalls.h); both
 * sides must stay in sync.  uuid_hi/uuid_lo form the kernel-issued
 * App Subject (uuid) drawn from the kernel PRNG at process creation;
 * the kernel/system process (subject 0) has (0, 0).
 */
typedef struct {
    i32  pid;      /* process ID */
    char name[64]; /* NUL-terminated process name */
    u64  uuid_hi;  /* kernel-issued app UUID, high 64 bits */
    u64  uuid_lo;  /* kernel-issued app UUID, low 64 bits */
} proc_ident_t;

#endif /* KERNEL_PROC_INFO_H */
