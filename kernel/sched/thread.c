/*
 * thread.c - Thread creation and management
 * Copyright (c) 2026 OpSys Project
 *
 * Maintains a static thread table indexed by TID.  A singly-linked
 * free list provides O(1) allocation.  Each new kernel thread gets
 * an 8-page (32 KiB) kernel stack allocated from the PMM.  The
 * initial stack frame is laid out so that the first context_switch
 * into the thread will pop straight into thread_trampoline().
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
#define KSTACK_PAGES    8

/* ------------------------------------------------------------------ */
/*  Internal data                                                      */
/* ------------------------------------------------------------------ */

/* Static thread table — indexed by TID */
static thread_t s_thread_table[MAX_THREADS];

/* Head of the singly-linked free list of unused thread_t slots */
static thread_t *s_free_list;

/* Per-thread start info used by thread_trampoline */
static void (*s_start_entry[MAX_THREADS])(void *);
static void            *s_start_arg[MAX_THREADS];

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
static void thread_trampoline(void)
{
    thread_t *t = sched_get_current();
    if (!t)
        return;

    void (*entry)(void *) = s_start_entry[t->tid];
    void            *arg = s_start_arg[t->tid];

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
        *(--sp) = 0x23ULL;        /* SS  — user data segment */
        *(--sp) = t->user_rsp;    /* RSP — user stack top    */
        *(--sp) = 0x202ULL;       /* RFLAGS — IF=1           */
        *(--sp) = 0x1BULL;        /* CS  — user code segment */
        *(--sp) = (u64)entry;     /* RIP — user entry point  */

        /* Set RDI = arg then IRETQ (never returns) */
        __asm__ volatile (
            "mov  %0, %%rdi\n"
            "mov  %1, %%rsp\n"
            "iretq\n"
            :
            : "r"(arg), "r"(sp)
            : "rdi", "memory"
        );
        __builtin_unreachable();
    }

    /* Kernel thread: call entry directly in ring 0 */
    entry(arg);

    /* If the entry returns, exit the thread */
    thread_exit(0);
}

/* ------------------------------------------------------------------ */
/*  Idle thread                                                        */
/* ------------------------------------------------------------------ */

/*
 * The idle thread runs at the lowest priority and simply halts the
 * CPU until the next interrupt arrives.
 */
static void idle_thread_func(void *arg)
{
    (void)arg;
    for (;;) {
        __asm__ volatile ("sti; hlt");
    }
}

/* ------------------------------------------------------------------ */
/*  Free-list helpers                                                  */
/* ------------------------------------------------------------------ */

/*
 * Build the free list from indices 1 .. MAX_THREADS-1.
 * Index 0 is reserved for the idle thread and is NOT on the free list.
 */
static void init_free_list(void)
{
    s_free_list = NULL;
    for (int i = MAX_THREADS - 1; i >= 1; i--) {
        s_thread_table[i].next = s_free_list;
        s_free_list = &s_thread_table[i];
    }
}

/*
 * Allocate a thread_t from the free list.
 * Returns NULL when the table is full.
 */
static thread_t *alloc_thread(void)
{
    if (!s_free_list)
        return NULL;

    thread_t *t = s_free_list;
    s_free_list = t->next;

    /* Zero every field explicitly (avoids <string.h> dependency) */
    t->pid           = 0;
    t->state         = THREAD_STATE_READY;
    t->priority      = 0;
    t->rsp           = 0;
    t->rbx           = 0;
    t->rbp           = 0;
    t->r12           = 0;
    t->r13           = 0;
    t->r14           = 0;
    t->r15           = 0;
    t->rflags        = 0;
    t->rip           = 0;
    t->addr_space    = NULL;
    t->kstack_base   = 0;
    t->kstack_top    = 0;
    t->blocked_port  = PORT_NULL;
    t->pending_signals = 0;
    t->wait_mask       = 0;
    for (u8 i = 0; i < MAX_HELD_MUTEXES; i++)
        t->held_mutexes[i] = 0;
    t->held_mutex_count = 0;
    t->next          = NULL;
    t->time_slice    = 0;
    t->affinity      = 0;
    t->exit_code     = 0;
    t->joiner_tid    = -1;
    t->wake_tick     = 0;
    t->sleep_next    = NULL;
    t->user_rsp      = 0;
    t->vruntime      = 0;
    rb_init_node(&t->rb);

    /* TID = index in the static table */
    t->tid = (tid_t)(t - s_thread_table);

    return t;
}

