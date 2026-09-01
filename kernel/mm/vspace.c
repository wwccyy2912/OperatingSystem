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
 * vspace.c - Virtual address space allocation (SYS_VSPACE_ALLOC)
 * Copyright (c) 2026 OpSys Project
 *
 * Roadmap P1 (docs/kernel_roadmap.md): 内核只分配虚拟地址 + 页表构建，
 * 格式语义交给用户态 — the kernel hands out VIRTUAL address ranges only
 * and pre-builds the page-table hierarchy; the caller (libos vspace_alloc
 * wrapper, later a user-space ELF loader) owns the format semantics and
 * supplies the physical pages afterwards with SYS_MAP_MEMORY.
 *
 * 4-level paging (PML4 -> PDP -> PD -> PT) for x86_64, mirroring the
 * page-walk conventions in vmm.c: page-table pages live in low physical
 * memory and are accessed via KERNEL_VIRT_BASE, and intermediate
 * entries always carry PTE_USER (the final PT entry's U/S bit controls
 * actual access permission).
 *
 * All index/table helpers below are static copies of vmm.c's because
 * vmm.c exports no partial-build primitive: no exported vmm function can
 * create intermediate levels while leaving the leaf PTEs not-present, and
 * no exported function can test "any covering hierarchy entry exists".
 * The table PAGE ALLOCATION itself reuses the same exported helper vmm.c
 * uses — PmmAllocPage() — with identical entry flags.
 *
 * ------------------------------------------------------------------
 * Structure (vspace):
 *   Candidate regions, scanned in order: [VSPACE_FLOOR .. heap guard),
 *   [heap guard .. stack region), [stack end .. USER_PTR_MAX);
 *   VSPACE_FLOOR = 1 GiB protects the ELF image and fixed test maps.
 *   Occupancy is decided by the page-table hierarchy itself: any present
 *   PML4E/PDPTE/PDE makes a page "occupied" — no interval tree, but a
 *   linear sweep with run tracking (O(pages scanned) per interval).
 *
 * How it works:
 *   sc_sys_vspace_alloc() (SYS_VSPACE_ALLOC 53) validates size/flags,
 *   bounds-checks the table-page cost (VspaceTablesAffordable), scans
 *   the intervals for a contiguous free run (VspaceScanRanges), then
 *   VspaceBuildTables() pre-builds PML4E/PDPTE/PDE with leaf PTEs left
 *   not-present; VspaceFreeTables() rolls the hierarchy back bottom-up
 *   on build failure.
 *
 * Purpose:
 *   Roadmap P1: the kernel only hands out VIRTUAL ranges with page
 *   tables pre-built; the caller supplies physical pages afterwards
 *   via SYS_MAP_MEMORY. Allocations never overlap the heap, stacks,
 *   ELF image, or earlier vspace ranges.
 *
 * Caveats:
 *   The range is not accessible until SYS_MAP_MEMORY completes the
 *   leaves. Pre-built hierarchies count as occupied even though
 *   VmmVirtToPhys() is 0, so two allocations can never overlap.
 * ------------------------------------------------------------------
 */

#include <kernel/vmm.h>
#include <kernel/pmm.h>
#include <kernel/process.h>
#include <kernel/rng.h>

/*
 * Scan floor: 1 GiB.  Everything below the floor is never touched:
 * the ELF image (linked at 0x400000, scripts/user.ld) and the fixed
 * test mappings [0x10000000, 0x30000000).
 */
#define VSPACE_FLOOR 0x40000000ULL

/* ---- Small page-table helpers (static copies of vmm.c's) ---- */

static inline void *vspace_phys_to_virt(u64 phys) {
    return (void *)(phys + KERNEL_VIRT_BASE);
}

static inline u64 Vspacepml4Index(u64 v) {
    return (v >> 39) & 0x1FF;
}
static inline u64 VspacepdpIndex(u64 v) {
    return (v >> 30) & 0x1FF;
}
static inline u64 VspacepdIndex(u64 v) {
    return (v >> 21) & 0x1FF;
}

static inline void VspaceZeroPage(void *addr) {
    u64 *p = (u64 *)addr;
    for (int i = 0; i < 512; i++)
        p[i] = 0;
}

static inline bool VspacePageTableEmpty(const u64 *table) {
    for (int i = 0; i < 512; i++) {
        if (table[i] & PTE_PRESENT)
            return false;
    }
    return true;
}

