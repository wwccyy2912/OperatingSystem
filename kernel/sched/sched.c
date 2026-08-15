/*
 * sched.c - CFS (Completely Fair Scheduler) with red-black tree
 * Copyright (c) 2026 OpSys Project
 *
 * Each thread has a virtual runtime (vruntime) that tracks its
 * weighted CPU consumption.  Threads are stored in a red-black
 * tree keyed by vruntime.  The leftmost node (lowest vruntime)
 * is always the next to run.
 *
 * Weight calculation: higher priority → higher weight → slower
 * vruntime growth → more CPU time.
 *
 * vruntime_increment = delta * (NICE_0_LOAD / weight)
 *
 * Nice 0 (priority 10) is the baseline weight.
 */

#include <kernel/sched.h>
#include <kernel/gdt.h>
#include <kernel/rbtree.h>
#include <kernel/thread.h>

#define MAX_CPUS    1
#define NICE_0_LOAD 1024

/* Per-thread FPU/SSE state buffers.  Each buffer is 512 bytes and
 * 16-byte aligned (required by fxsave/fxrstor).  Indexed by TID.
 * Using a separate array avoids struct-layout alignment issues. */
static u8 s_fpu_state[MAX_THREADS][512] __attribute__((aligned(16)));

/* Save/restore FPU state around context_switch so user-space programs
 * can use SSE/SSE2 floating-point.  Called with interrupts disabled,
 * immediately before context_switch().  Saves prev's live FPU state and
 * loads next's saved state — context_switch's IRETQ transfer preserves
 * the FPU registers as-is, so the load takes effect for next. */
static inline void fpu_switch(thread_t *prev, thread_t *next) {
    /* TEMPORARILY DISABLED - causes #GP */
    (void)prev;
    (void)next;
    return;
}

/* CFS weights indexed by priority (0 = lowest, 31 = highest) */
static const int cfs_weights[32] = {
    /*  0 */ 15,   20,   25,   30,   35,   45,   55,   70,
    /*  8 */ 85,   100,  120,  145,  175,  210,  255,  310,
    /* 16 */ 375,  450,  540,  650,  780,  940,  1130, 1360,
    /* 24 */ 1630, 1960, 2350, 2820, 3380, 4060, 4870, 5850,
};

/* ------------------------------------------------------------------ */
/*  Internal data                                                      */
/* ------------------------------------------------------------------ */

/* RB tree of ready threads, ordered by vruntime (leftmost = next) */
static rb_root_t s_ready_tree;

/* Currently running thread per CPU */
static thread_t *s_current[MAX_CPUS];

/*
 * Global pointer to the currently running thread.
 * Mirrors s_current[0] so that assembly code (syscall_entry.S)
 * can access the current TCB without going through a C function.
 */
thread_t *g_current_thread;

/* Monotonic tick counter, incremented each timer interrupt */
static u64 s_ticks;

/*
 * Sleepers are kept on a singly-linked list sorted by wake_tick
 * (absolute, ascending).  Each tick, wake_sleepers() only inspects the
 * list HEAD: pop + re-enqueue every entry whose deadline has passed.
 * Amortized O(1) per tick instead of scanning all MAX_THREADS.
 *
 * Invariants (enforced to prevent CFS double-inserts):
 *  - sched_sleep() inserts a thread into the list, sets it BLOCKED and
 *    dequeues it from the ready tree while interrupts are disabled, so
 *    the timer IRQ can never observe a thread that is simultaneously on
 *    this list and in the ready tree.
 *  - wake_sleepers() only re-enqueues entries whose state is still
 *    BLOCKED: a sleeper woken early by another path (all wake paths
 *    match state == BLOCKED, and a sleeping thread IS BLOCKED) is
 *    already READY and in the ready tree.
 *  - sched_enqueue() is idempotent (no-op when the node is in_tree).
 */
static thread_t *s_sleep_list;

/* Forward declarations */
static void wake_sleepers(void);

/* ------------------------------------------------------------------ */
/*  Init                                                              */
/* ------------------------------------------------------------------ */

