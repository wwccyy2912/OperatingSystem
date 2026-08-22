/*
 * rng.h - Boot-time PRNG and ASLR address helpers
 * Copyright (c) 2026 OpSys Project
 *
 * Single kernel-wide xorshift64* PRNG.  Seeded once at boot from
 * hardware timing entropy (rdtsc, PIT channel-0, CMOS RTC, boot
 * stack address); additional entropy can be folded in later with
 * rng_mix() (e.g. scheduler ticks + PID at process creation).
 *
 * All user-space address randomization (ASLR design item ⑭) routes
 * through the aslr_*() helpers so the entropy source and the address
 * regions are centralized in one place.  When the full PIE variant
 * (design option A) is implemented, only aslr_elf_base() + the ELF
 * loader need to change.
 */

#ifndef KERNEL_RNG_H
#define KERNEL_RNG_H

#include <kernel/types.h>

/**
 * Seed the PRNG from hardware entropy.  Call very early in boot
 * (kernel_main, right after serial_init) so every later random draw
 * is unpredictable across boots.  Safe to call once; idempotent.
 */
void rng_init(void);

/**
 * Fold additional entropy into the PRNG state.
 * Use after subsystems with timing/identity entropy come up
 * (scheduler ticks, PID counter) to decorrelate per-process draws.
 */
void rng_mix(u64 entropy);

/**
 * Return the next 64-bit pseudo-random value.
 */
u64 rng_u64(void);

/**
 * Return a pseudo-random value in [0, limit).
 * limit must be non-zero (values are rng_u64() % limit).
 */
u64 rng_range(u64 limit);

/* ==================================================================
 * ASLR address helpers (design item ⑭, option B: stack/heap/canary)
 * ================================================================== */

/*
 * User thread stack region: [0x90000000, 0x100000000).
 * Each address space gets one MAX_THREADS-block-aligned block
 * (ASLR_STACK_BLOCK = MAX_THREADS * USER_STACK_PAGES * PAGE_SIZE;
 * 32 MB at MAX_THREADS=2048); thread TID maps its USER_STACK_PAGES
 * stack pages at block_base + tid*USER_STACK_PAGES*PAGE_SIZE, so
 * threads never collide and the base varies per address space.
 * Clear of the ELF image, the fixed test mappings (0x10000000-0x30000000)
 * and the per-process heap region ([heap_base, heap_base+256 MB)).
 */
#define ASLR_STACK_BASE  0x90000000ULL
#define ASLR_STACK_END   0x100000000ULL
#define ASLR_STACK_BLOCK (MAX_THREADS * USER_STACK_PAGES * PAGE_SIZE)

/**
 * Random ASLR_STACK_BLOCK-aligned base for a process's user thread stacks.
 */
u64 aslr_stack_base(void);

/*
 * init boot stack region: [0x4000000, 0x10000000).
 * Used only for PID 1's initial stack (enter_user_mode RSP).  Kept
 * separate from the thread-stack region so the boot stack can never
 * collide with a spawned thread's stack.
 */
#define ASLR_BOOT_STACK_BASE 0x4000000ULL
#define ASLR_BOOT_STACK_END  0x10000000ULL

/**
 * Random page-aligned address for the init boot stack.
 */
u64 aslr_boot_stack(void);

/*
 * User heap region: the heap base is randomized within
 * [0x70000000, 0x78000000) at 64 KB granularity; the region size
 * (HEAP_USER_SIZE = 256 MB) is unchanged, so the heap always has at
 * least 128 MB of room.  The kernel stores the base per process
 * (process_t.heap_base) and hands it to user-space via SYS_GET_HEAP_BASE.
 */
#define ASLR_HEAP_BASE_MIN 0x70000000ULL
#define ASLR_HEAP_BASE_MAX 0x78000000ULL /* exclusive */
#define ASLR_HEAP_ALIGN    0x10000ULL    /* 64 KB */

/**
 * Random 64 KB-aligned heap base for a new process.
 */
u64 aslr_heap_base(void);

/* ==================================================================
 * Option A (full PIE) reservation
 * ================================================================== */

/*
 * PIE load region: [0x40000000, 0x70000000), page-aligned.
 * RESERVED for a future ET_DYN conversion: ELF parsing now lives in
 * user-space libos (user/lib/libos/elf_parse.c) and SYS_PROCESS_CREATE
 * takes pre-parsed descriptors (kernel/proc_image.h), so a PIE switch
 * would rebase p_vaddr in the user-space parser, not here.  Unused
 * today (all blobs are ET_EXEC linked at 0x400000).
 */
#define ASLR_ELF_BASE_MIN 0x40000000ULL
#define ASLR_ELF_BASE_MAX 0x70000000ULL /* exclusive */

/**
 * Random page-aligned PIE load base (option A reservation).
 */
u64 aslr_elf_base(void);

#endif /* KERNEL_RNG_H */
