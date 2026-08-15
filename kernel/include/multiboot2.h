/*
 * multiboot2.h - Multiboot2 specification constants
 * Copyright (c) 2026 OpSys Project
 */

#ifndef MULTIBOOT2_H
#define MULTIBOOT2_H

#include <kernel/types.h>

/* Multiboot2 magic value */
#define MULTIBOOT2_MAGIC     0x36d76289

/* Multiboot2 header tags */
#define MULTIBOOT2_HEADER_END_TAG      0
#define MULTIBOOT2_HEADER_INFORMATION  1
#define MULTIBOOT2_HEADER_ADDRESS      2
#define MULTIBOOT2_HEADER_ENTRY        3
#define MULTIBOOT2_HEADER_CONSOLE      4
#define MULTIBOOT2_HEADER_FRAMEBUFFER  5
#define MULTIBOOT2_HEADER_EFI_BS       7
#define MULTIBOOT2_HEADER_EFI_EFI64    8
#define MULTIBOOT2_HEADER_RELOCATABLE  10

/* Multiboot2 memory map types */
#define MULTIBOOT2_MEMORY_AVAILABLE     1
#define MULTIBOOT2_MEMORY_RESERVED      2
#define MULTIBOOT2_MEMORY_ACPI_RECLAIM  3
#define MULTIBOOT2_MEMORY_ACPI_NVS     4
#define MULTIBOOT2_MEMORY_BAD          5

/* Tag structure layout */
typedef struct {
        u32 type;
        u32 size;
} mboot2_tag_t;

/* Memory map entry */
typedef struct {
        u64 base_addr;
        u64 length;
        u32 type;
        u32 reserved;
} mboot2_mmap_entry_t;

/* Memory map tag */
typedef struct {
        u32 entry_size;
        u32 entry_version;
        /* Followed by array of mboot2_mmap_entry_t */
} mboot2_mmap_tag_t;

/* ELF sections tag */
typedef struct {
        u32 num;
        u32 entsize;
        u32 shndx;
        /* Followed by ELF section header entries */
} mboot2_elf_tag_t;

/* Header structure (must be placed in .multiboot2 section) */
typedef struct {
        u32 magic;
        u32 architecture;  /* 0 = i386 (32-bit) */
        u32 header_length;
        u32 checksum;
        /* Tags follow */
} mboot2_header_t;

/**
 * Find a specific tag in the multiboot2 info structure.
 * @param mboot_addr  Physical address of multiboot2 info.
 * @param tag_type    Tag type to find.
 * @return Pointer to tag, or NULL.
 */
mboot2_tag_t *mboot2_find_tag(u64 mboot_addr, u32 tag_type);

/**
 * Get the memory map from multiboot2 info.
 * @param mboot_addr  Physical address of multiboot2 info.
 * @param count_out   Output: number of entries.
 * @return Pointer to first mmap entry, or NULL.
 */
mboot2_mmap_entry_t *mboot2_get_mmap(u64 mboot_addr, u32 *count_out);

#endif /* MULTIBOOT2_H */