void sched_init(void) {
    rb_init(&s_ready_tree);
    for (int i = 0; i < MAX_CPUS; i++)
        s_current[i] = NULL;
}

/* ------------------------------------------------------------------ */
/*  Weight helper                                                      */
/* ------------------------------------------------------------------ */

static int sched_weight(int priority) {
    if (priority < 0)
        priority = 0;
    if (priority > 31)
        priority = 31;
    return cfs_weights[priority];
}

/* ------------------------------------------------------------------ */
/*  Enqueue / Dequeue                                                 */
/* ------------------------------------------------------------------ */

void sched_enqueue(thread_t *t) {
    if (!t)
        return;

    /* Idempotency guard: a thread can be woken twice (e.g. by a wake
     * path and then by wake_sleepers() while it was on the sleep list).
     * rb_insert() has NO in_tree check and would re-link a node that is
     * already linked in the tree, creating a self-cycle that makes
     * rb_insert_fixup() spin forever.  Symmetric with rb_remove(). */
    if (t->rb.in_tree)
        return;

    t->state  = THREAD_STATE_READY;
    t->rb.key = t->vruntime;
    rb_insert(&s_ready_tree, &t->rb);
}

void sched_dequeue(thread_t *t) {
    if (!t)
        return;

    rb_remove(&s_ready_tree, &t->rb);
}

/* ------------------------------------------------------------------ */
/*  Pick next thread (leftmost = lowest vruntime)                     */
/* ------------------------------------------------------------------ */

static thread_t *pick_next(void) {
    rb_node_t *leftmost = rb_min(&s_ready_tree);
    if (!leftmost)
        return NULL;

    /* Get the thread_t from the rb_node_t (container_of) */
    return (thread_t *)((char *)leftmost - offsetof(thread_t, rb));
}

/* ------------------------------------------------------------------ */
/*  Tick (called from timer IRQ handler)                              */
/* ------------------------------------------------------------------ */

/*
 * CFS preemption + context switch, shared by the PIT tick and the
 * explicit reschedule points.  Picks the lowest-vruntime ready thread
 * and switches to it, accounting the current thread's CPU usage.
 *
 * Does NOT advance s_ticks: the time base belongs exclusively to the
 * PIT IRQ0 (sched_tick).  Blocking/yield paths (join, wait, sleep,
 * exit, yield) call sched_reschedule() purely to hand over the CPU;
 * if they called this via sched_tick(), every blocking operation
 * would advance the clock, inflating the tick rate far beyond the
 * PIT's 100 Hz (observed ~1500-2000 ticks/s) and making sleep()/
 * get_time() load-dependent instead of wall-clock based.
 */