/*
 * Return true if the 4 KB page `vaddr` is unallocated in `as`.
 *
 * A page counts as occupied when ANY covering hierarchy entry (PML4E /
 * PDPTE / PDE) is present.  This is deliberately STRICTER than
 * VmmVirtToPhys() == 0: a previous SYS_VSPACE_ALLOC pre-builds the
 * intermediate tables but leaves the leaf PTEs not-present, and such
 * pages must NOT be handed out again — otherwise two allocations could
 * overlap and both be completed by a later SYS_MAP_MEMORY.  A PDE that
 * is present is therefore always "occupied", whether it is a 2 MB huge
 * mapping or a pre-built PT page.
 */
static bool VspacePageFree(addr_space_t *as, u64 vaddr) {
    u64 *pml4 = (u64 *)vspace_phys_to_virt(as->pml4_phys);

    u64 idx = Vspacepml4Index(vaddr);
    if (!(pml4[idx] & PTE_PRESENT))
        return true; /* whole 512 GB region empty */

    u64 *pdp = (u64 *)vspace_phys_to_virt(pml4[idx] & ~0xFFFULL);
    idx      = VspacepdpIndex(vaddr);
    if (!(pdp[idx] & PTE_PRESENT))
        return true; /* whole 1 GB region empty */
    if (pdp[idx] & PTE_HUGE)
        return false; /* 1 GB huge mapping */

    u64 *pd = (u64 *)vspace_phys_to_virt(pdp[idx] & ~0xFFFULL);
    idx     = VspacepdIndex(vaddr);
    if (!(pd[idx] & PTE_PRESENT))
        return true; /* whole 2 MB region empty */

    return false; /* PDE present: huge mapping, live PT, or a pre-built
                   * PT page from an earlier SYS_VSPACE_ALLOC */
}

/*
 * Scan [start, end) for a contiguous run of `page_count` free pages.
 * Linear sweep with run tracking: O(pages scanned) per interval, and it
 * returns the first usable range.  start and end must both be
 * page-aligned.
 */
static bool
VspaceScanInterval(addr_space_t *as, u64 start, u64 end, u64 page_count, u64 *out_base) {
    u64 run_start = 0;
    u64 run_len   = 0;

    for (u64 v = start; v < end; v += PAGE_SIZE) {
        /* Even a fully-free tail of this interval cannot extend the
         * current run far enough — give up on this interval. */
        if (run_len + (end - v) / PAGE_SIZE < page_count)
            break;

        if (VspacePageFree(as, v)) {
            if (run_len == 0)
                run_start = v;
            run_len++;
            if (run_len >= page_count) {
                *out_base = run_start;
                return true;
            }
        } else {
            run_len = 0;
        }
    }
    return false;
}

/*
 * Scan the candidate intervals of the calling address space, in order:
 *   1. [VSPACE_FLOOR, heap low guard)   (up to the stack region if the
 *      process has no heap)
 *   2. [heap high guard, stack region)
 *   3. [stack region end, USER_PTR_MAX)
 * The per-process ASLR heap occupies [heap_base, heap_base +
 * HEAP_USER_SIZE) with one guard page below and one above (the same
 * guard policy sys_map_memory/sys_unmap_memory enforce), and the
 * thread-stack region is [0x90000000, 0x100000000) (rng.h).  Both are
 * skipped so a returned range can never overlap them.
 */
static bool VspaceScanRanges(addr_space_t *as, u64 heap_base, u64 page_count, u64 *out_base) {
    bool have_heap = heap_base >= PAGE_SIZE &&
                     heap_base + HEAP_USER_SIZE + PAGE_SIZE <= USER_PTR_MAX;
    u64  heap_lo   = have_heap ? heap_base - PAGE_SIZE : 0;
    u64  heap_hi   = have_heap ? heap_base + HEAP_USER_SIZE + PAGE_SIZE : 0;

    if (VspaceScanInterval(
            as, VSPACE_FLOOR, have_heap ? heap_lo : ASLR_STACK_BASE, page_count, out_base))
        return true;

    if (have_heap && VspaceScanInterval(as, heap_hi, ASLR_STACK_BASE, page_count, out_base))
        return true;

    return VspaceScanInterval(as, ASLR_STACK_END, USER_PTR_MAX, page_count, out_base);
}

/*
 * Pre-build the page-table hierarchy for [base, base + page_count*PAGE).
 *
 * Walks the three intermediate levels (PML4E, PDPTE, PDE) for every
 * page, allocating a table page from the PMM when an entry is absent.
 * The final leaf PTEs are left 0 (NOT present): SYS_MAP_MEMORY later
 * completes them via VmmAllocAndMap() -> VmmMap(), whose
 * walk_page_table(..., allocate=true) short-circuits on the pre-built
 * intermediate entries and only fills in the leaf.
 *
 * The range was validated hierarchy-free before building, so every
 * table page allocated here is owned by this call.
 */
