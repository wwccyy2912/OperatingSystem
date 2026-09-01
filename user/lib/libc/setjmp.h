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
 * setjmp.h - Non-local jumps (C11 §7.13)
 * Copyright (c) 2026 OpSys Project
 *
 * Saves the calling environment (callee-saved registers + stack +
 * return address) for later restoration by longjmp.
 *
 * jmp_buf layout (8 entries, 64 bytes total):
 *   [0] rbx    [1] rbp    [2] r12   [3] r13
 *   [4] r14    [5] r15    [6] rsp    [7] rip
 */

#ifndef LIBC_SETJMP_H
#define LIBC_SETJMP_H

#include <stddef.h>

typedef long long jmp_buf[8];

/* Saves the calling environment into env.  Returns 0 on direct call,
 * non-zero (val) when restored via longjmp. */
int setjmp(jmp_buf env);

/* Restores the environment saved by setjmp.  Does not return — instead,
 * setjmp returns `val` (or 1 if val == 0). */
_Noreturn void longjmp(jmp_buf env, int val);

#endif /* LIBC_SETJMP_H */
