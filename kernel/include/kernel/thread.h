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
 * thread.h - Thread management
 * Copyright (c) 2026 OpSys Project
 */

#ifndef KERNEL_THREAD_H
#define KERNEL_THREAD_H

#include <kernel/types.h>
#include <kernel/vmm.h>
#include <kernel/rbtree.h>

/* Thread states */
typedef enum {
    THREAD_STATE_READY = 0,
    THREAD_STATE_RUNNING,
    THREAD_STATE_BLOCKED,
    THREAD_STATE_ZOMBIE,
    THREAD_STATE_FINISHED,
} thread_state_t;

/* Thread control block */
typedef struct thread {
    tid_t          tid;
    pid_t          pid; /* Owning process ID */
    thread_state_t state;
    int            priority; /* 0 (lowest) to 31 (highest) */

    /* CPU context (saved registers) */
    u64 rsp; /* Stack pointer */
    u64 rbx;
    u64 rbp;
    u64 r12;
    u64 r13;
    u64 r14;
    u64 r15;
    u64 rflags;
    u64 rip; /* Instruction pointer */

    /* Address space */
    addr_space_t *addr_space;

    /* Kernel stack */
    u64 kstack_base;
    u64 kstack_top;

    /* SYSCALL fast-path entry (syscall_entry.S): the CPU does NOT switch
     * stacks for SYSCALL, so the entry stashes the USER RSP here
     * (GS-relative, no scratch register) before loading kstack_top.
     * The frame is then synthesized at kstack_top-40. */
    u64 syscall_save_rsp;

    /* IPC blocked-on info */
    port_t blocked_port;

    /* Kernel mutexes currently held (for exit handoff) */
    u32 held_mutexes[MAX_HELD_MUTEXES];
    u8  held_mutex_count;

    /* Async notification (seL4-style signal/wait) state */
    u32 pending_signals; /* Pending notification bitmask */
    u32 wait_mask;       /* Bitmask waiting on (0 = not waiting) */

    /* Scheduling */
    struct thread *next; /* Linked list for scheduler queue */

    /* CPU affinity (-1 = any) */
    i32 affinity;

    /* Thread join support */
    int   exit_code;  /* Exit code (valid when state == FINISHED) */
    tid_t joiner_tid; /* TID of thread waiting to join (-1 = none) */

    /* Force-kill support: set by process_kill() on blocked threads.
     * The syscall-return hook (syscall_return_check) calls ThreadExit()
     * with exit_code when it sees this flag on the current thread. */
    bool force_exit;

    /* Sleep support: absolute wake tick + sorted-sleep-list link.
     * wake_tick == 0 means "not sleeping".  Threads are inserted into
     * s_sleep_list (sched.c) sorted by wake_tick ascending, so each
     * tick only the list head needs inspecting (O(1) amortized) rather
     * than scanning every thread.  Only SchedSleep() inserts and only
     * WakeSleepers() removes; IPC/mutex/notify waiters live in their
     * own structures and are never on this list. */
    u64            wake_tick;  /* Absolute tick to wake (0 = not sleeping) */
    struct thread *sleep_next; /* Next entry in the sorted sleep list */

    /* User-mode stack pointer (for ring 3 threads) */
    u64 user_rsp;

    /* Physical page backing the per-thread user stack.  Recorded by
     * ThreadCreateUser() so FreeThread() can unmap the mapping
     * (as->stack_base + tid*PAGE_SIZE) and release the page before the
     * slot is recycled — without this, a recycled TID would re-collide
     * with the stale PTE and VmmMap() would fail with ERR_BUSY. */
    u64 user_stack_phys;

    /* CFS scheduler: virtual runtime and RB tree node */
    u64       vruntime; /* Accumulated weighted runtime */
    rb_node_t rb;       /* RB tree node (key = vruntime) */
} thread_t;

/**
 * Initialize the thread subsystem.
 */
void ThreadInit(void);

/**
 * Seed a thread's FPU/SSE state slot to the x86 hardware defaults
 * (all exceptions masked).  Called at thread creation AND slot reuse;
 * keeps fpu_switch's fxrstor always valid.
 * @param tid Thread whose slot to seed.
 */
void FpuStateInit(tid_t tid);

/**
 * Create a new kernel thread.
 * @param entry   Entry function.
 * @param arg     Argument passed to entry.
 * @param priority Initial priority (0-31).
 * @return Thread ID, or negative error code.
 */
tid_t ThreadCreateKernel(void (*entry)(void *), void *arg, int priority);

/**
 * Create a new user thread in the given address space.
 * @param entry       User-space entry address.
 * @param arg         Argument (user pointer).
 * @param as          Address space for the thread.
 * @param priority    Initial priority.
 * @return Thread ID, or negative error code.
 */
tid_t ThreadCreateUser(u64 entry, u64 arg, addr_space_t *as, int priority);

/**
 * Exit the current thread.
 * @param code  Exit code.
 */
void ThreadExit(int code);

/**
 * Yield the CPU to the scheduler.
 */
void ThreadYield(void);

/**
 * Get the current running thread.
 */
thread_t *thread_current(void);

/**
 * Get a thread by TID.
 */
thread_t *thread_get(tid_t tid);

/**
 * Release a FINISHED thread's slot back to the free list (frees its
 * kernel stack pages).  Caller must be a DIFFERENT thread: the exiting
 * thread cannot free itself while running on its own stack.  No-op for
 * NULL / already-released / non-FINISHED threads.
 */
void ThreadRelease(thread_t *t);

/**
 * Set CPU affinity for a thread.
 * @param tid     Thread ID.
 * @param cpu     CPU core (-1 = any).
 */
error_t ThreadSetAffinity(tid_t tid, i32 cpu);

/**
 * Switch context from one thread to another (defined in context_switch.S).
 * Saves callee-saved registers of prev, loads from next.
 * Switches CR3 if address spaces differ.  Must be called with IF=0.
 * @param prev      Current running thread.
 * @param next      Thread to switch to.
 * @param resume_if 1 = prev resumes with IF=1 (voluntary yield/sleep/block),
 *                  0 = prev resumes with IF=0 (preempted: resumes via the
 *                  ISR return chain, whose iretq restores the true flags).
 */
void context_switch(thread_t *prev, thread_t *next, int resume_if);

/**
 * Enter user mode via IRETQ (defined in context_switch.S).
 * @param rip     User instruction pointer.
 * @param rsp     User stack pointer.
 * @param rflags  Initial RFLAGS value.
 * @param cs      User code segment selector.
 * @param ss      User stack segment selector.
 */
void context_switchToUser(u64 rip, u64 rsp, u64 rflags, u64 cs, u64 ss);

/**
 * Enter ring 3 for the first time.  Switches CR3 to the user address
 * space and performs IRETQ.  NEVER RETURNS.
 *
 * Reads temp_rsp from the global s_enter_user_temp_rsp (set by caller
 * before invocation).
 *
 * @param rip      User instruction pointer (entry point).
 * @param user_rsp User stack pointer.
 * @param rflags   Initial RFLAGS value.
 * @param cs       User code segment selector (e.g. 0x1B).
 * @param ss       User stack segment selector (e.g. 0x23).
 * @param cr3      Physical address of the user PML4.
 */
void enter_user_mode(u64 rip, u64 user_rsp, u64 rflags, u64 cs, u64 ss, u64 cr3);

#endif /* KERNEL_THREAD_H */
