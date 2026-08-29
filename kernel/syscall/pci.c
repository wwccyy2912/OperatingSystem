/*
 * pci.c - PCI config-space enumeration syscalls (SYS_PCI_GET_COUNT/GET_DEVICE)
 * Copyright (c) 2026 OpSys Project
 *
 * PCI is enumerated over the classic configuration-space I/O ports
 * (0xCF8 CONFIG_ADDRESS / 0xCFC CONFIG_DATA) on the first
 * SYS_PCI_GET_COUNT call and cached in a kernel-side table.  A user
 * process holding no I/O-port capability can still read the table via
 * SYS_PCI_GET_DEVICE — only the kernel ever touches the ports.
 *
 * Scan scope (v0.1): bus 0, device 0..31, function 0..7.  A function
 * slot whose vendor ID reads back 0xFFFF has no device behind it and
 * is skipped; function 1..7 are only probed when function 0 of the
 * same device is present (a device is almost always multifunction if
 * its function 0 exists).  Results are capped at PCI_MAX_DEVICES; any
 * overflow keeps the first 64 devices (the slot count is exposed so a
 * client can detect truncation).
 *
 * Handler ABI: both handlers take the uniform 5-arg form declared in
 * syscall_handlers.h so the dispatch table in syscall.c can treat
 * them like every other syscall (SYSCALLn() wrappers are only used
 * for handlers living inside syscall.c).
 *
 *   SYS_PCI_GET_COUNT  (46) → number of devices found
 *   SYS_PCI_GET_DEVICE (47) → arg1 = index, arg2 = user pci_device_info_t
 *                             (kernel writes it).  ERR_INVAL on a bad
 *                             index, ERR_FAULT on a bad pointer.
 *
 * QEMU's i440FX exposes at least the host bridge (device 0, function 0)
 * and a VGA controller (device 1), so a count >= 2 is expected; an
 * empty bus must still return 0 cleanly.
 */

#include <kernel/cap.h>
#include <kernel/io.h>
#include <kernel/pci.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/syscall_handlers.h>
#include <kernel/types.h>
#include <kernel/vmm.h>

/* Configuration-space port addresses */
#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

/* CONFIG_ADDRESS enable bit + bus/device/function/register fields */
#define PCI_CONFIG_ENABLE   0x80000000u
#define PCI_MAX_BUS         1  /* bus 0 only in v0.1 */
#define PCI_MAX_DEVICES     32 /* device 0..31 on each bus */
#define PCI_MAX_FUNCTIONS   8  /* function 0..7             */
#define PCI_CONFIG_MAX_DEVS 64 /* cached table cap          */

/* Vendor ID 0xFFFF = no device present at this slot */
#define PCI_INVALID_VENDOR 0xFFFF

/* ====================================================================
 * Cached enumeration state
 *
 * Enumerated lazily on the first SYS_PCI_GET_COUNT call: the scan
 * touches real I/O ports, so it runs exactly once per boot and the
 * snapshot is served to every subsequent caller.
 * ==================================================================== */

static pci_device_info_t s_devices[PCI_CONFIG_MAX_DEVS];
static u32               s_device_count = 0;
static bool              s_enumerated   = false;

/*
 * Validate a user pointer range before the kernel writes into it.
 * Mirrors the private helper in syscall.c: the range must be non-zero,
 * overflow-free, entirely below USER_PTR_MAX and mapped (and writable)
 * in the calling process's address space — otherwise the kernel's
 * copy-out would #PF inside a syscall.
 */
static bool pci_validate_user_ptr(u64 ptr, u64 size) {
    process_t *proc = process_current();
    if (!proc || !proc->addr_space)
        return false;
    if (ptr == 0 || ptr >= USER_PTR_MAX || size > USER_PTR_MAX - ptr)
        return false;
    return vmm_validate_user_range(proc->addr_space, ptr, size, true);
}

/* Convenience: cast a validated user pointer */
#define USER_PTR(p) ((void *)(uintptr_t)(p))

/*
 * Read one 32-bit dword from the config space of (bus, dev, func) at
 * register offset `reg` (must be dword-aligned; the address is masked
 * to 0xFC).  Writes the dword address to CONFIG_ADDRESS, then reads
 * the selected dword from CONFIG_DATA.
 */
static u32 pci_config_read(u32 bus, u32 dev, u32 func, u32 reg) {
    u32 addr = PCI_CONFIG_ENABLE | ((bus & 0xFF) << 16) | ((dev & 0x1F) << 11) |
               ((func & 0x07) << 8) | (reg & 0xFC);
    io_outl(PCI_CONFIG_ADDR, addr);
    return io_inl(PCI_CONFIG_DATA);
}