static void reschedule(int resume_if) {
    thread_t *cur = s_current[0];
    if (!cur)
        return;

    /* Increment vruntime: delta = 1 tick, scaled by weight.
     * Integer division truncates; if weight > NICE_0_LOAD the
     * result is zero, which would let high-priority threads never
     * advance vruntime and thus monopolise the CPU.  Clamp to 1. */
    int weight = sched_weight(cur->priority);
    u64 delta  = (u64)(NICE_0_LOAD / weight);
    if (delta == 0)
        delta = 1;
    cur->vruntime += delta;

    /* Preempt if a ready thread has lower vruntime */
    thread_t *next = pick_next();

    if (!next) {
        /* Ready tree is empty. If the current thread is still runnable
         * (a pure voluntary yield with no competitors), keep running it:
         * switching to idle here would halt until the next PIT tick
         * (10 ms) — the old idle-in-tree stall, measured as 1000 ticks
         * per 1000 solo yields. Only when the current thread is BLOCKED
         * or FINISHED (it cannot continue) do we fall back to idle,
         * which is never in the ready tree and is switched to directly
         * (see kernel_main.c). */
        if (cur->state == THREAD_STATE_RUNNING || cur->state == THREAD_STATE_READY)
            return;

        thread_t *idle = thread_get(0);
        if (idle && idle != cur) {
            idle->state      = THREAD_STATE_RUNNING;
            s_current[0]     = idle;
            g_current_thread = idle;
            gdt_set_tss_rsp0(idle->kstack_top);
            /* Enter context_switch with IF=0 (see comment below). */
            __asm__ volatile("cli" ::: "memory");
            fpu_switch(cur, idle);
            context_switch(cur, idle, resume_if);
            __asm__ volatile("" ::: "memory");
        }
        return;
    }

    /* Preempt: current thread is still runnable.
     * Idle (tid 0) is never re-enqueued: it must not compete in the
     * ready tree as a CFS thread (see kernel_main.c). When idle is
     * current and someone is woken, we switch to the woken thread and
     * leave idle out of the tree. */
    if (cur->tid != 0 && (cur->state == THREAD_STATE_RUNNING || cur->state == THREAD_STATE_READY)) {
        sched_dequeue(cur);
        sched_enqueue(cur);
    }

    /* Switch to next */
    sched_dequeue(next);
    next->state      = THREAD_STATE_RUNNING;
    s_current[0]     = next;
    g_current_thread = next;

    if (cur != next) {
        gdt_set_tss_rsp0(next->kstack_top);
        /* Enter context_switch with IF=0: the whole save/restore is
         * interrupt-atomic (see context_switch.S).  cli must precede
         * the call — an IRQ at the call/entry boundary would nest a
         * context_switch into a half-prepared state. */
        __asm__ volatile("cli" ::: "memory");
        fpu_switch(cur, next);
        context_switch(cur, next, resume_if);
    }

    /* Prevent tail-call optimization (see original sched.c) */
    __asm__ volatile("" ::: "memory");
}

/*
 * Timer tick — called ONLY from the PIT IRQ0 handler (idt.c).  This is
 * the single source of truth for the time base: s_ticks advances at
 * the PIT rate (100 Hz) and wake_sleepers() runs on the same cadence.
 */
void sched_tick(void) {
    s_ticks++;
    wake_sleepers();
    reschedule(0); /* preempted: prev resumes via the ISR iretq (IF=0) */
}

/*
 * Explicit reschedule from a blocking/yield path: join, process_wait,
 * sched_sleep, thread_exit, thread_yield.  Pure context switch — the
 * clock (s_ticks) must NOT advance here, otherwise every blocking
 * operation becomes a "tick" and time drifts with system load.
 */
void sched_reschedule(void) {
    reschedule(1); /* voluntary: prev resumes at its continuation (IF=1) */
}

/* ------------------------------------------------------------------ */
/*  Direct switch                                                     */
/* ------------------------------------------------------------------ */

void sched_switch_to(thread_t *t) {
    if (!t)
        return;

    thread_t *cur = s_current[0];
    if (cur == t)
        return;

    sched_dequeue(t);

    t->state         = THREAD_STATE_RUNNING;
    s_current[0]     = t;
    g_current_thread = t;

    if (cur) {
        gdt_set_tss_rsp0(t->kstack_top);
        /* Interrupt-atomic switch (see context_switch.S). */
        __asm__ volatile("cli" ::: "memory");
        fpu_switch(cur, t);
        context_switch(cur, t, 1); /* direct switch: prev resumes with IF=1 */
    }
}

/* ------------------------------------------------------------------ */
/*  Current-thread accessor (no context switch)                        */
/* ------------------------------------------------------------------ */

void sched_set_current(thread_t *t) {
    /* The running thread must never sit in the ready tree: once its
     * vruntime grows in place, its rb.key is a stale snapshot that
     * keeps rb_min() returning this thread forever (init-thread
     * starvation).  sched_dequeue() is a no-op for a thread that is
     * not in the tree (rb_remove guards on in_tree). */
    sched_dequeue(t);

    s_current[0]     = t;
    g_current_thread = t;
}

/* ------------------------------------------------------------------ */
/*  Queries                                                            */
/* ------------------------------------------------------------------ */

thread_t *sched_get_current(void) {
    return s_current[0];
}

