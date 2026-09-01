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
 * elf_boot.c - BOOTSTRAP-ONLY ELF loader for the embedded init blob
 * Copyright (c) 2026 OpSys Project
 *
 * ⚠ BOOTSTRAP ONLY: this loader exists solely to start the embedded
 * init process (PID 1) during kernel boot.  Init is a trusted,
 * kernel-embedded image (blob.c owns the linker symbols); this path is
 * UNREACHABLE from user input — no syscall or IPC message can ever
 * reach it.
 *
 * Roadmap P1: all general-purpose ELF parsing moved OUT of the kernel
 * into user-space libos (user/lib/libos/elf_parse.c).  User processes
 * are spawned via SYS_PROCESS_CREATE with pre-parsed descriptors
 * (kernel/syscall/process_desc.c + kernel/proc_image.h).  The former
 * general-purpose kernel/mm/elf.c was deleted; do NOT grow this file
 * back into a general loader. *
 * ------------------------------------------------------------------
 * Structure (elf_boot):
 *   ELF header/program headers -> ElfBootLoad(): for each PT_LOAD,
 *   copy bytes to the target vaddr and set page permissions.
 * How it works:
 *   Validates the ELF magic/class, maps segments via VmmMapRange and
 *   copies from the blob with copy_string/page-safe helpers.
 * Purpose:
 *   Load init and user service ELF images into fresh address spaces.
 * Caveats:
 *   Only simple ELF64 static images are supported (no relocations,
 *   no interpreter); entry must be page-aligned-mapped.
 * ------------------------------------------------------------------
 */
#include <kernel/elf_boot.h>
#include <kernel/serial.h>
#include <kernel/string.h>

/**
 * elf_boot_load - Load the embedded init ELF into an address space.
 *
 * Walks the PT_LOAD program headers of the (trusted) embedded init
 * blob, allocates + maps each segment's pages, copies the file data,
 * and zeroes the BSS tail — the same vmm/direct-map mechanics the
 * general elf.c used, trimmed to the boot path's needs.
 */
