/*
 * virtio_blk.c - Legacy virtio-blk disk driver (Phase 1)
 * Copyright (c) 2026 OpSys Project
 *
 * Exposes three block-device syscalls backed by a legacy (pre-1.0)
 * virtio-blk PCI adapter (vendor 0x1AF4, device 0x1001):
 *
 *   SYS_BLK_READ  (62) — sc_sys_blk_read  (disk, lba, count, buf)
 *   SYS_BLK_WRITE (63) — sc_sys_blk_write (disk, lba, count, buf)
 *   SYS_BLK_INFO  (64) — sc_sys_blk_info  (disk, out blk_info_t)
 *
 * Every handler is gated on the calling process holding a
 * CAP_TYPE_PCI_DEV capability whose obj_id equals the disk argument
 * (the PCI table index of the adapter) with both RIGHT_READ and
 * RIGHT_WRITE — a direct mirror of the CAP_TYPE_IO_PORT gate in
 * syscall.c (proc_has_io_port_cap), including the ERR_NOCAP return.
 *
 * Transport: legacy virtio-pci I/O-bar registers (HOST_FEATURES /
 * GUEST_FEATURES / QUEUE_* / STATUS / ISR / DEVICE_CONFIG relative to
 * io_base), one multi-page virtqueue in the direct map (pages scale
 * with the device-dictated queue size: 2 for 128 slots, 3 for 256),
 * and a single 3-descriptor request per I/O with a per-op 4 KB DMA
 * bounce buffer (the kernel knows the physical address; there is no
 * kernel heap).  The device is initialized LAZILY on the first
 * SYS_BLK_* call (it stays untouched — and the boot sequence
 * unchanged — until then).
 *
 * I/O model: polled completion on used.idx (no IRQ binding), the
 * whole DMA window runs under spin_lock() — the kernel's irq
 * save/restore primitive (cli + saved-IF restore), which masks the
 * PIT tick; the missed tick fires late after the unlock.  A busy flag
 * serializes concurrent SYS_BLK_* calls (ERR_BUSY).
 */

#include <kernel/blk.h>
#include <kernel/cap.h>
#include <kernel/io.h>
#include <kernel/pci.h>
#include <kernel/pmm.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/serial.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/syscall_handlers.h>
#include <kernel/types.h>
#include <kernel/vmm.h>

/* ---- Legacy virtio-pci I/O registers (relative to io_base) ---- */
#define VIRTIO_PCI_HOST_FEATURES  0x00 /* r   32b */
#define VIRTIO_PCI_GUEST_FEATURES 0x04 /* w   32b */
#define VIRTIO_PCI_QUEUE_PFN      0x08 /* w   32b */
#define VIRTIO_PCI_QUEUE_NUM      0x0C /* r   16b */
#define VIRTIO_PCI_QUEUE_SEL      0x0E /* w   16b */
#define VIRTIO_PCI_QUEUE_NOTIFY   0x10 /* w   16b */
#define VIRTIO_PCI_STATUS         0x12 /* r/w  8b */
#define VIRTIO_PCI_ISR            0x13 /* r    8b (unused: poll only) */
#define VIRTIO_PCI_DEVICE_CONFIG  0x14 /* r/w  first device config bytes */

/* ---- virtio device IDs ---- */
#define VIRTIO_VENDOR_ID  0x1AF4
#define VIRTIO_DEV_ID_BLK 0x1001

/* ---- Virtio status bits (legacy virtio_pci_common_cfg->device_status) ---- */
#define VIRTIO_STATUS_ACKNOWLEDGE 1
#define VIRTIO_STATUS_DRIVER      2
#define VIRTIO_STATUS_DRIVER_OK   4
#define VIRTIO_STATUS_FAILED      128

/* ---- virtio-blk request types / status ---- */
#define VIRTIO_BLK_T_IN        0 /* read  */
#define VIRTIO_BLK_T_OUT       1 /* write */
#define VIRTIO_BLK_S_OK        0
#define VIRTIO_BLK_SECTOR_SIZE 512

/* ---- Virtqueue descriptor flags ---- */
#define VIRTQ_DESC_F_NEXT  1
#define VIRTQ_DESC_F_WRITE 2

/* ---- PCI config-space ports (pci.c owns the cached enumeration; we
 * only re-read BAR0 of the specific device we found, so this file
 * never duplicates the scan logic) ---- */