static error_t VspaceBuildTables(addr_space_t *as, u64 base, u64 page_count) {
    for (u64 i = 0; i < page_count; i++) {
        u64  vaddr = base + i * PAGE_SIZE;
        u64 *pml4  = (u64 *)vspace_phys_to_virt(as->pml4_phys);

        u64 idx = Vspacepml4Index(vaddr);
        if (!(pml4[idx] & PTE_PRESENT)) {
            u64 page = PmmAllocPage();
            if (!page)
                return ERR_NOMEM;
            VspaceZeroPage(vspace_phys_to_virt(page));
            pml4[idx] = page | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
        }
        u64 *pdp = (u64 *)vspace_phys_to_virt(pml4[idx] & ~0xFFFULL);

        idx = VspacepdpIndex(vaddr);
        if (!(pdp[idx] & PTE_PRESENT)) {
            u64 page = PmmAllocPage();
            if (!page)
                return ERR_NOMEM;
            VspaceZeroPage(vspace_phys_to_virt(page));
            pdp[idx] = page | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
        }
        u64 *pd = (u64 *)vspace_phys_to_virt(pdp[idx] & ~0xFFFULL);

        idx = VspacepdIndex(vaddr);
        if (!(pd[idx] & PTE_PRESENT)) {
            u64 page = PmmAllocPage();
            if (!page)
                return ERR_NOMEM;
            VspaceZeroPage(vspace_phys_to_virt(page));
            pd[idx] = page | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
        }
        /* Leaf PTE deliberately untouched (not present). */
    }
    return OK;
}

/*
 * Free every page-table page built by VspaceBuildTables() for the
 * range [base, base + page_count*PAGE), bottom-up: all PT pages (their
 * leaves were never set), then the PD/PDP pages that became empty, then
 * any PML4E we created.  Only called on build failure, where the whole
 * range is still hierarchy-free, so every freed table was allocated by
 * the failed build.  The PML4 page itself is never freed.
 */
static void VspaceFreeTables(addr_space_t *as, u64 base, u64 page_count) {
    u64  end  = base + page_count * PAGE_SIZE;
    u64 *pml4 = (u64 *)vspace_phys_to_virt(as->pml4_phys);

    /* 1. Free the PT page of every 2 MB window the range touches. */
    u64 win      = base & ~0x1FFFFFULL;
    u64 last_win = (end - 1) & ~0x1FFFFFULL;
    while (win <= last_win) {
        u64 pml4_i = Vspacepml4Index(win);
        u64 pdp_i  = VspacepdpIndex(win);
        u64 pd_i   = VspacepdIndex(win);
        if (pml4[pml4_i] & PTE_PRESENT) {
            u64 *pdp = (u64 *)vspace_phys_to_virt(pml4[pml4_i] & ~0xFFFULL);
            if ((pdp[pdp_i] & PTE_PRESENT) && !(pdp[pdp_i] & PTE_HUGE)) {
                u64 *pd = (u64 *)vspace_phys_to_virt(pdp[pdp_i] & ~0xFFFULL);
                if ((pd[pd_i] & PTE_PRESENT) && !(pd[pd_i] & PTE_HUGE)) {
                    PmmFreePage(pd[pd_i] & ~0xFFFULL);
                    pd[pd_i] = 0;
                }
            }
        }
        win += 0x200000;
    }

    /* 2. Free PD pages (1 GB windows) that became empty. */
    u64 gb      = base & ~0x3FFFFFFFULL;
    u64 last_gb = (end - 1) & ~0x3FFFFFFFULL;
    while (gb <= last_gb) {
        u64 pml4_i = Vspacepml4Index(gb);
        u64 pdp_i  = VspacepdpIndex(gb);
        if (pml4[pml4_i] & PTE_PRESENT) {
            u64 *pdp = (u64 *)vspace_phys_to_virt(pml4[pml4_i] & ~0xFFFULL);
            if ((pdp[pdp_i] & PTE_PRESENT) && !(pdp[pdp_i] & PTE_HUGE)) {
                u64 *pd = (u64 *)vspace_phys_to_virt(pdp[pdp_i] & ~0xFFFULL);
                if (VspacePageTableEmpty(pd)) {
                    PmmFreePage(pdp[pdp_i] & ~0xFFFULL);
                    pdp[pdp_i] = 0;
                }
            }
        }
        gb += 0x40000000;
    }

    /* 3. Free PDP pages (512 GB regions) that became empty. */
    u64 first_pml4 = Vspacepml4Index(base);
    u64 last_pml4  = Vspacepml4Index(end - 1);
    for (u64 pml4_i = first_pml4; pml4_i <= last_pml4; pml4_i++) {
        if (pml4[pml4_i] & PTE_PRESENT) {
            u64 *pdp = (u64 *)vspace_phys_to_virt(pml4[pml4_i] & ~0xFFFULL);
            if (VspacePageTableEmpty(pdp)) {
                PmmFreePage(pml4[pml4_i] & ~0xFFFULL);
                pml4[pml4_i] = 0;
            }
        }
    }
}

