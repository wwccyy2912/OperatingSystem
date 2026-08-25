/*
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
#include <libos/syscalls.h> /* get_time, get_heap_base, get_pid, debug_log */

__attribute__((used)) unsigned long __stack_chk_guard = 0xDEADBEEF0BADF00DUL;

/* Simple avalanche mixer (rng_mix style): fold the entropy words into
 * the guard without needing a syscall-provided PRNG. */
static unsigned long mix(unsigned long h, unsigned long x) {
    h ^= x;
    h *= 0x9E3779B97F4A7C15UL; /* golden-ratio constant */
    h ^= h >> 29;
    return h;
}

/* Seed the per-process canary.  Called from _init() before any
 * constructor: no instrumented frame is live across the change. */
__attribute__((no_stack_protector)) void __stack_chk_init(void) {
    unsigned long g = mix(0x243F6A8885A308D3UL, (unsigned long)get_time());
    g               = mix(g, get_heap_base());
    g               = mix(g, (unsigned long)get_pid());
    g               = mix(g, (unsigned long)&g); /* stack address (ASLR-ish) */
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
    (void)debug_log("STACK SMASHING DETECTED: user stack canary mismatch\n");
    exit(128 + 6); /* SIGABRT */
}

/* Some GCC configurations emit calls to the local variant instead. */
__attribute__((noreturn, no_stack_protector)) void __stack_chk_fail_local(void) {
    __stack_chk_fail();
}
