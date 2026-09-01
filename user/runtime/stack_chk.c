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
 * stack_chk.c - User-space stack canary support (GCC -fstack-protector-strong)
 * Copyright (c) 2026 OpSys Project
 *
 * Ring 3 services (term, vfs, shell, pkg, ...) are untrusted inputs away
 * from a stack-buffer overflow: every IPC message is attacker-shaped.
 * GCC instruments eligible functions with a canary slot: the prologue
 * stores __stack_chk_guard, the epilogue compares it back and calls
 * __stack_chk_fail() on mismatch — catching an overflow before the
 * corrupted return address can be consumed (same design as the kernel
 * canary, kernel/arch/x86_64/stack_chk.c).
 *
 * The guard is a non-zero global (sentinel below), so instrumented code
 * is valid from the very first call.  __stack_chk_init() — called from
 * _init() BEFORE any constructor runs — replaces it with a per-process
 * value derived from available entropy (ticks, ASLR heap base, PID and
 * a stack address).  The low byte is forced to 0 so a byte-wise string
 * overflow (which stops at the first NUL) cannot clobber the canary
 * undetected on little-endian x86-64.
 */

#include <runtime.h>        /* exit() */
#include <libos/syscalls.h> /* get_time, GetHeapBase, GetPid, debug_log */

__attribute__((used)) unsigned long __stack_chk_guard = 0xDEADBEEF0BADF00DUL;

/* Simple avalanche mixer (rng_mix style): fold the entropy words into
 * the guard without needing a syscall-provided PRNG. */
static unsigned long Mix(unsigned long h, unsigned long x) {
    h ^= x;
    h *= 0x9E3779B97F4A7C15UL; /* golden-ratio constant */
    h ^= h >> 29;
    return h;
}

/* Seed the per-process canary.  Called from _init() before any
 * constructor: no instrumented frame is live across the change. */
__attribute__((no_stack_protector)) void __stack_chk_init(void) {
    unsigned long g = Mix(0x243F6A8885A308D3UL, (unsigned long)GetTime());
    g               = Mix(g, GetHeapBase());
    g               = Mix(g, (unsigned long)GetPid());
    g               = Mix(g, (unsigned long)&g); /* stack address (ASLR-ish) */
    g &= ~0xFFUL;                                /* low byte NUL */
    if (g == 0)
        g = 0x9E3779B97F4A7C00UL; /* nonzero fallback */
    __stack_chk_guard = g;
}

/*
 * Canary mismatch: the frame is untrustworthy.  Report to the debug log
 * and terminate the process; do NOT attempt to unwind.
 */
__attribute__((noreturn, no_stack_protector)) void __stack_chk_fail(void) {
    (void)DebugLog("STACK SMASHING DETECTED: user stack canary mismatch\n");
    exit(128 + 6); /* SIGABRT */
}

/* Some GCC configurations emit calls to the local variant instead. */
__attribute__((noreturn, no_stack_protector)) void __stack_chk_fail_local(void) {
    __stack_chk_fail();
}
