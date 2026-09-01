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
 * elf_parse.c - User-space ELF64 parser (Roadmap P1)
 * Copyright (c) 2026 OpSys Project
 *
 * Parses an ELF64 executable — the build output of scripts/user.ld:
 * ET_EXEC, linked at 0x400000, non-PIE, so p_vaddr are absolute — and
 * describes its PT_LOAD segments as a proc_image_desc_t + segment
 * table for SYS_PROCESS_CREATE.  The kernel treats the blob as opaque
 * bytes and never parses file formats; the only ELF walker left in the
 * kernel is the bootstrap-only init loader (kernel/mm/elf_boot.c).
 *
 * The ELF64 structures are defined here privately: the kernel no
 * longer exposes a general elf.h to user space.
 *
 * ------------------------------------------------------------------
 * Structure (single entry point):
 *   ELF blob (ET_EXEC, ELF64, little-endian, x86-64)
 *     |
 *     v
 *   ElfParse(): header magic/class/machine checks
 *     |  bounds-check phdr table, iterate PT_LOAD only
 *     +--> proc_image_desc_t { entry, seg_count }
 *     +--> proc_seg_desc_t[]   { vaddr, filesz, memsz, src_offset, prot }
 *     v
 *   ProcessCreate()  (kernel treats blob as opaque bytes)
 *
 * How it works:
 *   The ELF64 structs are defined locally; ElfParse validates the
 *   blob, strides the program header table by sizeof(elf64_phdr_t),
 *   copies each PT_LOAD into a proc_seg_desc_t, and maps p_flags to
 *   the PROT_* values.
 *
 * Purpose:
 *   User-side ELF parser that turns a built executable into the
 *   process-image description consumed by SYS_PROCESS_CREATE, keeping
 *   the kernel free of any file-format logic.
 *
 * Caveats:
 *   Only ELF64 little-endian x86-64 ET_EXEC blobs (non-PIE, linked at
 *   0x400000).  Section headers and non-PT_LOAD segments are ignored;
 *   segments must fit the caller's max_segs array and every file range
 *   must lie inside the blob.
 * ------------------------------------------------------------------
 */

#include "elf_parse.h"
#include "syscalls.h"

/* ---- Minimal ELF64 layout (parsed here, not in the kernel) ---- */

#define EI_NIDENT 16

#define ELFMAG0 0x7F
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'

#define ELFCLASS64  2
#define ELFDATA2LSB 1 /* Little-endian */

#define EM_X86_64 0x3E /* AMD x86-64 */

#define PT_LOAD 1 /* Loadable segment */

#define PF_X 0x1 /* Execute */
#define PF_W 0x2 /* Write */
#define PF_R 0x4 /* Read */

typedef struct {
    unsigned char  e_ident[EI_NIDENT]; /* ELF identification */
    unsigned short e_type;             /* Object file type */
    unsigned short e_machine;          /* Architecture */
    unsigned int   e_version;          /* Object file version */
    unsigned long  e_entry;            /* Entry point virtual address */
    unsigned long  e_phoff;            /* Program header table offset */
    unsigned long  e_shoff;            /* Section header table offset */
    unsigned int   e_flags;            /* Processor-specific flags */
    unsigned short e_ehsize;           /* ELF header size */
    unsigned short e_phentsize;        /* Program header entry size */
    unsigned short e_phnum;            /* Number of program header entries */
    unsigned short e_shentsize;        /* Section header entry size */
    unsigned short e_shnum;            /* Number of section header entries */
    unsigned short e_shstrndx;         /* Section name string table index */
} elf64_ehdr_t;

typedef struct {
    unsigned int  p_type;   /* Segment type */
    unsigned int  p_flags;  /* Segment flags */
    unsigned long p_offset; /* Offset in file */
    unsigned long p_vaddr;  /* Virtual address */
    unsigned long p_paddr;  /* Physical address */
    unsigned long p_filesz; /* Size in file */
    unsigned long p_memsz;  /* Size in memory */
    unsigned long p_align;  /* Alignment */
} elf64_phdr_t;