/* Fill one pci_device_info_t from the config-space register block. */
static void pci_read_device(u32 bus, u32 dev, u32 func, pci_device_info_t *out) {
    u32 id  = pci_config_read(bus, dev, func, 0x00); /* vendor | device */
    u32 rev = pci_config_read(bus, dev, func, 0x08); /* revision | prog_if
                                                      * | subclass | base */
    u32 cl = pci_config_read(bus, dev, func, 0x3C);  /* interrupt line  */
    u32 i;

    out->bus         = bus;
    out->dev         = dev;
    out->func        = func;
    out->vendor_id   = (u16)(id & 0xFFFF);
    out->device_id   = (u16)(id >> 16);
    out->revision_id = (u8)(rev & 0xFF);
    out->prog_if     = (u8)((rev >> 8) & 0xFF);
    /* class_code = (base_class << 8) | subclass_class */
    out->class_code = (u16)((((rev >> 24) & 0xFF) << 8) | ((rev >> 16) & 0xFF));

    /* Base address registers 0..5; 0 = absent (IO/legacy or unimplemented) */
    for (i = 0; i < PCI_MAX_BARS; i++)
        out->bar[i] = pci_config_read(bus, dev, func, 0x10 + i * 4);

    out->irq_line = (u8)(cl & 0xFF);
}

/*
 * Scan the PCI bus once and cache the device table.  Runs on the first
 * SYS_PCI_GET_COUNT call; every later call returns the same snapshot.
 * count == 0 (no devices found) is a valid, clean result.
 */
static void pci_scan(void) {
    u32 bus, dev, func;

    s_device_count = 0;
    s_enumerated   = true;

    for (bus = 0; bus < PCI_MAX_BUS; bus++) {
        for (dev = 0; dev < PCI_MAX_DEVICES; dev++) {
            /* Function 0 first: its vendor ID tells us whether the slot
             * is occupied at all.  Only probe functions 1..7 when
             * function 0 exists (multifunction device heuristic). */
            u32 id = pci_config_read(bus, dev, 0, 0x00);
            if ((u16)(id & 0xFFFF) == PCI_INVALID_VENDOR)
                continue;

            for (func = 0; func < PCI_MAX_FUNCTIONS; func++) {
                if (func > 0) {
                    u32 fid = pci_config_read(bus, dev, func, 0x00);
                    if ((u16)(fid & 0xFFFF) == PCI_INVALID_VENDOR)
                        continue;
                }
                if (s_device_count >= PCI_CONFIG_MAX_DEVS)
                    return; /* capped: keep the first PCI_CONFIG_MAX_DEVS */

                pci_read_device(bus, dev, func, &s_devices[s_device_count]);
                s_device_count++;
            }
        }
    }
}

/* ====================================================================
 * Syscall handlers (5-arg uniform form — see syscall_handlers.h)
 * ==================================================================== */

/*
 * SYS_PCI_GET_COUNT: lazily enumerate and return the number of PCI
 * devices found.  All five ABI arguments are unused (the table is
 * queried by index, not by argument).
 */
i64 sc_sys_pci_get_count(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;

    if (!s_enumerated)
        pci_scan();

    return (i64)s_device_count;
}

/*
 * SYS_PCI_GET_DEVICE: copy the cached descriptor for device `a1`
 * (0-based) into the user pci_device_info_t at `a2`.
 *   ERR_INVAL — index out of range
 *   ERR_FAULT — output pointer not writable user memory
 */
i64 sc_sys_pci_get_device(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    (void)a3;
    (void)a4;
    (void)a5;

    if (!s_enumerated)
        pci_scan();

    u32 index = (u32)a1;
    if (index >= s_device_count)
        return (i64)ERR_INVAL;

    if (!pci_validate_user_ptr(a2, sizeof(pci_device_info_t)))
        return (i64)ERR_FAULT;

    memcpy(USER_PTR(a2), &s_devices[index], sizeof(pci_device_info_t));
    return 0;
}

/*
 * Gate: the calling process must hold a CAP_TYPE_PCI_DEV capability
 * naming PCI table index `obj_id` with both RIGHT_READ and RIGHT_WRITE
 * (mirror of the io-port gate in syscall.c / virtio_blk.c).
 */
static bool proc_has_pci_dev_cap(u64 obj_id) {
    process_t *proc = process_current();
    if (!proc || !proc->cap_table)
        return false;

    for (u32 i = 0; i < MAX_CAPS; i++) {
        cap_entry_t *e = &proc->cap_table->entries[i];
        if (e->type == CAP_TYPE_NONE)
            continue;
        if (e->expiry_ticks != 0 && e->expiry_ticks <= sched_get_ticks())
            continue;
        if (e->type == CAP_TYPE_PCI_DEV && e->obj_id == obj_id &&
            (e->rights & (RIGHT_READ | RIGHT_WRITE)) == (RIGHT_READ | RIGHT_WRITE))
            return true;
    }
    return false;
}

