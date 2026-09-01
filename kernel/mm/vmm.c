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
 * vmm.c - Virtual Memory Manager
 * Copyright (c) 2026 OpSys Project
 *
 * 4-level paging (PML4 -> PDP -> PD -> PT) for x86_64.
 * Assumes boot.asm set up higher-half mapping for the first 4 GB via
 * 2 MB huge pages (PML4[256] → same PDP as PML4[0]). All page table
 * pages (allocated from PMM in low physical memory) are accessed via
 * phys_to_virt() which adds KERNEL_VIRT_BASE.
 *
 * ------------------------------------------------------------------
 * Structure (vmm):
 *   Virtual address: [ user 0 .. USER_PTR_MAX ) | kernel half PML4[256..511]
 *   Page walk: CR3 -> PML4 -> PDP -> PD -> PT -> 4 KB PTE (512 entries
 *   each); table pages live in low physical memory, reached via
 *   phys_to_virt() (+ KERNEL_VIRT_BASE). s_kernel_as / vmm_kernel_cr3
 *   adopt the boot PML4; each process has an addr_space_t {pml4_phys,
 *   stack_base}.
 *
 * How it works:
 *   VmmInit() reads CR3. walk_page_table() walks the four levels and
 *   allocates intermediate tables from PMM on demand (allocate=true).
 *   VmmMap / VmmMapRange / VmmAllocAndMap fill leaf PTEs; VmmUnmap
 *   clears them (invlpg) and frees empty tables bottom-up. New address
 *   spaces copy PML4[256..511], so all processes share the higher-half
 *   direct map; VmmSwitchAddrSpace() reloads CR3 (short-circuited).
 *
 * Purpose:
 *   Backs every user mapping (ELF segments, thread stacks, heap growth,
 *   SYS_MAP_MEMORY, shm) and provides VmmValidateUserRange() /
 *   VmmVirtToPhys() for syscall safety. Kernel memory lives in the
 *   shared higher half — there is no kernel heap: kernel allocations
 *   are PMM pages accessed through the direct map.
 *
 * Caveats:
 *   VmmMap refuses to overwrite a present PTE (ERR_BUSY) — unmap first.
 *   Intermediate entries always carry PTE_USER; only the leaf PTE's U/S
 *   bit controls access. VmmUnmap on a not-present leaf frees dead
 *   pre-built hierarchies but never a PT page with live leaves.
 * ------------------------------------------------------------------
 */

#include <kernel/vmm.h>
#include <kernel/process.h>
#include <kernel/pmm.h>
#include <kernel/rng.h>
#include <kernel/serial.h>

/* ---- State ---- */

static addr_space_t s_kernel_as;
static bool         s_initialized;

/* Kernel CR3 for syscall entry (exported for assembly) */
u64 vmm_kernel_cr3;

/* ---- Helpers ---- */

static inline u64 pml4Index(u64 virt) {
    return (virt >> 39) & 0x1FF;
}
static inline u64 pdpIndex(u64 virt) {
    return (virt >> 30) & 0x1FF;
}
static inline u64 pdIndex(u64 virt) {
    return (virt >> 21) & 0x1FF;
}
static inline u64 PtIndex(u64 virt) {
    return (virt >> 12) & 0x1FF;
}

/*
 * Physical ↔ virtual address conversion.
 * All page table pages live in low physical memory (< 4 GB) which is
 * mapped at KERNEL_VIRT_BASE via the boot page tables (2 MB huge pages).
 * User page tables inherit these higher-half mappings (PML4[256-511]),
 * so phys_to_virt works for any process's page table walk.
 */
static inline void *phys_to_virt(u64 phys) {
    return (void *)(phys + KERNEL_VIRT_BASE);
}
static inline u64 VirtToPhys(void *virt) {
    return (u64)virt - KERNEL_VIRT_BASE;
}

/* Zero a 4 KB page (512 u64 entries). */
static inline void ZeroPage(void *addr) {
    u64 *p = (u64 *)addr;
    for (int i = 0; i < 512; i++) {
        p[i] = 0;
    }
}

/* Return true if every entry in a page table page is not present. */
static inline bool PageTableEmpty(const u64 *table) {
    for (int i = 0; i < 512; i++) {
        if (table[i] & PTE_PRESENT) {
            return false;
        }
    }
    return true;
}

