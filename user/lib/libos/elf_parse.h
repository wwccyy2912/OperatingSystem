/*
 * elf_parse.h - User-space ELF64 parser (Roadmap P1)
 * Copyright (c) 2026 OpSys Project
 *
 * Parses an ELF64 executable in USER space and describes its PT_LOAD
 * segments as a proc_image_desc_t + proc_seg_desc_t[] for
 * SYS_PROCESS_CREATE.  The kernel no longer parses file formats — it
 * receives the descriptor (arg2), the opaque blob (arg3) and its size
 * (arg4) and treats the blob as raw bytes (see
 * kernel/syscall/process_desc.c).
 *
 * The process-image structs are the fixed kernel/user ABI
 * (kernel/proc_image.h, dual-compatible via stdint).  The kernel's
 * SYS_PROCESS_CREATE expects the descriptor to be immediately followed
 * in memory by its segment table — build them contiguously (see
 * process_create() in syscalls.c).
 */

#ifndef LIBOS_ELF_PARSE_H
#define LIBOS_ELF_PARSE_H

#include <stdint.h>
#include <kernel/proc_image.h>

/* Upper bound on PT_LOAD segments we accept (the service ELFs built by
 * scripts/user.ld have 4).  Must fit the caller's proc_seg_desc_t
 * buffer. */
#define ELF_MAX_LOAD_SEGS   8

/**
 * elf_parse - Parse an ELF64 blob into a process-image descriptor.
 * @elf:      Pointer to the ELF binary (user memory).
 * @size:     Size of the ELF binary in bytes.
 * @desc_out: Receives proc_image_desc_t { entry, seg_count }.
 * @segs:     Caller-provided array filled with one proc_seg_desc_t
 *            per PT_LOAD segment (vaddr/filesz/memsz/prot/src_offset).
 * @max_segs: Capacity of @segs (pass ELF_MAX_LOAD_SEGS).
 *
 * Validates magic, ELFCLASS64, little-endian encoding and that the
 * program header table lies within the blob.  For every PT_LOAD:
 *   - vaddr   = p_vaddr (absolute; the service ELFs are ET_EXEC linked
 *     at 0x400000 by scripts/user.ld, non-PIE)
 *   - filesz  = p_filesz
 *   - memsz   = p_memsz
 *   - src_offset = p_offset
 *   - prot    = PF_R→PROT_READ, PF_W→PROT_WRITE, PF_X→PROT_EXEC
 *     (PROT_* values mirror kernel/types.h via libos syscalls.h)
 *
 * @return 0 on success (desc_out->seg_count segments written), or a
 *         negative errno (ERR_INVAL for malformed/oversized ELF).
 */
int elf_parse(const void *elf, unsigned long size,
              proc_image_desc_t *desc_out, proc_seg_desc_t *segs,
              unsigned long max_segs);

#endif /* LIBOS_ELF_PARSE_H */
