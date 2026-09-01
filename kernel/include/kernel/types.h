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
 * types.h - Basic kernel type definitions
 * Copyright (c) 2026 OpSys Project
 */

#ifndef KERNEL_TYPES_H
#define KERNEL_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Standard type aliases */
typedef int8_t  i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef uintptr_t uptr;
typedef intptr_t  iptr;

/* Process / Thread identifiers */
typedef i32 pid_t;
typedef i32 tid_t;

/* Capability handle */
typedef u64 cap_t;

/* Port for IPC */
typedef u64 port_t;

/*
 * Subject ID: kernel-issued, globally unique, never reused (P0 地基).
 * Subject 0 = System (reserved, kernel); user subjects start at 1.
 */
typedef u64 subject_id_t;

/* User/group identifiers (POSIX compatible) */
typedef u32 uid_t;
typedef u32 gid_t;

/* Superuser (root) has UID 0 */
#define UID_ROOT ((uid_t)0)

/* Error codes */
typedef enum {
    OK              = 0,
    ERR_NOMEM       = -1,
    ERR_INVAL       = -2,
    ERR_NOCAP       = -3,
    ERR_NOENT       = -4,
    ERR_BUSY        = -5,
    ERR_AGAIN       = -6,
    ERR_FAULT       = -7,
    ERR_OVERFLOW    = -8,
    ERR_DENIED      = -9,
    ERR_INTERRUPTED = -10, /* blocking wait aborted by a signal kill */
} error_t;

/* Memory protection flags */
#define PROT_NONE  0x00
#define PROT_READ  0x01
#define PROT_WRITE 0x02
#define PROT_EXEC  0x04

/* Capability rights bitmask */
typedef u32 rights_t;
#define RIGHT_READ  (1 << 0)
#define RIGHT_WRITE (1 << 1)
#define RIGHT_EXEC  (1 << 2)
#define RIGHT_GRANT (1 << 3)
#define RIGHT_ALL   (RIGHT_READ | RIGHT_WRITE | RIGHT_EXEC | RIGHT_GRANT)

/* Null handles */
#define CAP_NULL  ((cap_t)0)
#define PORT_NULL ((port_t)0)
#define PID_NULL  ((pid_t)0)

/* Maximum values */
/*
 * MAX_THREADS bounds the thread table, the process table (one entry
 * per PID), the per-process capability table pointer array and the
 * user thread stack region (ASLR_STACK_BLOCK = MAX_THREADS * PAGE_SIZE).
 *
 * Raised from 256 to 1024 so a single process can run 1000+ concurrent
 * threads (stress test), then to 2048 so the P4 exhaustion test can
 * fill the table even with ~24 live service threads (the old assert
 * needed 1000 of 1023 usable slots — the user service's single thread
 * tipped it to 999).  Static memory cost at 2048:
 *   - s_cap_tables:       2048 x 8 B = 16 KB (BSS, pointer array only)
 *   - s_thread_table:     ~0.6 MB
 *   - s_process_table:    ~1.4 MB
 *   - ASLR stack region:  2048 x 4 pages = 32 MB virtual (fits in the
 *     [0x90000000, 0x100000000) block; physical only when touched)
 *   - Cap tables themselves are dynamically allocated (~73 KB each via
 *     pmm_alloc_pages) — ~1 MB total for 14 live processes.
 */
#define MAX_THREADS  2048

/* User stack size per thread (4 pages = 16 KiB).  Service processes
 * (vfs/term/shell…) run deep call chains on this stack; a single page
 * overflows once the code grows (observed: vfs SIGSEGV at the stack
 * bottom).  Also sizes ASLR_STACK_BLOCK (rng.h). */
#define USER_STACK_PAGES 4
#define MAX_PORTS    256
#define MAX_CAPS     1024
#define MAX_MSG_SIZE 4096
#define MAX_PATH_LEN 256

/* Maximum mutexes a single thread may hold at once (for exit handoff) */
#define MAX_HELD_MUTEXES 16

/* Page size */
#define PAGE_SIZE 4096UL

#endif /* KERNEL_TYPES_H */
