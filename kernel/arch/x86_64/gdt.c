/*
 * gdt.c - Global Descriptor Table for x86_64 long mode
 * Copyright (c) 2026 OpSys Project
 *
 * GDT layout:
 *   0x00: Null descriptor
 *   0x08: Kernel code segment  (selector 0x08)
 *   0x10: Kernel data segment  (selector 0x10)
 *   0x18: User code segment    (selector 0x1B, RPL=3)
 *   0x20: User data segment    (selector 0x23, RPL=3)
 *   0x28: TSS low  (selector 0x28)
 *   0x30: TSS high
 */

#include <kernel/gdt.h>
#include <kernel/io.h>
#include <kernel/string.h>

/* GDT selector values */
#define GDT_SEL_NULL       0x00
#define GDT_SEL_KCODE      0x08
#define GDT_SEL_KDATA      0x10
#define GDT_SEL_UCODE      0x1B  /* 0x18 | RPL=3 */
#define GDT_SEL_UDATA      0x23  /* 0x20 | RPL=3 */
#define GDT_SEL_TSS        0x28

/* GDT entry count (including TSS which takes 2 slots) */
#define GDT_ENTRIES  7

/* Task State Segment for x86_64 */
typedef struct {
    u32 reserved0;
    u64 rsp0;       /* Ring 0 stack pointer (on privilege-level change) */
    u64 rsp1;
    u64 rsp2;
    u64 reserved1;
    u64 ist1;       /* Interrupt Stack Table entries 1-7 */
    u64 ist2;
    u64 ist3;
    u64 ist4;
    u64 ist5;
    u64 ist6;
    u64 ist7;
    u64 reserved2;
    u16 reserved3;
    u16 iopb_offset; /* I/O permission bitmap offset from TSS base */
} __attribute__((packed)) tss_t;

/* 8-byte GDT entry */
typedef struct {
    u16 limit_low;
    u16 base_low;
    u8  base_mid;
    u8  access;
    u8  flags_limit_high;  /* flags(4 bits) | limit_high(4 bits) */
    u8  base_high;
} __attribute__((packed)) gdt_entry_t;

/* 10-byte GDT pointer (used by LGDT) */
typedef struct {
    u16 limit;
    u64 base;
} __attribute__((packed)) gdt_ptr_t;

/* Static storage */
static gdt_entry_t gdt[GDT_ENTRIES];
static tss_t tss;
static gdt_ptr_t gdt_ptr;

/*
 * Encode a GDT entry from base, limit, access byte, and flags.
 */
static void gdt_set_entry(gdt_entry_t *entry, u32 base, u32 limit,
                          u8 access, u8 flags)
{
    entry->limit_low        = limit & 0xFFFF;
    entry->base_low         = base & 0xFFFF;
    entry->base_mid         = (base >> 16) & 0xFF;
    entry->access           = access;
    entry->flags_limit_high = ((flags & 0x0F) << 4) | ((limit >> 16) & 0x0F);
    entry->base_high        = (base >> 24) & 0xFF;
}

/*
 * Set the full 16-byte TSS descriptor (spans two GDT entries).
 * In long mode, the TSS descriptor is 16 bytes:
 *   Entry N:   base[15:0], limit[15:0], base[23:16], access, flags|limit[19:16], base[31:24]
 *   Entry N+1: base[63:32], reserved (0)
 */
static void gdt_set_tss_entry(gdt_entry_t *entries, u64 tss_base, u32 tss_limit)
{
    /* First entry (lower 8 bytes) */
    entries[0].limit_low        = tss_limit & 0xFFFF;
    entries[0].base_low         = tss_base & 0xFFFF;
    entries[0].base_mid         = (tss_base >> 16) & 0xFF;
    entries[0].access           = 0x89;  /* P=1, DPL=0, type=0x9 (available 64-bit TSS) */
    entries[0].flags_limit_high = ((0x00 & 0x0F) << 4) | ((tss_limit >> 16) & 0x0F);
    entries[0].base_high        = (tss_base >> 24) & 0xFF;

    /* Second entry (upper 8 bytes: base[63:32] and reserved) */
    entries[1].limit_low        = (tss_base >> 32) & 0xFFFF;
    entries[1].base_low         = (tss_base >> 48) & 0xFFFF;
    entries[1].base_mid         = 0;
    entries[1].access           = 0;
    entries[1].flags_limit_high = 0;
    entries[1].base_high        = 0;
}

