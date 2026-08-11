/*
 * pmm.h - Physical Memory Manager
 * Copyright (c) 2026 OpSys Project
 *
 * Bitmap-based physical page frame allocator.
 */

#ifndef KERNEL_PMM_H
#define KERNEL_PMM_H

#include <kernel/types.h>

/**
 * Initialize the physical memory manager using multiboot memory map.
 * @param mboot_addr  Physical address of multiboot info structure.
 * @param kernel_end  Physical address of kernel end symbol.
 */
void pmm_init(u64 mboot_addr, u64 kernel_end);

/**
 * Allocate a single 4KB physical page frame.
 * @return Physical address of allocated page, or 0 on failure.
 */
u64 pmm_alloc_page(void);

/**
 * Free a single 4KB physical page frame.
 * @param phys  Physical address of the page to free.
 */
void pmm_free_page(u64 phys);

/**
 * Allocate contiguous physical pages.
 * @param count  Number of pages to allocate.
 * @return Physical address of the first page, or 0 on failure.
 */
u64 pmm_alloc_pages(u64 count);

/**
 * Free contiguous physical pages.
 * @param phys  Physical address of the first page.
 * @param count Number of pages to free.
 */
void pmm_free_pages(u64 phys, u64 count);

/**
 * Get total physical memory in bytes.
 */
u64 pmm_get_total_memory(void);

/**
 * Get free physical memory in bytes.
 */
u64 pmm_get_free_memory(void);

#endif /* KERNEL_PMM_H */
