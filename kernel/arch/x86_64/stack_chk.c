/*
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
 * ASLR (design item ⑭): kernel_main calls stack_chk_randomize() right
 * after rng_init(), replacing the boot-time sentinel with a per-boot
 * random value.  The low byte is forced to 0 so a byte-wise string
 * overflow (which stops at the first NUL) cannot clobber the canary
 * undetected on little-endian x86-64.  kernel_main never returns, so no
 * instrumented frame holds the pre-randomization guard across the change.
 */

#include <kernel/rng.h>
#include <kernel/panic.h>

__attribute__((used))
unsigned long __stack_chk_guard = 0xDEADBEEF0BADF00DUL;  /* pre-PRNG sentinel */

/*
 * Randomize the canary from the boot PRNG.  Called once by kernel_main
 * immediately after rng_init() and BEFORE any instrumented function that
 * may remain live across the change (kernel_main itself never returns,
 * so its own frame is never checked).
 */
void stack_chk_randomize(void)
{
        u64 g = rng_u64() & ~0xFFULL;   /* low byte NUL — stops string overflows */
        if (g == 0)
                g = 0x9E3779B97F4A7C00ULL;  /* nonzero fallback */
        __stack_chk_guard = g;
}

/*
 * Called by instrumented code when the canary mismatch is detected.
 * A corrupted stack frame means the return address is untrustworthy --
 * halt instead of attempting to unwind.
 */
__attribute__((noreturn, no_stack_protector))
void __stack_chk_fail(void)
{
        panic("STACK SMASHING DETECTED: kernel stack canary mismatch");
}

/* Some GCC configurations emit calls to the local variant instead. */
__attribute__((noreturn, no_stack_protector))
void __stack_chk_fail_local(void)
{
        __stack_chk_fail();
}
