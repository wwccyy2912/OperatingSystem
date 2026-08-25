/*
 * process_desc.c - Descriptor-based process creation (SYS_PROCESS_CREATE)
 * Copyright (c) 2026 OpSys Project
 *
 * Roadmap P1: ELF parsing moved OUT of the kernel into user-space libos
 * (user/lib/libos/elf_parse.c).  The caller parses the ELF and describes
 * the image with a proc_image_desc_t (entry + seg_count) followed by
 * seg_count proc_seg_desc_t entries.  This handler:
 *
 *   - validates the name, the descriptor, the segment table and the
 *     blob range (validate_user_ptr + USER_PTR, same pattern as the
 *     other syscalls in syscall.c),
 *   - creates a fresh address space,
 *   - for each segment: allocates + maps its pages with PTE flags built
 *     from seg->prot (exactly like the old elf.c loader did from ELF
 *     p_flags), copies seg->filesz opaque bytes from the caller's blob
 *     via the direct map, and zeroes the memsz - filesz BSS tail,
 *   - hands the loaded image to process_create() (the kernel never
 *     parses the blob — it is treated as opaque bytes).
 *
 * The kernel-side ELF parser (kernel/mm/elf.c, kernel/include/kernel/
 * elf.h) is gone; the only ELF walking left in the kernel is the
 * bootstrap-only init loader in kernel/mm/elf_boot.c.
 */

#include <kernel/types.h>
#include <kernel/vmm.h>
#include <kernel/process.h>
#include <kernel/syscall_handlers.h>
#include <kernel/proc_image.h>
#include <kernel/serial.h>
#include <kernel/string.h>
#include <kernel/blob.h>
#include <kernel/cap.h>

/* Process names are copied into process_t.name[64] (bounded read). */
#define PROC_NAME_MAX 64

/* Upper bound on segments per image: bounds the segment-table range
 * validation and the loop below (no overflow in the byte math). */
#define PROC_IMAGE_MAX_SEGS 16

/*
 * Validate a user pointer range.  Mirrors syscall.c: every page in the
 * range must be mapped in the current process's address space so the
 * kernel can dereference it without #PF.  Same checks as the removed
 * sys_process_create in syscall.c.
 */
static bool validate_user_ptr(u64 ptr, u64 size, bool need_write) {
    return vmm_validate_user_ptr(ptr, size, need_write);
}

/* Convenience: cast a validated user pointer */
#define USER_PTR(p) ((void *)(uintptr_t)(p))

/*
 * Map one segment into the new address space and copy its contents.
 *
 * Mirrors the old elf.c PT_LOAD handling: allocate + map the segment's
 * page range (PTE_USER always set, PTE_WRITABLE iff PROT_WRITE,
 * PTE_NO_EXECUTE unless PROT_EXEC), then walk each page, resolve its
 * physical address, write through the kernel direct map (phys +
 * KERNEL_VIRT_BASE — valid because every user page table inherits the
 * kernel higher-half), copy the in-page portion of the file data from
 * the caller's blob, and zero the remainder of the page (BSS).
 *
 * All segment fields were validated before this is called.
 */
