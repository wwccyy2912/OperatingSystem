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
 * process.c - Process creation and management
 * Copyright (c) 2026 OpSys Project
 *
 * Maintains a static process table indexed by PID.  Each process
 * owns an address space, a capability table, and one or more threads.
 * PID 0 is the kernel; PID 1 is the init process.
 *
 * ------------------------------------------------------------------
 * Structure (process):
 *   s_process_table[MAX_THREADS] -- static table indexed by slot
 *     +-- s_process_used[] bitmap; s_process_list (user processes)
 *   process_t: pid, state, addr_space, cap_table, main_tid,
 *              thread_count, exit_code, waiting_tid, heap_base,
 *              subject_id/app_uuid, sig_dispatcher/sig_pending
 *   PID 0 = kernel (idle thread), PID 1 = init (s_init_process)
 *   addr_space: adopted from the caller, which mapped the image from
 *   an embedded ELF blob (BlobGet / SYS_PROCESS_CREATE, process_desc.c)
 * How it works:
 *   process_create() finds a free slot, creates the cap table, spawns
 *   the main thread via ThreadCreateUser() (priority 10) in the
 *   caller-loaded address space, back-links t->pid and appends to the
 *   list.  When the last thread exits, ProcessThreadExited() marks the
 *   process ZOMBIE and wakes waiting_tid (or orphan-reaps);
 *   ProcessReap() frees IPC/IRQ/SHM resources, the cap table, the main
 *   thread and the address space, then recycles the slot -- PIDs are
 *   never reused, only slots are.
 * Purpose:
 *   Central process lifecycle: create, lookup by pid/subject, thread
 *   accounting, and zombie reaping with full resource teardown.
 * Caveats:
 *   - process_get() walks s_process_list: slot index != pid once a
 *     slot is recycled, and PID 0 never appears in the list.
 *   - ProcessReap() releases the main thread's slot BEFORE
 *     VmmDestroyAddrSpace() (FreeThread unmaps the per-thread user
 *     stack) to avoid double-free / use-after-free.
 *   - The caller transfers address-space ownership; process_create()
 *     rolls everything back if setup fails.
 * ------------------------------------------------------------------
 */

#include <kernel/process.h>
#include <kernel/thread.h>
#include <kernel/sched.h>
#include <kernel/cap.h>
#include <kernel/vmm.h>
#include <kernel/rng.h>
#include <kernel/serial.h>
#include <kernel/string.h>
#include <kernel/proc_info.h>
#include <kernel/ipc.h>
#include <kernel/irq.h>
#include <kernel/shm.h>

/* ------------------------------------------------------------------ */
/*  Internal data                                                      */
/* ------------------------------------------------------------------ */

/* Static process table — indexed by PID */
static process_t s_process_table[MAX_THREADS];

/* Whether each table slot is in use */
static bool s_process_used[MAX_THREADS];

/* Linked list of active (non-finished) user processes */
static process_t *s_process_list;

/* Next PID to assign (0 and 1 are reserved) */
static pid_t s_next_pid;

/* P0 地基: next kernel-issued subject ID.  Subject 0 = System (kernel);
 * init gets 1; user processes get 2, 3, ...  Never reused (u64). */
static u64 s_next_subject = 1;

/* The init process (PID 1) */
static process_t *s_init_process;

/* Total number of live processes */
static u32 s_process_count;

/* ------------------------------------------------------------------ */
/*  Helper: copy a C string without <string.h>                         */
/* ------------------------------------------------------------------ */