/*
 * Return a thread_t to the free list.
 */
static void free_thread(thread_t *t)
{
    if (!t)
        return;

    /*
     * A thread returned to the free list must not own resources:
     * release its kernel-stack pages before recycling the slot.
     * kstack_base is a virtual address (stack_phys + KERNEL_VIRT_BASE),
     * so convert it back to physical for pmm_free_pages.
     */
    if (t->kstack_base) {
        pmm_free_pages(t->kstack_base - KERNEL_VIRT_BASE, KSTACK_PAGES);
        t->kstack_base = 0;
        t->kstack_top  = 0;
    }

    t->next    = s_free_list;
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
static int setup_thread_stack(thread_t *t, void (*entry)(void *), void *arg)
{
    /* Allocate physical pages for the kernel stack */
    u64 stack_phys = pmm_alloc_pages(KSTACK_PAGES);
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

    *(--sp) = 0;                            /* rbx  */
    *(--sp) = 0;                            /* rbp  */
    *(--sp) = 0;                            /* r12  */
    *(--sp) = 0;                            /* r13  */
    *(--sp) = 0;                            /* r14  */
    *(--sp) = 0;                            /* r15  */
    *(--sp) = 0x200;                        /* rflags (IF set) */
    *(--sp) = (u64)thread_trampoline;       /* rip  */

    t->rsp = (u64)sp;

    /*
     * IMPORTANT: context_switch loads rip and rflags from the thread_t
     * struct fields, NOT from the stack frame above.  The stack frame
     * is only consumed if context_switch restored from the stack (which
     * it doesn't).  So we must also set these fields here.
     */
    t->rip    = (u64)thread_trampoline;
    t->rflags = 0x200;  /* IF enabled */

    /* Record the real entry point for the trampoline */
    s_start_entry[t->tid] = entry;
    s_start_arg[t->tid]   = arg;

    return OK;
}

/* ================================================================== */
/*  PUBLIC API                                                         */
/* ================================================================== */

void thread_init(void)
{
    /*
     * Step 1-2: Initialise table and free list.
     * Index 0 is excluded from the free list (reserved for idle).
     */
    init_free_list();

    /* Step 3: Create the idle thread at TID 0 (manually). */
    thread_t *idle = &s_thread_table[0];

    idle->tid        = 0;
    idle->pid        = 0;
    idle->state      = THREAD_STATE_RUNNING;
    idle->priority   = 0;
    idle->time_slice = 1;          /* will be rescheduled after 1 tick */
    idle->affinity   = -1;         /* any CPU */
    idle->addr_space = vmm_get_kernel_addr_space();

    if (setup_thread_stack(idle, idle_thread_func, NULL) != OK) {
        panic("thread: cannot allocate idle stack");
    }

    /* Step 4: Make idle the current thread for CPU 0. */
    /* Note: sched_init() is called by kernel_main before thread_init() */
    sched_switch_to(idle);
}

/* ------------------------------------------------------------------ */

tid_t thread_create_kernel(void (*entry)(void *), void *arg, int priority)
{
    thread_t *t = alloc_thread();
    if (!t)
        return ERR_NOMEM;

    t->pid     = 0;            /* kernel thread — no user process */
    t->state   = THREAD_STATE_READY;
    t->priority = priority;
    t->time_slice = (u64)(priority + 1);
    t->affinity = -1;
    t->addr_space = vmm_get_kernel_addr_space();

    int rc = setup_thread_stack(t, entry, arg);
    if (rc != OK) {
        free_thread(t);
        return rc;
    }

    sched_enqueue(t);
    return t->tid;
}

/* ------------------------------------------------------------------ */

tid_t thread_create_user(u64 entry, u64 arg, addr_space_t *as, int priority)
{
    thread_t *t = alloc_thread();
    if (!t)
        return ERR_NOMEM;

    t->pid     = 0;            /* caller (process_create) will set pid */
    t->state   = THREAD_STATE_READY;
    t->priority = priority;
    t->time_slice = (u64)(priority + 1);
    t->affinity = -1;
    t->addr_space = as;

    /*
     * Allocate a dedicated user-stack page for this thread.
     * Virtual address: as->stack_base + tid * PAGE_SIZE, where
     * stack_base is a random 1 MB-aligned region base chosen per
     * address space at creation (ASLR, design item ⑭ — see rng.h).
     * The per-tid offset guarantees threads of one process never
     * collide; the random base makes the layout unpredictable.
     * Top of stack (RSP starts here, grows downward).
     */
    u64 user_stack_phys = pmm_alloc_page();
    if (!user_stack_phys) {
        free_thread(t);
        return ERR_NOMEM;
    }
    u64 user_stack_virt = as->stack_base + (u64)t->tid * PAGE_SIZE;
    error_t err = vmm_map(as, user_stack_virt, user_stack_phys,
                          PTE_PRESENT | PTE_WRITABLE | PTE_USER |
                          PTE_NO_EXECUTE);
    if (err != OK) {
        pmm_free_page(user_stack_phys);
        free_thread(t);
        return err;
    }
    t->user_rsp = user_stack_virt + PAGE_SIZE;  /* top of page */

    int rc = setup_thread_stack(t, (void (*)(void *))entry,
                                (void *)(uptr)arg);
    if (rc != OK) {
        pmm_free_page(user_stack_phys);
        free_thread(t);
        return rc;
    }

    sched_enqueue(t);
    return t->tid;
}

/* ------------------------------------------------------------------ */

void thread_exit(int code)
{
    thread_t *cur = thread_current();
    if (!cur)
        return;

    /*
     * Hand every mutex this thread still holds to the next waiter (or
     * free it).  Must happen while we are still RUNNING: the waiters
     * get woken via sched_enqueue(), which is safe because the running
     * thread is never in the CFS tree.
     */
    mutex_release_all(cur);

    cur->exit_code = code;
    cur->state = THREAD_STATE_FINISHED;

    /* Wake the joiner if one is waiting */
    if (cur->joiner_tid >= 0) {
        thread_t *joiner = thread_get(cur->joiner_tid);
        if (joiner && joiner->state == THREAD_STATE_BLOCKED) {
            joiner->state = THREAD_STATE_READY;
            sched_enqueue(joiner);
        }
        cur->joiner_tid = -1;
    }

    /* Process-level bookkeeping: if this was the process's last thread,
     * mark the process ZOMBIE and wake any process_wait() caller.
     * Kernel threads (pid == 0) do not belong to a user process. */
    if (cur->pid > 0) {
        process_t *proc = process_get(cur->pid);
        if (proc)
            process_thread_exited(proc, code);
    }

    /* Remove from the scheduler if it was queued */
    sched_dequeue(cur);

    /* Give up the CPU — this must never return */
    sched_reschedule();

    /* Should be unreachable — sched_reschedule never returns for FINISHED threads */
    panic("thread: sched_reschedule returned for FINISHED thread TID=%d", cur->tid);
}

/* ------------------------------------------------------------------ */

void thread_yield(void)
{
    sched_reschedule();
}

/* ------------------------------------------------------------------ */

thread_t *thread_current(void)
{
    return sched_get_current();
}

/* ------------------------------------------------------------------ */

thread_t *thread_get(tid_t tid)
{
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

error_t thread_set_affinity(tid_t tid, i32 cpu)
{
    thread_t *t = thread_get(tid);
    if (!t)
        return ERR_INVAL;

    t->affinity = cpu;
    return OK;
}