/*
 * Rough upper bound of the table pages a `page_count`-page range needs
 * (PT pages, PD pages, PDP pages, plus one PML4 entry).  Reject
 * allocations that could never be built: without this a caller could
 * request a huge range and force the scan to walk gigabytes of address
 * space before the build (and the PMM) fails — a freeze DoS, since
 * syscalls run with interrupts masked.
 */
static bool VspaceTablesAffordable(u64 page_count) {
    u64 pt_pages  = (page_count + 511) / 512;
    u64 pd_pages  = (page_count + 262143) / 262144;
    u64 pdp_pages = (page_count + 134217727) / 134217728;
    u64 need      = pt_pages + pd_pages + pdp_pages + 1;

    return need <= PmmGetFreeMemory() / PAGE_SIZE;
}

/*
 * SYS_VSPACE_ALLOC (syscall 53) — reserve a contiguous virtual range in
 * the CALLING process's address space and pre-build its page-table
 * hierarchy.  Roadmap P1: 内核只分配虚拟地址 + 页表构建，格式语义交给
 * 用户态 — the kernel allocates virtual addresses only; the caller
 * (libos vspace_alloc wrapper / user-space ELF loader) owns the format
 * semantics and maps physical pages into the range afterwards with
 * SYS_MAP_MEMORY.
 *
 * Arguments:
 *   a1 = size    Range length in bytes; must be non-zero and 4 KB
 *                aligned (PAGE_SIZE).
 *   a2 = flags   Reserved for future use; must be 0 for now.
 *   a3..a5       Unused.
 *
 * Return value:
 *   >= 0       Page-aligned base virtual address of the reserved range.
 *   ERR_INVAL  (-2): size zero / misaligned, or flags != 0.
 *   ERR_FAULT  (-7): no current process / no address space.
 *   ERR_NOMEM  (-1): no contiguous free range of the requested size, or
 *                not enough physical memory to build the page tables.
 *
 * Contract:
 *   - The returned range lies entirely in user space and is free: no
 *     mapped page AND no pre-built hierarchy from an earlier
 *     SYS_VSPACE_ALLOC sits inside it (vspace_page_free), so two
 *     allocations can never overlap.
 *   - The intermediate page tables (PML4E / PDPTE / PDE) for the whole
 *     range are present after the call.  The leaf PTEs are all NOT
 *     present: the range is not accessible yet.  A later SYS_MAP_MEMORY
 *     reuses the pre-built hierarchy and completes the leaf PTEs
 *     (vmm_map refuses to overwrite a present leaf; ours are zero, so
 *     the first map always succeeds).
 *   - SYS_UNMAP_MEMORY / process teardown free the hierarchy normally
 *     (vmm_unmap / vmm_destroy_addr_space walk the same entries).
 *
 * Reserved regions skipped by the scan (VSPACE_FLOOR = 1 GiB):
 *   - Below the floor: ELF image (linked at 0x400000) and the fixed
 *     test mappings [0x10000000, 0x30000000).
 *   - Per-process ASLR heap [heap_base, heap_base + HEAP_USER_SIZE)
 *     plus its guard pages (heap_base - PAGE, heap_base + HEAP_USER_SIZE).
 *   - Thread-stack region [0x90000000, 0x100000000) (rng.h).
 *   Above the stack region, everything up to USER_PTR_MAX is eligible.
 */
i64 sc_sys_vspace_alloc(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    (void)a3;
    (void)a4;
    (void)a5;
    u64 size  = a1;
    u64 flags = a2;

    if (size == 0 || (size % PAGE_SIZE) != 0)
        return (i64)ERR_INVAL;
    if (flags != 0)
        return (i64)ERR_INVAL;

    u64 page_count = size / PAGE_SIZE;

    /* Must be able to fit in the user address space at all. */
    if (size > USER_PTR_MAX - VSPACE_FLOOR)
        return (i64)ERR_NOMEM;
    /* Table pages must fit in physical memory (bounds the scan too). */
    if (!VspaceTablesAffordable(page_count))
        return (i64)ERR_NOMEM;

    process_t *proc = process_current();
    if (!proc || !proc->addr_space)
        return (i64)ERR_FAULT;

    u64 base;
    if (!VspaceScanRanges(proc->addr_space, proc->heap_base, page_count, &base))
        return (i64)ERR_NOMEM;

    if (VspaceBuildTables(proc->addr_space, base, page_count) != OK) {
        VspaceFreeTables(proc->addr_space, base, page_count);
        return (i64)ERR_NOMEM;
    }

    return (i64)base;
}
