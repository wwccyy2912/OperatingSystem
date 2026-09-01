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
