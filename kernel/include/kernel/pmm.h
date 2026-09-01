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
void PmmInit(u64 mboot_addr, u64 kernel_end);

/**
 * Allocate a single 4KB physical page frame.
 * @return Physical address of allocated page, or 0 on failure.
 */
u64 PmmAllocPage(void);

/**
 * Free a single 4KB physical page frame.
 * @param phys  Physical address of the page to free.
 */
void PmmFreePage(u64 phys);

/**
 * Allocate contiguous physical pages.
 * @param count  Number of pages to allocate.
 * @return Physical address of the first page, or 0 on failure.
 */
u64 PmmAllocPages(u64 count);

/**
 * Free contiguous physical pages.
 * @param phys  Physical address of the first page.
 * @param count Number of pages to free.
 */
void PmmFreePages(u64 phys, u64 count);

/**
 * Get total physical memory in bytes.
 */
u64 PmmGetTotalMemory(void);

/**
 * Get free physical memory in bytes.
 */
u64 PmmGetFreeMemory(void);

#endif /* KERNEL_PMM_H */
