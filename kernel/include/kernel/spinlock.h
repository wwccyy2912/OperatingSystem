/*
 * spinlock.h - Kernel spinlock (single-CPU cli/sti)
 * Copyright (c) 2026 OpSys Project
 *
 * On a single CPU, the only preemption source is the PIT IRQ0 -> sched_tick.
 * Disabling interrupts with cli() therefore gives mutual exclusion for free,
 * and the saved IF flag lets us restore the caller's preemption state
 * exactly (so nested locks don't accidentally enable interrupts early).
 *
 * The API is MP-ready: swapping the body for a lock;xchg test-and-set is a
 * 2-line change when the kernel gains SMP.  The hard rule stays the same
 * either way:
 *
 *   NEVER call thread_yield / sched_reschedule / sched_sleep while
 *   holding a spinlock -- blocking with IF=0 is a guaranteed deadlock.
 */

#ifndef KERNEL_SPINLOCK_H
#define KERNEL_SPINLOCK_H

#include <kernel/types.h>

typedef struct {
    volatile bool locked;
    u64           saved_rflags;
} spinlock_t;

#define SPINLOCK_INIT {.locked = false, .saved_rflags = 0}

static inline void spin_lock(spinlock_t *l) {
    u64 rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags)::"memory");
    l->saved_rflags = rflags;
    l->locked       = true;
}

static inline void spin_unlock(spinlock_t *l) {
    l->locked = false;
    if (l->saved_rflags & 0x200) /* IF was set before cli */
        __asm__ volatile("sti" ::: "memory");
}

#endif /* KERNEL_SPINLOCK_H */
