/*
 * process.h - Process management
 * Copyright (c) 2026 OpSys Project
 */

#ifndef KERNEL_PROCESS_H
#define KERNEL_PROCESS_H

#include <kernel/types.h>
#include <kernel/vmm.h>
#include <kernel/cap.h>
#include <kernel/cred.h>
#include <kernel/signal.h>
#include <kernel/proc_info.h>

/* Process states */
typedef enum {
    PROC_STATE_CREATED = 0,
    PROC_STATE_READY,
    PROC_STATE_RUNNING,
    PROC_STATE_ZOMBIE,
    PROC_STATE_FINISHED,
} proc_state_t;

/* Process control block */
typedef struct process {
    pid_t           pid;
    proc_state_t    state;
    addr_space_t   *addr_space;
    cap_table_t    *cap_table;
    cred_t          cred;     /* User/group credentials (POSIX) */
    tid_t           main_tid; /* Main thread ID */
    u32             thread_count;
    char            name[64];
    struct process *next;

    /* Process exit + wait support (SYS_PROCESS_WAIT) */
    int   exit_code;   /* Exit code (valid when state == ZOMBIE) */
    tid_t waiting_tid; /* TID blocked in process_wait (-1 = none) */

    /* ASLR (design item ⑭): per-process randomized user heap base.
     * The heap occupies [heap_base, heap_base + HEAP_USER_SIZE) with a
     * guard page below and at the top; user-space malloc.c fetches it
     * via SYS_GET_HEAP_BASE.  Valid once the process is created. */
    u64 heap_base;

    /* P0 地基: permission-model identity (docs/permission_model.md §三).
     * subject_id is kernel-issued, globally unique, never reused
     * (0 = System/kernel; init gets 1).  persona_id is the person's
     * persona (工作/儿童模式...); not used further in P0. */
    subject_id_t subject_id;
    u32          persona_id;

    /* Unit 1 (TUI 权限查询): App Subject (uuid) — the 128-bit
     * kernel-issued app identity, allocated at app instantiation
     * (docs/permission_model.md §三), replacing the forgeable
     * self-reported u32 app_id_hash.  Unforgeable like subject_id:
     * drawn from the kernel PRNG at process_create()/process_init(),
     * never from user input.  (0,0) only for the kernel/system
     * process (subject 0). */
    u64 app_uuid_hi;
    u64 app_uuid_lo;

    /* POSIX-style signals (kernel/signal.h).  Handler table indexed by
     * signal number (1..NSIG-1): SIG_DFL (0), SIG_IGN (1), or a user
     * handler address.  sig_pending is the process-wide pending bitmask
     * (bit N = signal N pending).  sig_restorer is the per-process
     * __restore_rt address passed at SYS_SIGNAL registration time. */
    u64 sig_handlers[NSIG];
    u64 sig_pending;
    u64 sig_restorer;
} process_t;

/**
 * Initialize process management.
 */
void process_init(void);

/**
 * Create a new process.
 * @param name   Process name.
 * @param entry  User entry point address.
 * @param as     Address space with the ELF image already loaded.
 *               Ownership is taken: on success the process owns it,
 *               on failure it is destroyed by process_create().
 * @return Pointer to process, or NULL.
 */
process_t *process_create(const char *name, u64 entry, addr_space_t *as);

/**
 * Get a process by PID.
 * Walks the process list (PIDs are never reused, table slots are).
 * Returns NULL for the kernel (PID 0), a reaped PID, or an unknown PID.
 */
process_t *process_get(pid_t pid);

/**
 * P1 地基: get a process by its kernel-issued subject ID.
 * Walks the process list like process_get; subjects are never reused,
 * so at most one live process can match.  Returns NULL when no live
 * process holds the subject (e.g. the kernel subject 0, a reaped
 * process, or an unknown subject).
 */
process_t *process_get_by_subject(subject_id_t subject);

/**
 * Get the current process.
 */
process_t *process_current(void);

/**
 * Get the init process (PID 1).
 */
process_t *process_get_init(void);

/**
 * Notify the process that one of its threads has exited.
 * When the LAST thread exits, the process becomes PROC_STATE_ZOMBIE
 * with the given exit code and either:
 *  - hands the reap to a thread blocked in process_wait() on it (that
 *    thread wakes, collects the code and calls process_reap()), or
 *  - if nobody is waiting, reaps the orphan zombie immediately.
 * Called by thread_exit(); safe for kernel threads (caller must pass
 * NULL or skip when pid == 0).
 */
void process_thread_exited(process_t *proc, int code);

/**
 * Reap a ZOMBIE process: free its address space and capability table,
 * release its table slot and detach it from the process list.
 * The caller must hold reap ownership (see process_thread_exited).
 */
void process_reap(process_t *proc);

/**
 * Get total process count.
 */
u32 process_get_count(void);

/**
 * Fill a caller buffer with one proc_info_t per user process
 * (init + spawned, including zombies), in creation order.
 * Writes at most max_entries.  Returns the number written.
 */
u32 process_list_fill(proc_info_t *out, u32 max_entries);

#endif /* KERNEL_PROCESS_H */
