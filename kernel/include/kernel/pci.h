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
 * pci.h - PCI enumeration ABI (kernel + user shared)
 * Copyright (c) 2026 OpSys Project
 *
 * Filled by SYS_PCI_GET_DEVICE.  Uses stdint types so the same header
 * compiles in kernel (freestanding) and user space (libos includes
 * kernel/include via the shared include path).
 *
 * PciFind()/PciDeviceCount() are kernel-internal helpers (not
 * syscalls) for kernel device drivers: they query the cached
 * enumeration snapshot and are declared here only because the header
 * is also on the kernel include path.  A plain prototype compiles fine
 * in user space even though userspace code never calls them.
 */

#ifndef KERNEL_PCI_H
#define KERNEL_PCI_H

#include <stdint.h>

#define PCI_MAX_BARS 6

typedef struct {
    uint32_t bus;
    uint32_t dev;
    uint32_t func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t class_code; /* (base_class << 8) | subclass_class */
    uint8_t  prog_if;
    uint8_t  revision_id;
    uint32_t bar[PCI_MAX_BARS]; /* 0 = absent (IO/legacy or unimplemented) */
    uint8_t  irq_line;
} pci_device_info_t;

/* Kernel-internal (not syscalls): query the cached enumeration snapshot.
 * PciFind() returns the table index of the first device matching
 * vendor/device, or -1; PciDeviceCount() returns the cached count.
 * Both trigger the lazy bus-0 scan on first use. */
int PciFind(uint16_t vendor_id, uint16_t device_id);
int PciDeviceCount(void);

#endif /* KERNEL_PCI_H */
