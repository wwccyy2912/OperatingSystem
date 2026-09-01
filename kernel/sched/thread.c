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
 * thread.c - Thread creation and management
 * Copyright (c) 2026 OpSys Project
 *
 * Maintains a static thread table indexed by TID.  A singly-linked
 * free list provides O(1) allocation.  Each new kernel thread gets
 * an 8-page (32 KiB) kernel stack allocated from the PMM.  The
 * initial stack frame is laid out so that the first context_switch
 * into the thread will pop straight into ThreadTrampoline().
 *
 * ------------------------------------------------------------------
 * Structure (thread):
 *   s_thread_table[MAX_THREADS] -- static table indexed by TID
 *     +-- s_free_list: free slots (O(1) alloc; tid 0 = idle excluded)
 *   per thread:
 *     +-- kernel stack: 8 pages (32 KiB) from PMM; initial frame at
 *     |   kstack_top: rbx..r15, rflags=0x200, rip=ThreadTrampoline
 *     +-- user threads: per-TID user stack at as->stack_base + tid*...
 *     `-- entry/arg kept in s_start_entry[]/s_start_arg[]; trampoline:
 *         entry(arg) in ring 0, or IRETQ (CS=0x1B) into ring 3
 * How it works:
 *   ThreadCreateKernel()/ThreadCreateUser() take a slot, lay out the
 *   stack + fake context (rip/rflags also stored in the TCB), record
 *   the real entry/arg, then SchedEnqueue().  The first context_switch
 *   lands in ThreadTrampoline(), which calls entry(arg) directly or
 *   IRETQes a ring-3 frame (SS=0x23, RSP=top-8, RFLAGS=0x202).
 *   ThreadExit() marks FINISHED, wakes the joiner, does process
 *   bookkeeping and reschedules; a reaper (ThreadRelease/FreeThread)
 *   later frees both stacks and recycles the slot.
 * Purpose:
 *   Uniform O(1) thread-slot allocation and one creation path for
 *   kernel and user threads, with per-thread TCB and stacks.
 * Caveats:
 *   - context_switch restores RIP/RFLAGS from the TCB fields, NOT the
 *     stack frame; SetupThreadStack() must set both.
 *   - Recycled slots are fully re-zeroed by alloc_thread(): a stale
 *     force_exit would kill the new thread at its first checkpoint.
 *   - ThreadRelease() refuses to free the current thread's own stack
 *     and only recycles THREAD_STATE_FINISHED slots.
 * ------------------------------------------------------------------
 */

#include <kernel/thread.h>
#include <kernel/sched.h>
#include <kernel/process.h>
#include <kernel/pmm.h>
#include <kernel/vmm.h>
#include <kernel/serial.h>
#include <kernel/mutex.h>
#include <kernel/panic.h>

/* Kernel stack size for each thread (8 pages = 32 KiB) */
#define KSTACK_PAGES 8


/* ------------------------------------------------------------------ */
/*  Internal data                                                      */
/* ------------------------------------------------------------------ */

/* Static thread table — indexed by TID */
static thread_t s_thread_table[MAX_THREADS];

/* Head of the singly-linked free list of unused thread_t slots */
static thread_t *s_free_list;

/* Per-thread start info used by thread_trampoline */
static void (*s_start_entry[MAX_THREADS])(void *);
static void *s_start_arg[MAX_THREADS];

/* ------------------------------------------------------------------ */
/*  Trampoline                                                         */
/* ------------------------------------------------------------------ */

/*
 * thread_trampoline is the first function a newly-created thread
 * executes.  It retrieves the real entry point and argument from
 * the per-thread arrays, calls the entry, then cleans up.
 *
 * For user threads (addr_space != kernel), it transitions to ring 3
 * via IRETQ before calling the user entry function.
 *
 * NOTE: this function takes NO arguments via registers; everything
 *       is looked up through sched_get_current().
 */
static void ThreadTrampoline(void) {
    thread_t *t = sched_get_current();
    if (!t)
        return;

    void (*entry)(void *) = s_start_entry[t->tid];
    void *arg             = s_start_arg[t->tid];

    /*
     * User threads: transition to ring 3 via IRETQ.
     *
     * Build an IRETQ frame on the kernel stack and jump to the user
     * entry function.  After IRETQ the CPU is in ring 3 (CS=0x1B,
     * SS=0x23) executing the user function with RDI = arg.
     *
     * The kernel stack is abandoned after IRETQ; subsequent interrupts
     * use TSS.RSP0 (= kstack_top) which is fine — the IRETQ frame
     * occupies only the top 40 bytes and is overwritten by the first
     * interrupt frame pushed there.
     */
    if (t->addr_space != vmm_get_kernel_addr_space() && t->user_rsp) {
        /* Build IRETQ frame (pushed high→low: SS, RSP, RFLAGS, CS, RIP) */
        u64 *sp = (u64 *)t->kstack_top;
        *(--sp) = 0x23ULL;         /* SS  — user data segment */
        *(--sp) = t->user_rsp - 8; /* RSP — top-8: "as if called".
                                    * SysV ABI: a function entered via `call` sees
                                    * RSP%16==8 (the call pushed an 8-byte return
                                    * address).  User threads are entered directly by
                                    * IRETQ, so give them RSP = top-8 to satisfy the
                                    * same convention — otherwise compiler-generated
                                    * movaps/movdqa (16-byte aligned) #GP.  Matches
                                    * signal delivery, which also enters handlers at
                                    * base-8 with the restore-rt slot as fake ret.   */
        *(--sp) = 0x202ULL;        /* RFLAGS — IF=1           */
        *(--sp) = 0x1BULL;         /* CS  — user code segment */
        *(--sp) = (u64)entry;      /* RIP — user entry point  */

        /* Set RDI = arg then IRETQ (never returns) */
        __asm__ volatile("mov  %0, %%rdi\n"
                         "mov  %1, %%rsp\n"
                         "iretq\n"
                         :
                         : "r"(arg), "r"(sp)
                         : "rdi", "memory");
        __builtin_unreachable();
    }

    /* Kernel thread: call entry directly in ring 0 */
    entry(arg);

    /* If the entry returns, exit the thread */
    ThreadExit(0);
}

/* ------------------------------------------------------------------ */
/*  Idle thread                                                        */
/* ------------------------------------------------------------------ */

/*
 * The idle thread runs at the lowest priority and simply halts the
 * CPU until the next interrupt arrives.
 */
static void IdleThreadFunc(void *arg) {
    (void)arg;
    for (;;) {
        __asm__ volatile("sti; hlt");
    }
}

/* ------------------------------------------------------------------ */
/*  Free-list helpers                                                  */
/* ------------------------------------------------------------------ */

/*
 * Build the free list from indices 1 .. MAX_THREADS-1.
 * Index 0 is reserved for the idle thread and is NOT on the free list.
 */
static void InitFreeList(void) {
    s_free_list = NULL;
    for (int i = MAX_THREADS - 1; i >= 1; i--) {
        s_thread_table[i].next = s_free_list;
        s_free_list            = &s_thread_table[i];
    }
}

/*
 * Allocate a thread_t from the free list.
 * Returns NULL when the table is full.
 */
static thread_t *alloc_thread(void) {
    if (!s_free_list)
        return NULL;

    thread_t *t = s_free_list;
    s_free_list = t->next;

    /* Zero every field explicitly (avoids <string.h> dependency) */
    t->pid             = 0;
    t->state           = THREAD_STATE_READY;
    t->priority        = 0;
    t->rsp             = 0;
    t->rbx             = 0;
    t->rbp             = 0;
    t->r12             = 0;
    t->r13             = 0;
    t->r14             = 0;
    t->r15             = 0;
    t->rflags          = 0;
    t->rip             = 0;
    t->addr_space      = NULL;
    t->kstack_base     = 0;
    t->kstack_top      = 0;
    t->blocked_port    = PORT_NULL;
    t->pending_signals = 0;
    t->wait_mask       = 0;
    for (u8 i = 0; i < MAX_HELD_MUTEXES; i++)
        t->held_mutexes[i] = 0;
    t->held_mutex_count = 0;
    t->next             = NULL;
    t->affinity         = 0;
    t->exit_code        = 0;
    t->joiner_tid       = -1;
    t->force_exit       = false; /* MUST clear: a recycled slot from a
                                  * force-killed thread (SIGKILL) would
                                  * otherwise kill the new thread at its
                                  * first checkpoint (exit code 0) */
    t->wake_tick        = 0;
    t->sleep_next       = NULL;
    t->user_rsp         = 0;
    t->user_stack_phys  = 0;
    t->vruntime         = 0;
    RbInitNode(&t->rb);

    /* TID = index in the static table */
    t->tid = (tid_t)(t - s_thread_table);

    /* Seed the FPU/SSE slot to x86 defaults (fresh AND recycled
     * slots): fpu_switch's fxrstor must always see valid state. */
    FpuStateInit(t->tid);

    return t;
}

/* Forward declaration — defined below. */
static void FreeThread(thread_t *t);

/*
 * Release a FINISHED thread's slot back to the free list.  Public
 * wrapper around FreeThread() for reapers (thread_join, ProcessReap)
 * that run in a different thread's context.  Guards against releasing
 * the current thread (would free the stack we run on), NULL, and
 * slots that are still live (RUNNING/BLOCKED) — those must never be
 * recycled.
 */
void ThreadRelease(thread_t *t) {
    if (!t || t == thread_current())
        return;
    if (t->state != THREAD_STATE_FINISHED)
        return;
    FreeThread(t);
}

/* ------------------------------------------------------------------ */

/*
 * Return a thread_t to the free list.
 */
static void FreeThread(thread_t *t) {
    if (!t)
        return;

    /*
     * A thread returned to the free list must not own resources:
     * release its kernel-stack pages before recycling the slot.
     * kstack_base is a virtual address (stack_phys + KERNEL_VIRT_BASE),
     * so convert it back to physical for pmm_free_pages.
     */
    if (t->kstack_base) {
        PmmFreePages(t->kstack_base - KERNEL_VIRT_BASE, KSTACK_PAGES);
        t->kstack_base = 0;
        t->kstack_top  = 0;
    }

    /*
     * Release the per-thread user-stack mapping.  The page was mapped
     * at as->stack_base + tid * PAGE_SIZE by ThreadCreateUser(); if
     * it is left in place, a recycled TID re-maps the same virtual
     * address and VmmMap() refuses with ERR_BUSY (stale PTE).  Unmap
     * it and hand the physical page back to the PMM.  Kernel threads
     * and threads that never reached the mapping (setup failed before
     * vmm_map) have user_stack_phys == 0 and are skipped.
     */
    if (t->user_stack_phys) {
        if (t->user_rsp && t->addr_space)
            VmmUnmapRange(t->addr_space,
                            t->user_rsp - (u64)USER_STACK_PAGES * PAGE_SIZE,
                            USER_STACK_PAGES);
        PmmFreePages(t->user_stack_phys, USER_STACK_PAGES);
        t->user_stack_phys = 0;
        t->user_rsp        = 0;
    }

    /*
     * Reset the state so thread_get() rejects a recycled slot:
     * its guard is (state == THREAD_STATE_READY && kstack_base == 0).
     * Without this, a stale TID would resolve to the recycled slot and
     * could double-join or observe a half-initialized new thread.
     */
    t->state = THREAD_STATE_READY;

    t->next     = s_free_list;
    s_free_list = t;
}

/* ------------------------------------------------------------------ */
/*  Common thread setup (kernel & user)                                */
/* ------------------------------------------------------------------ */

/*
 * Allocate a stack and lay out the initial context frame that
 * context_switch will restore.  The frame layout (growing DOWN
 * from kstack_top) is:
 *
 *   kstack_top -  8 : rip   = thread_trampoline
 *   kstack_top - 16 : rflags= 0x200  (IF enabled)
 *   kstack_top - 24 : r15   = 0
 *   kstack_top - 32 : r14   = 0
 *   kstack_top - 40 : r13   = 0
 *   kstack_top - 48 : r12   = 0
 *   kstack_top - 56 : rbp   = 0
 *   kstack_top - 64 : rbx   = 0
 *
 * rsp is set to kstack_top - 64.
 */
static int SetupThreadStack(thread_t *t, void (*entry)(void *), void *arg) {
    /* Allocate physical pages for the kernel stack */
    u64 stack_phys = PmmAllocPages(KSTACK_PAGES);
    if (!stack_phys)
        return ERR_NOMEM;

    /*
     * Assume a direct physical-to-virtual mapping at KERNEL_VIRT_BASE.
     * This is the standard higher-half layout established by vmm_init.
     */
    t->kstack_base = stack_phys + KERNEL_VIRT_BASE;
    t->kstack_top  = t->kstack_base + KSTACK_PAGES * PAGE_SIZE;

    /* Build the fake "saved context" frame on the stack */
    u64 *sp = (u64 *)t->kstack_top;

    *(--sp) = 0;                      /* rbx  */
    *(--sp) = 0;                      /* rbp  */
    *(--sp) = 0;                      /* r12  */
    *(--sp) = 0;                      /* r13  */
    *(--sp) = 0;                      /* r14  */
    *(--sp) = 0;                      /* r15  */
    *(--sp) = 0x200;                  /* rflags (IF set) */
    *(--sp) = (u64)ThreadTrampoline; /* rip  */

    t->rsp = (u64)sp;

    /*
     * IMPORTANT: context_switch loads rip and rflags from the thread_t
     * struct fields, NOT from the stack frame above.  The stack frame
     * is only consumed if context_switch restored from the stack (which
     * it doesn't).  So we must also set these fields here.
     */
    t->rip    = (u64)ThreadTrampoline;
    t->rflags = 0x200; /* IF enabled */

    /* Record the real entry point for the trampoline */
    s_start_entry[t->tid] = entry;
    s_start_arg[t->tid]   = arg;

    return OK;
}

/* ================================================================== */
/*  PUBLIC API                                                         */
/* ================================================================== */

void ThreadInit(void) {
    /*
     * Step 1-2: Initialise table and free list.
     * Index 0 is excluded from the free list (reserved for idle).
     */
    InitFreeList();

    /* Step 3: Create the idle thread at TID 0 (manually). */
    thread_t *idle = &s_thread_table[0];

    idle->tid        = 0;
    idle->pid        = 0;
    idle->state      = THREAD_STATE_RUNNING;
    idle->priority   = 0;
    idle->affinity   = -1; /* any CPU */
    idle->addr_space = vmm_get_kernel_addr_space();
    FpuStateInit(0);     /* idle's FPU slot: valid for fxrstor */

    if (SetupThreadStack(idle, IdleThreadFunc, NULL) != OK) {
        panic("thread: cannot allocate idle stack");
    }

    /* Step 4: Make idle the current thread for CPU 0. */
    /* Note: SchedInit() is called by kernel_main before ThreadInit() */
    SchedSwitchTo(idle);
}

/* ------------------------------------------------------------------ */

tid_t ThreadCreateKernel(void (*entry)(void *), void *arg, int priority) {
    thread_t *t = alloc_thread();
    if (!t)
        return ERR_NOMEM;

    t->pid        = 0; /* kernel thread — no user process */
    t->state      = THREAD_STATE_READY;
    t->priority   = priority;
    t->affinity   = -1;
    t->addr_space = vmm_get_kernel_addr_space();

    int rc = SetupThreadStack(t, entry, arg);
    if (rc != OK) {
        FreeThread(t);
        return rc;
    }

    SchedEnqueue(t);
    return t->tid;
}

/* ------------------------------------------------------------------ */

tid_t ThreadCreateUser(u64 entry, u64 arg, addr_space_t *as, int priority) {
    thread_t *t = alloc_thread();
    if (!t)
        return ERR_NOMEM;

    t->pid        = 0; /* caller (process_create) will set pid */
    t->state      = THREAD_STATE_READY;
    t->priority   = priority;
    t->affinity   = -1;
    t->addr_space = as;

    /*
     * Allocate a dedicated USER_STACK_PAGES-page user stack.
     * Virtual address: as->stack_base + tid*USER_STACK_PAGES*PAGE_SIZE,
     * where stack_base is a random ASLR_STACK_BLOCK-aligned region base
     * chosen per address space (ASLR, rng.h).  The per-tid offset
     * guarantees threads of one process never collide.  Top of stack
     * (RSP starts here, grows downward).
     */
    u64 user_stack_phys = PmmAllocPages(USER_STACK_PAGES);
    if (!user_stack_phys) {
        FreeThread(t);
        return ERR_NOMEM;
    }
    t->user_stack_phys      = user_stack_phys;
    u64     user_stack_virt =
        as->stack_base + (u64)t->tid * USER_STACK_PAGES * PAGE_SIZE;
    error_t err             = VmmMapRange(as,
                                            user_stack_virt,
                                            user_stack_phys,
                                            USER_STACK_PAGES,
                                            PTE_PRESENT | PTE_WRITABLE | PTE_USER |
                                                PTE_NO_EXECUTE);
    if (err != OK) {
        FreeThread(t);
        return err;
    }
    t->user_rsp = user_stack_virt + (u64)USER_STACK_PAGES * PAGE_SIZE; /* top */

    int rc = SetupThreadStack(t, (void (*)(void *))entry, (void *)(uptr)arg);
    if (rc != OK) {
        FreeThread(t);
        return rc;
    }

    SchedEnqueue(t);
    return t->tid;
}

/* ------------------------------------------------------------------ */

void ThreadExit(int code) {
    thread_t *cur = thread_current();
    if (!cur)
        return;

    /*
     * Hand every mutex this thread still holds to the next waiter (or
     * free it).  Must happen while we are still RUNNING: the waiters
     * get woken via SchedEnqueue(), which is safe because the running
     * thread is never in the CFS tree.
     */
    MutexReleaseAll(cur);

    cur->exit_code = code;
    cur->state     = THREAD_STATE_FINISHED;

    /* Wake the joiner if one is waiting */
    if (cur->joiner_tid >= 0) {
        thread_t *joiner = thread_get(cur->joiner_tid);
        if (joiner && joiner->state == THREAD_STATE_BLOCKED) {
            joiner->state = THREAD_STATE_READY;
            SchedEnqueue(joiner);
        }
        cur->joiner_tid = -1;
    }

    /* Process-level bookkeeping: if this was the process's last thread,
     * mark the process ZOMBIE and wake any process_wait() caller.
     * Kernel threads (pid == 0) do not belong to a user process. */
    if (cur->pid > 0) {
        process_t *proc = process_get(cur->pid);
        if (proc)
            ProcessThreadExited(proc, code);
    }

    /* Remove from the scheduler if it was queued */
    SchedDequeue(cur);

    /* Give up the CPU — this must never return */
    SchedReschedule();

    /* Should be unreachable — sched_reschedule never returns for FINISHED threads */
    panic("thread: sched_reschedule returned for FINISHED thread TID=%d", cur->tid);
}

/* ------------------------------------------------------------------ */

void ThreadYield(void) {
    SchedReschedule();
}

/* ------------------------------------------------------------------ */

thread_t *thread_current(void) {
    return sched_get_current();
}

/* ------------------------------------------------------------------ */

thread_t *thread_get(tid_t tid) {
    if (tid < 0 || tid >= MAX_THREADS)
        return NULL;

    thread_t *t = &s_thread_table[tid];

    /* A slot is "used" if its TID matches the index (always true
     * after alloc_thread), but also reject the free-list entries
     * by checking the state — free slots are zeroed.              */
    if (t->state == 0 && t->kstack_base == 0)
        return NULL;

    return t;
}

/* ------------------------------------------------------------------ */

error_t ThreadSetAffinity(tid_t tid, i32 cpu) {
    thread_t *t = thread_get(tid);
    if (!t)
        return ERR_INVAL;

    t->affinity = cpu;
    return OK;
}