/*
 * Walk the four-level page table to the entry that would map `virt`.
 *
 * If the entry is present, returns a pointer to it.
 * If not present and `allocate` is true, allocates intermediate tables
 * from PMM (which must already be initialised) and returns the pointer.
 * If not present and `allocate` is false, returns NULL.
 *
 * All page table pages live in low physical memory (< 4 GB) and are
 * accessed via phys_to_virt() (higher-half mapping), since user page
 * tables do NOT have identity mapping (PML4[0] = 0).
 *
 * NOTE: Intermediate entries (PML4E, PDPTE, PDE) always have PTE_USER
 * set. On x86-64, if the U/S bit is 0 in ANY paging-structure entry,
 * the address is supervisor-mode only, regardless of the final PTE.
 * Setting PTE_USER on intermediates is safe: the final PT entry's U/S
 * bit controls actual access permission.
 */
static u64 *walk_page_table(addr_space_t *as, u64 virt, bool allocate) {
    u64 *pml4 = (u64 *)phys_to_virt(as->pml4_phys);

    /* PML4 -> PDP */
    u64 idx = pml4Index(virt);
    if (!(pml4[idx] & PTE_PRESENT)) {
        if (!allocate)
            return NULL;
        u64 page = PmmAllocPage();
        if (!page)
            return NULL;
        ZeroPage(phys_to_virt(page));
        pml4[idx] = page | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    }
    u64 *pdp = (u64 *)phys_to_virt(pml4[idx] & ~0xFFFULL);

    /* PDP -> PD */
    idx = pdpIndex(virt);
    if (!(pdp[idx] & PTE_PRESENT)) {
        if (!allocate)
            return NULL;
        u64 page = PmmAllocPage();
        if (!page)
            return NULL;
        ZeroPage(phys_to_virt(page));
        pdp[idx] = page | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    }
    u64 *pd = (u64 *)phys_to_virt(pdp[idx] & ~0xFFFULL);

    /* PD -> PT */
    idx = pdIndex(virt);
    if (!(pd[idx] & PTE_PRESENT)) {
        if (!allocate)
            return NULL;
        u64 page = PmmAllocPage();
        if (!page)
            return NULL;
        ZeroPage(phys_to_virt(page));
        pd[idx] = page | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    }
    u64 *pt = (u64 *)phys_to_virt(pd[idx] & ~0xFFFULL);

    return &pt[PtIndex(virt)];
}

/* ---- Public API ---- */

/*
 * Return true if the 4 KB page containing `vaddr` is mapped in `as`.
 * Handles 1 GB huge pages (PDPE.PS) and 2 MB huge pages (PDE.PS).
 * If need_write, the mapping must also be writable (PTE_WRITABLE).
 */
static bool PageIsMapped(addr_space_t *as, u64 vaddr, bool need_write) {
    u64 *pml4 = (u64 *)phys_to_virt(as->pml4_phys);

    u64 idx = pml4Index(vaddr);
    if (!(pml4[idx] & PTE_PRESENT))
        return false;

    u64 *pdp = (u64 *)phys_to_virt(pml4[idx] & ~0xFFFULL);
    idx      = pdpIndex(vaddr);
    if (!(pdp[idx] & PTE_PRESENT))
        return false;

    /* 1 GB huge page */
    if (pdp[idx] & PTE_HUGE) {
        return !need_write || (pdp[idx] & PTE_WRITABLE) != 0;
    }

    u64 *pd = (u64 *)phys_to_virt(pdp[idx] & ~0xFFFULL);
    idx     = pdIndex(vaddr);
    if (!(pd[idx] & PTE_PRESENT))
        return false;

    /* 2 MB huge page */
    if (pd[idx] & PTE_HUGE) {
        return !need_write || (pd[idx] & PTE_WRITABLE) != 0;
    }

    u64 *pt = (u64 *)phys_to_virt(pd[idx] & ~0xFFFULL);
    idx     = PtIndex(vaddr);
    if (!(pt[idx] & PTE_PRESENT))
        return false;
    return !need_write || (pt[idx] & PTE_WRITABLE) != 0;
}

