/*
 * panic.h - Unified kernel panic path
 * Copyright (c) 2026 OpSys Project
 */

#ifndef KERNEL_PANIC_H
#define KERNEL_PANIC_H

/*
 * panic() — terminate the system on an unrecoverable kernel fault.
 *
 * Prints the fault reason to serial (printf-style, same specifiers as
 * serial_printf), disables interrupts, and halts the CPU forever.
 * Never returns.
 *
 * Policy: fault DETECTION may stay distributed (exception register
 * dumps, canary checks, allocation failure checks...), but every kernel
 * fault that must STOP THE SYSTEM funnels through panic() — never a raw
 * cli+hlt loop.  Non-fault shutdown paths (last thread exited, reboot)
 * are NOT panics and keep their own halt semantics.
 */
__attribute__((noreturn)) void panic(const char *fmt, ...);

#endif /* KERNEL_PANIC_H */