void gdt_init(void)
{
    /* Clear GDT */
    memset(gdt, 0, sizeof(gdt));

    /* Null descriptor (index 0, selector 0x00) */
    gdt_set_entry(&gdt[0], 0, 0, 0x00, 0x00);

    /* Kernel code segment (index 1, selector 0x08)
     * L=1 (long mode), D=0, P=1 (present), S=1 (code/data), type=0xA (execute/read)
     * G=1 (4KB granularity), D=0, L=1, AVL=0 → flags=0xA
     * Access: P=1, DPL=00, S=1, type=1010 → 0x9A
     */
    gdt_set_entry(&gdt[1], 0, 0xFFFFF, 0x9A, 0xA);

    /* Kernel data segment (index 2, selector 0x10)
     * P=1, S=1, type=0x2 (data, expand-up, writable)
     * G=1, B=1, L=0, AVL=0 → flags=0xC
     * Access: P=1, DPL=00, S=1, type=0010 → 0x92
     */
    gdt_set_entry(&gdt[2], 0, 0xFFFFF, 0x92, 0xC);

    /* User code segment (index 3, selector 0x1B with RPL=3)
     * Same as kernel code but DPL=3
     * Access: P=1, DPL=11, S=1, type=1010 → 0xFA
     */
    gdt_set_entry(&gdt[3], 0, 0xFFFFF, 0xFA, 0xA);

    /* User data segment (index 4, selector 0x23 with RPL=3)
     * Same as kernel data but DPL=3
     * Access: P=1, DPL=11, S=1, type=0010 → 0xF2
     */
    gdt_set_entry(&gdt[4], 0, 0xFFFFF, 0xF2, 0xC);

    /* TSS (index 5-6, selector 0x28) */
    memset(&tss, 0, sizeof(tss));
    tss.iopb_offset = sizeof(tss);

    gdt_set_tss_entry(&gdt[5], (u64)&tss, sizeof(tss) - 1);

    /* Build GDT pointer */
    gdt_ptr.limit = sizeof(gdt) - 1;
    gdt_ptr.base  = (u64)&gdt;

    /* Load GDT and reload segment registers.
     * In 64-bit long mode, ljmp is not encodable.  We reload CS
     * via a far return (lretq): push CS selector and target RIP,
     * then pop far-return frame. */
    __asm__ volatile(
        "lgdt (%0)\n"
        /* Reload CS via far return: push cs, push rip, lretq */
        "pushq $0x08\n"         /* CS = kernel code selector */
        "leaq 1f(%%rip), %%rax\n"  /* RIP-relative: avoids R_X86_64_32S at higher-half VMAs */
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        /* Reload data segment registers */
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%ss\n"
        "xor %%ax, %%ax\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        /* Load TSS */
        "ltr %%bx\n"
        :
        : "r"(&gdt_ptr), "b"(GDT_SEL_TSS)
        : "rax", "memory"
    );

    /* Set GS base for per-CPU data (set to 0 for now) */
    __asm__ volatile(
        "wrmsr"
        :
        : "a"(0), "d"(0), "c"(0xC0000101)  /* GS_BASE MSR */
    );
}

void gdt_set_tss_rsp0(u64 rsp)
{
    tss.rsp0 = rsp;
}

void gdt_set_tss_ist(int ist_num, u64 stack)
{
    switch (ist_num) {
        case 1: tss.ist1 = stack; break;
        case 2: tss.ist2 = stack; break;
        case 3: tss.ist3 = stack; break;
        case 4: tss.ist4 = stack; break;
        case 5: tss.ist5 = stack; break;
        case 6: tss.ist6 = stack; break;
        case 7: tss.ist7 = stack; break;
    }
}