bool VmmValidateUserRange(addr_space_t *as, u64 ptr, u64 size, bool need_write) {
    if (ptr == 0 || size == 0)
        return false;
    if (ptr + size < ptr) /* overflow */
        return false;
    if (ptr + size > USER_PTR_MAX)
        return false;
    if (!as)
        return false;

    /* Verify EVERY page in the range is mapped (and writable when
     * requested), not just the first.  A gap in the middle would #PF
     * the kernel on access. */
    u64 start = ptr & ~(u64)(PAGE_SIZE - 1);
    u64 end   = (ptr + size - 1) & ~(u64)(PAGE_SIZE - 1);
    for (u64 vaddr = start; vaddr <= end; vaddr += PAGE_SIZE) {
        if (!PageIsMapped(as, vaddr, need_write))
            return false;
    }
    return true;
}

/* Shared user-pointer validation against the current process.  This
 * is the ONE implementation every syscall handler uses; the former
 * per-file copies (syscall.c, process_desc.c, pci.c, virtio_blk.c)
 * are removed (dedup: v0.6 architecture cleanup).
 *
 * Race-freedom: syscalls run with IF=0 (0x80 interrupt gate, see
 * syscall_entry.S), so no timer IRQ can preempt between this check and
 * the caller's subsequent copy — the mapping cannot change mid-syscall. */
bool VmmValidateUserPtr(u64 ptr, u64 size, bool need_write) {
    process_t *proc = process_current();
    if (!proc || !proc->addr_space)
        return false;
    return VmmValidateUserRange(proc->addr_space, ptr, size, need_write);
}

void VmmInit(void) {
    SerialPuts("VMM: Initializing...\n");

    /* The boot loader set up page tables; read CR3 to find the PML4. */
    u64 cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));

    s_kernel_as.pml4_phys = cr3;
    vmm_kernel_cr3        = cr3;
    s_initialized         = true;

    SerialPrintf("VMM: Kernel PML4 at 0x%x\n", (u32)cr3);
    SerialPuts("VMM: Initialized\n");
}

addr_space_t *vmm_create_addr_space(void) {
    if (!s_initialized)
        return NULL;

    /* Allocate a fresh PML4 page from PMM. */
    u64 pml4_phys = PmmAllocPage();
    if (!pml4_phys)
        return NULL;

    u64 *new_pml4 = (u64 *)phys_to_virt(pml4_phys);
    ZeroPage(new_pml4);

    /* Copy kernel-half entries (256-511) so the new process inherits
     * kernel mappings (higher-half + identity-mapped low memory). */
    u64 *kernel_pml4 = (u64 *)phys_to_virt(s_kernel_as.pml4_phys);
    for (int i = 256; i < 512; i++) {
        new_pml4[i] = kernel_pml4[i];
    }

    /* Allocate a page to hold the addr_space_t struct itself.
     * The struct is tiny (8 bytes) but without a general-purpose
     * allocator we reserve a full page; the pointer IS the page
     * address so pmm_free_page works in destroy. */
    u64 as_page = PmmAllocPage();
    if (!as_page) {
        PmmFreePage(pml4_phys);
        return NULL;
    }
    ZeroPage(phys_to_virt(as_page));

    addr_space_t *as = (addr_space_t *)phys_to_virt(as_page);
    as->pml4_phys    = pml4_phys;

    /* ASLR: randomize the thread-stack region base for this address
     * space (design item ⑭).  ThreadCreateUser() maps thread TID's
     * stack at stack_base + tid*PAGE_SIZE. */
    as->stack_base = AslrStackBase();

    SerialPrintf("VMM: Created addr space, PML4 at 0x%x, stack_base=0x%x\n",
                  (u32)pml4_phys,
                  (u32)as->stack_base);
    return as;
}

