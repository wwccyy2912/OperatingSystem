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
 * stack_chk.c - Kernel stack canary support (GCC -fstack-protector-strong)
 * Copyright (c) 2026 OpSys Project
 *
 * GCC instruments eligible functions with a local canary slot: the prologue
 * stores __stack_chk_guard, the epilogue compares it back and calls
 * __stack_chk_fail() on mismatch.  This catches stack buffer overflows
 * before the corrupted return address can be consumed.
 *
 * The guard is a non-zero initialized global, so it lives in .data and is
 * loaded by GRUB from the ELF -- valid from the very first kernel C call.
 *
 * ASLR (design item ⑭): kernel_main calls StackChkRandomize() right
 * after RngInit(), replacing the boot-time sentinel with a per-boot
 * random value.  The low byte is forced to 0 so a byte-wise string
 * overflow (which stops at the first NUL) cannot clobber the canary
 * undetected on little-endian x86-64.  kernel_main never returns, so no
 * instrumented frame holds the pre-randomization guard across the change. *
 * ------------------------------------------------------------------
 * Structure (stack_chk):
 *   __stack_chk_guard (randomised at boot) + __stack_chk_fail ->
 *   panic "stack smashing detected".
 * How it works:
 *   -fstack-protector-strong prologues compare the guard slot; on
 *   mismatch the fail routine fires (kernel) / raises SIGSEGV (user).
 * Purpose:
 *   Detect stack corruption (buffer overruns) early.
 * Caveats:
 *   The guard lives in .bss; StackChkRandomize() must run before any
 *   protected function returns into checked code.
 * ------------------------------------------------------------------
 */
#include <kernel/rng.h>
#include <kernel/panic.h>

__attribute__((used)) unsigned long __stack_chk_guard =
    0xDEADBEEF0BADF00DUL; /* pre-PRNG sentinel */

/*
 * Randomize the canary from the boot PRNG.  Called once by kernel_main
 * immediately after RngInit() and BEFORE any instrumented function that
 * may remain live across the change (kernel_main itself never returns,
 * so its own frame is never checked).
 */
void StackChkRandomize(void) {
    u64 g = RngU64() & ~0xFFULL; /* low byte NUL — stops string overflows */
    if (g == 0)
        g = 0x9E3779B97F4A7C00ULL; /* nonzero fallback */
    __stack_chk_guard = g;
}

/*
 * Called by instrumented code when the canary mismatch is detected.
 * A corrupted stack frame means the return address is untrustworthy --
 * halt instead of attempting to unwind.
 */
__attribute__((noreturn, no_stack_protector)) void __stack_chk_fail(void) {
    panic("STACK SMASHING DETECTED: kernel stack canary mismatch");
}

/* Some GCC configurations emit calls to the local variant instead. */
__attribute__((noreturn, no_stack_protector)) void __stack_chk_fail_local(void) {
    __stack_chk_fail();
}
