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