void VmmDestroyAddrSpace(addr_space_t *as) {
    if (!as || as == &s_kernel_as) {
        return;
    }

    u64 *pml4 = (u64 *)phys_to_virt(as->pml4_phys);

    /* Walk user-space entries only (indices 0-255). */
    for (int pml4_i = 0; pml4_i < 256; pml4_i++) {
        if (!(pml4[pml4_i] & PTE_PRESENT))
            continue;

        u64 *pdp = (u64 *)phys_to_virt(pml4[pml4_i] & ~0xFFFULL);
        for (int pdp_i = 0; pdp_i < 512; pdp_i++) {
            if (!(pdp[pdp_i] & PTE_PRESENT))
                continue;

            /* 1 GB huge page */
            if (pdp[pdp_i] & PTE_HUGE) {
                PmmFreePage(pdp[pdp_i] & ~0xFFFULL);
                continue;
            }

            u64 *pd = (u64 *)phys_to_virt(pdp[pdp_i] & ~0xFFFULL);
            for (int pd_i = 0; pd_i < 512; pd_i++) {
                if (!(pd[pd_i] & PTE_PRESENT))
                    continue;

                /* 2 MB huge page */
                if (pd[pd_i] & PTE_HUGE) {
                    PmmFreePage(pd[pd_i] & ~0xFFFULL);
                    continue;
                }

                u64 *pt = (u64 *)phys_to_virt(pd[pd_i] & ~0xFFFULL);
                for (int pt_i = 0; pt_i < 512; pt_i++) {
                    if (pt[pt_i] & PTE_PRESENT) {
                        PmmFreePage(pt[pt_i] & ~0xFFFULL);
                    }
                }
                PmmFreePage(VirtToPhys(pt));
            }
            PmmFreePage(VirtToPhys(pd));
        }
        PmmFreePage(VirtToPhys(pdp));
    }

    /* Free the PML4 page and the page holding the struct. */
    PmmFreePage(as->pml4_phys);
    PmmFreePage(VirtToPhys(as));
}

error_t VmmMap(addr_space_t *as, u64 virt, u64 phys, u64 flags) {
    if (!as)
        return ERR_INVAL;
    /* Defense in depth: never touch the kernel-half page tables
     * (PML4 entries 256-511) from a user-supplied VA.  All callers
     * pass user addresses; this rejects a wrapped/forged virt that
     * slipped past syscall-layer checks. */
    if (virt >= USER_PTR_MAX)
        return ERR_INVAL;

    u64 *pte = walk_page_table(as, virt, true);
    if (!pte)
        return ERR_NOMEM;

    /* Refuse to overwrite an existing mapping.  The old code freed the
     * previously-mapped physical page here, which is unsafe: the page may
     * still be referenced elsewhere (aliased into another address space,
     * or the caller may still hold a pointer to it).  Callers must
     * explicitly VmmUnmap() first.  No legitimate call site re-maps an
     * already-mapped VA (ELF segments, thread stacks, heap growth and the
     * init stack all map fresh VAs). */
    if (*pte & PTE_PRESENT)
        return ERR_BUSY;

    *pte = (phys & ~0xFFFULL) | flags | PTE_PRESENT;
    return OK;
}

