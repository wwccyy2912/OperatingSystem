/*
 * vmm.h - Virtual Memory Manager
 * Copyright (c) 2026 OpSys Project
 *
 * 4-level paging (PML4 -> PDP -> PD -> PT) for x86_64.
 */

#ifndef KERNEL_VMM_H
#define KERNEL_VMM_H

#include <kernel/types.h>

/* Kernel virtual address offset (higher-half) */
#define KERNEL_VIRT_BASE    0xFFFF800000000000ULL

/* Upper bound of the user address space (identity-mapped region for v0.1) */
#define USER_PTR_MAX        0x0000800000000000ULL

/*
 * User heap region — MUST match user/runtime/malloc.c (HEAP_USER_SIZE).
 *
 * ASLR (design item ⑭): each process's heap base is randomized at
 * creation (process_t.heap_base, via aslr_heap_base()); the region size
 * HEAP_USER_SIZE is fixed.  The kernel reserves one guard page below the
 * base and one at base + HEAP_USER_SIZE: sys_map_memory/sys_unmap_memory
 * refuse to map/unmap them (checked against the CURRENT process's
 * heap_base), so a heap overflow (write past the last chunk) or
 * underflow (write below the first chunk) hits an unmapped page and
 * faults instead of silently corrupting adjacent user memory.
 * User-space malloc.c fetches its base via SYS_GET_HEAP_BASE.
 */
#define HEAP_USER_BASE      0x70000000ULL   /* default (minimum) base */
#define HEAP_USER_SIZE      0x10000000ULL   /* 256 MB region per process */

/* Kernel CR3 (physical address of kernel PML4).
 * Set during vmm_init(), used by syscall_entry.S to switch page tables. */
extern u64 vmm_kernel_cr3;

/* Page table entry flags */
#define PTE_PRESENT     (1ULL << 0)
#define PTE_WRITABLE    (1ULL << 1)
#define PTE_USER        (1ULL << 2)
#define PTE_WRITE_THRU  (1ULL << 3)
#define PTE_CACHE_DIS   (1ULL << 4)
#define PTE_ACCESSED    (1ULL << 5)
#define PTE_DIRTY       (1ULL << 6)
#define PTE_HUGE        (1ULL << 7)
#define PTE_GLOBAL      (1ULL << 8)
#define PTE_NO_EXECUTE  (1ULL << 63)

/* Address space structure */
typedef struct {
        u64 pml4_phys;      /* Physical address of PML4 */
        u64 stack_base;     /* ASLR: per-address-space user stack region base
                         * (1 MB aligned; thread TID stacks at
                         * stack_base + tid*PAGE_SIZE).  Random per
                         * address space — see rng.h aslr_stack_base(). */
} addr_space_t;

/**
 * Initialize virtual memory: set up kernel page tables.
 * Called after pmm_init.
 */
void vmm_init(void);

/**
 * Create a new user address space with only kernel mappings.
 * @return New address space, or NULL on failure.
 */
addr_space_t *vmm_create_addr_space(void);

/**
 * Destroy an address space and free all user page tables.
 * @param as  Address space to destroy.
 */
void vmm_destroy_addr_space(addr_space_t *as);

/**
 * Map a virtual address to a physical address in an address space.
 * @param as    Target address space.
 * @param virt  Virtual address (page-aligned).
 * @param phys  Physical address (page-aligned).
 * @param flags PTE flags (PTE_PRESENT | PTE_WRITABLE | PTE_USER | ...).
 * @return OK on success, error code on failure.
 */
error_t vmm_map(addr_space_t *as, u64 virt, u64 phys, u64 flags);

/**
 * Unmap a virtual address.
 * @param as    Target address space.
 * @param virt  Virtual address (page-aligned).
 */
error_t vmm_unmap(addr_space_t *as, u64 virt);

/**
 * Get the physical address mapped to a virtual address.
 * @param as    Address space to query.
 * @param virt  Virtual address.
 * @return Physical address, or 0 if not mapped.
 */
u64 vmm_virt_to_phys(addr_space_t *as, u64 virt);

/**
 * Get the current (bootstrapped) kernel address space.
 */
addr_space_t *vmm_get_kernel_addr_space(void);

/**
 * Switch to a different address space (loads CR3).
 * @param as  Address space to switch to.
 */
void vmm_switch_addr_space(addr_space_t *as);

/**
 * Map a range of pages.
 */
error_t vmm_map_range(addr_space_t *as, u64 virt, u64 phys,
                                            u64 page_count, u64 flags);

/**
 * Unmap a range of pages.
 */
error_t vmm_unmap_range(addr_space_t *as, u64 virt, u64 page_count);

/**
 * Allocate a page from PMM and map it to a virtual address.
 * @param as    Address space.
 * @param virt  Virtual address to map to.
 * @param flags PTE flags.
 * @return OK or error.
 */
error_t vmm_alloc_and_map(addr_space_t *as, u64 virt, u64 flags);

/**
 * Check whether a range of user virtual addresses is safe to access
 * from the kernel on behalf of a user process.  Returns true only if:
 *   - ptr is non-zero and size is non-zero
 *   - ptr + size does not overflow
 *   - the entire range lies below USER_PTR_MAX
 *  - EVERY page in the range is mapped in the given address space
 *     (prevents #PF in the kernel when dereferencing user pointers).
 * @param as         Address space to validate against (may be NULL).
 * @param ptr        User virtual start address.
 * @param size       Range length in bytes.
 * @param need_write If true, every page must also be writable (the kernel
 *                   will write into the range); if false, Present alone
 *                   suffices (the kernel only reads from the range).
 */
bool vmm_validate_user_range(addr_space_t *as, u64 ptr, u64 size,
                             bool need_write);

#endif /* KERNEL_VMM_H */
