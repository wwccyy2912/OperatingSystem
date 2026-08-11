/*
 * elf_boot.h - ELF64 definitions for the BOOTSTRAP-ONLY init loader
 * Copyright (c) 2026 OpSys Project
 *
 * ⚠ BOOTSTRAP ONLY: this header serves kernel/mm/elf_boot.c, which
 * exists solely to load the embedded init blob during kernel boot.
 * Init is a trusted, kernel-embedded image; this path is UNREACHABLE
 * from user input — no syscall or IPC message can ever reach it.
 *
 * Roadmap P1: all general-purpose ELF parsing moved OUT of the kernel
 * into user-space libos (user/lib/libos/elf_parse.c).  User processes
 * are created via SYS_PROCESS_CREATE with pre-parsed descriptors
 * (kernel/proc_image.h).  The former general-purpose elf.h/elf.c were
 * deleted; this trimmed header replaces only the boot path's need.
 */

#ifndef KERNEL_ELF_BOOT_H
#define KERNEL_ELF_BOOT_H

#include <kernel/types.h>
#include <kernel/vmm.h>

/* ---- ELF Magic ---- */
#define EI_NIDENT       16

#define ELFMAG0         0x7F
#define ELFMAG1         'E'
#define ELFMAG2         'L'
#define ELFMAG3         'F'

/* ---- ELF class and data encoding ---- */
#define ELFCLASS64      2
#define ELFDATA2LSB     1       /* Little-endian */

/* ---- Program header types ---- */
#define PT_LOAD         1       /* Loadable segment */

/* ---- Program header flags ---- */
#define PF_X            0x1     /* Execute */
#define PF_W            0x2     /* Write */
#define PF_R            0x4     /* Read */

/* ---- ELF64 header ---- */
typedef struct {
    u8      e_ident[EI_NIDENT]; /* ELF identification */
    u16     e_type;             /* Object file type */
    u16     e_machine;          /* Architecture */
    u32     e_version;          /* Object file version */
    u64     e_entry;            /* Entry point virtual address */
    u64     e_phoff;            /* Program header table offset */
    u64     e_shoff;            /* Section header table offset */
    u32     e_flags;            /* Processor-specific flags */
    u16     e_ehsize;           /* ELF header size */
    u16     e_phentsize;        /* Program header entry size */
    u16     e_phnum;            /* Number of program header entries */
    u16     e_shentsize;        /* Section header entry size */
    u16     e_shnum;            /* Number of section header entries */
    u16     e_shstrndx;         /* Section name string table index */
} Elf64_Ehdr;

/* ---- ELF64 program header ---- */
typedef struct {
    u32     p_type;             /* Segment type */
    u32     p_flags;            /* Segment flags */
    u64     p_offset;           /* Offset in file */
    u64     p_vaddr;            /* Virtual address */
    u64     p_paddr;            /* Physical address */
    u64     p_filesz;           /* Size in file */
    u64     p_memsz;            /* Size in memory */
    u64     p_align;            /* Alignment */
} Elf64_Phdr;

/**
 * elf_boot_load - Load the embedded init ELF into an address space.
 * @as:        Target address space (init's).
 * @elf_data:  Pointer to the ELF binary in kernel memory.
 * @elf_size:  Size of the ELF binary in bytes (bounds-checks phdrs).
 * @entry_out: Receives the entry point virtual address.
 *
 * BOOTSTRAP ONLY — see the header comment.  Iterates PT_LOAD program
 * headers, allocates and maps pages for each segment, copies file data
 * and zeros BSS regions.  The init image is ET_EXEC linked at
 * 0x400000 (scripts/user.ld), so p_vaddr are absolute.
 *
 * @return OK on success, ERR_INVAL on malformed ELF, ERR_NOMEM on
 *         allocation failure.
 */
int elf_boot_load(addr_space_t *as, const void *elf_data, u64 elf_size,
                  u64 *entry_out);

#endif /* KERNEL_ELF_BOOT_H */
