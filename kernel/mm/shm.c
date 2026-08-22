/*
 * shm.c - Shared physical-page pools (zero-copy read path, vfs_design §8.4)
 * Copyright (c) 2026 OpSys Project
 *
 * Production hardening / Phase 3 (zero-copy): a trusted service
 * (fs_mem_driver) allocates a POOL of contiguous physical pages with
 * SYS_SHM_CREATE and stores file data in it.  When a client reads a
 * file, vfs_server validates the client's read permission and then
 * maps the file's pool pages READ-ONLY into the client's address space
 * with SYS_SHM_MAP — no 4096-byte IPC copies for bulk data.
 *
 * Security:
 *   - SYS_SHM_CREATE is gated on ATOM_SERVICE_MANAGE: only kernel-
 *     endorsed services (blob-identity seeded) can create pools.
 *   - SYS_SHM_MAP is gated on ATOM_SERVICE_MANAGE AND verifies the
 *     requested (phys_base, count) against the pool table: a caller
 *     can only map pages the kernel itself allocated as a pool, never
 *     arbitrary physical memory.
 *   - Client mappings are READ-ONLY and NON-EXECUTABLE; the client
 *     supplies a vspace_alloc()'d target range, so the mapping cannot
 *     clobber existing mappings.
 *   - Pools owned by a dying process are released by shm_cleanup_process()
 *     (called from process_reap), so physical pages never leak.
 */

#include <kernel/types.h>
#include <kernel/pmm.h>
#include <kernel/vmm.h>
#include <kernel/process.h>
#include <kernel/cap.h>
#include <kernel/serial.h>
#include <kernel/shm.h>

#define SHM_MAX_POOLS 8
#define SHM_MAX_PAGES 1024 /* per pool: 4 MiB */

typedef struct {
    bool in_use;
    subject_id_t owner_subject; /* kernel-issued, unforgeable */
    u64          phys_base;     /* first physical page */
    u32          page_count;
} shm_pool_t;

static shm_pool_t s_pools[SHM_MAX_POOLS];

/* Look up a pool by its physical base (the only handle a caller has). */
static shm_pool_t *shm_pool_find(u64 phys_base) {
    for (u32 i = 0; i < SHM_MAX_POOLS; i++) {
        if (s_pools[i].in_use && s_pools[i].phys_base == phys_base)
            return &s_pools[i];
    }
    return NULL;
}

/*
 * SYS_SHM_CREATE (67) — allocate a contiguous physical-page pool and
 * map it into the CALLER's address space at `virt` (a vspace_alloc()'d
 * range with pre-built, leaf-absent page tables).
 * a1 = page count, a2 = target virtual base.
 * @return The physical base (kernel-owned handle for SYS_SHM_MAP), or
 *         a negative error.
 */
i64 sc_sys_shm_create(u64 count, u64 virt) {
    if (count == 0 || count > SHM_MAX_PAGES)
        return (i64)ERR_INVAL;
    if (virt == 0 || virt >= USER_PTR_MAX || (virt & (PAGE_SIZE - 1)) != 0)
        return (i64)ERR_INVAL;

    process_t *proc = process_current();
    if (!proc || !proc->cap_table || !proc->addr_space)
        return (i64)ERR_FAULT;

    /* GATE (docs/ops_format.md §6): pool creation is management-plane. */
    if (cap_lookup_by_atom(proc->cap_table, proc->subject_id, ATOM_SERVICE_MANAGE, 0) ==
        CAP_NULL)
        return (i64)ERR_NOCAP;

    /* Overflow-safe range check against the caller's user space. */
    if (count * PAGE_SIZE > USER_PTR_MAX - virt)
        return (i64)ERR_INVAL;

    shm_pool_t *slot = NULL;
    for (u32 i = 0; i < SHM_MAX_POOLS; i++) {
        if (!s_pools[i].in_use) {
            slot = &s_pools[i];
            break;
        }
    }
    if (!slot)
        return (i64)ERR_NOMEM;

    u64 phys = pmm_alloc_pages(count);
    if (!phys)
        return (i64)ERR_NOMEM;

    error_t err = vmm_map_range(proc->addr_space,
                                virt,
                                phys,
                                count,
                                PTE_PRESENT | PTE_USER | PTE_WRITABLE | PTE_NO_EXECUTE);
    if (err != OK) {
        pmm_free_pages(phys, count);
        return (i64)err;
    }

    slot->in_use        = true;
    slot->owner_subject = proc->subject_id;
    slot->phys_base     = phys;
    slot->page_count    = (u32)count;
    serial_printf("shm: pool created subject=%u phys=0x%x count=%u\n",
                  (unsigned)proc->subject_id,
                  (u32)phys,
                  (unsigned)count);
    return (i64)phys;
}

/*
 * SYS_SHM_MAP (68) — map an existing pool's pages READ-ONLY into the
 * target process's address space at `target_virt` (vspace_alloc'ed by
 * the client).
 * a1 = pool physical base, a2 = page count, a3 = target subject,
 * a4 = target virtual base.
 * @return OK, or a negative error.
 */
i64 sc_sys_shm_map(u64 phys_base, u64 count, u64 target_subject, u64 target_virt) {
    if (count == 0 || count > SHM_MAX_PAGES)
        return (i64)ERR_INVAL;
    if (target_virt == 0 || target_virt >= USER_PTR_MAX ||
        (target_virt & (PAGE_SIZE - 1)) != 0)
        return (i64)ERR_INVAL;

    process_t *proc = process_current();
    if (!proc || !proc->cap_table)
        return (i64)ERR_FAULT;

    /* GATE: management-plane caller (vfs_server exports on a client's
     * behalf after its own permission check). */
    if (cap_lookup_by_atom(proc->cap_table, proc->subject_id, ATOM_SERVICE_MANAGE, 0) ==
        CAP_NULL)
        return (i64)ERR_NOCAP;

    /* The physical range must be an EXISTING POOL (kernel-allocated),
     * and the requested extent must lie inside it — arbitrary physical
     * addresses can never be mapped this way. */
    shm_pool_t *pool = shm_pool_find(phys_base);
    if (!pool || pool->page_count < count)
        return (i64)ERR_NOENT;
    if (count * PAGE_SIZE > USER_PTR_MAX - target_virt)
        return (i64)ERR_INVAL;

    process_t *target = process_get_by_subject((subject_id_t)target_subject);
    if (!target || !target->addr_space)
        return (i64)ERR_NOENT;

    /* READ-ONLY, NON-EXECUTABLE: the client may not modify the shared
     * file pages or jump into them. */
    error_t err = vmm_map_range(target->addr_space,
                                target_virt,
                                phys_base,
                                (u32)count,
                                PTE_PRESENT | PTE_USER | PTE_NO_EXECUTE);
    return (i64)err;
}

/*
 * Release every pool owned by a dying process.  Called from
 * process_reap(); without this the pool's physical pages would leak.
 */
void shm_cleanup_process(subject_id_t owner) {
    for (u32 i = 0; i < SHM_MAX_POOLS; i++) {
        if (s_pools[i].in_use && s_pools[i].owner_subject == owner) {
            pmm_free_pages(s_pools[i].phys_base, s_pools[i].page_count);
            s_pools[i].in_use = false;
            serial_printf("shm: pool freed (subject=%u, %u pages)\n",
                          (unsigned)owner,
                          (unsigned)s_pools[i].page_count);
        }
    }
}