error_t VmmUnmap(addr_space_t *as, u64 virt) {
    if (!as)
        return ERR_INVAL;
    /* Defense in depth: mirror vmm_map — never touch the kernel-half
     * page tables from a user-supplied VA. */
    if (virt >= USER_PTR_MAX)
        return ERR_INVAL;

    u64 *pml4 = (u64 *)phys_to_virt(as->pml4_phys);
    u64  idx;

    /* PML4 -> PDP */
    idx = pml4Index(virt);
    if (!(pml4[idx] & PTE_PRESENT))
        return ERR_NOENT;
    u64 *pdp = (u64 *)phys_to_virt(pml4[idx] & ~0xFFFULL);

    /* PDP -> PD */
    idx = pdpIndex(virt);
    if (!(pdp[idx] & PTE_PRESENT))
        return ERR_NOENT;
    if (pdp[idx] & PTE_HUGE) {
        pdp[idx] = 0;
        __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
        return OK;
    }
    u64 *pd = (u64 *)phys_to_virt(pdp[idx] & ~0xFFFULL);

    /* PD -> PT */
    idx = pdIndex(virt);
    if (!(pd[idx] & PTE_PRESENT))
        return ERR_NOENT;
    if (pd[idx] & PTE_HUGE) {
        pd[idx] = 0;
        __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
        return OK;
    }
    u64 *pt = (u64 *)phys_to_virt(pd[idx] & ~0xFFFULL);

    /* Clear the final PTE and invalidate TLB. */
    idx = PtIndex(virt);
    if (!(pt[idx] & PTE_PRESENT)) {
        /* Leaf not mapped.  SYS_VSPACE_ALLOC pre-builds whole PML4E/PDPTE/
         * PDE/PT hierarchies with leaf PTEs left not-present (vspace.c), so
         * unmapping such a leaf -- or a gap in a partially-mapped range --
         * reaches here.  If the PT page has no live leaves it is a dead
         * pre-built hierarchy: free the empty chain bottom-up, exactly like
         * the post-unmap cleanup below.  If it does have live leaves (other
         * pages in the same 2 MB window) the PT is shared -- never free it.
         * Return OK in both cases so vmm_unmap_range keeps walking
         * subsequent pages instead of aborting mid-range on ERR_NOENT. */
        if (PageTableEmpty(pt)) {
            pd[pdIndex(virt)] = 0;
            PmmFreePage(VirtToPhys(pt));

            if (PageTableEmpty(pd)) {
                pdp[pdpIndex(virt)] = 0;
                PmmFreePage(VirtToPhys(pd));

                if (PageTableEmpty(pdp)) {
                    pml4[pml4Index(virt)] = 0;
                    PmmFreePage(VirtToPhys(pdp));
                }
            }
        }
        return OK;
    }
    pt[idx] = 0;
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");

    /* Walk back up and free empty page tables. */
    if (PageTableEmpty(pt)) {
        pd[pdIndex(virt)] = 0;
        PmmFreePage(VirtToPhys(pt));

        if (PageTableEmpty(pd)) {
            pdp[pdpIndex(virt)] = 0;
            PmmFreePage(VirtToPhys(pd));

            if (PageTableEmpty(pdp)) {
                pml4[pml4Index(virt)] = 0;
                PmmFreePage(VirtToPhys(pdp));
            }
        }
    }

    return OK;
}

u64 VmmVirtToPhys(addr_space_t *as, u64 virt) {
    if (!as)
        return 0;

    u64 *pte = walk_page_table(as, virt, false);
    if (!pte || !(*pte & PTE_PRESENT))
        return 0;

    /* Physical address is bits 12-51. Clear low flags (0-11) and high flags (52-63, including NX). */
    return (*pte & 0x000FFFFFFFFFF000ULL) | (virt & 0xFFFULL);
}

addr_space_t *vmm_get_kernel_addr_space(void) {
    return &s_kernel_as;
}

void VmmSwitchAddrSpace(addr_space_t *as) {
    if (!as)
        return;
    /* Short-circuit: if CR3 already points at this address space, do
     * not reload it.  A CR3 write flushes the entire TLB (including
     * the shared kernel-half entries) and forces every user page to be
     * re-walked on next access.  IPC hot path calls this twice per
     * message (deliver_to_waiter: switch to receiver, switch back);
     * for same-process IPC both sides share one addr_space, so the
     * two writes are pure overhead.  Reading CR3 is a single register
     * read -- far cheaper than the TLB shootdown a write triggers. */
    u64 cur_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cur_cr3));
    if (cur_cr3 == as->pml4_phys)
        return;
    __asm__ volatile("mov %0, %%cr3" : : "r"(as->pml4_phys) : "memory");
}

error_t VmmMapRange(addr_space_t *as, u64 virt, u64 phys, u64 page_count, u64 flags) {
    for (u64 i = 0; i < page_count; i++) {
        error_t err = VmmMap(as, virt + i * PAGE_SIZE, phys + i * PAGE_SIZE, flags);
        if (err != OK)
            return err;
    }
    return OK;
}

error_t VmmUnmapRange(addr_space_t *as, u64 virt, u64 page_count) {
    for (u64 i = 0; i < page_count; i++) {
        error_t err = VmmUnmap(as, virt + i * PAGE_SIZE);
        if (err != OK)
            return err;
    }
    return OK;
}

error_t VmmAllocAndMap(addr_space_t *as, u64 virt, u64 flags) {
    u64 page = PmmAllocPage();
    if (!page)
        return ERR_NOMEM;

    ZeroPage(phys_to_virt(page));
    error_t err = VmmMap(as, virt, page, flags);
    if (err != OK) {
        /* vmm_map failed (e.g. VA already mapped -> ERR_BUSY): the page
         * was allocated but never inserted, so return it to the pool. */
        PmmFreePage(page);
        return err;
    }
    return OK;
}
