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
 * fs_virtio_blk_driver.c - Virtio-blk disk filesystem driver (ring-3)
 * Copyright (c) 2026 OpSys Project
 *
 * Third VFS storage driver (after fs_mem_driver), same A1 model: runs
 * as its own process spawned by the manager, performs the
 * driver-initiated VFS_OP_MOUNT handshake against the vfs_server and
 * registers ONE persistent read-write volume:
 *
 *   Disk  — RW.  Backed by the kernel virtio-blk adapter
 *           (kernel/arch/x86_64/virtio_blk.c, SYS_BLK_READ/WRITE/INFO)
 *           over QEMU disk.img (8 MiB, cache=writethrough) — the only
 *           persistence surface in the system.
 *
 * On-disk format (Phase 1, 512-byte sectors, 8 MiB = 16384 sectors):
 *
 *   sector 0      superblock  {magic 'VBDK', block_size, inode-table
 *                              geometry, volume UUID, root_inode=1}
 *   sectors 1-256 inode table — 256 inodes × 512 B (one per sector);
 *                              the FULL table is loaded into a 128 KB
 *                              RAM array at mount and every mutation
 *                              is written through synchronously (RMW
 *                              over the inode's own sector).
 *   sectors 257+  file data.  Each inode owns ONE contiguous extent
 *                              (extent_start block + extent_blocks);
 *                              first-fit allocation from a DERIVED
 *                              free bitmap (never persisted).
 *
 * Crash-consistency: the free bitmap is rebuilt from the inode table
 * on every mount, so a crash can never lose allocation state.  Every
 * inode-table mutation is a synchronous read-modify-write (writethrough
 * flushes it to the host file immediately); file data writes RMW the
 * touched sectors so a partial-sector write never clobbers neighbours.
 * There is no caching, no background flush thread.
 *
 * UUID: generated ONCE at format time and persisted in the superblock
 * (mirrors the fs_mem_driver uuid precedent — time-derived).  Phase-2
 * bookmarks key on it, so it must survive reboots.
 *
 * Driver port: "vfs.fs.virtio_blk"   (driver_name "virtio_blk")
 * Protocol:    drv_req_t/drv_resp_t (vfs.h) — compact unions, every
 *              exchange < 4096 bytes.
 *
 * Cap gate: every SYS_BLK_* requires a CAP_TYPE_PCI_DEV cap with
 * obj_id == the PCI table index of the 0x1AF4/0x1001 device and
 * RIGHT_READ|RIGHT_WRITE (cap_create_obj).  The disk index is found
 * dynamically by scanning the userspace PCI enumeration.
 *
 * ------------------------------------------------------------------
 * Structure (disk volume): one persistent RW "Disk" volume over the
 *   kernel virtio-blk adapter (SYS_BLK_READ/WRITE/INFO); on-disk
 *   layout = superblock (sector 0) + inode table (sectors 1-256,
 *   mirrored to a 128 KB RAM array) + data extents (sector 257+);
 *   driver port "vfs.fs.virtio_blk".
 * How it works:
 *   The manager-spawned process scans userspace PCI for the
 *   0x1AF4/0x1001 device, opens CAP_TYPE_PCI_DEV caps, runs
 *   VbdkFormat() when the superblock magic is absent, then VbdkMount()
 *   (MOUNT handshake, registers "Disk").  Every inode-table mutation
 *   is a synchronous RMW; the free bitmap is derived from the inode
 *   table on every mount.  DRV_OP_WRITE with len == 0 is the truncate
 *   form (VbdkTruncate): the offset field carries the target length,
 *   growing zero-fills, shrinking releases the tail blocks, and a length
 *   the volume cannot hold answers VFS_ERR_NOSPC.  The v0.9 maintenance
 *   plane adds SYNC (re-land
 *   superblock + inode table, any mount state, not cap-gated) and the
 *   read-only diagnostics CHECK / INFO / RAW_READ (ATOM_SERVICE_MANAGE
 *   gated, like CTRL_*).
 * Purpose:
 *   The only persistence surface in the system: an 8 MiB QEMU disk.img
 *   (cache=writethrough) backing the Disk volume.
 * Caveats:
 *   No caching and no background flush; RMW must never clobber the
 *   sibling inodes in a sector, and partial-sector data writes RMW
 *   the touched sectors.  The UUID is generated once at format time.
 * ------------------------------------------------------------------
 */

#include <stdint.h>
#include "../lib/libc/stdio.h"
#include "../lib/libc/string.h"
#include "../lib/libos/syscalls.h"
#include "vfs.h"

/* ====================================================================
 * Constants
 * ==================================================================== */

#define VBDK_MAGIC               0x4B444256u /* 'VBDK' (little-endian) */
#define VBDK_SECTOR_SIZE         512
#define VBDK_MAX_SECTORS         16384                                /* 8 MiB disk geometry  */
#define VBDK_MAX_INODES          256                                  /* inode table size     */
#define VBDK_INODE_SIZE          512                                  /* bytes per inode      */
#define VBDK_NAME_MAX            256                                  /* on-disk name bound
                                                                       * (255 usable + NUL),
                                                                       * matching the mem
                                                                       * volume so a CJK
                                                                       * name(up to 85
                                                                       * chars) behaves the
                                                                       * same on Disk        */
#define VBDK_INODES_PER_SECTOR   (VBDK_SECTOR_SIZE / VBDK_INODE_SIZE) /* 1 */
#define VBDK_INODE_TABLE_SECTORS (VBDK_MAX_INODES * VBDK_INODE_SIZE / VBDK_SECTOR_SIZE) /* 256 */
#define VBDK_INODE_TABLE_START   1 /* sector               */
#define VBDK_DATA_START          (VBDK_INODE_TABLE_START + VBDK_INODE_TABLE_SECTORS) /* 257 */
#define VBDK_MOUNT_WAIT          200 /* × 1 tick port_get retries */

#define VBDK_VIRTIO_VENDOR 0x1AF4
#define VBDK_VIRTIO_DEVICE 0x1001

/* UUID hi half — same "opsys-vf" magic the mem driver uses. */
#define VBDK_UUID_HI_MAGIC 0x6f707379732d7666ULL

/* ====================================================================
 * On-disk format
 * ==================================================================== */

/* Superblock — sector 0 (padded to exactly one sector). */
typedef struct {
    u32 magic;               /* VBDK_MAGIC */
    u32 block_size;          /* 512 */
    u64 inode_table_start;   /* sector 1 */
    u64 inode_table_sectors; /* 64 */
    u64 data_start;          /* sector 65 */
    u64 uuid_hi;             /* persisted volume UUID (hi half) */
    u64 uuid_lo;             /* persisted volume UUID (lo half) */
    u64 root_inode;          /* 1 */
    u8  pad[456];
} vbdk_sb_t;

/* Inode — 512 bytes, 1 per sector.  flags bit 0 = in_use (persisted,
 * so in-use survives reboot); type is a VFS_ITEM_* value (FILE=0, so
 * a free inode is flags==0, NOT type==0).  The name field matches the
 * mem volume's 255-byte limit (VBDK_NAME_MAX=256 incl. NUL). */
typedef struct {
    u32  flags;     /* bit 0 = in_use */
    u32  type;      /* VFS_ITEM_FILE / VFS_ITEM_DIR */
    u64  parent_id; /* 0 = volume root */
    u64  item_id;   /* = inode number (1-based) */
    u64  size;
    u32  extent_start;  /* first data block (0 = none) */
    u32  extent_blocks; /* contiguous blocks in extent */
    u64  created;       /* RTC ticks */
    u64  modified;
    char name[VBDK_NAME_MAX];
    u8   pad[200];
} vbdk_inode_t;

_Static_assert(sizeof(vbdk_sb_t) == VBDK_SECTOR_SIZE, "superblock not 512");
_Static_assert(sizeof(vbdk_inode_t) == VBDK_INODE_SIZE, "inode not 512");

/* ====================================================================
 * Volume state (RAM)
 * ==================================================================== */

#define VBDK_IN_FLAG 1u

typedef struct {
    vbdk_inode_t inodes[VBDK_MAX_INODES]; /* authoritative RAM table */
    u32          inode_count;             /* highest allocated inode number;
                                           * grows only — deleted numbers never
                                           * reused (mem driver precedent) */
    i32 disk;                             /* PCI table index, or -1 */
    u32 nsectors;                         /* device geometry (min 16384) */
    u64 total_bytes;                      /* usable data capacity in bytes */
    u64 used_bytes;                       /* sum of file sizes */
    u64 uuid_hi, uuid_lo;
    u32 read_only; /* 0 — Disk is RW */
    u32 mounted;   /* 1 after a successful MOUNT */
} vbdk_vol_t;

static vbdk_vol_t s_vol;

/* Derived free-block bitmap (2048 B = all 16384 sectors), rebuilt from
 * the inode table at mount/format; never written to disk. */
static u8 s_blkmap[VBDK_MAX_SECTORS / 8];

/* Request/response buffers (drv_req_t/drv_resp_t both < 4096) */
static u8 s_req[DRV_REQ_MAX];
static u8 s_resp[DRV_RESP_MAX];

/* ====================================================================
 * Block device I/O
 * ==================================================================== */

static i32 VbdkSectorRead(u64 lba, void *buf) {
    if (lba >= (u64)s_vol.nsectors)
        return ERR_INVAL;
    int64_t r = sys_blk_read((u64)s_vol.disk, lba, 1, buf);
    if (r < 0) {
        printf("fs_virtio_blk: read sector %u failed (%d)\n", (unsigned)lba, (int)r);
        return (i32)r;
    }
    return 0;
}

static i32 VbdkSectorWrite(u64 lba, const void *buf) {
    if (lba >= (u64)s_vol.nsectors)
        return ERR_INVAL;
    int64_t r = sys_blk_write((u64)s_vol.disk, lba, 1, buf);
    if (r < 0) {
        printf("fs_virtio_blk: write sector %u failed (%d)\n", (unsigned)lba, (int)r);
        return (i32)r;
    }
    return 0;
}

/* ====================================================================
 * Derived free-block bitmap
 * ==================================================================== */

static int BmTest(u32 blk) {
    return (s_blkmap[blk >> 3] >> (blk & 7)) & 1;
}

static void BmSet(u32 blk) {
    s_blkmap[blk >> 3] |= (u8)(1u << (blk & 7));
}

static void BmClear(u32 blk) {
    s_blkmap[blk >> 3] &= (u8) ~(1u << (blk & 7));
}

/* Rebuild the derived bitmap from the inode table: sectors 0..64
 * (superblock + inode table) are reserved; every in-use inode's extent
 * is marked; everything else is free.  Also recomputes used_bytes. */
static void VbdkRebuildBitmap(void) {
    memset(s_blkmap, 0, sizeof(s_blkmap));
    for (u32 b = 0; b < VBDK_DATA_START; b++)
        BmSet(b); /* superblock + inode table */

    s_vol.used_bytes = 0;
    for (u32 i = 1; i <= VBDK_MAX_INODES; i++) {
        vbdk_inode_t *it = &s_vol.inodes[i - 1];
        if (!(it->flags & VBDK_IN_FLAG))
            continue;
        s_vol.used_bytes += it->size;
        if (it->extent_blocks > 0)
            for (u32 j = 0; j < it->extent_blocks; j++)
                BmSet(it->extent_start + j);
    }
}

/* Release an extent back to the free map. */
static void VbdkExtentRelease(u32 start, u32 blocks) {
    for (u32 i = 0; i < blocks; i++)
        BmClear(start + i);
}

/* First-fit allocation of `blocks` contiguous data blocks.
 * Returns the start block, or -1 when the volume is full. */
static i32 VbdkExtentAlloc(u32 blocks) {
    if (blocks == 0)
        return 0;
    u32 run = 0;
    for (u32 blk = VBDK_DATA_START; blk < s_vol.nsectors; blk++) {
        if (BmTest(blk)) {
            run = 0;
            continue;
        }
        if (++run == blocks) {
            u32 start = blk - blocks + 1;
            for (u32 i = 0; i < blocks; i++)
                BmSet(start + i);
            return (i32)start;
        }
    }
    return -1;
}

/* ====================================================================
 * Inode table primitives (synchronous write-through, RMW)
 * ==================================================================== */

static vbdk_inode_t *vbdk_find(vfs_item_id_t id) {
    if (id == 0 || id > (vfs_item_id_t)s_vol.inode_count)
        return NULL;
    vbdk_inode_t *it = &s_vol.inodes[id - 1];
    return (it->flags & VBDK_IN_FLAG) ? it : NULL;
}

/* RMW store of one inode: read its sector, splice the 512-byte record
 * in, write the sector back — nothing else on that sector is touched
 * (VBDK_INODES_PER_SECTOR is 1 today, and the RMW stays correct if the
 * geometry ever packs several inodes into one sector). */
static i32 VbdkInodeStore(const vbdk_inode_t *in, u64 num) {
    static u8 sbuf[VBDK_SECTOR_SIZE];
    u64       sec = VBDK_INODE_TABLE_START + (num - 1) / VBDK_INODES_PER_SECTOR;
    size_t    off = (size_t)((num - 1) % VBDK_INODES_PER_SECTOR) * VBDK_INODE_SIZE;

    i32 r = VbdkSectorRead(sec, sbuf);
    if (r < 0)
        return r;
    memcpy(sbuf + off, in, VBDK_INODE_SIZE);
    return VbdkSectorWrite(sec, sbuf);
}

static i32 VbdkLookup(vfs_item_id_t parent, const char *name, vfs_item_id_t *out) {
    if (!name)
        return ERR_INVAL;
    size_t nlen = strlen(name);
    for (u32 i = 1; i <= s_vol.inode_count; i++) {
        vbdk_inode_t *it = &s_vol.inodes[i - 1];
        if (!(it->flags & VBDK_IN_FLAG) || it->parent_id != parent)
            continue;
        if (strlen(it->name) == nlen && memcmp(it->name, name, nlen) == 0) {
            *out = (vfs_item_id_t)i;
            return 0;
        }
    }
    return ERR_NOENT;
}

/* Internal create primitive — no read-only check (the Disk volume is
 * RW; the check lives in vbdk_create for protocol parity). */
static i32
VbdkCreateItem(vfs_item_id_t parent, const char *name, u32 type, vfs_item_id_t *out_id) {
    if (!name || !name[0])
        return ERR_INVAL;
    if (strlen(name) >= VBDK_NAME_MAX)
        return ERR_OVERFLOW;

    vbdk_inode_t *p = vbdk_find(parent);
    if (!p || p->type != VFS_ITEM_DIR)
        return ERR_NOENT;

    vfs_item_id_t dup = 0;
    if (VbdkLookup(parent, name, &dup) == 0)
        return VFS_ERR_EXISTS;

    if (s_vol.inode_count >= VBDK_MAX_INODES)
        return ERR_NOMEM;

    u32 idx = s_vol.inode_count++; /* inode number = idx + 1;
                                    * never reused after delete */
    vbdk_inode_t *it = &s_vol.inodes[idx];
    memset(it, 0, sizeof(*it));
    it->flags     = VBDK_IN_FLAG;
    it->type      = type;
    it->parent_id = parent;
    it->item_id   = (u64)(idx + 1);
    strncpy(it->name, name, sizeof(it->name) - 1);
    it->name[sizeof(it->name) - 1] = '\0';
    it->created                    = (u64)GetTime();
    it->modified                   = it->created;
    *out_id                        = (vfs_item_id_t)(idx + 1);

    if (VbdkInodeStore(it, (u64)(idx + 1)) < 0) {
        /* Rollback: inode_count published the slot but the on-disk table
         * never received the record (a write-through failure), so drop
         * the half-created item instead of leaving a nameless inode that
         * would reappear after a reboot.  The number is only given back
         * when it is still the highest one (it always is here: the bump
         * above happened with no other allocation in between). */
        memset(it, 0, sizeof(*it));
        if (s_vol.inode_count == idx + 1)
            s_vol.inode_count = idx;
        return ERR_FAULT;
    }
    return 0;
}

static i32 VbdkCreate(vfs_item_id_t parent, const char *name, u32 type, vfs_item_id_t *out_id) {
    if (s_vol.read_only)
        return VFS_ERR_READONLY;
    return VbdkCreateItem(parent, name, type, out_id);
}

/* Recursive delete of inode id (dir children first).  Frees the data
 * extent, subtracts the size, zeroes the record (write-through).  The
 * inode NUMBER is never reused. */
static i32 VbdkDelete(vfs_item_id_t id, u32 recursive) {
    if (s_vol.read_only)
        return VFS_ERR_READONLY;
    vbdk_inode_t *it = vbdk_find(id);
    if (!it)
        return ERR_NOENT;

    if (it->type == VFS_ITEM_DIR) {
        for (u32 i = 1; i <= s_vol.inode_count; i++) {
            vbdk_inode_t *c = &s_vol.inodes[i - 1];
            if (!(c->flags & VBDK_IN_FLAG) || c->parent_id != id)
                continue;
            if (!recursive)
                return ERR_BUSY; /* dir not empty */
            i32 r = VbdkDelete((vfs_item_id_t)i, 1);
            if (r < 0)
                return r;
            i = 0; /* table changed; rescan */
        }
    }

    if (it->extent_blocks > 0)
        VbdkExtentRelease(it->extent_start, it->extent_blocks);
    s_vol.used_bytes -= it->size;
    memset(it, 0, sizeof(*it)); /* free; number never reused */
    if (VbdkInodeStore(it, id) < 0)
        return ERR_FAULT;
    return 0;
}

/*
 * Move/rename inode id under new_parent.  The inode NUMBER never
 * changes — a move only rewrites parent_id (and optionally name) in
 * place, the foundation for "bookmark survives a move".  A non-empty
 * payload.name renames; an empty name keeps it.  Rejects moving a dir
 * into its own subtree.
 */
static i32 VbdkMove(vfs_item_id_t id, vfs_item_id_t new_parent, const char *name) {
    if (s_vol.read_only)
        return VFS_ERR_READONLY;
    vbdk_inode_t *it = vbdk_find(id);
    if (!it)
        return ERR_NOENT;
    vbdk_inode_t *p = vbdk_find(new_parent);
    if (!p || p->type != VFS_ITEM_DIR)
        return ERR_NOENT;

    if (name && strlen(name) >= VBDK_NAME_MAX)
        return ERR_OVERFLOW;

    /* Rename semantics: empty name keeps the current one. */
    const char *final_name = (name && name[0]) ? name : it->name;

    /* Destination collision (same parent + same name = no-op). */
    if (it->parent_id != new_parent || (name && name[0] && strcmp(name, it->name) != 0)) {
        vfs_item_id_t dup = 0;
        if (VbdkLookup(new_parent, final_name, &dup) == 0)
            return VFS_ERR_EXISTS;
    }

    /* A directory cannot be moved into its own subtree. */
    if (it->type == VFS_ITEM_DIR) {
        vfs_item_id_t cur = new_parent;
        while (cur != 0) {
            if (cur == id)
                return ERR_INVAL; /* would create a cycle */
            vbdk_inode_t *a = vbdk_find(cur);
            if (!a)
                break;
            cur = a->parent_id;
        }
    }

    it->parent_id = new_parent;
    if (name && name[0]) {
        strncpy(it->name, name, sizeof(it->name) - 1);
        it->name[sizeof(it->name) - 1] = '\0';
    }
    it->modified = (u64)GetTime();
    if (VbdkInodeStore(it, id) < 0)
        return ERR_FAULT;
    return 0; /* inode number stays stable */
}

/* ====================================================================
 * File data I/O — byte offsets over the contiguous extent, with
 * read-modify-write so partial-sector operations never clobber
 * neighbouring bytes.
 * ==================================================================== */

/* Write `len` bytes at byte-offset `off` inside the inode's extent.
 * off+len must be <= extent_blocks * 512. */
static i32 VbdkRwWrite(u64 off, u32 len, const u8 *data, const vbdk_inode_t *it) {
    static u8 sbuf[VBDK_SECTOR_SIZE];
    u64       end = off + len;
    for (u64 s = off / VBDK_SECTOR_SIZE; s <= (end - 1) / VBDK_SECTOR_SIZE; s++) {
        u64 sect_off = s * VBDK_SECTOR_SIZE;
        u64 cstart   = (off > sect_off) ? off : sect_off;
        u64 cend     = (end < sect_off + VBDK_SECTOR_SIZE) ? end : sect_off + VBDK_SECTOR_SIZE;
        i32 r        = VbdkSectorRead((u64)it->extent_start + s, sbuf);
        if (r < 0)
            return r;
        memcpy(sbuf + (size_t)(cstart - sect_off),
               data + (size_t)(cstart - off),
               (size_t)(cend - cstart));
        r = VbdkSectorWrite((u64)it->extent_start + s, sbuf);
        if (r < 0)
            return r;
    }
    return 0;
}

/* Read `len` bytes at byte-offset `off`; caller guarantees the range
 * fits the extent. */
static i32 VbdkRwRead(u64 off, u32 len, u8 *out, const vbdk_inode_t *it) {
    static u8 sbuf[VBDK_SECTOR_SIZE];
    u64       end = off + len;
    for (u64 s = off / VBDK_SECTOR_SIZE; s <= (end - 1) / VBDK_SECTOR_SIZE; s++) {
        u64 sect_off = s * VBDK_SECTOR_SIZE;
        u64 cstart   = (off > sect_off) ? off : sect_off;
        u64 cend     = (end < sect_off + VBDK_SECTOR_SIZE) ? end : sect_off + VBDK_SECTOR_SIZE;
        i32 r        = VbdkSectorRead((u64)it->extent_start + s, sbuf);
        if (r < 0)
            return r;
        memcpy(out + (size_t)(cstart - off),
               sbuf + (size_t)(cstart - sect_off),
               (size_t)(cend - cstart));
    }
    return 0;
}

/* Zero the byte range [from, to) inside the extent (hole fill — new
 * blocks read as NUL, matching fs_mem_driver hole semantics). */
static i32 VbdkZeroRange(u64 from, u64 to, const vbdk_inode_t *it) {
    static u8 sbuf[VBDK_SECTOR_SIZE];
    if (to <= from)
        return 0;
    u64 end = to;
    for (u64 s = from / VBDK_SECTOR_SIZE; s <= (end - 1) / VBDK_SECTOR_SIZE; s++) {
        u64 sect_off = s * VBDK_SECTOR_SIZE;
        u64 cstart   = (from > sect_off) ? from : sect_off;
        u64 cend     = (end < sect_off + VBDK_SECTOR_SIZE) ? end : sect_off + VBDK_SECTOR_SIZE;
        i32 r        = VbdkSectorRead((u64)it->extent_start + s, sbuf);
        if (r < 0)
            return r;
        memset(sbuf + (size_t)(cstart - sect_off), 0, (size_t)(cend - cstart));
        r = VbdkSectorWrite((u64)it->extent_start + s, sbuf);
        if (r < 0)
            return r;
    }
    return 0;
}

/*
 * Ensure the inode's single extent covers `need` data blocks.  Grows
 * in place when the following blocks are free; otherwise migrates the
 * file to a fresh contiguous extent (copy old data, free the old
 * extent).  Mutates RAM state only — the caller persists the inode.
 */
static i32 VbdkExtentEnsure(vbdk_inode_t *it, u32 need) {
    static u8 cbuf[VBDK_SECTOR_SIZE];

    if (it->extent_blocks >= need)
        return 0; /* already covers */

    if (it->extent_blocks > 0) {
        u32 end   = it->extent_start + it->extent_blocks;
        u32 extra = need - it->extent_blocks;
        int ok    = (end + extra <= s_vol.nsectors);
        for (u32 i = 0; ok && i < extra; i++)
            ok = !BmTest(end + i);
        if (ok) { /* extend in place */
            for (u32 i = 0; i < extra; i++)
                BmSet(end + i);
            it->extent_blocks = need;
            return 0;
        }
    }

    /* Migrate: fresh contiguous extent, copy old data, free the old. */
    i32 ns = VbdkExtentAlloc(need);
    if (ns < 0)
        return VFS_ERR_NOSPC;

    u32 old_blocks = it->extent_blocks;
    if (old_blocks > need)
        old_blocks = need;
    for (u32 i = 0; i < old_blocks; i++) {
        i32 r = VbdkSectorRead((u64)it->extent_start + i, cbuf);
        if (r < 0) {
            /* Roll back the fresh extent: it was bm_set by
             * vbdk_extent_alloc but is not yet referenced by the
             * inode — without this the blocks leak from the derived
             * bitmap until the next rebuild (volume appears full). */
            VbdkExtentRelease((u32)ns, need);
            return r;
        }
        r = VbdkSectorWrite((u64)ns + i, cbuf);
        if (r < 0) {
            VbdkExtentRelease((u32)ns, need);
            return r;
        }
    }
    if (it->extent_blocks > 0)
        VbdkExtentRelease(it->extent_start, it->extent_blocks);
    it->extent_start  = (u32)ns;
    it->extent_blocks = need;
    return 0;
}

static i32 VbdkRead(vfs_item_id_t id, u64 offset, u32 len, u8 *out) {
    vbdk_inode_t *it = vbdk_find(id);
    if (!it)
        return ERR_NOENT;
    if (it->type != VFS_ITEM_FILE)
        return ERR_INVAL; /* dirs have no content */
    if (offset >= it->size)
        return 0; /* EOF */
    u64 avail = it->size - offset;
    if ((u64)len > avail)
        len = (u32)avail;
    if (len > DRV_MAX_PAYLOAD)
        len = DRV_MAX_PAYLOAD;
    i32 r = VbdkRwRead(offset, len, out, it);
    return (r < 0) ? r : (i32)len;
}

/*
 * Truncate / length change (DRV_OP_WRITE with len == 0).
 *
 * vfs.h:570 documents "len==0 && offset==0 = truncate (OPEN+TRUNCATE)".
 * The same form with offset = N is the general truncate the vfs_server
 * uses for VFS_OP_TRUNCATE (op 22): the target length rides in the
 * existing offset field, so no new DRV_* opcode (and no vfs.h change) is
 * needed, and offset == 0 keeps exactly its old meaning.  A zero-length
 * VFS_OP_WRITE is answered by the server itself, so the two meanings can
 * never collide.
 *
 * Shrinking gives the tail blocks back to the derived bitmap and resizes
 * the inode's single extent (a file truncated to 0 owns no extent at
 * all); growing extends or migrates the extent through VbdkExtentEnsure
 * and zero-fills the added range, so the hole reads as NUL like every
 * other hole this driver creates.  The inode is written through before
 * the call returns, so the on-disk table and the RAM mirror never
 * disagree — a length change that cannot be satisfied (no free extent,
 * past the end of the volume) is reported as VFS_ERR_NOSPC with the
 * inode record untouched.  The one case that leaves RAM ahead of the
 * medium is a failed zero-fill after a successful extension: the inode
 * is not stored, and the derived bitmap is rebuilt from the inode table
 * at the next mount / SYNC, so nothing inconsistent can be published.
 *
 * Bytes past the new length inside the last retained sector are not
 * zeroed: they are unreachable while size says so, and every grow path
 * zero-fills [size, newsize) before it can expose them again.
 */
static i32 VbdkTruncate(vfs_item_id_t id, vbdk_inode_t *it, u64 newsize) {
    if (newsize == it->size)
        return 0; /* already that length */

    if (newsize < it->size) {
        u64 old  = it->size;
        u32 keep = (u32)((newsize + VBDK_SECTOR_SIZE - 1) / VBDK_SECTOR_SIZE);
        if (keep < it->extent_blocks) {
            VbdkExtentRelease(it->extent_start + keep, it->extent_blocks - keep);
            it->extent_blocks = keep;
            if (keep == 0)
                it->extent_start = 0; /* no data blocks at all */
        }
        it->size = newsize;
        s_vol.used_bytes -= old - newsize;
        it->modified = (u64)GetTime();
        return (VbdkInodeStore(it, id) < 0) ? ERR_FAULT : 0;
    }

    /* Grow: the added range must read as zeros. */
    if (newsize > s_vol.total_bytes)
        return VFS_ERR_NOSPC;

    u32 need_blk = (u32)((newsize + VBDK_SECTOR_SIZE - 1) / VBDK_SECTOR_SIZE);
    i32 r        = VbdkExtentEnsure(it, need_blk);
    if (r < 0)
        return r; /* VFS_ERR_NOSPC when the volume cannot hold it */

    r = VbdkZeroRange(it->size, newsize, it);
    if (r < 0)
        return r; /* inode not stored yet: the old length still stands */

    s_vol.used_bytes += newsize - it->size;
    it->size = newsize;
    it->modified = (u64)GetTime();
    return (VbdkInodeStore(it, id) < 0) ? ERR_FAULT : 0;
}

static i32 VbdkWrite(vfs_item_id_t id, u64 offset, u32 len, const u8 *in) {
    if (s_vol.read_only)
        return VFS_ERR_READONLY;
    vbdk_inode_t *it = vbdk_find(id);
    if (!it)
        return ERR_NOENT;
    if (it->type != VFS_ITEM_FILE)
        return ERR_INVAL;

    /* len == 0 is the truncate form (see VbdkTruncate): the offset field
     * carries the target length, offset 0 being the documented
     * OPEN+TRUNCATE clear. */
    if (len == 0)
        return VbdkTruncate(id, it, offset);

    u64 need = offset + len;
    if (need > s_vol.total_bytes)
        return VFS_ERR_NOSPC;

    u32 need_blk = (u32)((need + VBDK_SECTOR_SIZE - 1) / VBDK_SECTOR_SIZE);
    i32 r        = VbdkExtentEnsure(it, need_blk);
    if (r < 0)
        return r;

    /* Hole: zero-fill old_size..offset so gaps read as NUL. */
    if (offset > it->size)
        VbdkZeroRange(it->size, offset, it);

    r = VbdkRwWrite(offset, len, in, it);
    if (r < 0)
        return r;

    if (need > it->size) {
        s_vol.used_bytes += need - it->size;
        it->size = need;
    }
    it->modified = (u64)GetTime();
    if (VbdkInodeStore(it, id) < 0)
        return ERR_FAULT;
    return (i32)len;
}

/* Collect up to VFS_ENUM_BATCH child names starting at index `from`. */
static i32 VbdkEnum(vfs_item_id_t parent, u32 from, drv_resp_t *resp) {
    u32 n = 0;
    for (u32 i = 1; i <= s_vol.inode_count && n < VFS_ENUM_BATCH; i++) {
        vbdk_inode_t *it = &s_vol.inodes[i - 1];
        if (!(it->flags & VBDK_IN_FLAG) || it->parent_id != parent)
            continue;
        if (from > 0) {
            from--;
            continue;
        }
        strncpy(resp->u.en.items[n].name, it->name, sizeof(resp->u.en.items[n].name) - 1);
        resp->u.en.items[n].name[sizeof(resp->u.en.items[n].name) - 1] = '\0';
        resp->u.en.items[n].id                                         = (vfs_item_id_t)i;
        resp->u.en.items[n].type                                       = it->type;
        n++;
    }
    resp->u.en.count = n;
    return (i32)n;
}

static i32 VbdkGetattr(vfs_item_id_t id, vfs_item_info_t *out) {
    vbdk_inode_t *it = vbdk_find(id);
    if (!it)
        return ERR_NOENT;

    memset(out, 0, sizeof(*out));
    out->parent_id = it->parent_id;
    out->item_id   = id;
    out->type      = (vfs_item_type_t)it->type;
    strncpy(out->name, it->name, sizeof(out->name) - 1);
    out->name[sizeof(out->name) - 1] = '\0';
    out->size                        = it->size;
    out->creation_date               = it->created;
    out->mod_date                    = it->modified;
    out->posix_mode                  = 0;
    out->uid                         = 0;
    out->gid                         = 0;
    return 0;
}

/* ====================================================================
 * Format / mount (format-on-first-boot)
 * ==================================================================== */

/* Write a fresh volume: superblock + zeroed inode table with root
 * inode 1, and a NEW time-derived UUID (persisted — Phase-2 bookmarks
 * key on it). */
static i32 VbdkFormat(void) {
    static u8 zbuf[VBDK_SECTOR_SIZE];

    /* Superblock. */
    vbdk_sb_t sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic               = VBDK_MAGIC;
    sb.block_size          = VBDK_SECTOR_SIZE;
    sb.inode_table_start   = VBDK_INODE_TABLE_START;
    sb.inode_table_sectors = VBDK_INODE_TABLE_SECTORS;
    sb.data_start          = VBDK_DATA_START;
    sb.uuid_hi             = VBDK_UUID_HI_MAGIC;
    int t0                 = GetTime();
    int t1                 = GetTime();
    sb.uuid_lo             = ((u64)(u32)t0 << 32) | (u32)(t1 ^ 0x5642444Bu);
    sb.root_inode          = 1;

    /* Zero the inode table. */
    memset(zbuf, 0, sizeof(zbuf));
    for (u64 s = 0; s < VBDK_INODE_TABLE_SECTORS; s++)
        if (VbdkSectorWrite(VBDK_INODE_TABLE_START + s, zbuf) < 0)
            return ERR_FAULT;

    /* Root inode 1 (a directory).  Preserve the device state (disk
     * index, geometry) across the RAM-table reset. */
    i32 disk        = s_vol.disk;
    u32 nsectors    = s_vol.nsectors;
    u64 total_bytes = s_vol.total_bytes;
    memset(&s_vol, 0, sizeof(s_vol));
    s_vol.disk         = disk;
    s_vol.nsectors     = nsectors;
    s_vol.total_bytes  = total_bytes;
    vbdk_inode_t *root = &s_vol.inodes[0];
    root->flags        = VBDK_IN_FLAG;
    root->type         = VFS_ITEM_DIR;
    root->parent_id    = 0;
    root->item_id      = 1;
    root->created      = (u64)GetTime();
    root->modified     = root->created;
    s_vol.inode_count  = 1;
    if (VbdkInodeStore(root, 1) < 0)
        return ERR_FAULT;

    /* Superblock sector last (commit point). */
    if (VbdkSectorWrite(0, (u8 *)&sb) < 0)
        return ERR_FAULT;

    s_vol.uuid_hi = sb.uuid_hi;
    s_vol.uuid_lo = sb.uuid_lo;
    VbdkRebuildBitmap();

    printf("fs_virtio_blk: formatted Disk (uuid=%x-%x-%x-%x)\n",
           (unsigned)(u32)(s_vol.uuid_hi >> 32),
           (unsigned)(u32)s_vol.uuid_hi,
           (unsigned)(u32)(s_vol.uuid_lo >> 32),
           (unsigned)(u32)s_vol.uuid_lo);
    return 0;
}

/* Load an existing format into RAM (magic must match, else ERR_NOENT). */
static i32 VbdkLoad(void) {
    vbdk_sb_t sb;
    i32       r = VbdkSectorRead(0, (u8 *)&sb);
    if (r < 0)
        return r;
    if (sb.magic != VBDK_MAGIC)
        return ERR_NOENT; /* not formatted */

    /* Geometry sanity — a mismatch means the on-disk format version
     * differs (e.g. the v2 name-limit bump from 64 to 255 bytes).
     * Treat it as unformatted: the mount path re-inits a fresh volume
     * (disk.img is a disposable test artifact, so no migration). */
    if (sb.block_size != VBDK_SECTOR_SIZE || sb.inode_table_start != VBDK_INODE_TABLE_START ||
        sb.inode_table_sectors != VBDK_INODE_TABLE_SECTORS || sb.data_start != VBDK_DATA_START ||
        sb.root_inode != 1)
        return ERR_NOENT;

    s_vol.uuid_hi = sb.uuid_hi;
    s_vol.uuid_lo = sb.uuid_lo;

    /* Load the full inode table into the 128 KB RAM array. */
    for (u64 s = 0; s < VBDK_INODE_TABLE_SECTORS; s++) {
        u8 *dst = (u8 *)s_vol.inodes + (size_t)(s * VBDK_SECTOR_SIZE);
        r       = VbdkSectorRead(VBDK_INODE_TABLE_START + s, dst);
        if (r < 0)
            return r;
    }

    /* inode_count = highest allocated inode number (never-reuse
     * invariant is only enforced within a session; slots with
     * flags==0 are free). */
    s_vol.inode_count = 0;
    for (u32 i = 1; i <= VBDK_MAX_INODES; i++)
        if (s_vol.inodes[i - 1].flags & VBDK_IN_FLAG)
            s_vol.inode_count = i;

    VbdkRebuildBitmap();

    printf("fs_virtio_blk: mounting Disk (uuid=%x-%x-%x-%x)\n",
           (unsigned)(u32)(s_vol.uuid_hi >> 32),
           (unsigned)(u32)s_vol.uuid_hi,
           (unsigned)(u32)(s_vol.uuid_lo >> 32),
           (unsigned)(u32)s_vol.uuid_lo);
    return 0;
}

/* ====================================================================
 * MOUNT handshake (A1: driver-initiated, design §7.2)
 * ==================================================================== */

static int VbdkMount(int vfs_port) {
    vfs_req_mount_t  req;
    vfs_resp_mount_t resp;
    memset(&req, 0, sizeof(req));
    req.op = VFS_OP_MOUNT;
    strncpy(req.driver_name, "virtio_blk", sizeof(req.driver_name) - 1);
    strncpy(req.mount_name, "Disk", sizeof(req.mount_name) - 1);
    req.uuid.hi      = s_vol.uuid_hi; /* persisted volume UUID */
    req.uuid.lo      = s_vol.uuid_lo;
    req.root_item_id = 1;
    req.read_only    = s_vol.read_only;

    int resp_len = (int)sizeof(resp);
    int ret      = IpcCall(vfs_port, &req, (int)sizeof(req), &resp, &resp_len);
    if (ret < 0)
        return ret;
    return resp.ret;
}

/* ====================================================================
 * Management control plane (v0.7.1)
 *
 * An admin proxy (the user service, which holds ATOM_SERVICE_MANAGE)
 * asks the DRIVER — not the vfs_server — to mount/unmount/format/fill
 * the volume.  The driver performs the VFS handshake itself, so the A1
 * owner-subject binding stays with the driver: an arbitrary client can
 * neither tear the volume down nor bypass the server's mount-table
 * validation.  These ops may run while the volume is unmounted.
 * ==================================================================== */

static int s_vfs_port = -1; /* vfs_server port (resolved at boot) */

/* Deregister the volume from the vfs_server (VFS_OP_UNMOUNT). */
static i32 VbdkCtrlUnmount(void) {
    if (!s_vol.mounted)
        return ERR_NOENT;
    vfs_req_unmount_t req;
    memset(&req, 0, sizeof(req));
    req.op = VFS_OP_UNMOUNT;
    strncpy(req.driver_name, "virtio_blk", sizeof(req.driver_name) - 1);
    strncpy(req.mount_name, "Disk", sizeof(req.mount_name) - 1);
    vfs_resp_unmount_t resp;
    int                rlen = (int)sizeof(resp);
    int                r    = IpcCall(s_vfs_port, &req, (int)sizeof(req), &resp, &rlen);
    if (r < 0)
        return r;
    if (resp.ret < 0)
        return resp.ret;
    s_vol.mounted = 0;
    printf("fs_virtio_blk: Disk volume unmounted\n");
    return 0;
}

/* Re-register the volume (VFS_OP_MOUNT). */
static i32 VbdkCtrlMount(void) {
    if (s_vol.mounted)
        return ERR_BUSY;
    i32 r = VbdkMount(s_vfs_port);
    if (r < 0)
        return r;
    s_vol.mounted = 1;
    printf("fs_virtio_blk: Disk volume mounted (RW)\n");
    return 0;
}

/* Wipe + re-format + re-mount.  The UUID changes, so the old volume
 * entry must be dropped first (a stale UUID would break bookmarks). */
static i32 VbdkCtrlFormat(void) {
    if (s_vol.mounted) {
        i32 r = VbdkCtrlUnmount();
        if (r < 0)
            return r;
    }
    i32 r = VbdkFormat();
    if (r < 0)
        return r;
    r = VbdkMount(s_vfs_port);
    if (r < 0)
        return r;
    s_vol.mounted = 1;
    printf("fs_virtio_blk: Disk volume formatted + remounted\n");
    return 0;
}

/* Fill: create/replace "fill.bin" and write pattern bytes until the
 * requested budget or the volume is full (exercises the ENOSPC path).
 * budget==0 → fill until NOSPC.  Reports bytes written via *out_bytes. */
static i32 VbdkCtrlFill(u32 budget, u64 *out_bytes) {
    static u8 pattern[DRV_MAX_PAYLOAD]; /* not s_req/s_resp */
    for (u32 i = 0; i < sizeof(pattern); i++)
        pattern[i] = (u8)(i * 31u + 7u);

    vfs_item_id_t old = 0;
    if (VbdkLookup(1, "fill.bin", &old) == 0) {
        i32 r = VbdkDelete(old, 1);
        if (r < 0)
            return r;
    }
    vfs_item_id_t id;
    i32 r = VbdkCreate(1, "fill.bin", VFS_ITEM_FILE, &id);
    if (r < 0)
        return r;

    u64 off = 0;
    for (;;) {
        u32 chunk = sizeof(pattern);
        if (budget > 0 && (u64)chunk > (u64)budget - off)
            chunk = (u32)((u64)budget - off);
        if (chunk == 0)
            break;
        r = VbdkWrite(id, off, chunk, pattern);
        if (r == VFS_ERR_NOSPC || r == ERR_NOMEM)
            break; /* volume full: this is the fill point */
        if (r < 0)
            return r;
        off += (u32)r;
        if (budget > 0 && off >= (u64)budget)
            break;
    }
    *out_bytes = off;
    return 0;
}

/* ====================================================================
 * Maintenance plane (v0.9): SYNC / CHECK / INFO / RAW_READ
 *
 * Reached through the same admin proxy as CTRL_* (the user service
 * forwards USER_OP_DISK_*), except SYNC, which the vfs_server also sends
 * on behalf of the "sync" / "power off" commands.  CHECK / INFO /
 * RAW_READ are read-only diagnostics and are gated on
 * ATOM_SERVICE_MANAGE exactly like CTRL_* (vfs.h:531); SYNC is not
 * gated, see VbdkCtrlSync.
 * ==================================================================== */

/* Check-report error classes (drv_check_report_t.first_error). */
enum {
    VBDK_CHK_OK      = 0,
    VBDK_CHK_SUPER   = 1, /* superblock missing / magic or geometry bad */
    VBDK_CHK_TABLE   = 2, /* the inode table cannot be read back        */
    VBDK_CHK_TYPE    = 3, /* type is neither a file nor a directory     */
    VBDK_CHK_EXTENT  = 4, /* extent leaves the data area / the device   */
    VBDK_CHK_OVERLAP = 5, /* two live inodes claim the same block       */
    VBDK_CHK_NAME    = 6, /* name not NUL-terminated, or empty          */
    VBDK_CHK_PARENT  = 7, /* parent_id is not a live directory          */
    VBDK_CHK_CYCLE   = 8, /* the parent chain loops or never ends       */
};

/* In-use inode count.  Read-only. */
static u32 VbdkInodesUsed(void) {
    u32 n = 0;
    for (u32 i = 1; i <= VBDK_MAX_INODES; i++)
        if (s_vol.inodes[i - 1].flags & VBDK_IN_FLAG)
            n++;
    return n;
}

/* Blocks in use: the reserved metadata (superblock + inode table =
 * VBDK_DATA_START sectors) plus every live extent.  This mirrors the
 * derived free bitmap (VbdkRebuildBitmap marks exactly those), so
 * used_blocks + free_blocks == nsectors.  Read-only. */
static u32 VbdkUsedBlocks(void) {
    u64 used = VBDK_DATA_START;
    for (u32 i = 1; i <= VBDK_MAX_INODES; i++) {
        const vbdk_inode_t *it = &s_vol.inodes[i - 1];
        if (it->flags & VBDK_IN_FLAG)
            used += it->extent_blocks;
    }
    if (used > (u64)s_vol.nsectors)
        used = (u64)s_vol.nsectors; /* never report more than the device */
    return (u32)used;
}

/*
 * DRV_OP_SYNC — re-land the volume metadata on the medium.
 *
 * This driver is write-through and keeps NO cache (see the file header:
 * "There is no caching, no background flush thread"), so every accepted
 * write is already on the device.  What SYNC adds is the two guarantees
 * "sync" / "power off" actually need:
 *   1. the medium is still writable — every write below fails loudly
 *      otherwise, and
 *   2. the on-device metadata matches the authoritative RAM inode table
 *      (the two can drift if disk.img is touched out of band).
 * It therefore re-writes the whole inode table from RAM, then the
 * superblock, and recomputes the derived free bitmap.
 *
 * UNMOUNTED volume: returns 0 and does NOTHING (chosen over ERR_INVAL).
 * An unmounted volume has nothing buffered, so there is nothing to lose;
 * "power off" must not report a spurious failure for a volume an admin
 * deliberately detached, and writing to a device the admin unmounted
 * would be a surprise.  vfs.h:534 makes SYNC legal in ANY mount state.
 * This is also why SYNC is not capability-gated: the vfs_server (which
 * does not hold ATOM_SERVICE_MANAGE) is one of its callers, and the op
 * can only push already-accepted writes towards the medium.
 */
static i32 VbdkCtrlSync(void) {
    if (!s_vol.mounted)
        return 0; /* documented no-op — see the comment above */

    vbdk_sb_t sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic               = VBDK_MAGIC;
    sb.block_size          = VBDK_SECTOR_SIZE;
    sb.inode_table_start   = VBDK_INODE_TABLE_START;
    sb.inode_table_sectors = VBDK_INODE_TABLE_SECTORS;
    sb.data_start          = VBDK_DATA_START;
    sb.uuid_hi             = s_vol.uuid_hi;
    sb.uuid_lo             = s_vol.uuid_lo;
    sb.root_inode          = 1;

    /* Inode table first, superblock last: sector 0 stays the commit
     * point of the format (VbdkFormat writes it last for exactly this
     * reason), so a crash in the middle of a sync can never publish a
     * half-written table under a valid signature. */
    for (u64 s = 0; s < VBDK_INODE_TABLE_SECTORS; s++) {
        const u8 *src = (const u8 *)s_vol.inodes + (size_t)(s * VBDK_SECTOR_SIZE);
        if (VbdkSectorWrite(VBDK_INODE_TABLE_START + s, src) < 0)
            return ERR_FAULT;
    }
    if (VbdkSectorWrite(0, &sb) < 0)
        return ERR_FAULT;

    VbdkRebuildBitmap(); /* derived state follows the table */

    printf("fs_virtio_blk: Disk synced (%u inode-table sectors + superblock)\n",
           (unsigned)VBDK_INODE_TABLE_SECTORS);
    return 0;
}

/* Append one problem to the report: bump the error count and keep the
 * FIRST one (class + human-readable phrase).  Read-only. */
static void VbdkChkError(drv_check_report_t *rep, u32 cls, const char *what, u32 inode) {
    rep->errors++;
    if (rep->first_error != 0)
        return;
    rep->first_error = cls;
    if (inode)
        snprintf(rep->note, sizeof(rep->note), "%s (inode %u)", what, (unsigned)inode);
    else
        snprintf(rep->note, sizeof(rep->note), "%s", what);
}

/*
 * Walk the parent chain of inode "id", bounded by the table size.
 *   0 = the chain reaches the volume root,
 *   1 = it loops or never terminates,
 *   2 = it leaves the table (a parent that is not a live inode).
 * Read-only.
 */
static int VbdkParentChain(u32 id) {
    u64 cur = (u64)id;
    for (u32 hops = 0; hops <= VBDK_MAX_INODES; hops++) {
        if (cur == 0)
            return 0; /* reached the root */
        if (cur > (u64)VBDK_MAX_INODES)
            return 2;
        const vbdk_inode_t *it = &s_vol.inodes[cur - 1];
        if (!(it->flags & VBDK_IN_FLAG))
            return 2;
        u64 parent = it->parent_id;
        if (parent == cur)
            return 1; /* self-parent: a 1-cycle */
        cur = parent;
    }
    return 1; /* longer than the table: it must contain a cycle */
}

/*
 * DRV_OP_CTRL_CHECK — read-only consistency scan.  Nothing here writes
 * to the device (or to the RAM table): the report is the only output, so
 * a check is always safe to run on a live volume.
 *
 * Scanned: the superblock read back from the device (magic + geometry),
 * inode-table readability (first and last table sector), and for every
 * in-use inode its type, name termination, parent link, parent chain and
 * extent range; live extents are additionally cross-checked against each
 * other for overlap.  files / dirs / used_blocks / free_blocks are
 * counted on the way.
 */
static i32 VbdkCtrlCheck(drv_check_report_t *rep) {
    /* Scan-local overlap map (VBDK_MAX_SECTORS bits = 2 KB).  Deliberately
     * NOT s_blkmap: a diagnostic must not touch the live allocation
     * bitmap, and this buffer is cleared before every scan. */
    static u8 seen[VBDK_MAX_SECTORS / 8];
    u8        probe[VBDK_SECTOR_SIZE];
    vbdk_sb_t sb;

    memset(rep, 0, sizeof(*rep));
    memset(seen, 0, sizeof(seen));
    rep->inodes_total = VBDK_MAX_INODES;

    /* --- the medium is still readable: superblock + inode table --- */
    memset(&sb, 0, sizeof(sb));
    if (VbdkSectorRead(0, &sb) < 0) {
        VbdkChkError(rep, VBDK_CHK_SUPER, "superblock unreadable", 0);
    } else if (sb.magic != VBDK_MAGIC || sb.block_size != VBDK_SECTOR_SIZE ||
               sb.inode_table_start != VBDK_INODE_TABLE_START ||
               sb.inode_table_sectors != VBDK_INODE_TABLE_SECTORS ||
               sb.data_start != VBDK_DATA_START || sb.root_inode != 1) {
        VbdkChkError(rep, VBDK_CHK_SUPER, "bad superblock magic/geometry", 0);
    } else {
        rep->magic_ok = 1;
    }
    for (u32 k = 0; k < 2; k++) {
        u64 sec = VBDK_INODE_TABLE_START + (k ? (u64)VBDK_INODE_TABLE_SECTORS - 1 : 0);
        if (VbdkSectorRead(sec, probe) < 0)
            VbdkChkError(rep, VBDK_CHK_TABLE, "inode table unreadable", 0);
    }

    /* --- per-inode scan (the RAM table is the authoritative mirror) --- */
    u32 used = VBDK_DATA_START; /* superblock + inode table are reserved */
    for (u32 i = 1; i <= VBDK_MAX_INODES; i++) {
        const vbdk_inode_t *it = &s_vol.inodes[i - 1];
        if (!(it->flags & VBDK_IN_FLAG))
            continue;
        rep->inodes_used++;
        if (it->type == VFS_ITEM_FILE)
            rep->files++;
        else if (it->type == VFS_ITEM_DIR)
            rep->dirs++;
        else
            VbdkChkError(rep, VBDK_CHK_TYPE, "unknown inode type", i);

        /* the name field must be terminated inside itself; only the root
         * may be nameless */
        if (!memchr(it->name, '\0', sizeof(it->name)))
            VbdkChkError(rep, VBDK_CHK_NAME, "name is not NUL-terminated", i);
        else if (i != 1 && it->name[0] == '\0')
            VbdkChkError(rep, VBDK_CHK_NAME, "empty name", i);

        /* parent link: only the root (inode 1) hangs off 0, every other
         * parent must be a live directory */
        int parent_ok = 1;
        if (i == 1) {
            if (it->parent_id != 0) {
                parent_ok = 0;
                VbdkChkError(rep, VBDK_CHK_PARENT, "root inode has a parent", i);
            }
        } else {
            const vbdk_inode_t *p = NULL;
            if (it->parent_id >= 1 && it->parent_id <= (u64)VBDK_MAX_INODES)
                p = &s_vol.inodes[it->parent_id - 1];
            if (!p || !(p->flags & VBDK_IN_FLAG) || p->type != VFS_ITEM_DIR) {
                parent_ok = 0;
                VbdkChkError(rep, VBDK_CHK_PARENT, "parent is not a live directory", i);
            }
        }

        /* no directory may sit on a looping parent chain */
        if (parent_ok && it->type == VFS_ITEM_DIR) {
            int chain = VbdkParentChain(i);
            if (chain == 1)
                VbdkChkError(rep, VBDK_CHK_CYCLE, "directory cycle", i);
            else if (chain == 2)
                VbdkChkError(rep, VBDK_CHK_PARENT, "parent chain leaves the table", i);
        }

        /* the extent must sit inside the data area, and no two live
         * inodes may own the same block */
        if (it->extent_blocks > 0) {
            u64 end = (u64)it->extent_start + (u64)it->extent_blocks;
            if (it->extent_start < VBDK_DATA_START || end > (u64)s_vol.nsectors) {
                VbdkChkError(rep, VBDK_CHK_EXTENT, "extent outside the data area", i);
            } else {
                for (u32 b = it->extent_start; b < (u32)end; b++) {
                    if (seen[b >> 3] & (u8)(1u << (b & 7)))
                        VbdkChkError(rep, VBDK_CHK_OVERLAP, "extent overlaps another inode", i);
                    seen[b >> 3] |= (u8)(1u << (b & 7));
                }
                used += it->extent_blocks;
            }
        }
    }

    if (used > s_vol.nsectors)
        used = s_vol.nsectors;
    rep->used_blocks = used;
    rep->free_blocks = s_vol.nsectors - used;

    if (rep->errors == 0)
        snprintf(rep->note, sizeof(rep->note), "clean: %u files, %u dirs, %u free blocks",
                 (unsigned)rep->files, (unsigned)rep->dirs, (unsigned)rep->free_blocks);
    return 0;
}

/* DRV_OP_CTRL_INFO — volume detail for the shell's "disk info".  Pure
 * RAM state, no device I/O; every field the driver cannot supply stays
 * zero (vfs.h:562). */
static i32 VbdkCtrlInfo(drv_info_t *info) {
    memset(info, 0, sizeof(*info));
    strncpy(info->driver, "virtio_blk", sizeof(info->driver) - 1);
    strncpy(info->mount, "Disk", sizeof(info->mount) - 1);
    info->read_only    = s_vol.read_only;  /* 0 — Disk is RW            */
    info->block_size   = VBDK_SECTOR_SIZE; /* allocation unit = sector  */
    info->total_blocks = s_vol.nsectors;
    info->used_blocks  = VbdkUsedBlocks();
    info->inode_total  = VBDK_MAX_INODES;
    info->inode_used   = VbdkInodesUsed();
    info->persistent   = 1; /* disk.img survives a reboot */
    info->uuid_hi      = s_vol.uuid_hi;
    info->uuid_lo      = s_vol.uuid_lo;
    return 0;
}

/*
 * DRV_OP_CTRL_RAW_READ — read raw sectors straight off the device (the
 * debug plane).  req.offset is the first LBA, req.len the byte count:
 * rounded DOWN to whole 512-byte sectors, capped at DRV_RAW_MAX (one
 * reply must stay inside the IPC limit, vfs.h:544); len == 0 means one
 * sector, and a sub-sector len is served as one sector too — the device
 * cannot read less.
 *
 * Returns the number of bytes actually read (>= 0), like DRV_OP_READ,
 * or a negative error.  The payload lands in the caller's out[] buffer
 * (= drv_resp_t.u.data).  NOTE: drv_resp_t.u is a UNION — u.ctrl.bytes
 * aliases u.data[0..7], so the byte count CANNOT ride in u.ctrl.bytes
 * without clobbering the first 8 payload bytes.  It travels in the
 * response's ret field instead (the DRV_OP_READ convention); the user
 * service proxy maps it onto user_resp_disk_t.bytes.
 */
static i32 VbdkCtrlRawRead(u64 lba, u32 len, u8 *out) {
    u32 bytes = len;
    if (bytes == 0)
        bytes = VBDK_SECTOR_SIZE; /* default: one sector */
    if (bytes > DRV_RAW_MAX)
        bytes = DRV_RAW_MAX;
    bytes &= ~(u32)(VBDK_SECTOR_SIZE - 1U); /* whole sectors only */
    if (bytes < VBDK_SECTOR_SIZE)
        bytes = VBDK_SECTOR_SIZE; /* never read zero bytes */

    u32 sectors = bytes / VBDK_SECTOR_SIZE;
    if (lba >= (u64)s_vol.nsectors || (u64)sectors > (u64)s_vol.nsectors - lba)
        return ERR_INVAL; /* outside the device: no partial read */

    int64_t r = sys_blk_read((u64)s_vol.disk, lba, (u64)sectors, out);
    if (r < 0)
        return (i32)r;
    return (i32)(sectors * VBDK_SECTOR_SIZE);
}

/* ====================================================================
 * Driver protocol handlers
 * ==================================================================== */

static void DrvHandle(int token, drv_req_t *req, u64 caller) {
    drv_resp_t *resp = (drv_resp_t *)s_resp;
    memset(resp, 0, sizeof(*resp));

    /* v0.9 SYNC is dispatched here, BEFORE both the management gate and
     * the mounted/volume check: it is legal in ANY mount state and is
     * deliberately NOT gated on ATOM_SERVICE_MANAGE — one of its senders
     * is the vfs_server, which does not hold that atom (vfs.h:531-534).
     * It can only push writes the volume already accepted towards the
     * medium. */
    switch (req->op) {
    case DRV_OP_SYNC:
        resp->ret = VbdkCtrlSync();
        goto out;
    default:
        break;
    }

    /* Management control plane: gated on ATOM_SERVICE_MANAGE (the user
     * service proxies admin commands).  Runs even while unmounted — the
     * v0.9 diagnostics are read-only and the CTRL_* ops do their own
     * mount bookkeeping. */
    if (req->op >= DRV_OP_CTRL_MOUNT && req->op <= DRV_OP_CTRL_RAW_READ) {
        if (CapHasAtom(caller, ATOM_SERVICE_MANAGE) != 1) {
            resp->ret = ERR_DENIED;
            goto out;
        }
        switch (req->op) {
        case DRV_OP_CTRL_MOUNT:
            resp->ret = VbdkCtrlMount();
            break;
        case DRV_OP_CTRL_UNMOUNT:
            resp->ret = VbdkCtrlUnmount();
            break;
        case DRV_OP_CTRL_FORMAT:
            resp->ret = VbdkCtrlFormat();
            break;
        case DRV_OP_CTRL_FILL:
            resp->ret = VbdkCtrlFill(req->len, &resp->u.ctrl.bytes);
            break;
        case DRV_OP_CTRL_CHECK:
            resp->ret = VbdkCtrlCheck(&resp->u.check);
            break;
        case DRV_OP_CTRL_INFO:
            resp->ret = VbdkCtrlInfo(&resp->u.info);
            break;
        case DRV_OP_CTRL_RAW_READ:
            /* offset = first LBA, len = byte count; the byte count comes
             * back in ret (see VbdkCtrlRawRead: u.ctrl.bytes aliases
             * u.data[0..7] in the response union). */
            resp->ret = VbdkCtrlRawRead(req->offset, req->len, resp->u.data);
            break;
        default:
            resp->ret = ERR_INVAL;
            break;
        }
        goto out;
    }

    /* Degraded state (e.g. MOUNT was rejected): reject everything so
     * the process stays alive without corrupting the namespace. */
    if (!s_vol.mounted || req->volume != 0) {
        resp->ret = ERR_INVAL;
        goto out;
    }

    switch (req->op) {
    case DRV_OP_GETATTR:
        resp->ret = VbdkGetattr(req->item_id, &resp->u.item);
        break;
    case DRV_OP_LOOKUP:
        resp->ret = VbdkLookup(req->parent_id, req->payload.name, &resp->u.item_id);
        break;
    case DRV_OP_READ:
        resp->ret = VbdkRead(req->item_id, req->offset, req->len, resp->u.data);
        break;
    case DRV_OP_WRITE:
        resp->ret = VbdkWrite(req->item_id, req->offset, req->len, req->payload.data);
        break;
    case DRV_OP_CREATE_DIR:
        resp->ret = VbdkCreate(req->parent_id, req->payload.name, VFS_ITEM_DIR, &resp->u.item_id);
        break;
    case DRV_OP_MKFILE:
        resp->ret = VbdkCreate(req->parent_id, req->payload.name, VFS_ITEM_FILE, &resp->u.item_id);
        break;
    case DRV_OP_DELETE:
        resp->ret = VbdkDelete(req->item_id, req->recursive);
        break;
    case DRV_OP_ENUM:
        resp->ret = VbdkEnum(req->parent_id, req->from, resp);
        break;
    case DRV_OP_MOVE:
        resp->ret = VbdkMove(req->item_id, req->parent_id, req->payload.name);
        break;
    case DRV_OP_STAT:
        resp->u.stat.total_bytes = s_vol.total_bytes;
        resp->u.stat.used_bytes  = s_vol.used_bytes;
        resp->u.stat.read_only   = s_vol.read_only;
        resp->ret                = 0;
        break;
    default:
        resp->ret = ERR_INVAL;
        break;
    }

out:
    int r = IpcReply(token, resp, (int)sizeof(*resp));
    if (r < 0)
        printf("fs_virtio_blk: ipc_reply failed (%d)\n", r);
}

/* ====================================================================
 * Entry point (fs_virtio_blk_driver process main)
 * ==================================================================== */

/* Degraded-alive idle: something below the driver protocol failed
 * (no device / no cap / device error) — stay up, do not crash the boot. */
static void VbdkDegrade(const char *why) {
    printf("fs_virtio_blk: %s — degraded, staying alive\n", why);
    for (;;)
        Sleep(10);
}

int main(void) {
    printf("fs_virtio_blk: starting block-device filesystem driver\n");

    /* ---- 1. Find the virtio-blk adapter (vendor 0x1AF4, device
     * 0x1001) in the userspace PCI enumeration.  Index = disk arg. */
    int count = PciGetCount();
    if (count <= 0) {
        printf("fs_virtio_blk: pci_get_count failed (%d)\n", count);
        VbdkDegrade("no PCI enumeration");
    }
    s_vol.disk = -1;
    for (int i = 0; i < count; i++) {
        pci_device_info_t dev;
        if (PciGetDevice(i, &dev) < 0)
            continue;
        if (dev.vendor_id == VBDK_VIRTIO_VENDOR && dev.device_id == VBDK_VIRTIO_DEVICE) {
            s_vol.disk = i;
            printf("fs_virtio_blk: virtio-blk device at PCI[%d]\n", i);
            break;
        }
    }
    if (s_vol.disk < 0)
        VbdkDegrade("no virtio-blk device found");

    /* ---- 2. Cap gate: CAP_TYPE_PCI_DEV naming this disk index with
     * both rights (every SYS_BLK_* requires it). */
    int cap = CapCreateObj(CAP_TYPE_PCI_DEV, RIGHT_READ | RIGHT_WRITE, (unsigned long)s_vol.disk);
    if (cap < 0) {
        printf("fs_virtio_blk: CapCreateObj(PCI_DEV %d) failed (%d)\n", s_vol.disk, cap);
        VbdkDegrade("no device capability");
    }
    printf("fs_virtio_blk: device cap %d minted\n", cap);

    /* ---- 3. Device geometry.  The kernel DMA path is lazy — this is
     * the first sys_blk_* call, safe on a fresh boot. */
    blk_info_t info;
    int64_t    r = sys_blk_info((u64)s_vol.disk, &info);
    if (r < 0 || info.sector_size != VBDK_SECTOR_SIZE || info.sectors == 0) {
        printf("fs_virtio_blk: sys_blk_info failed (%d)\n", (int)r);
        VbdkDegrade("bad device geometry");
    }
    s_vol.nsectors    = (info.sectors > VBDK_MAX_SECTORS) ? VBDK_MAX_SECTORS : (u32)info.sectors;
    s_vol.total_bytes = (u64)(s_vol.nsectors - VBDK_DATA_START) * VBDK_SECTOR_SIZE;
    printf("fs_virtio_blk: device ready - %u sectors, %u bytes/sector\n",
           s_vol.nsectors,
           (unsigned)info.sector_size);

    /* ---- 4. Format on first boot, else mount the existing format. */
    r = VbdkLoad();
    if (r == ERR_NOENT)
        r = VbdkFormat();
    if (r < 0) {
        printf("fs_virtio_blk: Disk volume init FAILED (%d)\n", (int)r);
        VbdkDegrade("volume init failed");
    }
    printf("fs_virtio_blk: Disk volume ready - %u KiB RW\n", (unsigned)(s_vol.total_bytes / 1024u));

    /* ---- 5. Driver port ---- */
    int port = IpcPortCreate();
    if (port < 0) {
        printf("fs_virtio_blk: ipc_port_create failed (%d)\n", port);
        ThreadExit(1);
    }
    int ret = PortRegister("vfs.fs.virtio_blk", port);
    if (ret < 0) {
        printf("fs_virtio_blk: PortRegister('vfs.fs.virtio_blk') failed "
               "(%d)\n",
               ret);
        ThreadExit(1);
    }
    printf("fs_virtio_blk: port %d registered as 'vfs.fs.virtio_blk'\n", port);

    /* ---- 6. MOUNT handshake: wait for the vfs_server, register the
     * Disk volume.  The server validates the row against its static
     * mount table.  A rejected mount (e.g. ERR_BUSY — already
     * mounted) leaves the driver alive in the degraded serve loop. */
    int vfs_port = -1;
    for (int i = 0; i < VBDK_MOUNT_WAIT && vfs_port < 0; i++) {
        vfs_port = PortGet("vfs");
        if (vfs_port < 0)
            Sleep(1);
    }
    if (vfs_port < 0) {
        printf("fs_virtio_blk: 'vfs' port never resolved\n");
        ThreadExit(1);
    }
    printf("fs_virtio_blk: vfs_server port %d resolved\n", vfs_port);
    s_vfs_port = vfs_port;

    ret = VbdkMount(s_vfs_port);
    if (ret < 0) {
        printf("fs_virtio_blk: MOUNT Disk failed (%d) - degraded, "
               "serving ERR_INVAL\n",
               ret);
    } else {
        s_vol.mounted = 1;
        printf("fs_virtio_blk: Disk volume mounted (RW)\n");
    }

    /* ---- 7. Serve the driver protocol ---- */
    for (;;) {
        int msg_len = (int)sizeof(s_req);
        int token   = 0;
        u64 sender  = 0;
        ret         = IpcRecvFrom(port, s_req, &msg_len, &token, &sender);
        if (ret < 0) {
            printf("fs_virtio_blk: ipc_recv failed (%d)\n", ret);
            ThreadExit(1);
        }
        if (msg_len < (int)sizeof(u32)) { /* no op code: reject */
            drv_resp_t *resp = (drv_resp_t *)s_resp;
            resp->ret        = ERR_INVAL;
            (void)IpcReply(token, resp, (int)sizeof(*resp));
            continue;
        }
        DrvHandle(token, (drv_req_t *)s_req, sender);
    }
}