static void CopyString(char *dst, const char *src, size_t max) {
    size_t i;
    for (i = 0; i < max - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* ------------------------------------------------------------------ */
/*  PUBLIC API                                                         */
/* ================================================================== */

void ProcessInit(void) {
    /* Clear the entire table */
    for (int i = 0; i < MAX_THREADS; i++) {
        s_process_used[i]               = false;
        s_process_table[i].pid          = -1;
        s_process_table[i].state        = PROC_STATE_FINISHED;
        s_process_table[i].addr_space   = NULL;
        s_process_table[i].cap_table    = NULL;
        s_process_table[i].main_tid     = -1;
        s_process_table[i].thread_count = 0;
        s_process_table[i].exit_code    = 0;
        s_process_table[i].waiting_tid  = -1;
        s_process_table[i].heap_base    = HEAP_USER_BASE;
        s_process_table[i].sig_dispatcher = 0;
        s_process_table[i].sig_pending    = 0;
        s_process_table[i].name[0]      = '\0';
        s_process_table[i].next         = NULL;
    }

    s_process_list  = NULL;
    s_process_count = 0;
    s_next_pid      = 2; /* 0 and 1 are created explicitly below */
    s_next_subject  = 1; /* 0 = System; init will take 1 */
    s_init_process  = NULL;

    /* ---- PID 0: kernel process (no user process, no address space) ---- */
    process_t *kern    = &s_process_table[0];
    s_process_used[0]  = true;
    kern->pid          = 0;
    kern->state        = PROC_STATE_RUNNING;
    kern->addr_space   = vmm_get_kernel_addr_space();
    kern->cap_table    = NULL;
    kern->main_tid     = 0; /* idle thread */
    kern->thread_count = 1;
    kern->subject_id   = 0; /* System (reserved, kernel) */
    kern->next         = NULL;
    CopyString(kern->name, "kernel", sizeof(kern->name));
    /* PID 0 does NOT appear in the user process list */
    s_process_count = 1;

    /* ---- PID 1: init process (created now, started later) ---- */
    process_t *init    = &s_process_table[1];
    s_process_used[1]  = true;
    init->pid          = 1;
    init->state        = PROC_STATE_CREATED;
    init->addr_space   = vmm_create_addr_space();
    init->cap_table    = cap_table_create(1);
    init->main_tid     = -1;                       /* thread not created yet */
    init->thread_count = 0;
    /* P0 地基: init is the first user subject (subject 1). */
    init->subject_id = s_next_subject++;
    /* Unit 1: kernel-issued App Subject (uuid), drawn from the PRNG
     * at app instantiation.  The kernel/system process (PID 0,
     * subject 0) keeps (0,0). */
    init->app_uuid_hi = RngU64();
    init->app_uuid_lo = RngU64();
    /* ASLR: randomize init's heap base; fold in boot timing entropy. */
    RngMix(SchedGetTicks() ^ 1ULL);
    init->heap_base = AslrHeapBase();
    CopyString(init->name, "init", sizeof(init->name));

    /* Add to user process list */
    init->next     = s_process_list;
    s_process_list = init;
    s_process_count++;

    s_init_process = init;
}

/* ------------------------------------------------------------------ */

process_t *process_create(const char *name, u64 entry, addr_space_t *as) {
    /* Find a free slot in the process table */
    int slot = -1;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (!s_process_used[i]) {
            slot              = i;
            s_process_used[i] = true;
            break;
        }
    }
    if (!as || slot < 0)
        return NULL;

    process_t *proc  = &s_process_table[slot];
    proc->pid        = s_next_pid++;
    proc->state      = PROC_STATE_CREATED;
    proc->addr_space = as; /* caller-loaded ELF; ownership transferred */
    proc->cap_table  = cap_table_create(proc->pid);
    proc->thread_count = 0;
    proc->exit_code    = 0;
    proc->waiting_tid  = -1;
    /* P0 地基: every new process gets a fresh, never-reused subject. */
    proc->subject_id = s_next_subject++;
    /* Unit 1: kernel-issued App Subject (uuid), drawn from the PRNG
     * at app instantiation.  Unforgeable: never user-supplied. */
    proc->app_uuid_hi = RngU64();
    proc->app_uuid_lo = RngU64();
    /* Signals: fresh process starts with no dispatcher, nothing pending. */
    proc->sig_dispatcher = 0;
    proc->sig_pending    = 0;
    /* ASLR: randomize the child's heap base; mix in creation timing +
     * PID so sibling processes get decorrelated layouts. */
    RngMix(SchedGetTicks() ^ ((u64)s_next_pid << 32));
    proc->heap_base = AslrHeapBase();
    proc->next      = NULL;
    CopyString(proc->name, name, sizeof(proc->name));

    if (!proc->cap_table) {
        /* Roll back: release the address space and free the SLOT
         * (index, not pid — they diverge once slots are recycled). */
        VmmDestroyAddrSpace(proc->addr_space);
        s_process_used[slot] = false;
        return NULL;
    }

    /* Create the main thread in the process's address space */
    tid_t tid = ThreadCreateUser(entry, 0, proc->addr_space, 10);
    if (tid < 0) {
        /* Roll back */
        CapTableDestroy(proc->cap_table);
        if (proc->addr_space)
            VmmDestroyAddrSpace(proc->addr_space);
        s_process_used[slot] = false;
        return NULL;
    }

    proc->main_tid     = tid;
    proc->thread_count = 1;
    /* Main thread is enqueued and runnable: leave CREATED for the state
     * machine.  The process is now READY to run (ZOMBIE is reached via
     * ProcessThreadExited() when the last thread exits). */
    proc->state = PROC_STATE_READY;

    /* Back-link the thread to this process */
    thread_t *t = thread_get(tid);
    if (t)
        t->pid = proc->pid;

    /* Append to the process list */
    process_t *tail = s_process_list;
    if (!tail) {
        s_process_list = proc;
    } else {
        while (tail->next)
            tail = tail->next;
        tail->next = proc;
    }
    s_process_count++;

    SerialPrintf("proc: CREATE pid=%d name=%s slot=%d count=%d\n",
                  proc->pid,
                  proc->name,
                  slot,
                  s_process_count);

    return proc;
}

/* ------------------------------------------------------------------ */

process_t *process_get(pid_t pid) {
    /* PID 0 (the kernel) is not a user process and never appears in
     * the list.  Table slots are recycled by ProcessReap() while PIDs
     * themselves are never reused, so the slot index and the pid
     * diverge once a slot has been recycled — lookup must walk the
     * list instead of indexing the table by pid. */
    if (pid <= 0)
        return NULL;
    for (process_t *p = s_process_list; p; p = p->next)
        if (p->pid == pid)
            return p;
    return NULL;
}

/* ------------------------------------------------------------------ */

process_t *process_get_by_subject(subject_id_t subject) {
    if (subject == 0)
        return NULL; /* System/kernel is not a user process */
    for (process_t *p = s_process_list; p; p = p->next)
        if (p->subject_id == subject)
            return p;
    return NULL;
}

/* ------------------------------------------------------------------ */

process_t *process_current(void) {
    thread_t *t = thread_current();
    if (!t)
        return NULL;
    return process_get(t->pid);
}

/* ------------------------------------------------------------------ */

process_t *process_get_init(void) {
    return s_init_process;
}

/* ------------------------------------------------------------------ */

/*
 * Reap a ZOMBIE process: detach it from s_process_list, free its
 * address space and capability table and release its table slot.
 *
 * The caller must hold the reap ownership: either the process died as
 * an orphan (nobody is waiting on it), or the caller claimed it via
 * SysProcessWait() (waiting_tid matched and was cleared).  All
 * threads are gone by construction (thread_count hit zero), so no code
 * runs in the freed address space.  PIDs are never reused — only table
 * slots are recycled.
 */
void ProcessReap(process_t *proc) {
    /* Detach from the process list. */
    process_t **pp = &s_process_list;
    while (*pp && *pp != proc)
        pp = &(*pp)->next;
    bool detached = (*pp == proc);
    if (detached)
        *pp = proc->next;

    if (s_process_count > 0)
        s_process_count--;
    if (s_init_process == proc)
        s_init_process = NULL;

    SerialPrintf(
        "proc: REAP pid=%d detached=%d count->%d\n", proc->pid, detached, s_process_count);

    /* Tear down the process's IPC + IRQ resources FIRST: every client
     * blocked on its ports (recv / pending send / awaiting reply) is
     * woken with ERR_NOENT — never left hanging on a dead peer — and
     * the port-registry names are freed so a restarted service can
     * re-register them.  IRQ lines it bound are released immediately
     * (irq_handle's lazy self-heal only fires on the next IRQ).  Any
     * shared-page pools it owned are returned to the PMM. */
    IpcCleanupProcess(proc->pid);
    IrqCleanupProcess(proc->pid);
    ShmCleanupProcess(proc->subject_id);

    /* Free the process's resources. */
    if (proc->cap_table)
        CapTableDestroy(proc->cap_table);

    /*
     * Return the main thread's slot to the free list FIRST, while the
     * address space is still alive: FreeThread() unmaps the per-thread
     * user stack (a page owned by this address space) and releases its
     * physical page.  Reversing the order would double-free that page
     * (VmmDestroyAddrSpace() below frees every user page) and would
     * use-after-free the page tables when FreeThread() calls VmmUnmap()
     * on an already-destroyed address space.
     *
     * The main thread exited by construction (thread_count hit zero), so
     * its kernel stack is dead — EXCEPT in the orphan-reap path, where the
     * reaper IS the exiting main thread itself (ProcessThreadExited()
     * runs on its own stack).  ThreadRelease() skips the current thread,
     * so that path cannot free the stack we are executing on.  (The slot
     * then leaks, but that only affects the rare nobody-waits case.)
     */
    if (proc->main_tid >= 0)
        ThreadRelease(thread_get(proc->main_tid));

    if (proc->addr_space)
        VmmDestroyAddrSpace(proc->addr_space);

    /* Release the table slot and clear the PCB so a stale pointer can
     * never observe half-recycled state. */
    int slot             = (int)(proc - s_process_table);
    proc->pid            = -1;
    proc->state          = PROC_STATE_FINISHED;
    proc->addr_space     = NULL;
    proc->cap_table      = NULL;
    proc->main_tid       = -1;
    proc->thread_count   = 0;
    proc->exit_code      = 0;
    proc->waiting_tid    = -1;
    proc->subject_id     = 0;
    proc->next           = NULL;
    s_process_used[slot] = false;
}

/*
 * Called from ThreadExit() when a thread of this process terminates.
 * Decrements the process's thread count; when the LAST thread exits,
 * the process becomes ZOMBIE, records the exit code and either hands
 * the reap to a thread blocked in process_wait() (which wakes with
 * waiting_tid ownership) or — if nobody is waiting — reaps the orphan
 * zombie immediately so it cannot linger in the table forever.
 */
void ProcessThreadExited(process_t *proc, int code) {
    if (!proc)
        return;

    /* The process becomes ZOMBIE exactly once, when its last counted
     * thread exits.  Guard against double-marking (threads force-killed
     * after the process is already ZOMBIE) and against the count ever
     * drifting below zero. */
    if (proc->state == PROC_STATE_ZOMBIE)
        return;

    if (proc->thread_count > 0)
        proc->thread_count--;
    if (proc->thread_count != 0) {
        SerialPrintf(
            "proc: THREAD_EXITED pid=%d tc->%d (not last)\n", proc->pid, proc->thread_count);
        return;
    }

    /* Last thread of the process has exited: mark it ZOMBIE. */
    proc->state     = PROC_STATE_ZOMBIE;
    proc->exit_code = code;
    SerialPrintf(
        "proc: LAST_THREAD pid=%d code=%d waiting_tid=%d\n", proc->pid, code, proc->waiting_tid);

    /* Wake a process_wait() caller, if any.  waiting_tid is left in
     * place: the woken caller checks it in SysProcessWait() and is
     * the one that reaps the process (collecting the exit code before
     * the resources are freed). */
    if (proc->waiting_tid >= 0) {
        thread_t *waiter = thread_get(proc->waiting_tid);
        if (waiter && waiter->state == THREAD_STATE_BLOCKED) {
            waiter->state = THREAD_STATE_READY;
            SchedEnqueue(waiter);
            SerialPrintf(
                "proc: WAKE_WAITER pid=%d tid=%d (waiter reaps)\n", proc->pid, waiter->tid);
            return; /* the woken caller owns the reap */
        }
        proc->waiting_tid = -1; /* waiter vanished: fall through */
    }

    /* Orphan zombie: no live waiter will ever collect this process.
     * Reap it now instead of leaving it in the table permanently. */
    SerialPrintf("proc: ORPHAN pid=%d -> reap\n", proc->pid);
    ProcessReap(proc);
}

/* ------------------------------------------------------------------ */

u32 ProcessGetCount(void) {
    return s_process_count;
}

/* ------------------------------------------------------------------ */

/*
 * Enumerate the user process table for SYS_PROCESS_LIST.  Iterates
 * s_process_list (PID 0 kernel is deliberately not in it), copying
 * each process's pid/state/thread_count/exit_code/main_tid/name into
 * consecutive out entries in list (creation) order.  Writes at most
 * max_entries; returns the number written.
 */
u32 ProcessListFill(proc_info_t *out, u32 max_entries) {
    u32        n = 0;
    process_t *proc;

    for (proc = s_process_list; proc && n < max_entries; proc = proc->next) {
        proc_info_t *info  = &out[n];
        info->pid          = (i32)proc->pid;
        info->state        = (u32)proc->state;
        info->thread_count = proc->thread_count;
        info->exit_code    = (i32)proc->exit_code;
        info->main_tid     = (u32)proc->main_tid;
        CopyString(info->name, proc->name, sizeof(info->name));
        n++;
    }

    return n;
}