int ElfBootLoad(addr_space_t *as, const void *elf_data, u64 elf_size, u64 *entry_out) {
    const u8         *data = (const u8 *)elf_data;
    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)data;

    /* ---- Blob must at least hold the ELF header ---- */
    if (elf_size < sizeof(Elf64_Ehdr)) {
        SerialPuts("  ELF: Truncated header\n");
        return ERR_INVAL;
    }

    /* ---- Validate magic, class (64-bit) and encoding (LE) ---- */
    if (ehdr->e_ident[0] != ELFMAG0 || ehdr->e_ident[1] != ELFMAG1 || ehdr->e_ident[2] != ELFMAG2 ||
        ehdr->e_ident[3] != ELFMAG3) {
        SerialPuts("  ELF: Bad magic\n");
        return ERR_INVAL;
    }
    if (ehdr->e_ident[4] != ELFCLASS64 || ehdr->e_ident[5] != ELFDATA2LSB) {
        SerialPuts("  ELF: Not ELFCLASS64/little-endian\n");
        return ERR_INVAL;
    }

    /* ---- Entry point must live in the user address space ---- */
    u64 entry = ehdr->e_entry;
    if (entry == 0 || entry >= USER_PTR_MAX) {
        SerialPuts("  ELF: Bad entry point\n");
        return ERR_INVAL;
    }

    /* ---- Bounds-check the program header table ---- */
    if (ehdr->e_phnum == 0) {
        SerialPuts("  ELF: No program headers\n");
        return ERR_INVAL;
    }
    if (ehdr->e_phentsize < sizeof(Elf64_Phdr)) {
        SerialPuts("  ELF: phentsize too small\n");
        return ERR_INVAL;
    }
    /* The loader strides phdrs by sizeof(Elf64_Phdr); the whole table
     * it will read must lie within the blob. */
    u64 phdr_bytes = (u64)ehdr->e_phnum * (u64)sizeof(Elf64_Phdr);
    if (ehdr->e_phoff > elf_size || phdr_bytes > elf_size - ehdr->e_phoff) {
        SerialPuts("  ELF: Program header table out of bounds\n");
        return ERR_INVAL;
    }

    SerialPrintf("  ELF: entry=0x%x, phnum=%d\n", (u32)entry, ehdr->e_phnum);

    /* ---- Iterate program headers (PT_LOAD only) ---- */
    const Elf64_Phdr *phdr = (const Elf64_Phdr *)(data + ehdr->e_phoff);

    for (u16 i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD)
            continue;

        u64 vaddr  = phdr[i].p_vaddr; /* init is ET_EXEC: absolute */
        u64 memsz  = phdr[i].p_memsz;
        u64 filesz = phdr[i].p_filesz;
        u64 offset = phdr[i].p_offset;

        /* ---- Bounds-check the segment itself ---- */
        if (filesz > elf_size || offset > elf_size - filesz) {
            SerialPrintf("  ELF: PT_LOAD [%d] file range out of bounds\n", i);
            return ERR_INVAL;
        }
        /* ELF spec: memsz >= filesz. */
        if (memsz < filesz) {
            SerialPrintf("  ELF: PT_LOAD [%d] memsz < filesz\n", i);
            return ERR_INVAL;
        }
        /* [vaddr, vaddr+memsz) must stay in the user address space. */
        if (memsz > USER_PTR_MAX || vaddr > USER_PTR_MAX - memsz) {
            SerialPrintf("  ELF: PT_LOAD [%d] vaddr out of user space\n", i);
            return ERR_INVAL;
        }

        SerialPrintfLevel(SERIAL_LOG_DEBUG,
                            "  ELF: PT_LOAD [%d] vaddr=0x%x filesz=0x%x memsz=0x%x flags=0x%x\n",
                            i,
                            (u32)vaddr,
                            (u32)filesz,
                            (u32)memsz,
                            phdr[i].p_flags);

        /* ---- Map pages for this segment ---- */
        u64 page_vaddr = vaddr & ~(PAGE_SIZE - 1);
        u64 page_end   = (vaddr + memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

        /* Build PTE flags from ELF segment flags.
         * PTE_USER is always set; PTE_NO_EXECUTE unless executable. */
        u64 pte_flags = PTE_PRESENT | PTE_USER;
        if (phdr[i].p_flags & PF_W)
            pte_flags |= PTE_WRITABLE;
        if (!(phdr[i].p_flags & PF_X))
            pte_flags |= PTE_NO_EXECUTE;

        for (u64 pg = page_vaddr; pg < page_end; pg += PAGE_SIZE) {
            error_t err = VmmAllocAndMap(as, pg, pte_flags);
            if (err != OK) {
                SerialPrintf("  ELF: VmmAllocAndMap(0x%x) failed: %d\n", (u32)pg, err);
                return ERR_NOMEM;
            }
        }

        /* ---- Copy file data into mapped pages, zero BSS tail ----
         * The blob is in kernel memory (higher-half).  Resolve each
         * user page to its physical address and write via the kernel's
         * direct-mapped identity. */
        u64 copy_offset = 0;
        for (u64 pg = page_vaddr; pg < page_end; pg += PAGE_SIZE) {
            u64 phys = VmmVirtToPhys(as, pg);
            if (!phys)
                return ERR_NOMEM;
            u8 *dest = (u8 *)(phys + KERNEL_VIRT_BASE);

            /* Region of segment data falling in [pg, pg+PAGE_SIZE). */
            u64 seg_end       = vaddr + memsz; /* one past last byte */
            u64 pg_page_end   = pg + PAGE_SIZE;
            u64 data_start    = (vaddr > pg) ? vaddr : pg;
            u64 data_end      = (seg_end < pg_page_end) ? seg_end : pg_page_end;
            u64 pg_data_start = data_start - pg;
            u64 pg_data_end   = (data_end > data_start) ? (data_end - pg) : 0;

            u64 in_page_filesz = 0;
            if (copy_offset < filesz) {
                u64 remaining_file = filesz - copy_offset;
                u64 remaining_mem  = pg_data_end - pg_data_start;
                in_page_filesz     = remaining_file;
                if (in_page_filesz > remaining_mem)
                    in_page_filesz = remaining_mem;
            }

            if (in_page_filesz > 0) {
                memcpy(dest + pg_data_start, data + offset + copy_offset, in_page_filesz);
                copy_offset += in_page_filesz;
            }

            /* Zero the rest of this page region (BSS) */
            u64 zero_start = pg_data_start + in_page_filesz;
            if (zero_start < pg_data_end)
                memset(dest + zero_start, 0, pg_data_end - zero_start);
        }
    }

    *entry_out = entry;
    SerialPrintf("  ELF: init loaded successfully, entry=0x%x\n", (u32)entry);
    return OK;
}
