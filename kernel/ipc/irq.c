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
 * irq.c - IRQ binding table (interrupt → notification forwarding)
 * Copyright (c) 2026 OpSys Project
 *
 * A static 16-entry table binds each hardware IRQ line (0-15) to at
 * most one owner thread.  When a hardware IRQ fires, IrqHandle() is
 * called from the ISR and forwards the event to the bound thread as
 * notification bits via Notify().  If the bound thread has exited
 * (Notify() returns ERR_NOENT) the binding self-heals: the entry is
 * cleared and the IRQ line is disabled.
 * v0.1: Single-CPU, no spinlocks. *
 * ------------------------------------------------------------------
 * Structure (irq):
 *   IRQ capability (cap type IRQ, vector) -> BindIrq: installs a
 *   kernel handler that posts the owning thread's notify bit.
 * How it works:
 *   IrqHandle() looks up the bound handler by vector and wakes the
 *   waiter; user drivers bind via sys_bind_irq + wait_notification.
 * Purpose:
 *   Route device interrupts to user-space driver threads safely.
 * Caveats:
 *   Only one binder per vector; the handler runs on the kernel stack
 *   so it must stay short (defer work to the user thread).
 * ------------------------------------------------------------------
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
error_t IrqBind(u8 irq, u32 mask) {
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

    IrqEnable(irq);
    return OK;
}

/*
 * Clear the binding for an IRQ line and disable it.
 * Idempotent: unbinding an unbound IRQ is OK.
 */
error_t IrqUnbind(u8 irq) {
    if (irq >= IRQ_BINDING_MAX)
        return ERR_INVAL;

    if (s_irq_bindings[irq].active) {
        s_irq_bindings[irq].active = false;
        IrqDisable(irq);
    }
    return OK;
}

/*
 * Release every IRQ line bound by threads of a dying process.
 * Complement to irq_handle's lazy self-heal: the line is freed and
 * disabled immediately on process death instead of waiting for the
 * next IRQ to discover the dead owner.  Called from ProcessReap().
 */
void IrqCleanupProcess(pid_t pid) {
    for (u32 i = 0; i < IRQ_BINDING_MAX; i++) {
        if (!s_irq_bindings[i].active)
            continue;
        thread_t *t = thread_get(s_irq_bindings[i].tid);
        if (!t || t->pid == pid)
            IrqUnbind((u8)i);
    }
}

/*
 * ISR-side forwarding.  Returns true if a binding was active, so the
 * caller knows the IRQ was consumed (forwarded, or self-cleaned after
 * the owner thread died).
 */
bool IrqHandle(u8 irq) {
    if (irq >= IRQ_BINDING_MAX)
        return false;
    if (!s_irq_bindings[irq].active)
        return false;

    error_t err = Notify(s_irq_bindings[irq].tid, s_irq_bindings[irq].mask);

    /* Bound thread died: self-heal — clear the binding, disable the IRQ */
    if (err == ERR_NOENT) {
        s_irq_bindings[irq].active = false;
        IrqDisable(irq);
    }
    return true;
}
