/*
 * gdt.h - Global Descriptor Table management
 * Copyright (c) 2026 OpSys Project
 */

#ifndef KERNEL_GDT_H
#define KERNEL_GDT_H

#include <kernel/types.h>

/**
 * Initialize the GDT with kernel and user segments.
 */
void gdt_init(void);

/**
 * Set the TSS RSP0 field (ring-0 stack pointer on privilege change).
 * Must be called before any INT 0x80 from ring 3.
 * @param rsp  Kernel stack pointer (virtual address, must be mapped).
 */
void gdt_set_tss_rsp0(u64 rsp);

/**
 * Set an IST (Interrupt Stack Table) entry in the TSS.
 * IST entries provide dedicated kernel stacks for specific interrupts.
 * @param ist_num  IST entry number (1-7).
 * @param stack    Stack top pointer (virtual address, must be mapped).
 */
void gdt_set_tss_ist(int ist_num, u64 stack);

#endif /* KERNEL_GDT_H */
