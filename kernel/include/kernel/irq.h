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
error_t IrqBind(u8 irq, u32 mask);

/**
 * Clear the binding for an IRQ line and disable the IRQ.
 * @param irq  IRQ number (0-15).
 * @return OK (idempotent), or ERR_INVAL if out of range.
 */
error_t IrqUnbind(u8 irq);

/**
 * Release every IRQ line bound by threads of a dying process.
 * Called from ProcessReap().
 * @param pid  PID of the dying process.
 */
void IrqCleanupProcess(pid_t pid);

/**
 * Forward an IRQ to its bound thread (called from the ISR).
 * @param irq  IRQ number (0-15).
 * @return true if a binding was active (forwarded or self-cleaned),
 *         false if unbound.
 */
bool IrqHandle(u8 irq);

#endif /* KERNEL_IRQ_H */