#define PCI_CONFIG_ADDR    0xCF8
#define PCI_CONFIG_DATA    0xCFC
#define PCI_CONFIG_ENABLE  0x80000000u
#define PCI_INVALID_VENDOR 0xFFFF

/* ---- Bounce-buffer geometry: [16B header][data <= 4032B][1B status] ---- */
#define BLK_HEADER_SIZE 16 /* {u32 type; u32 reserved; u64 sector} */
#define BLK_STATUS_SIZE 1
#define BLK_MAX_DATA    4032                                    /* 16 + 4032 + 1 = 4049 <= 4096 */
#define BLK_MAX_SECTORS (BLK_MAX_DATA / VIRTIO_BLK_SECTOR_SIZE) /* 7 */

/* ---- Virtqueue geometry ---- */
/* Device-dictated queue size, validated 2..VQ_MAX_NUM.  QEMU 10.2.2
 * virtio-blk reports 256 (its default queue size) — 128 was the old
 * spec minimum and must NOT be assumed. */
#define VQ_MAX_NUM    256
#define VQ_POLL_LIMIT 10000000ULL

/* Descriptor table entries (16 bytes each, at offset 0) */
typedef struct {
    u64 addr;
    u32 len;
    u16 flags;
    u16 next;
} vq_desc_t;

/* Available ring: {u16 flags; u16 idx; u16 ring[num]} */
typedef struct {
    u16 flags;
    u16 idx;
    u16 ring[];
} vq_avail_t;

/* Used ring element: {u32 id; u32 len} */
typedef struct {
    u32 id;
    u32 len;
} vq_used_elem_t;

/* Used ring: {u16 flags; u16 idx; vq_used_elem_t ring[num]} */
typedef struct {
    u16            flags;
    u16            idx;
    vq_used_elem_t ring[];
} vq_used_t;

/* Ring offsets inside the virtqueue allocation, mirroring QEMU's
 * vring_init / virtio_queue_update_rings layout exactly:
 *   avail = desc + num*16                (no alignment)
 *   used  = ALIGN_UP(avail + 4 + 2*num, 4096)
 * num=128: desc 0x0000-0x07FF, avail 0x0800-0x08FF,
 *          used ALIGN_UP(0x0904, 4096) = 0x1000-0x1406 -> 2 pages.
 * num=256: desc 0x0000-0x0FFF, avail 0x1000-0x1205,
 *          used ALIGN_UP(0x1206, 4096) = 0x2000-0x2806 -> 3 pages. */
#define VQ_DESC_OFF(num)    (0u)
#define VQ_AVAIL_OFF(num)   ((num) * 16u)
#define VQ_USED_OFF(num)    (((VQ_AVAIL_OFF(num) + 4u + 2u * (num) + 4095u) & ~4095u))
#define VQ_TOTAL_BYTES(num) (VQ_USED_OFF(num) + 6u + 8u * (num))
#define VQ_PAGES(num)       ((VQ_TOTAL_BYTES(num) + 4095u) / 4096u)

/* ---- Driver state (single legacy device, one shared queue) ---- */
static bool s_initialized  = false; /* lazy init done and device DRIVER_OK */
static bool s_busy         = false; /* a DMA op owns the queue right now   */
static u16  s_io_base      = 0;     /* legacy I/O base from BAR0           */
static u16  s_queue_num    = 0;     /* device-dictated queue size          */
static u64  s_vq_phys      = 0;     /* phys addr of the vq (VQ_PAGES pages) */
static u16  s_last_used    = 0;     /* last seen used.idx (per-queue)      */
static u64  s_capacity     = 0;     /* sectors, from DEVICE_CONFIG         */
static int  s_device_index = -1;    /* PCI table index (the "disk" arg)    */

/* irq save/restore (cli + saved-IF restore) around the DMA window */
static spinlock_t s_vq_lock = SPINLOCK_INIT;

/* ---- Helpers ---- */

/* Validate a user pointer range before the kernel touches it.  Mirrors
 * the private helper in syscall.c / pci.c: non-zero, overflow-free,
 * below USER_PTR_MAX, and mapped (writable iff need_write) in the
 * calling process's address space — the kernel then dereferences it
 * directly under the caller's CR3 (the repo-wide convention). */
static bool blk_validate_user_ptr(u64 ptr, u64 size, bool need_write) {
    return vmm_validate_user_ptr(ptr, size, need_write);
}

/* Convenience: cast a validated user pointer */
#define USER_PTR(p) ((void *)(uintptr_t)(p))

