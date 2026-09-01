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
 * gdt.h - Global Descriptor Table management
 * Copyright (c) 2026 OpSys Project
 */

#ifndef KERNEL_GDT_H
#define KERNEL_GDT_H

#include <kernel/types.h>

/* Segment selectors (single source of truth).  GDT layout in gdt.c:
 * 0x08 kernel code, 0x10 kernel data, 0x18 user code, 0x20 user data,
 * each with RPL=3 for the user variants.  Used by the signal frame
 * rebuild (signal.c) and by gdt.c itself. */
#define GDT_SEL_UCODE 0x1B /* 0x18 | RPL=3 */
#define GDT_SEL_UDATA 0x23 /* 0x20 | RPL=3 */

/**
 * Initialize the GDT with kernel and user segments.
 */
void GdtInit(void);

/**
 * Set the TSS RSP0 field (ring-0 stack pointer on privilege change).
 * Must be called before any INT 0x80 from ring 3.
 * @param rsp  Kernel stack pointer (virtual address, must be mapped).
 */
void GdtSetTssRsp0(u64 rsp);

/**
 * Set an IST (Interrupt Stack Table) entry in the TSS.
 * IST entries provide dedicated kernel stacks for specific interrupts.
 * @param ist_num  IST entry number (1-7).
 * @param stack    Stack top pointer (virtual address, must be mapped).
 */
void GdtSetTssIst(int ist_num, u64 stack);

#endif /* KERNEL_GDT_H */