u32 sched_get_thread_count(void) {
    return (u32)(s_ready_tree.count + (s_current[0] ? 1 : 0));
}

u64 sched_get_ticks(void) {
    return s_ticks;
}

/* ------------------------------------------------------------------ */
/*  Sleep (blocking timeout)                                           */
/* ------------------------------------------------------------------ */

void sched_sleep(u64 ticks) {
    thread_t *cur = s_current[0];
    if (!cur)
        return;

    if (ticks == 0)
        ticks = 1;

    /*
     * Critical section: the sleep-list insert, the state change and the
     * ready-tree dequeue must be atomic w.r.t. the timer IRQ.
     * wake_sleepers() runs from sched_tick() (PIT IRQ0 context); if it
     * observed this thread between the list insert and the dequeue, it
     * would re-enqueue a thread that is STILL linked in the ready tree ->
     * double insert -> rb_insert() self-cycle -> infinite loop (the
     * `sleep 1` hang).  cli/sti suffices: on this single-CPU kernel the
     * PIT IRQ is the only preemption source.
     */
    __asm__ volatile("cli" ::: "memory");

    /* Insert into the sorted sleep list: absolute deadline, ascending */
    cur->wake_tick = s_ticks + ticks;
    thread_t **pp  = &s_sleep_list;
    while (*pp && (*pp)->wake_tick <= cur->wake_tick)
        pp = &(*pp)->sleep_next;
    cur->sleep_next = *pp;
    *pp             = cur;

    cur->state = THREAD_STATE_BLOCKED;
    sched_dequeue(cur);

    __asm__ volatile("sti" ::: "memory");

    /* Pure reschedule — the clock (s_ticks) must not advance here.
     * The wakeup is done by wake_sleepers() on the next PIT tick. */
    sched_reschedule();
}

/*
 * Unlink a thread from the sleep list.  Called when a blocked sleeper
 * is woken early by a path other than wake_sleepers() (e.g. a dying
 * process force-waking its threads): between the wake and the sleep
 * deadline the thread must never sit simultaneously on the sleep list
 * and in the ready tree.
 *
 * Must be called with interrupts disabled (syscall/interrupt context,
 * IF=0), the same requirement as sched_sleep: wake_sleepers() runs
 * from the PIT IRQ and must not observe a half-unlinked entry.
 */
void sched_unsleep(thread_t *t) {
    if (!t)
        return;

    /* wake_tick != 0 is the "on sleep list" sentinel: sched_sleep sets
     * it to s_ticks + ticks (>= 1) before inserting, wake_sleepers()
     * clears it when popping, alloc_thread zeroes it at creation. */
    thread_t **pp = &s_sleep_list;
    while (*pp && *pp != t)
        pp = &(*pp)->sleep_next;

    if (*pp == t) {
        *pp           = t->sleep_next;
        t->sleep_next = NULL;
    }

    /* Defensive: even if t was not found on the list, make sure it is
     * no longer marked as a sleeper. */
    t->wake_tick = 0;
}

/*
 * Called once per tick.  Wake every sleeper whose absolute deadline has
 * passed.  The list is sorted by wake_tick, so only the head chain
 * needs inspection -- amortized O(1) per tick.
 */
static void wake_sleepers(void) {
    while (s_sleep_list && s_sleep_list->wake_tick <= s_ticks) {
        thread_t *t   = s_sleep_list;
        s_sleep_list  = t->sleep_next;
        t->sleep_next = NULL;
        t->wake_tick  = 0;

        /* Wake only threads that are still blocked.  A sleeper can be
         * woken early by another path while on this list (every wake
         * path matches state == BLOCKED, and a sleeping thread IS
         * BLOCKED); such a thread is already READY and enqueued, so
         * re-enqueueing it would double-insert it into the CFS tree.
         * The list entry is removed either way. */
        if (t->state != THREAD_STATE_BLOCKED)
            continue;

        t->state = THREAD_STATE_READY;
        sched_enqueue(t);
    }
}