/*
 * Management-plane gate: the caller must hold an ATOM_SERVICE_MANAGE
 * atom cap (the same seed used by SYS_SHM_CREATE / SYS_SET_TIME).
 * Raw PCI config access is a privileged operation — a self-minted
 * CAP_TYPE_PCI_DEV cap must not be enough to rewrite BARs or disable
 * bus mastering on arbitrary devices (the permission model routes
 * sensitive syscalls through atom gates; see docs/permission_model.md).
 */
static bool proc_has_service_manage(void) {
    process_t *proc = process_current();
    if (!proc || !proc->cap_table)
        return false;
    return cap_lookup_by_atom(proc->cap_table, proc->subject_id,
                              ATOM_SERVICE_MANAGE, 0) != CAP_NULL;
}

/*
 * SYS_PCI_CFG_READ: read a 32-bit dword from the config space of the
 * cached device at index `a1` at dword-aligned offset `a2`.
 *   ERR_NOCAP — caller holds no CAP_TYPE_PCI_DEV cap for this device,
 *               or lacks ATOM_SERVICE_MANAGE (management-plane gate:
 *               raw config access can rewrite BARs / disable bus
 *               mastering, so it must NOT be reachable via a
 *               self-minted cap)
 *   ERR_INVAL — index out of range / unaligned or oversized offset
 */
i64 sc_sys_pci_cfg_read(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    (void)a3;
    (void)a4;
    (void)a5;

    if (!proc_has_pci_dev_cap(a1))
        return (i64)ERR_NOCAP;
    if (!proc_has_service_manage())
        return (i64)ERR_NOCAP;
    if (a1 >= s_device_count)
        return (i64)ERR_INVAL;
    if ((a2 & 3) || a2 > 0xFC)
        return (i64)ERR_INVAL;

    pci_device_info_t *d = &s_devices[a1];
    return (i64)pci_config_read(d->bus, d->dev, d->func, (u32)a2);
}

/*
 * SYS_PCI_CFG_WRITE: write a 32-bit dword to the config space of the
 * cached device at index `a1` at dword-aligned offset `a2`.
 * Same capability + management-plane gate as the read variant.
 */
i64 sc_sys_pci_cfg_write(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    (void)a4;
    (void)a5;

    if (!proc_has_pci_dev_cap(a1))
        return (i64)ERR_NOCAP;
    if (!proc_has_service_manage())
        return (i64)ERR_NOCAP;
    if (a1 >= s_device_count)
        return (i64)ERR_INVAL;
    if ((a2 & 3) || a2 > 0xFC)
        return (i64)ERR_INVAL;

    pci_device_info_t *d = &s_devices[a1];
    u32 addr = PCI_CONFIG_ENABLE | ((d->bus & 0xFF) << 16) |
               ((d->dev & 0x1F) << 11) | ((d->func & 0x07) << 8) |
               ((u32)a2 & 0xFC);
    io_outl(PCI_CONFIG_ADDR, addr);
    io_outl(PCI_CONFIG_DATA, (u32)a3);
    return 0;
}

/* ====================================================================
 * Kernel-internal helpers (not syscalls — used by device drivers)
 *
 * A kernel driver that wants to find a specific PCI device (e.g. a
 * virtio-blk adapter) can query the cached enumeration snapshot with
 * pci_find()/pci_device_count() without touching config-space ports
 * itself.  Like the syscall handlers, both trigger the lazy bus-0 scan
 * on first use and then serve the frozen snapshot.
 * ==================================================================== */

/*
 * Return the number of devices currently cached in the enumeration
 * snapshot, scanning the bus lazily first if needed.  0 is a valid,
 * clean result (no PCI devices found).
 */
int pci_device_count(void) {
    if (!s_enumerated)
        pci_scan();

    return (int)s_device_count;
}

/*
 * Return the index of the first cached device whose vendor_id and
 * device_id both match, triggering the lazy scan if not yet done.
 * Returns -1 when no cached device matches.
 */
int pci_find(u16 vendor_id, u16 device_id) {
    u32 i;

    if (!s_enumerated)
        pci_scan();

    for (i = 0; i < s_device_count; i++) {
        if (s_devices[i].vendor_id == vendor_id && s_devices[i].device_id == device_id)
            return (int)i;
    }

    return -1;
}
