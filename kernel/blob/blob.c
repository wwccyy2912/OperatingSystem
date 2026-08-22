/*
 * blob.c - Embedded ELF blob registry
 * Copyright (c) 2026 OpSys Project
 *
 * Holds a name -> (data,size) table for every user ELF image linked
 * into the kernel as a binary blob.  The linker symbols are produced
 * by objcopy in the build system (build/<name>_blob.o, one per service):
 *   - <name>_elf_start : first byte of the blob
 *   - <name>_elf_end   : one past the last byte
 *   - <name>_elf_size  : symbol whose ADDRESS equals the byte length
 *
 * kernel_main fetches "init" here instead of using extern symbols
 * directly; SYS_BLOB_GET lets user-space (e.g. the shell) fetch any
 * registered image by name to spawn it via SYS_PROCESS_CREATE.
 */

#include <kernel/blob.h>
#include <kernel/string.h>
#include <kernel/panic.h>

/* Embedded blobs (defined by build/<name>_blob.o, one per service) */
extern char init_elf_start[], init_elf_end[], init_elf_size[];
extern char hello_elf_start[], hello_elf_end[], hello_elf_size[];
extern char manager_elf_start[], manager_elf_end[], manager_elf_size[];
extern char serial_elf_start[], serial_elf_end[], serial_elf_size[];
extern char keyboard_elf_start[], keyboard_elf_end[], keyboard_elf_size[];
extern char term_elf_start[], term_elf_end[], term_elf_size[];
extern char shell_elf_start[], shell_elf_end[], shell_elf_size[];
extern char flaky_elf_start[], flaky_elf_end[], flaky_elf_size[];
extern char crashpeer_elf_start[], crashpeer_elf_end[], crashpeer_elf_size[];
extern char vfs_elf_start[], vfs_elf_end[], vfs_elf_size[];
extern char fs_mem_driver_elf_start[], fs_mem_driver_elf_end[], fs_mem_driver_elf_size[];
extern char fs_virtio_blk_driver_elf_start[], fs_virtio_blk_driver_elf_end[],
    fs_virtio_blk_driver_elf_size[];
extern char perm_elf_start[], perm_elf_end[], perm_elf_size[];
extern char device_mgr_elf_start[], device_mgr_elf_end[], device_mgr_elf_size[];
extern char pkg_elf_start[], pkg_elf_end[], pkg_elf_size[];
extern char sbox_demo_elf_start[], sbox_demo_elf_end[], sbox_demo_elf_size[];
extern char runtime_demo_elf_start[], runtime_demo_elf_end[], runtime_demo_elf_size[];
extern char tui_demo_elf_start[], tui_demo_elf_end[], tui_demo_elf_size[];
extern char window_demo_elf_start[], window_demo_elf_end[], window_demo_elf_size[];

static blob_entry_t s_blobs[BLOB_MAX_ENTRIES];
static int          s_blob_count = 0;

void blob_init(void) {
    /* Every registration MUST succeed: a missing blob means a service
     * can never be spawned (it would fail subtly at runtime).  Fail
     * the boot loudly instead of limping on. */
#define BLOB_REG(name, data, sz)                              \
    do {                                                      \
        if (blob_register(name, data, sz) != OK)              \
            panic("blob_init: register '%s' failed", name);   \
    } while (0)

    BLOB_REG("init", init_elf_start, (u64)init_elf_size);
    BLOB_REG("manager", manager_elf_start, (u64)manager_elf_size);
    BLOB_REG("serial", serial_elf_start, (u64)serial_elf_size);
    BLOB_REG("keyboard", keyboard_elf_start, (u64)keyboard_elf_size);
    BLOB_REG("term", term_elf_start, (u64)term_elf_size);
    BLOB_REG("shell", shell_elf_start, (u64)shell_elf_size);
    BLOB_REG("flaky", flaky_elf_start, (u64)flaky_elf_size);
    BLOB_REG("crashpeer", crashpeer_elf_start, (u64)crashpeer_elf_size);
    BLOB_REG("hello", hello_elf_start, (u64)hello_elf_size);
    BLOB_REG("vfs", vfs_elf_start, (u64)vfs_elf_size);
    BLOB_REG("fs_mem_driver", fs_mem_driver_elf_start, (u64)fs_mem_driver_elf_size);
    BLOB_REG(
        "fs_virtio_blk_driver", fs_virtio_blk_driver_elf_start, (u64)fs_virtio_blk_driver_elf_size);
    BLOB_REG("perm", perm_elf_start, (u64)perm_elf_size);
    BLOB_REG("device_mgr", device_mgr_elf_start, (u64)device_mgr_elf_size);
    BLOB_REG("pkg", pkg_elf_start, (u64)pkg_elf_size);
    BLOB_REG("sbox_demo", sbox_demo_elf_start, (u64)sbox_demo_elf_size);
    BLOB_REG("runtime_demo", runtime_demo_elf_start, (u64)runtime_demo_elf_size);
    BLOB_REG("tui_demo", tui_demo_elf_start, (u64)tui_demo_elf_size);
    BLOB_REG("window_demo", window_demo_elf_start, (u64)window_demo_elf_size);
    /* Alias: the same sandbox ELF installed WITHOUT permissions
     * (docs/ops_format.md §8.5: pkg install sbox_demo_noperm).  The
     * blob payload is identical — only the manifest atoms differ. */
    BLOB_REG("sbox_demo_noperm", sbox_demo_elf_start, (u64)sbox_demo_elf_size);

#undef BLOB_REG
}

int blob_register(const char *name, const void *data, u64 size) {
    size_t len;

    if (!name || !data || size == 0)
        return ERR_INVAL;

    len = strlen(name);
    if (len == 0 || len >= BLOB_NAME_MAX)
        return ERR_INVAL;

    if (s_blob_count >= BLOB_MAX_ENTRIES)
        return ERR_NOMEM;

    blob_entry_t *e = &s_blobs[s_blob_count];
    memcpy(e->name, name, len + 1);
    e->data = data;
    e->size = size;
    s_blob_count++;
    return OK;
}

int blob_get(const char *name, const void **data, u64 *size) {
    if (!name || !data || !size)
        return ERR_INVAL;

    for (int i = 0; i < s_blob_count; i++) {
        if (strlen(s_blobs[i].name) == strlen(name) &&
            memcmp(s_blobs[i].name, name, strlen(name) + 1) == 0) {
            *data = s_blobs[i].data;
            *size = s_blobs[i].size;
            return OK;
        }
    }
    return ERR_NOENT;
}