/*
 * Read one 32-bit dword from PCI config space.  Used only to decode
 * BAR0 of the adapter found by the (already lazily triggered) pci.c
 * enumeration; the scan order below mirrors pci.c's exactly so the
 * returned index is the same table index pci_find() returns.
 */
static u32 blk_pci_config_read(u32 bus, u32 dev, u32 func, u32 reg) {
    u32 addr = PCI_CONFIG_ENABLE | ((bus & 0xFF) << 16) | ((dev & 0x1F) << 11) |
               ((func & 0x07) << 8) | (reg & 0xFC);
    io_outl(PCI_CONFIG_ADDR, addr);
    return io_inl(PCI_CONFIG_DATA);
}

/*
 * Scan bus 0 (mirroring pci.c: function 0 first, functions 1..7 only
 * when function 0 is present) for the first virtio-blk adapter.
 * Returns its enumeration index and decodes the legacy I/O base from
 * BAR0 (bit0 must be set = I/O BAR; io_base = bar0 & ~0x3).
 *   >= 0        found — *out_io_base valid
 *   -1          no matching device
 *   -2          found, but BAR0 is not an I/O BAR
 */
static int blk_scan_device(u16 *out_io_base) {
    u32 bus, dev, func;
    int index = 0;

    for (bus = 0; bus < 1; bus++) {
        for (dev = 0; dev < 32; dev++) {
            u32 id0 = blk_pci_config_read(bus, dev, 0, 0x00);
            if ((u16)(id0 & 0xFFFF) == PCI_INVALID_VENDOR)
                continue;

            for (func = 0; func < 8; func++) {
                if (func > 0) {
                    u32 fid = blk_pci_config_read(bus, dev, func, 0x00);
                    if ((u16)(fid & 0xFFFF) == PCI_INVALID_VENDOR)
                        continue;
                }

                u32 id = blk_pci_config_read(bus, dev, func, 0x00);
                if ((u16)(id & 0xFFFF) == VIRTIO_VENDOR_ID &&
                    (u16)(id >> 16) == VIRTIO_DEV_ID_BLK) {
                    u32 bar0 = blk_pci_config_read(bus, dev, func, 0x10);
                    if ((bar0 & 1u) == 0)
                        return -2;
                    *out_io_base = (u16)(bar0 & ~0x3u);
                    return index;
                }
                index++;
            }
        }
    }
    return -1;
}

/* Reset the device: STATUS = 0, then wait one I/O cycle so the device
 * observes the reset before the next status write (virtio requires at
 * least one device read between status writes on real hardware). */
static void blk_virtio_reset(u16 io_base) {
    io_outb(io_base + VIRTIO_PCI_STATUS, 0);
    io_delay();
}

/*
 * Reset + negotiate the legacy adapter and program queue 0.
 * Order (virtio 0.9.5 + QEMU virtio_pci_io_write semantics):
 *   STATUS=0 -> |=ACKNOWLEDGE -> |=DRIVER -> read HOST_FEATURES
 *   (ignored) -> write GUEST_FEATURES=0 (QEMU masks; 0 is always
 *   safe) -> QUEUE_SEL=0 -> read QUEUE_NUM (device-dictated, validate
 *   2..VQ_MAX_NUM) -> QUEUE_PFN=vq_phys>>12 (NEVER 0 — that resets the
 *   device) -> |=DRIVER_OK.  DEVICE_CONFIG is only readable after
 *   DRIVER_OK; capacity (u64 sectors) is its first 8 bytes.
 */