int ElfParse(const void        *elf,
              unsigned long      size,
              proc_image_desc_t *desc_out,
              proc_seg_desc_t   *segs,
              unsigned long      max_segs) {
    const unsigned char *data = (const unsigned char *)elf;
    const elf64_ehdr_t  *ehdr = (const elf64_ehdr_t *)data;

    if (!elf || !desc_out || !segs || max_segs == 0)
        return ERR_INVAL;

    /* ---- Blob must at least hold the ELF header ---- */
    if (size < sizeof(elf64_ehdr_t)) {
        DebugLog("elf_parse: truncated header\n");
        return ERR_INVAL;
    }

    /* ---- Validate ELF magic ---- */
    if (ehdr->e_ident[0] != ELFMAG0 || ehdr->e_ident[1] != ELFMAG1 || ehdr->e_ident[2] != ELFMAG2 ||
        ehdr->e_ident[3] != ELFMAG3) {
        DebugLog("elf_parse: bad magic\n");
        return ERR_INVAL;
    }

    /* ---- Validate class (64-bit) and data encoding (little-endian) ---- */
    if (ehdr->e_ident[4] != ELFCLASS64 || ehdr->e_ident[5] != ELFDATA2LSB) {
        DebugLog("elf_parse: not ELFCLASS64/little-endian\n");
        return ERR_INVAL;
    }

    /* ---- Validate machine (x86-64) ---- */
    if (ehdr->e_machine != EM_X86_64) {
        DebugLog("elf_parse: not EM_X86_64\n");
        return ERR_INVAL;
    }

    /* ---- Bounds-check the program header table ---- */
    if (ehdr->e_phnum == 0) {
        DebugLog("elf_parse: no program headers\n");
        return ERR_INVAL;
    }
    if (ehdr->e_phentsize < sizeof(elf64_phdr_t)) {
        DebugLog("elf_parse: phentsize too small\n");
        return ERR_INVAL;
    }
    /* The parser strides phdrs by sizeof(elf64_phdr_t); the whole table
     * it will read must lie within the blob. */
    unsigned long phdr_bytes = (unsigned long)ehdr->e_phnum * (unsigned long)sizeof(elf64_phdr_t);
    if (ehdr->e_phoff > size || phdr_bytes > size - ehdr->e_phoff) {
        DebugLog("elf_parse: program header table out of bounds\n");
        return ERR_INVAL;
    }

    /* ---- Iterate program headers (PT_LOAD only) ---- */
    const elf64_phdr_t *phdr  = (const elf64_phdr_t *)(data + ehdr->e_phoff);
    unsigned long       nsegs = 0;

    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD)
            continue;

        if (nsegs >= max_segs) {
            DebugLog("elf_parse: too many PT_LOAD segments\n");
            return ERR_INVAL;
        }

        /* ---- Bounds-check the segment itself ---- */
        /* File data [p_offset, p_offset+p_filesz) must lie in the blob. */
        if (phdr[i].p_filesz > size || phdr[i].p_offset > size - phdr[i].p_filesz) {
            DebugLog("elf_parse: PT_LOAD file range out of bounds\n");
            return ERR_INVAL;
        }
        /* ELF spec: memsz >= filesz. */
        if (phdr[i].p_memsz < phdr[i].p_filesz) {
            DebugLog("elf_parse: PT_LOAD memsz < filesz\n");
            return ERR_INVAL;
        }

        /* ---- Describe the segment for SYS_PROCESS_CREATE ---- */
        proc_seg_desc_t *seg = &segs[nsegs];
        seg->vaddr           = phdr[i].p_vaddr;
        seg->filesz          = phdr[i].p_filesz;
        seg->memsz           = phdr[i].p_memsz;
        seg->src_offset      = phdr[i].p_offset;

        /* Map ELF p_flags to PROT_* (kernel/types.h values, mirrored in
         * syscalls.h).  PROT_NONE is the absence of all three. */
        seg->prot = 0;
        if (phdr[i].p_flags & PF_R)
            seg->prot |= PROT_READ;
        if (phdr[i].p_flags & PF_W)
            seg->prot |= PROT_WRITE;
        if (phdr[i].p_flags & PF_X)
            seg->prot |= PROT_EXEC;

        nsegs++;
    }

    if (nsegs == 0) {
        DebugLog("elf_parse: no PT_LOAD segments\n");
        return ERR_INVAL;
    }

    desc_out->entry     = ehdr->e_entry;
    desc_out->seg_count = nsegs;
    return 0;
}
