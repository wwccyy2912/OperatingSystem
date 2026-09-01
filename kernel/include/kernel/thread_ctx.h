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
 * thread_ctx.h - User-visible thread register context
 * Copyright (c) 2026 OpSys Project
 *
 * thread_ctx_t is the exact byte-level image of the saved register frame
 * that context_switch.S (kernel/arch/x86_64/context_switch.S) stores into
 * thread_t when switching away and reloads from when switching back.  The
 * nine u64 fields below map 1:1 onto thread_t's saved-context region
 * (thread.h fields rsp..rip, which context_switch.S addresses via its
 * RSP_OFFSET..RIP_OFFSET constants; both are pinned to the same offsets).
 *
 * SYS_THREAD_SET_CTX copies a caller-supplied thread_ctx_t over that
 * region, letting user-space signal trampolines rewrite a target
 * thread's saved state before it is resumed.  kernel/sched/thread_ctx.c
 * verifies at compile time that this struct exactly spans the
 * thread_t saved-context region, so the layout below cannot silently
 * drift apart from the asm frame.
 */

#ifndef KERNEL_THREAD_CTX_H
#define KERNEL_THREAD_CTX_H

#include <kernel/types.h>

/* Saved register context of a thread, in context_switch.S save order. */
typedef struct {
    u64 rsp;    /* Stack pointer */
    u64 rbx;    /* Callee-saved */
    u64 rbp;    /* Callee-saved */
    u64 r12;    /* Callee-saved */
    u64 r13;    /* Callee-saved */
    u64 r14;    /* Callee-saved */
    u64 r15;    /* Callee-saved */
    u64 rflags; /* EFLAGS (IF bit set by context_switch on resume) */
    u64 rip;    /* Next instruction pointer */
} thread_ctx_t;

#endif /* KERNEL_THREAD_CTX_H */