static error_t map_segment(addr_space_t *as, const proc_seg_desc_t *seg, const u8 *blob) {
    u64 page_vaddr = seg->vaddr & ~(PAGE_SIZE - 1);
    u64 page_end   = (seg->vaddr + seg->memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    /* Build PTE flags from the segment's protection bits. */
    u64 pte_flags = PTE_PRESENT | PTE_USER;
    if (seg->prot & PROT_WRITE)
        pte_flags |= PTE_WRITABLE;
    if (!(seg->prot & PROT_EXEC))
        pte_flags |= PTE_NO_EXECUTE;

    for (u64 pg = page_vaddr; pg < page_end; pg += PAGE_SIZE) {
        error_t err = vmm_alloc_and_map(as, pg, pte_flags);
        if (err != OK) {
            serial_printf("proc: vmm_alloc_and_map(0x%x) failed: %d\n", (u32)pg, err);
            return ERR_NOMEM;
        }
    }

    /* Copy file data into the mapped pages and zero the BSS tail.
     * The blob lives in the caller's (still active) address space; the
     * segment data starts at blob + src_offset.  We copy into each
     * mapped page individually, writing via the direct map. */
    const u8 *src         = blob + seg->src_offset;
    u64       copy_offset = 0;

    for (u64 pg = page_vaddr; pg < page_end; pg += PAGE_SIZE) {
        /* Resolve the user page to a physical page so we can write
         * through the kernel's direct-mapped identity. */
        u64 phys = vmm_virt_to_phys(as, pg);
        if (!phys)
            return ERR_NOMEM;
        u8 *dest = (u8 *)(phys + KERNEL_VIRT_BASE);

        /* Region of segment bytes that falls in [pg, pg+PAGE_SIZE). */
        u64 seg_end       = seg->vaddr + seg->memsz; /* one past last byte */
        u64 pg_page_end   = pg + PAGE_SIZE;
        u64 data_start    = (seg->vaddr > pg) ? seg->vaddr : pg;
        u64 data_end      = (seg_end < pg_page_end) ? seg_end : pg_page_end;
        u64 pg_data_start = data_start - pg;
        u64 pg_data_end   = (data_end > data_start) ? (data_end - pg) : 0;

        u64 in_page_filesz = 0;
        if (copy_offset < seg->filesz) {
            u64 remaining_file = seg->filesz - copy_offset;
            u64 remaining_mem  = pg_data_end - pg_data_start;
            in_page_filesz     = remaining_file;
            if (in_page_filesz > remaining_mem)
                in_page_filesz = remaining_mem;
        }

        if (in_page_filesz > 0) {
            memcpy(dest + pg_data_start, src + copy_offset, in_page_filesz);
            copy_offset += in_page_filesz;
        }

        /* Zero the rest of this page region (BSS) */
        u64 zero_start = pg_data_start + in_page_filesz;
        if (zero_start < pg_data_end)
            memset(dest + zero_start, 0, pg_data_end - zero_start);
    }

    return OK;
}

/*
 * SYS_PROCESS_CREATE — descriptor-based process creation.
 * arg1 = name (user string, bounded 64-byte read)
 * arg2 = desc_ptr: proc_image_desc_t, immediately followed by
 *        desc->seg_count proc_seg_desc_t entries (contiguous)
 * arg3 = blob_ptr: caller-owned buffer holding the opaque image blob
 * arg4 = blob_size
 *
 * The kernel never parses the blob's file format; it only copies the
 * segment byte ranges the caller's descriptor names.
 *
 * @return The new PID, or a negative errno.
 */
i64 sc_sys_process_create(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    (void)a5;

    /* ---- arg1: process name (bounded 64-byte read) ---- */
    if (a1 == 0 || !validate_user_ptr(a1, PROC_NAME_MAX, false))
        return (i64)ERR_FAULT;

    char        name[PROC_NAME_MAX];
    const char *u_name = (const char *)USER_PTR(a1);
    u64         i;
    for (i = 0; i < sizeof(name) - 1 && u_name[i]; i++)
        name[i] = u_name[i];
    name[i] = '\0';

    /* ---- arg2: image descriptor + segment table ---- */
    if (a2 == 0 || !validate_user_ptr(a2, sizeof(proc_image_desc_t), false))
        return (i64)ERR_FAULT;

    proc_image_desc_t desc;
    memcpy(&desc, USER_PTR(a2), sizeof(proc_image_desc_t));

    if (desc.seg_count == 0 || desc.seg_count > PROC_IMAGE_MAX_SEGS)
        return (i64)ERR_INVAL;
    if (desc.entry == 0 || desc.entry >= USER_PTR_MAX)
        return (i64)ERR_INVAL;

    /* The segment table immediately follows the descriptor.  Validate
     * the whole range is mapped before dereferencing it. */
    const proc_seg_desc_t *segs =
        (const proc_seg_desc_t *)((const u8 *)USER_PTR(a2) + sizeof(proc_image_desc_t));
    if (!validate_user_ptr(
            a2 + sizeof(proc_image_desc_t), desc.seg_count * sizeof(proc_seg_desc_t), false))
        return (i64)ERR_FAULT;

    /* ---- arg3/arg4: blob range (opaque bytes, never parsed) ---- */
    if (a3 == 0 || a4 == 0 || !validate_user_ptr(a3, a4, false))
        return (i64)ERR_FAULT;
    const u8 *blob      = (const u8 *)USER_PTR(a3);
    u64       blob_size = a4;

    /* ---- Validate every segment before touching the allocator ---- */
    for (u64 si = 0; si < desc.seg_count; si++) {
        const proc_seg_desc_t *seg = &segs[si];

        if (seg->vaddr == 0 || (seg->vaddr & (PAGE_SIZE - 1)) != 0) {
            serial_printf("proc: seg %d vaddr 0x%x not page-aligned\n", (int)si, (u32)seg->vaddr);
            return (i64)ERR_INVAL;
        }
        if (seg->memsz < seg->filesz) {
            serial_printf("proc: seg %d memsz 0x%x < filesz 0x%x\n",
                          (int)si,
                          (u32)seg->memsz,
                          (u32)seg->filesz);
            return (i64)ERR_INVAL;
        }
        /* prot must be a subset of PROT_NONE|PROT_READ|PROT_WRITE|PROT_EXEC */
        if ((seg->prot & ~(u64)(PROT_NONE | PROT_READ | PROT_WRITE | PROT_EXEC)) != 0) {
            serial_printf("proc: seg %d bad prot 0x%x\n", (int)si, (u32)seg->prot);
            return (i64)ERR_INVAL;
        }
        /* [vaddr, vaddr+memsz) must stay in the user address space. */
        if (seg->memsz > USER_PTR_MAX || seg->vaddr > USER_PTR_MAX - seg->memsz) {
            serial_printf("proc: seg %d vaddr out of user space\n", (int)si);
            return (i64)ERR_INVAL;
        }
        /* File data [src_offset, src_offset+filesz) must lie in blob. */
        if (seg->filesz > blob_size || seg->src_offset > blob_size - seg->filesz) {
            serial_printf("proc: seg %d blob range out of bounds\n", (int)si);
            return (i64)ERR_INVAL;
        }
    }

    serial_printf(
        "proc: CREATE name=%s entry=0x%x segs=%d\n", name, (u32)desc.entry, (int)desc.seg_count);

    /* ---- Build the address space ---- */
    addr_space_t *as = vmm_create_addr_space();
    if (!as)
        return (i64)ERR_NOMEM;

    /* ---- Map + fill every segment ---- */
    for (u64 si = 0; si < desc.seg_count; si++) {
        error_t err = map_segment(as, &segs[si], blob);
        if (err != OK) {
            vmm_destroy_addr_space(as);
            return (i64)err;
        }
    }

    /* ---- Start the process at desc->entry.
     * Ownership of `as` transfers to process_create(); on failure it
     * destroys the address space itself. */
    process_t *proc = process_create(name, desc.entry, as);
    if (!proc)
        return (i64)ERR_NOMEM;

    /* ---- Blob identity seeding (docs/ops_format.md §6): the manager,
     * perm, pkg, term, vfs and fs_mem_driver services are the only
     * subjects allowed to sign / answer atom caps
     * (sys_cap_grant_to_subject / perm decision_encode / pkg manifest
     * issue / Powerbox ANSWER — the term UI agent answers user
     * verdicts — and SYS_SHM_CREATE/MAP, which vfs_server uses to
     * export zero-copy file pages from the driver's pool).  Identity
     * is established by CONTENT: the caller's blob must be
     * byte-identical (blob_size + memcmp) to the kernel-embedded
     * service ELF — a name like "perm" alone is NOT trusted (an app
     * can name itself anything).
     *
     * CALLER GATE (production hardening): the spawner itself must
     * hold ATOM_SERVICE_MANAGE.  Otherwise an untrusted app could
     * blob_get("perm") + process_create() a byte-identical copy and
     * receive the management atom (privilege escalation).  The legit
     * chain is init -> manager -> services; init and manager both
     * hold the atom, so real service spawns keep working. ---- */
    process_t *cur = process_current();
    if (proc->cap_table && cur && cur->cap_table &&
        cap_lookup_by_atom(cur->cap_table, cur->subject_id, ATOM_SERVICE_MANAGE, 0) !=
            CAP_NULL) {
        static const char *const s_svc_blobs[] = {
            "manager", "perm", "pkg", "term", "vfs", "fs_mem_driver", "user",
            "policy"};
        for (u64 bi = 0; bi < sizeof(s_svc_blobs) / sizeof(s_svc_blobs[0]); bi++) {
            const void *blob_data = NULL;
            u64         blob_sz   = 0;
            if (blob_get(s_svc_blobs[bi], &blob_data, &blob_sz) != OK)
                continue;
            if (blob_size == blob_sz && memcmp(blob, blob_data, blob_sz) == 0) {
                cap_t h = CAP_NULL;
                cap_create_atom(
                    proc->cap_table, proc->subject_id, ATOM_SERVICE_MANAGE, RIGHT_ALL, 0, 0, 0, &h);
                break;
            }
        }
    }

    return (i64)proc->pid;
}
