/*
 * irq.h - IRQ binding (interrupt → notification forwarding)
 * Copyright (c) 2026 OpSys Project
 *
 * Binds a hardware IRQ line to the current thread so device interrupts
 * are delivered as async notification bits (see kernel/ipc/notify.c).
 * One owner per IRQ; the binding table is a static 16-entry array
 * in kernel/ipc/irq.c.
 */

#ifndef KERNEL_IRQ_H
#define KERNEL_IRQ_H

#include <kernel/types.h>

/**
 * Bind the current thread to an IRQ line.
 * @param irq   IRQ number (0-15).  IRQ0 (timer) and IRQ2 (PIC cascade)
 *              are kernel-reserved and rejected with ERR_DENIED.
 * @param mask  Notification bits delivered on each IRQ event.
 * @return OK, ERR_INVAL (irq out of range), ERR_DENIED (reserved IRQ),
 *         or ERR_FAULT (no current thread).
 */
error_t irq_bind(u8 irq, u32 mask);

/**
 * Clear the binding for an IRQ line and disable the IRQ.
 * @param irq  IRQ number (0-15).
 * @return OK (idempotent), or ERR_INVAL if out of range.
 */
error_t irq_unbind(u8 irq);

/**
 * Forward an IRQ to its bound thread (called from the ISR).
 * @param irq  IRQ number (0-15).
 * @return true if a binding was active (forwarded or self-cleaned),
 *         false if unbound.
 */
bool irq_handle(u8 irq);

#endif /* KERNEL_IRQ_H */