static error_t blk_negotiate(u16 io_base, u64 vq_phys, u16 *out_num, u64 *out_capacity) {
    if (vq_phys == 0 || (vq_phys & (PAGE_SIZE - 1)) != 0)
        return ERR_INVAL;

    blk_virtio_reset(io_base);

    io_outb(io_base + VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    io_delay();
    io_outb(io_base + VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
    io_delay();

    (void)io_inl(io_base + VIRTIO_PCI_HOST_FEATURES); /* ignore */
    io_outl(io_base + VIRTIO_PCI_GUEST_FEATURES, 0);  /* QEMU masks */

    io_outw(io_base + VIRTIO_PCI_QUEUE_SEL, 0);
    u16 num = io_inw(io_base + VIRTIO_PCI_QUEUE_NUM);
    if (num < 2 || num > VQ_MAX_NUM)
        return ERR_INVAL;

    io_outl(io_base + VIRTIO_PCI_QUEUE_PFN, (u32)(vq_phys >> 12));

    io_outb(io_base + VIRTIO_PCI_STATUS,
            VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);
    io_delay();

    u32 lo        = io_inl(io_base + VIRTIO_PCI_DEVICE_CONFIG);
    u32 hi        = io_inl(io_base + VIRTIO_PCI_DEVICE_CONFIG + 4);
    *out_capacity = ((u64)hi << 32) | lo;
    *out_num      = num;
    return OK;
}

/*
 * Lazy one-time init: find the adapter, allocate the 2-page
 * virtqueue, negotiate, cache capacity.  Runs on the first SYS_BLK_*
 * call; on failure the device is left reset and a later call retries
 * (reusing s_vq_phys if it was already allocated — no leak).
 */
static error_t blk_lazy_init(void) {
    if (s_initialized)
        return OK;

    /* pci_find() triggers pci.c's lazy bus scan and gives the
     * authoritative table index (what capability obj_ids name). */
    int idx = pci_find(VIRTIO_VENDOR_ID, VIRTIO_DEV_ID_BLK);
    if (idx < 0)
        return ERR_NOENT;

    u16 io_base = 0;
    if (blk_scan_device(&io_base) != idx)
        return ERR_NOENT; /* config-space view disagrees with pci.c */

    if (s_vq_phys == 0) {
        /* Allocate for the largest accepted queue size so the ring
         * layout never depends on the QUEUE_NUM read timing (the size
         * is device-dictated; 256 slots need 3 contiguous pages). */
        s_vq_phys = pmm_alloc_pages(VQ_PAGES(VQ_MAX_NUM));
        if (s_vq_phys == 0)
            return ERR_NOMEM;
    }
    memset((void *)(s_vq_phys + KERNEL_VIRT_BASE), 0, VQ_PAGES(VQ_MAX_NUM) * PAGE_SIZE);

    u16     num      = 0;
    u64     capacity = 0;
    error_t err      = blk_negotiate(io_base, s_vq_phys, &num, &capacity);
    if (err != OK) {
        blk_virtio_reset(io_base); /* leave the device clean */
        return err;
    }

    s_io_base      = io_base;
    s_queue_num    = num;
    s_capacity     = capacity;
    s_device_index = idx;
    s_last_used    = 0;
    s_initialized  = true;

    serial_printf("blk: virtio-blk pci[%d] io=0x%x vq=%u slots=%u sectors=%u\n",
                  idx,
                  (unsigned)io_base,
                  (unsigned)num,
                  (unsigned)num,
                  (u64)capacity);
    return OK;
}

/* Ensure the driver is up AND the disk argument names this adapter. */
static error_t blk_ensure_disk(u64 disk) {
    error_t err = blk_lazy_init();
    if (err != OK)
        return err;
    if (disk != (u64)s_device_index)
        return ERR_INVAL;
    return OK;
}

/*
 * One DMA request: nsectors (1..7) at `sector` on queue 0.
 *
 * Bounce layout: [0..15] header {u32 type; u32 reserved; u64 sector},
 * [16..16+data_len) data, [16+data_len] status byte (device-written).
 * Descriptor chain: desc0 -> header (NEXT), desc1 -> data (read:
 * NEXT|WRITE, write: NEXT), desc2 -> status (WRITE).  avail.ring
 * [avail.idx % num] = 0 (head), avail.idx++, outw(QUEUE_NOTIFY, 0),
 * then poll used.idx != s_last_used (bounded spin — QEMU completes in
 * microseconds).  Timeout resets the device and forces a re-init.
 *
 * The whole queue window runs under s_vq_lock (irq save/restore): a
 * PIT tick landing mid-DMA would preempt into another thread sharing
 * the queue.  The bounce page is per-op so copies happen outside.
 */
static error_t blk_io_one(u64 disk, bool is_write, u64 sector, u32 nsectors, void *user_buf) {
    if (disk != (u64)s_device_index)
        return ERR_INVAL;

    if (s_busy)
        return ERR_BUSY;
    s_busy = true;

    u64 bounce_phys = pmm_alloc_page();
    if (bounce_phys == 0) {
        s_busy = false;
        return ERR_NOMEM;
    }
    u8 *bounce   = (u8 *)(bounce_phys + KERNEL_VIRT_BASE);
    u32 data_len = nsectors * VIRTIO_BLK_SECTOR_SIZE;

    /* Header {u32 type; u32 reserved; u64 sector} */
    *(u32 *)(bounce + 0) = is_write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    *(u32 *)(bounce + 4) = 0;
    *(u64 *)(bounce + 8) = sector;

    if (is_write)
        memcpy(bounce + BLK_HEADER_SIZE, user_buf, data_len);

    vq_desc_t  *desc  = (vq_desc_t *)(s_vq_phys + KERNEL_VIRT_BASE);
    vq_avail_t *avail = (vq_avail_t *)((u8 *)desc + VQ_AVAIL_OFF(s_queue_num));
    vq_used_t  *used  = (vq_used_t *)((u8 *)desc + VQ_USED_OFF(s_queue_num));

    spin_lock(&s_vq_lock); /* cli + saved-IF restore (PIT masked) */

    desc[0].addr  = bounce_phys;
    desc[0].len   = BLK_HEADER_SIZE;
    desc[0].flags = VIRTQ_DESC_F_NEXT;
    desc[0].next  = 1;

    desc[1].addr  = bounce_phys + BLK_HEADER_SIZE;
    desc[1].len   = data_len;
    desc[1].flags = is_write ? VIRTQ_DESC_F_NEXT : (VIRTQ_DESC_F_NEXT | VIRTQ_DESC_F_WRITE);
    desc[1].next  = 2;

    desc[2].addr  = bounce_phys + BLK_HEADER_SIZE + data_len;
    desc[2].len   = BLK_STATUS_SIZE;
    desc[2].flags = VIRTQ_DESC_F_WRITE;
    desc[2].next  = 0;

    __asm__ volatile("" ::: "memory"); /* descs visible before ring */

    u16 avail_idx                        = avail->idx;
    avail->ring[avail_idx % s_queue_num] = 0; /* head descriptor */
    __asm__ volatile("" ::: "memory");        /* ring visible before idx */
    avail->idx = avail_idx + 1;
    __asm__ volatile("" ::: "memory"); /* idx visible before notify */

    io_outw(s_io_base + VIRTIO_PCI_QUEUE_NOTIFY, 0);

    /* Bounded poll: QEMU completes a 7-sector request in microseconds.
     * used->idx is written by the DEVICE (DMA), so it must be re-read
     * every iteration — the compiler barrier defeats hoisting of the
     * plain (non-volatile) memory load out of the loop. */
    u64 spins = 0;
    while (used->idx == s_last_used) {
        __asm__ volatile("" ::: "memory");
        if (++spins >= VQ_POLL_LIMIT) {
            spin_unlock(&s_vq_lock);
            blk_virtio_reset(s_io_base);
            s_initialized = false; /* re-negotiate on the next call */
            serial_puts("blk: I/O timeout, device reset\n");
            pmm_free_page(bounce_phys);
            s_busy = false;
            return ERR_AGAIN;
        }
    }

    u16 used_idx = used->idx;
    s_last_used  = used_idx;

    /* Status byte lives at the end of the bounce buffer (3rd
     * descriptor's buffer); the device writes it before advancing
     * used.idx.  Verify it is VIRTIO_BLK_S_OK (0). */
    u8   status = bounce[BLK_HEADER_SIZE + data_len];
    bool ok     = (used->ring[(used_idx - 1) % s_queue_num].id == 0) && (status == VIRTIO_BLK_S_OK);

    spin_unlock(&s_vq_lock);

    if (!ok) {
        serial_printf("blk: device status=%u\n", (unsigned)status);
        pmm_free_page(bounce_phys);
        s_busy = false;
        return ERR_FAULT;
    }

    if (!is_write)
        memcpy(user_buf, bounce + BLK_HEADER_SIZE, data_len);

    pmm_free_page(bounce_phys);
    s_busy = false;
    return OK;
}

/* Split a multi-sector request into <= BLK_MAX_SECTORS (7) chunks. */
static error_t blk_io_loop(u64 disk, bool is_write, u64 lba, u64 count, void *user_buf) {
    u64 remaining = count;
    u64 cur       = lba;
    u8 *p         = (u8 *)user_buf;

    while (remaining > 0) {
        u32     n   = (remaining > BLK_MAX_SECTORS) ? BLK_MAX_SECTORS : (u32)remaining;
        error_t err = blk_io_one(disk, is_write, cur, n, p);
        if (err != OK)
            return err;
        remaining -= n;
        cur += n;
        p += (u64)n * VIRTIO_BLK_SECTOR_SIZE;
    }
    return OK;
}

/*
 * Gate: mirror proc_has_io_port_cap (syscall.c) — the calling process
 * must hold a CAP_TYPE_PCI_DEV capability naming PCI table index
 * `obj_id` with both RIGHT_READ and RIGHT_WRITE.  Returns false when
 * no cap matches; handlers then return ERR_NOCAP (the exact error the
 * I/O-port gate returns).
 */
static bool proc_has_pci_dev_cap(u64 obj_id) {
    process_t *proc = process_current();
    if (!proc || !proc->cap_table)
        return false;

    for (u32 i = 0; i < MAX_CAPS; i++) {
        cap_entry_t *e = &proc->cap_table->entries[i];
        /* Lazy expiry, same rule as cap_lookup (see the io-port gate in
         * syscall.c): an expired PCI_DEV cap must not grant access. */
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

/* ====================================================================
 * Syscall handlers (uniform 5-arg form — see syscall_handlers.h)
 * ==================================================================== */

/*
 * SYS_BLK_READ: read `count` sectors starting at `lba` into the user
 * buffer at `buf` (count*512 bytes).  count must be > 0 and the range
 * must stay within the device capacity.  Internal loop chunks at
 * <= 7 sectors per DMA op.
 *   ERR_NOCAP — caller holds no CAP_TYPE_PCI_DEV cap for this disk
 *   ERR_INVAL — bad disk arg / count == 0 / range beyond capacity
 *   ERR_FAULT — buffer not writable user memory / device error
 *   ERR_NOMEM — no bounce page
 *   ERR_AGAIN — DMA timed out (device was reset)
 *   ERR_BUSY  — another op is in flight
 */
i64 sc_sys_blk_read(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    (void)a5;
    u64 disk = a1, lba = a2, count = a3, buf = a4;

    if (!proc_has_pci_dev_cap(disk))
        return (i64)ERR_NOCAP;
    if (count == 0)
        return (i64)ERR_INVAL;

    error_t err = blk_ensure_disk(disk);
    if (err != OK)
        return (i64)err;
    if (lba > s_capacity || count > s_capacity - lba)
        return (i64)ERR_INVAL;

    if (!blk_validate_user_ptr(buf, count * VIRTIO_BLK_SECTOR_SIZE, true))
        return (i64)ERR_FAULT; /* kernel writes buf */

    err = blk_io_loop(disk, false, lba, count, USER_PTR(buf));
    return (i64)err;
}

/*
 * SYS_BLK_WRITE: write `count` sectors at `lba` from the user buffer
 * at `buf`.  Mirror of SYS_BLK_READ; buffer is kernel-read (need_write
 * false in the validation).
 */
i64 sc_sys_blk_write(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    (void)a5;
    u64 disk = a1, lba = a2, count = a3, buf = a4;

    if (!proc_has_pci_dev_cap(disk))
        return (i64)ERR_NOCAP;
    if (count == 0)
        return (i64)ERR_INVAL;

    error_t err = blk_ensure_disk(disk);
    if (err != OK)
        return (i64)err;
    if (lba > s_capacity || count > s_capacity - lba)
        return (i64)ERR_INVAL;

    if (!blk_validate_user_ptr(buf, count * VIRTIO_BLK_SECTOR_SIZE, false))
        return (i64)ERR_FAULT; /* kernel reads buf */

    err = blk_io_loop(disk, true, lba, count, USER_PTR(buf));
    return (i64)err;
}

/*
 * SYS_BLK_INFO: write blk_info_t {u64 sectors; u64 sector_size;} to
 * the validated output pointer.  No DMA; only requires the device to
 * be up (which the cap gate + lazy init guarantee).
 */
i64 sc_sys_blk_info(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    (void)a3;
    (void)a4;
    (void)a5;
    u64 disk = a1, out = a2;

    if (!proc_has_pci_dev_cap(disk))
        return (i64)ERR_NOCAP;

    error_t err = blk_ensure_disk(disk);
    if (err != OK)
        return (i64)err;

    if (!blk_validate_user_ptr(out, sizeof(blk_info_t), true))
        return (i64)ERR_FAULT; /* kernel writes out */

    blk_info_t info;
    info.sectors     = s_capacity;
    info.sector_size = VIRTIO_BLK_SECTOR_SIZE;
    memcpy(USER_PTR(out), &info, sizeof(blk_info_t));
    return 0;
}
