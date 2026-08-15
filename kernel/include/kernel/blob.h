/*
 * blob.h - Embedded ELF blob registry
 * Copyright (c) 2026 OpSys Project
 *
 * The kernel image embeds one or more user-space ELF files as binary
 * blobs (build/init_blob.o, build/hello_blob.o, ...).  Each blob is
 * registered by name so kernel code and (via SYS_BLOB_GET) user code
 * can fetch an ELF image without scattering raw linker symbols through
 * the tree.
 */

#ifndef KERNEL_BLOB_H
#define KERNEL_BLOB_H

#include <kernel/types.h>

#define BLOB_NAME_MAX       32   /* Max blob name length incl. NUL */
#define BLOB_MAX_ENTRIES    18   /* Max registered blobs (services + sbox_demo_noperm alias + headroom) */

typedef struct {
        char     name[BLOB_NAME_MAX];
        const void *data;            /* Kernel virtual address of blob */
        u64      size;               /* Blob length in bytes */
} blob_entry_t;

/**
 * blob_init - Register every blob linked into the kernel image.
 *
 * Called once during boot (kernel_main Stage 8b), before any blob is
 * fetched.  Looks up the objcopy-renamed symbols emitted by the build
 * (init_elf_*, hello_elf_*) and inserts them into the registry.
 */
void blob_init(void);

/**
 * blob_register - Add a named blob to the registry.
 * @name:  NUL-terminated name (<= BLOB_NAME_MAX-1 chars)
 * @data:  Kernel virtual address of the blob
 * @size:  Blob length in bytes
 *
 * Returns OK, ERR_NOMEM (table full) or ERR_INVAL (bad arguments).
 */
int blob_register(const char *name, const void *data, u64 size);

/**
 * blob_get - Look up a blob by name.
 * @name:  NUL-terminated name to find
 * @data:  Out-param, receives the blob's kernel virtual address
 * @size:  Out-param, receives the blob's length in bytes
 *
 * Returns OK and fills *data and *size on success, or ERR_NOENT.
 */
int blob_get(const char *name, const void **data, u64 *size);

#endif /* KERNEL_BLOB_H */
