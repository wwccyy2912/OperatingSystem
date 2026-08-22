/*
 * rng.c - Boot-time PRNG and ASLR address helpers
 * Copyright (c) 2026 OpSys Project
 *
 * xorshift64* PRNG (Marsaglia): state 64-bit, zero state forbidden.
 * Seeded from hardware timing entropy gathered in rng_init():
 *   - rdtsc            CPU cycle counter (varies with boot timing)
 *   - PIT channel 0    free-running 1.19 MHz countdown (phase varies)
 *   - CMOS RTC         wall-clock time
 *   - stack address    boot stack placement differs run to run
 *
 * The ASLR helpers (design item ⑭, option B) return randomized user
 * virtual addresses for thread stacks, the init boot stack and the
 * per-process heap base.  Region constants live in rng.h so the
 * layout is auditable in one place.
 */

#include <kernel/rng.h>
#include <kernel/io.h>
#include <kernel/rtc.h>

/* xorshift64* state — must never be 0 (all-zero state is a fixed point) */
static u64 s_rng_state;

/* ------------------------------------------------------------------ */
/*  Entropy gathering                                                  */
/* ------------------------------------------------------------------ */

static inline u64 rng_rdtsc(void) {
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | (u64)lo;
}

/*
 * Read the current PIT channel-0 counter.  A LATCH command (0x00 to
 * 0x43) freezes the countdown so the two 0x40 reads return a coherent
 * 16-bit value.  Works in any PIT mode; at rng_init time the channel
 * is still running at the BIOS-default ~1.19 MHz rate.
 */
static inline u16 rng_pit_counter(void) {
    io_outb(0x43, 0x00);        /* latch channel 0 */
    u16 lo = (u16)io_inb(0x40); /* low byte first */
    u16 hi = (u16)io_inb(0x40);
    return (u16)(lo | (hi << 8));
}

static u64 rng_gather_entropy(void) {
    u64 e = rng_rdtsc();

    e ^= (u64)rng_pit_counter() << 16;
    e ^= (u64)rng_pit_counter() << 48; /* second sample: counter moved */

    rtc_time_t t;
    rtc_read(&t);
    e ^= (u64)t.year << 0;
    e ^= (u64)t.month << 16;
    e ^= (u64)t.day << 24;
    e ^= (u64)t.hour << 32;
    e ^= (u64)t.minute << 40;
    e ^= (u64)t.second << 48;

    /* Stack address of our own frame — varies with boot path depth. */
    e ^= (u64)(uptr)&e;

    return e;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

void rng_init(void) {
    if (s_rng_state != 0)
        return; /* already seeded */

    u64 seed = rng_gather_entropy();
    if (seed == 0)
        seed = 0x9E3779B97F4A7C15ULL; /* splitmix golden ratio, fallback */
    s_rng_state = seed;
}

void rng_mix(u64 entropy) {
    /* Fold without risk of landing on the forbidden zero state. */
    if (entropy == 0)
        return;
    s_rng_state ^= entropy;
    if (s_rng_state == 0)
        s_rng_state = 0x9E3779B97F4A7C15ULL;
}

u64 rng_u64(void) {
    /* xorshift64* — good statistical quality, trivial to implement. */
    u64 x = s_rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    s_rng_state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

u64 rng_range(u64 limit) {
    if (limit == 0)
        return 0;
    return rng_u64() % limit;
}

/* ------------------------------------------------------------------ */
/*  ASLR address helpers (design item ⑭, option B)                    */
/* ------------------------------------------------------------------ */

u64 aslr_stack_base(void) {
    /* One ASLR_STACK_BLOCK (MAX_THREADS * USER_STACK_PAGES pages,
     * 32 MB at MAX_THREADS=2048) per possible thread; pick a random
     * block. */
    u64 region = ASLR_STACK_END - ASLR_STACK_BASE;
    u64 blocks = region / ASLR_STACK_BLOCK;
    return ASLR_STACK_BASE + rng_range(blocks) * ASLR_STACK_BLOCK;
}

u64 aslr_boot_stack(void) {
    u64 region = ASLR_BOOT_STACK_END - ASLR_BOOT_STACK_BASE;
    u64 pages  = region / PAGE_SIZE;
    return ASLR_BOOT_STACK_BASE + rng_range(pages) * PAGE_SIZE;
}

u64 aslr_heap_base(void) {
    u64 region = ASLR_HEAP_BASE_MAX - ASLR_HEAP_BASE_MIN;
    u64 slots  = region / ASLR_HEAP_ALIGN;
    return ASLR_HEAP_BASE_MIN + rng_range(slots) * ASLR_HEAP_ALIGN;
}

u64 aslr_elf_base(void) {
    u64 region = ASLR_ELF_BASE_MAX - ASLR_ELF_BASE_MIN;
    u64 pages  = region / PAGE_SIZE;
    return ASLR_ELF_BASE_MIN + rng_range(pages) * PAGE_SIZE;
}
