/*
 * proc_image.h - Descriptor-based process image ABI (kernel + libos)
 * Copyright (c) 2026 OpSys Project
 *
 * Roadmap P1: ELF parsing moves OUT of the kernel.  The caller (libos
 * elf_parse) parses the ELF and describes the image with these structs;
 * SYS_PROCESS_CREATE (arg1=name, arg2=desc_ptr, arg3=blob_ptr,
 * arg4=blob_size) builds the address space, copies segment bytes from
 * the caller's blob, zeroes BSS, and starts the process at entry.
 *
 * The kernel treats the blob as opaque bytes — it never parses file
 * formats.  stdint types keep this header dual-compatible.
 */

#ifndef KERNEL_PROC_IMAGE_H
#define KERNEL_PROC_IMAGE_H

#include <stdint.h>

typedef struct {
    uint64_t vaddr;      /* target virtual address in the new address space */
    uint64_t filesz;     /* bytes to copy from blob[src_offset..] (may be 0) */
    uint64_t memsz;      /* total bytes to map; memsz >= filesz, tail zeroed */
    uint64_t prot;       /* PROT_NONE | PROT_READ | PROT_WRITE | PROT_EXEC */
    uint64_t src_offset; /* offset of this segment's data within the blob */
} proc_seg_desc_t;

typedef struct {
    uint64_t entry;     /* entry point (RIP) for the new process */
    uint64_t seg_count; /* number of proc_seg_desc_t entries */
} proc_image_desc_t;

#endif /* KERNEL_PROC_IMAGE_H */
