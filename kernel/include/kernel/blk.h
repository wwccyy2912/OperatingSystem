/*
 * blk.h - Block device ABI (kernel + user shared)
 * Copyright (c) 2026 OpSys Project
 *
 * Filled by SYS_BLK_INFO.  Uses stdint types so the same header
 * compiles in kernel (freestanding) and user space (libos includes
 * kernel/include via the shared include path) — same convention as
 * pci.h.  The Phase-1 disk driver behind this ABI is a legacy
 * virtio-blk PCI device (kernel/arch/x86_64/virtio_blk.c).
 */

#ifndef KERNEL_BLK_H
#define KERNEL_BLK_H

#include <stdint.h>

/* Capacity/geometry of a block device (SYS_BLK_INFO output).
 * sectors is the total number of 512-byte sectors; sector_size is the
 * size of one sector in bytes (512 for virtio-blk). */
typedef struct {
    uint64_t sectors;     /* total sectors (512-byte units) */
    uint64_t sector_size; /* bytes per sector (512) */
} blk_info_t;

#endif /* KERNEL_BLK_H */
