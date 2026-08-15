/*
 * idt.h - Interrupt Descriptor Table management
 * Copyright (c) 2026 OpSys Project
 */

#ifndef KERNEL_IDT_H
#define KERNEL_IDT_H

#include <kernel/types.h>

/**
 * IDT gate descriptor types
 */
#define IDT_GATE_INTERRUPT 0x0E
#define IDT_GATE_TRAP      0x0F
#define IDT_GATE_CALL      0x0C

/**
 * Interrupt frame passed to exception/IRQ handlers.
 * Built by isr_common_stub from saved GPRs + CPU-pushed IRET frame.
 *
 * Layout matches the push order in isr_common_stub (15 GPRs) followed
 * by the 2-value "vector + error_code" slot, then the 5-value CPU
 * interrupt frame (IRETQ).
 */
typedef struct {
    /* GPRs saved by isr_common_stub (bottom of frame) */
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;

    /* Pseudo-values pushed by individual ISR stubs */
    u64 vector;
    u64 error_code;

    /* Frame pushed by CPU on interrupt/exception */
    u64 rip;
    u64 cs;
    u64 rflags;
    u64 rsp; /* only valid on privilege-level change */
    u64 ss;  /* only valid on privilege-level change */
} __attribute__((packed)) interrupt_frame_t;

/**
 * Initialize the IDT with default ISR handlers.
 */
void idt_init(void);

/**
 * Register a custom handler for a specific interrupt vector.
 * @param vector  Interrupt vector number (0-255).
 * @param handler Function pointer to handler.
 * @param dpl     Descriptor privilege level (0 or 3).
 */
void idt_register_handler(u8 vector, void (*handler)(void), u8 dpl);

/**
 * Enable or disable interrupts.
 */
void idt_enable_interrupts(void);
void idt_disable_interrupts(void);

/**
 * Enable (unmask) a hardware IRQ line on the PIC.
 * @param irq  IRQ number (0-15).
 */
void irq_enable(u8 irq);

/**
 * Disable (mask) a hardware IRQ line on the PIC.
 * @param irq  IRQ number (0-15).
 */
void irq_disable(u8 irq);

/**
 * Initialize the PIT (8253/8254) channel 0 at the given frequency.
 * @param freq  Desired tick frequency in Hz (e.g. 100).
 */
void pit_init(u32 freq);

#endif /* KERNEL_IDT_H */
