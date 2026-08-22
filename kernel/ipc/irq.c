/*
 * irq.c - IRQ binding table (interrupt → notification forwarding)
 * Copyright (c) 2026 OpSys Project
 *
 * A static 16-entry table binds each hardware IRQ line (0-15) to at
 * most one owner thread.  When a hardware IRQ fires, irq_handle() is
 * called from the ISR and forwards the event to the bound thread as
 * notification bits via notify().  If the bound thread has exited
 * (notify() returns ERR_NOENT) the binding self-heals: the entry is
 * cleared and the IRQ line is disabled.
 * v0.1: Single-CPU, no spinlocks.
 */

#include <kernel/irq.h>
#include <kernel/notify.h>
#include <kernel/thread.h>
#include <kernel/idt.h>

#define IRQ_BINDING_MAX 16 /* PIC IRQ lines 0-15 */

typedef struct {
    tid_t tid;    /* Owner thread */
    u32   mask;   /* Notification bits delivered per event */
    bool  active; /* Binding in effect */
} irq_binding_t;

static irq_binding_t s_irq_bindings[IRQ_BINDING_MAX];

/*
 * Bind the current thread to an IRQ line.
 * Rebinding an already-owned IRQ simply overwrites the entry.
 */
error_t irq_bind(u8 irq, u32 mask) {
    if (irq >= IRQ_BINDING_MAX)
        return ERR_INVAL;

    /* Kernel-reserved lines: IRQ0 = timer, IRQ2 = PIC cascade */
    if (irq == 0 || irq == 2)
        return ERR_DENIED;

    thread_t *cur = thread_current();
    if (!cur)
        return ERR_FAULT;

    s_irq_bindings[irq].tid    = cur->tid;
    s_irq_bindings[irq].mask   = mask;
    s_irq_bindings[irq].active = true;

    irq_enable(irq);
    return OK;
}

/*
 * Clear the binding for an IRQ line and disable it.
 * Idempotent: unbinding an unbound IRQ is OK.
 */
error_t irq_unbind(u8 irq) {
    if (irq >= IRQ_BINDING_MAX)
        return ERR_INVAL;

    if (s_irq_bindings[irq].active) {
        s_irq_bindings[irq].active = false;
        irq_disable(irq);
    }
    return OK;
}

/*
 * Release every IRQ line bound by threads of a dying process.
 * Complement to irq_handle's lazy self-heal: the line is freed and
 * disabled immediately on process death instead of waiting for the
 * next IRQ to discover the dead owner.  Called from process_reap().
 */
void irq_cleanup_process(pid_t pid) {
    for (u32 i = 0; i < IRQ_BINDING_MAX; i++) {
        if (!s_irq_bindings[i].active)
            continue;
        thread_t *t = thread_get(s_irq_bindings[i].tid);
        if (!t || t->pid == pid)
            irq_unbind((u8)i);
    }
}

/*
 * ISR-side forwarding.  Returns true if a binding was active, so the
 * caller knows the IRQ was consumed (forwarded, or self-cleaned after
 * the owner thread died).
 */
bool irq_handle(u8 irq) {
    if (irq >= IRQ_BINDING_MAX)
        return false;
    if (!s_irq_bindings[irq].active)
        return false;

    error_t err = notify(s_irq_bindings[irq].tid, s_irq_bindings[irq].mask);

    /* Bound thread died: self-heal — clear the binding, disable the IRQ */
    if (err == ERR_NOENT) {
        s_irq_bindings[irq].active = false;
        irq_disable(irq);
    }
    return true;
}
