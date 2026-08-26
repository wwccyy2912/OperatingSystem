/*
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
 *   sectors 1-64  inode table — 256 inodes × 128 B (4/sector); the
 *                              FULL table is loaded into a 32 KB RAM
 *                              array at mount and every mutation is
 *                              written through synchronously (RMW —
 *                              never clobber the 3 sibling inodes in
 *                              a sector).
 *   sectors 65+   file data.  Each inode owns ONE contiguous extent
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
#define VBDK_INODE_SIZE          128                                  /* bytes per inode      */
#define VBDK_NAME_MAX            64                                   /* on-disk name bound   */
#define VBDK_INODES_PER_SECTOR   (VBDK_SECTOR_SIZE / VBDK_INODE_SIZE) /* 4 */
#define VBDK_INODE_TABLE_SECTORS (VBDK_MAX_INODES * VBDK_INODE_SIZE / VBDK_SECTOR_SIZE) /* 64 */
#define VBDK_INODE_TABLE_START   1 /* sector               */
#define VBDK_DATA_START          (VBDK_INODE_TABLE_START + VBDK_INODE_TABLE_SECTORS) /* 65 */
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

/* Inode — 128 bytes, 4 per sector.  flags bit 0 = in_use (persisted,
 * so in-use survives reboot); type is a VFS_ITEM_* value (FILE=0, so
 * a free inode is flags==0, NOT type==0). */
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
    u8   pad[8];
} vbdk_inode_t;

_Static_assert(sizeof(vbdk_sb_t) == VBDK_SECTOR_SIZE, "superblock not 512");
_Static_assert(sizeof(vbdk_inode_t) == VBDK_INODE_SIZE, "inode not 128");

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

static i32 vbdk_sector_read(u64 lba, void *buf) {
    if (lba >= (u64)s_vol.nsectors)
        return ERR_INVAL;
    int64_t r = sys_blk_read((u64)s_vol.disk, lba, 1, buf);
    if (r < 0) {
        printf("fs_virtio_blk: read sector %u failed (%d)\n", (unsigned)lba, (int)r);
        return (i32)r;
    }
    return 0;
}

static i32 vbdk_sector_write(u64 lba, const void *buf) {
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

static int bm_test(u32 blk) {
    return (s_blkmap[blk >> 3] >> (blk & 7)) & 1;
}

static void bm_set(u32 blk) {
    s_blkmap[blk >> 3] |= (u8)(1u << (blk & 7));
}

static void bm_clear(u32 blk) {
    s_blkmap[blk >> 3] &= (u8) ~(1u << (blk & 7));
}

/* Rebuild the derived bitmap from the inode table: sectors 0..64
 * (superblock + inode table) are reserved; every in-use inode's extent
 * is marked; everything else is free.  Also recomputes used_bytes. */
static void vbdk_rebuild_bitmap(void) {
    memset(s_blkmap, 0, sizeof(s_blkmap));
    for (u32 b = 0; b < VBDK_DATA_START; b++)
        bm_set(b); /* superblock + inode table */

    s_vol.used_bytes = 0;
    for (u32 i = 1; i <= VBDK_MAX_INODES; i++) {
        vbdk_inode_t *it = &s_vol.inodes[i - 1];
        if (!(it->flags & VBDK_IN_FLAG))
            continue;
        s_vol.used_bytes += it->size;
        if (it->extent_blocks > 0)
            for (u32 j = 0; j < it->extent_blocks; j++)
                bm_set(it->extent_start + j);
    }
}

/* Release an extent back to the free map. */
static void vbdk_extent_release(u32 start, u32 blocks) {
    for (u32 i = 0; i < blocks; i++)
        bm_clear(start + i);
}

/* First-fit allocation of `blocks` contiguous data blocks.
 * Returns the start block, or -1 when the volume is full. */
static i32 vbdk_extent_alloc(u32 blocks) {
    if (blocks == 0)
        return 0;
    u32 run = 0;
    for (u32 blk = VBDK_DATA_START; blk < s_vol.nsectors; blk++) {
        if (bm_test(blk)) {
            run = 0;
            continue;
        }
        if (++run == blocks) {
            u32 start = blk - blocks + 1;
            for (u32 i = 0; i < blocks; i++)
                bm_set(start + i);
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

/* RMW store of one inode: read its sector, splice the 128-byte record
 * in, write the sector back — the 3 sibling inodes are never touched. */
static i32 vbdk_inode_store(const vbdk_inode_t *in, u64 num) {
    static u8 sbuf[VBDK_SECTOR_SIZE];
    u64       sec = VBDK_INODE_TABLE_START + (num - 1) / VBDK_INODES_PER_SECTOR;
    size_t    off = (size_t)((num - 1) % VBDK_INODES_PER_SECTOR) * VBDK_INODE_SIZE;

    i32 r = vbdk_sector_read(sec, sbuf);
    if (r < 0)
        return r;
    memcpy(sbuf + off, in, VBDK_INODE_SIZE);
    return vbdk_sector_write(sec, sbuf);
}

static i32 vbdk_lookup(vfs_item_id_t parent, const char *name, vfs_item_id_t *out) {
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
vbdk_create_item(vfs_item_id_t parent, const char *name, u32 type, vfs_item_id_t *out_id) {
    if (!name || !name[0])
        return ERR_INVAL;
    if (strlen(name) >= VBDK_NAME_MAX)
        return ERR_OVERFLOW;

    vbdk_inode_t *p = vbdk_find(parent);
    if (!p || p->type != VFS_ITEM_DIR)
        return ERR_NOENT;

    vfs_item_id_t dup = 0;
    if (vbdk_lookup(parent, name, &dup) == 0)
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
    it->created                    = (u64)get_time();
    it->modified                   = it->created;
    *out_id                        = (vfs_item_id_t)(idx + 1);

    if (vbdk_inode_store(it, (u64)(idx + 1)) < 0)
        return ERR_FAULT;
    return 0;
}

static i32 vbdk_create(vfs_item_id_t parent, const char *name, u32 type, vfs_item_id_t *out_id) {
    if (s_vol.read_only)
        return VFS_ERR_READONLY;
    return vbdk_create_item(parent, name, type, out_id);
}

/* Recursive delete of inode id (dir children first).  Frees the data
 * extent, subtracts the size, zeroes the record (write-through).  The
 * inode NUMBER is never reused. */
static i32 vbdk_delete(vfs_item_id_t id, u32 recursive) {
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
            i32 r = vbdk_delete((vfs_item_id_t)i, 1);
            if (r < 0)
                return r;
            i = 0; /* table changed; rescan */
        }
    }

    if (it->extent_blocks > 0)
        vbdk_extent_release(it->extent_start, it->extent_blocks);
    s_vol.used_bytes -= it->size;
    memset(it, 0, sizeof(*it)); /* free; number never reused */
    if (vbdk_inode_store(it, id) < 0)
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
static i32 vbdk_move(vfs_item_id_t id, vfs_item_id_t new_parent, const char *name) {
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
        if (vbdk_lookup(new_parent, final_name, &dup) == 0)
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
    it->modified = (u64)get_time();
    if (vbdk_inode_store(it, id) < 0)
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
static i32 vbdk_rw_write(u64 off, u32 len, const u8 *data, const vbdk_inode_t *it) {
    static u8 sbuf[VBDK_SECTOR_SIZE];
    u64       end = off + len;
    for (u64 s = off / VBDK_SECTOR_SIZE; s <= (end - 1) / VBDK_SECTOR_SIZE; s++) {
        u64 sect_off = s * VBDK_SECTOR_SIZE;
        u64 cstart   = (off > sect_off) ? off : sect_off;
        u64 cend     = (end < sect_off + VBDK_SECTOR_SIZE) ? end : sect_off + VBDK_SECTOR_SIZE;
        i32 r        = vbdk_sector_read((u64)it->extent_start + s, sbuf);
        if (r < 0)
            return r;
        memcpy(sbuf + (size_t)(cstart - sect_off),
               data + (size_t)(cstart - off),
               (size_t)(cend - cstart));
        r = vbdk_sector_write((u64)it->extent_start + s, sbuf);
        if (r < 0)
            return r;
    }
    return 0;
}

/* Read `len` bytes at byte-offset `off`; caller guarantees the range
 * fits the extent. */
static i32 vbdk_rw_read(u64 off, u32 len, u8 *out, const vbdk_inode_t *it) {
    static u8 sbuf[VBDK_SECTOR_SIZE];
    u64       end = off + len;
    for (u64 s = off / VBDK_SECTOR_SIZE; s <= (end - 1) / VBDK_SECTOR_SIZE; s++) {
        u64 sect_off = s * VBDK_SECTOR_SIZE;
        u64 cstart   = (off > sect_off) ? off : sect_off;
        u64 cend     = (end < sect_off + VBDK_SECTOR_SIZE) ? end : sect_off + VBDK_SECTOR_SIZE;
        i32 r        = vbdk_sector_read((u64)it->extent_start + s, sbuf);
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
static i32 vbdk_zero_range(u64 from, u64 to, const vbdk_inode_t *it) {
    static u8 sbuf[VBDK_SECTOR_SIZE];
    if (to <= from)
        return 0;
    u64 end = to;
    for (u64 s = from / VBDK_SECTOR_SIZE; s <= (end - 1) / VBDK_SECTOR_SIZE; s++) {
        u64 sect_off = s * VBDK_SECTOR_SIZE;
        u64 cstart   = (from > sect_off) ? from : sect_off;
        u64 cend     = (end < sect_off + VBDK_SECTOR_SIZE) ? end : sect_off + VBDK_SECTOR_SIZE;
        i32 r        = vbdk_sector_read((u64)it->extent_start + s, sbuf);
        if (r < 0)
            return r;
        memset(sbuf + (size_t)(cstart - sect_off), 0, (size_t)(cend - cstart));
        r = vbdk_sector_write((u64)it->extent_start + s, sbuf);
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
static i32 vbdk_extent_ensure(vbdk_inode_t *it, u32 need) {
    static u8 cbuf[VBDK_SECTOR_SIZE];

    if (it->extent_blocks >= need)
        return 0; /* already covers */

    if (it->extent_blocks > 0) {
        u32 end   = it->extent_start + it->extent_blocks;
        u32 extra = need - it->extent_blocks;
        int ok    = (end + extra <= s_vol.nsectors);
        for (u32 i = 0; ok && i < extra; i++)
            ok = !bm_test(end + i);
        if (ok) { /* extend in place */
            for (u32 i = 0; i < extra; i++)
                bm_set(end + i);
            it->extent_blocks = need;
            return 0;
        }
    }

    /* Migrate: fresh contiguous extent, copy old data, free the old. */
    i32 ns = vbdk_extent_alloc(need);
    if (ns < 0)
        return VFS_ERR_NOSPC;

    u32 old_blocks = it->extent_blocks;
    if (old_blocks > need)
        old_blocks = need;
    for (u32 i = 0; i < old_blocks; i++) {
        i32 r = vbdk_sector_read((u64)it->extent_start + i, cbuf);
        if (r < 0) {
            /* Roll back the fresh extent: it was bm_set by
             * vbdk_extent_alloc but is not yet referenced by the
             * inode — without this the blocks leak from the derived
             * bitmap until the next rebuild (volume appears full). */
            vbdk_extent_release((u32)ns, need);
            return r;
        }
        r = vbdk_sector_write((u64)ns + i, cbuf);
        if (r < 0) {
            vbdk_extent_release((u32)ns, need);
            return r;
        }
    }
    if (it->extent_blocks > 0)
        vbdk_extent_release(it->extent_start, it->extent_blocks);
    it->extent_start  = (u32)ns;
    it->extent_blocks = need;
    return 0;
}

static i32 vbdk_read(vfs_item_id_t id, u64 offset, u32 len, u8 *out) {
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
    i32 r = vbdk_rw_read(offset, len, out, it);
    return (r < 0) ? r : (i32)len;
}

static i32 vbdk_write(vfs_item_id_t id, u64 offset, u32 len, const u8 *in) {
    if (s_vol.read_only)
        return VFS_ERR_READONLY;
    vbdk_inode_t *it = vbdk_find(id);
    if (!it)
        return ERR_NOENT;
    if (it->type != VFS_ITEM_FILE)
        return ERR_INVAL;

    /* Truncate (OPEN+TRUNCATE): len==0 && offset==0 clears the file. */
    if (len == 0) {
        if (offset == 0 && it->size > 0) {
            if (it->extent_blocks > 0)
                vbdk_extent_release(it->extent_start, it->extent_blocks);
            s_vol.used_bytes -= it->size;
            it->extent_start  = 0;
            it->extent_blocks = 0;
            it->size          = 0;
            it->modified      = (u64)get_time();
            if (vbdk_inode_store(it, id) < 0)
                return ERR_FAULT;
        }
        return 0;
    }

    u64 need = offset + len;
    if (need > s_vol.total_bytes)
        return VFS_ERR_NOSPC;

    u32 need_blk = (u32)((need + VBDK_SECTOR_SIZE - 1) / VBDK_SECTOR_SIZE);
    i32 r        = vbdk_extent_ensure(it, need_blk);
    if (r < 0)
        return r;

    /* Hole: zero-fill old_size..offset so gaps read as NUL. */
    if (offset > it->size)
        vbdk_zero_range(it->size, offset, it);

    r = vbdk_rw_write(offset, len, in, it);
    if (r < 0)
        return r;

    if (need > it->size) {
        s_vol.used_bytes += need - it->size;
        it->size = need;
    }
    it->modified = (u64)get_time();
    if (vbdk_inode_store(it, id) < 0)
        return ERR_FAULT;
    return (i32)len;
}

/* Collect up to VFS_ENUM_BATCH child names starting at index `from`. */
static i32 vbdk_enum(vfs_item_id_t parent, u32 from, drv_resp_t *resp) {
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

static i32 vbdk_getattr(vfs_item_id_t id, vfs_item_info_t *out) {
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
static i32 vbdk_format(void) {
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
    int t0                 = get_time();
    int t1                 = get_time();
    sb.uuid_lo             = ((u64)(u32)t0 << 32) | (u32)(t1 ^ 0x5642444Bu);
    sb.root_inode          = 1;

    /* Zero the inode table. */
    memset(zbuf, 0, sizeof(zbuf));
    for (u64 s = 0; s < VBDK_INODE_TABLE_SECTORS; s++)
        if (vbdk_sector_write(VBDK_INODE_TABLE_START + s, zbuf) < 0)
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
    root->created      = (u64)get_time();
    root->modified     = root->created;
    s_vol.inode_count  = 1;
    if (vbdk_inode_store(root, 1) < 0)
        return ERR_FAULT;

    /* Superblock sector last (commit point). */
    if (vbdk_sector_write(0, (u8 *)&sb) < 0)
        return ERR_FAULT;

    s_vol.uuid_hi = sb.uuid_hi;
    s_vol.uuid_lo = sb.uuid_lo;
    vbdk_rebuild_bitmap();

    printf("fs_virtio_blk: formatted Disk (uuid=%x-%x-%x-%x)\n",
           (unsigned)(u32)(s_vol.uuid_hi >> 32),
           (unsigned)(u32)s_vol.uuid_hi,
           (unsigned)(u32)(s_vol.uuid_lo >> 32),
           (unsigned)(u32)s_vol.uuid_lo);
    return 0;
}

/* Load an existing format into RAM (magic must match, else ERR_NOENT). */
static i32 vbdk_load(void) {
    vbdk_sb_t sb;
    i32       r = vbdk_sector_read(0, (u8 *)&sb);
    if (r < 0)
        return r;
    if (sb.magic != VBDK_MAGIC)
        return ERR_NOENT; /* not formatted */

    /* Geometry sanity — Phase 1 has no migration logic. */
    if (sb.block_size != VBDK_SECTOR_SIZE || sb.inode_table_start != VBDK_INODE_TABLE_START ||
        sb.inode_table_sectors != VBDK_INODE_TABLE_SECTORS || sb.data_start != VBDK_DATA_START ||
        sb.root_inode != 1)
        return ERR_INVAL;

    s_vol.uuid_hi = sb.uuid_hi;
    s_vol.uuid_lo = sb.uuid_lo;

    /* Load the full inode table into the 32 KB RAM array. */
    for (u64 s = 0; s < VBDK_INODE_TABLE_SECTORS; s++) {
        u8 *dst = (u8 *)s_vol.inodes + (size_t)(s * VBDK_SECTOR_SIZE);
        r       = vbdk_sector_read(VBDK_INODE_TABLE_START + s, dst);
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

    vbdk_rebuild_bitmap();

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

static int vbdk_mount(int vfs_port) {
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
    int ret      = ipc_call(vfs_port, &req, (int)sizeof(req), &resp, &resp_len);
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
static i32 vbdk_ctrl_unmount(void) {
    if (!s_vol.mounted)
        return ERR_NOENT;
    vfs_req_unmount_t req;
    memset(&req, 0, sizeof(req));
    req.op = VFS_OP_UNMOUNT;
    strncpy(req.driver_name, "virtio_blk", sizeof(req.driver_name) - 1);
    strncpy(req.mount_name, "Disk", sizeof(req.mount_name) - 1);
    vfs_resp_unmount_t resp;
    int                rlen = (int)sizeof(resp);
    int                r    = ipc_call(s_vfs_port, &req, (int)sizeof(req), &resp, &rlen);
    if (r < 0)
        return r;
    if (resp.ret < 0)
        return resp.ret;
    s_vol.mounted = 0;
    printf("fs_virtio_blk: Disk volume unmounted\n");
    return 0;
}

/* Re-register the volume (VFS_OP_MOUNT). */
static i32 vbdk_ctrl_mount(void) {
    if (s_vol.mounted)
        return ERR_BUSY;
    i32 r = vbdk_mount(s_vfs_port);
    if (r < 0)
        return r;
    s_vol.mounted = 1;
    printf("fs_virtio_blk: Disk volume mounted (RW)\n");
    return 0;
}

/* Wipe + re-format + re-mount.  The UUID changes, so the old volume
 * entry must be dropped first (a stale UUID would break bookmarks). */
static i32 vbdk_ctrl_format(void) {
    if (s_vol.mounted) {
        i32 r = vbdk_ctrl_unmount();
        if (r < 0)
            return r;
    }
    i32 r = vbdk_format();
    if (r < 0)
        return r;
    r = vbdk_mount(s_vfs_port);
    if (r < 0)
        return r;
    s_vol.mounted = 1;
    printf("fs_virtio_blk: Disk volume formatted + remounted\n");
    return 0;
}

/* Fill: create/replace "fill.bin" and write pattern bytes until the
 * requested budget or the volume is full (exercises the ENOSPC path).
 * budget==0 → fill until NOSPC.  Reports bytes written via *out_bytes. */
static i32 vbdk_ctrl_fill(u32 budget, u64 *out_bytes) {
    static u8 pattern[DRV_MAX_PAYLOAD]; /* not s_req/s_resp */
    for (u32 i = 0; i < sizeof(pattern); i++)
        pattern[i] = (u8)(i * 31u + 7u);

    vfs_item_id_t old = 0;
    if (vbdk_lookup(1, "fill.bin", &old) == 0) {
        i32 r = vbdk_delete(old, 1);
        if (r < 0)
            return r;
    }
    vfs_item_id_t id;
    i32 r = vbdk_create(1, "fill.bin", VFS_ITEM_FILE, &id);
    if (r < 0)
        return r;

    u64 off = 0;
    for (;;) {
        u32 chunk = sizeof(pattern);
        if (budget > 0 && (u64)chunk > (u64)budget - off)
            chunk = (u32)((u64)budget - off);
        if (chunk == 0)
            break;
        r = vbdk_write(id, off, chunk, pattern);
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
 * Driver protocol handlers
 * ==================================================================== */

static void drv_handle(int token, drv_req_t *req, u64 caller) {
    drv_resp_t *resp = (drv_resp_t *)s_resp;
    memset(resp, 0, sizeof(*resp));

    /* Management control plane: gated on ATOM_SERVICE_MANAGE (the user
     * service proxies admin commands).  Runs even while unmounted. */
    if (req->op >= DRV_OP_CTRL_MOUNT && req->op <= DRV_OP_CTRL_FILL) {
        if (cap_has_atom(caller, ATOM_SERVICE_MANAGE) != 1) {
            resp->ret = ERR_DENIED;
            goto out;
        }
        switch (req->op) {
        case DRV_OP_CTRL_MOUNT:
            resp->ret = vbdk_ctrl_mount();
            break;
        case DRV_OP_CTRL_UNMOUNT:
            resp->ret = vbdk_ctrl_unmount();
            break;
        case DRV_OP_CTRL_FORMAT:
            resp->ret = vbdk_ctrl_format();
            break;
        case DRV_OP_CTRL_FILL:
            resp->ret = vbdk_ctrl_fill(req->len, &resp->u.ctrl.bytes);
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
        resp->ret = vbdk_getattr(req->item_id, &resp->u.item);
        break;
    case DRV_OP_LOOKUP:
        resp->ret = vbdk_lookup(req->parent_id, req->payload.name, &resp->u.item_id);
        break;
    case DRV_OP_READ:
        resp->ret = vbdk_read(req->item_id, req->offset, req->len, resp->u.data);
        break;
    case DRV_OP_WRITE:
        resp->ret = vbdk_write(req->item_id, req->offset, req->len, req->payload.data);
        break;
    case DRV_OP_CREATE_DIR:
        resp->ret = vbdk_create(req->parent_id, req->payload.name, VFS_ITEM_DIR, &resp->u.item_id);
        break;
    case DRV_OP_MKFILE:
        resp->ret = vbdk_create(req->parent_id, req->payload.name, VFS_ITEM_FILE, &resp->u.item_id);
        break;
    case DRV_OP_DELETE:
        resp->ret = vbdk_delete(req->item_id, req->recursive);
        break;
    case DRV_OP_ENUM:
        resp->ret = vbdk_enum(req->parent_id, req->from, resp);
        break;
    case DRV_OP_MOVE:
        resp->ret = vbdk_move(req->item_id, req->parent_id, req->payload.name);
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
    int r = ipc_reply(token, resp, (int)sizeof(*resp));
    if (r < 0)
        printf("fs_virtio_blk: ipc_reply failed (%d)\n", r);
}

/* ====================================================================
 * Entry point (fs_virtio_blk_driver process main)
 * ==================================================================== */

/* Degraded-alive idle: something below the driver protocol failed
 * (no device / no cap / device error) — stay up, do not crash the boot. */
static void vbdk_degrade(const char *why) {
    printf("fs_virtio_blk: %s — degraded, staying alive\n", why);
    for (;;)
        sleep(10);
}

int main(void) {
    printf("fs_virtio_blk: starting block-device filesystem driver\n");

    /* ---- 1. Find the virtio-blk adapter (vendor 0x1AF4, device
     * 0x1001) in the userspace PCI enumeration.  Index = disk arg. */
    int count = pci_get_count();
    if (count <= 0) {
        printf("fs_virtio_blk: pci_get_count failed (%d)\n", count);
        vbdk_degrade("no PCI enumeration");
    }
    s_vol.disk = -1;
    for (int i = 0; i < count; i++) {
        pci_device_info_t dev;
        if (pci_get_device(i, &dev) < 0)
            continue;
        if (dev.vendor_id == VBDK_VIRTIO_VENDOR && dev.device_id == VBDK_VIRTIO_DEVICE) {
            s_vol.disk = i;
            printf("fs_virtio_blk: virtio-blk device at PCI[%d]\n", i);
            break;
        }
    }
    if (s_vol.disk < 0)
        vbdk_degrade("no virtio-blk device found");

    /* ---- 2. Cap gate: CAP_TYPE_PCI_DEV naming this disk index with
     * both rights (every SYS_BLK_* requires it). */
    int cap = cap_create_obj(CAP_TYPE_PCI_DEV, RIGHT_READ | RIGHT_WRITE, (unsigned long)s_vol.disk);
    if (cap < 0) {
        printf("fs_virtio_blk: cap_create_obj(PCI_DEV %d) failed (%d)\n", s_vol.disk, cap);
        vbdk_degrade("no device capability");
    }
    printf("fs_virtio_blk: device cap %d minted\n", cap);

    /* ---- 3. Device geometry.  The kernel DMA path is lazy — this is
     * the first sys_blk_* call, safe on a fresh boot. */
    blk_info_t info;
    int64_t    r = sys_blk_info((u64)s_vol.disk, &info);
    if (r < 0 || info.sector_size != VBDK_SECTOR_SIZE || info.sectors == 0) {
        printf("fs_virtio_blk: sys_blk_info failed (%d)\n", (int)r);
        vbdk_degrade("bad device geometry");
    }
    s_vol.nsectors    = (info.sectors > VBDK_MAX_SECTORS) ? VBDK_MAX_SECTORS : (u32)info.sectors;
    s_vol.total_bytes = (u64)(s_vol.nsectors - VBDK_DATA_START) * VBDK_SECTOR_SIZE;
    printf("fs_virtio_blk: device ready - %u sectors, %u bytes/sector\n",
           s_vol.nsectors,
           (unsigned)info.sector_size);

    /* ---- 4. Format on first boot, else mount the existing format. */
    r = vbdk_load();
    if (r == ERR_NOENT)
        r = vbdk_format();
    if (r < 0) {
        printf("fs_virtio_blk: Disk volume init FAILED (%d)\n", (int)r);
        vbdk_degrade("volume init failed");
    }
    printf("fs_virtio_blk: Disk volume ready - %u KiB RW\n", (unsigned)(s_vol.total_bytes / 1024u));

    /* ---- 5. Driver port ---- */
    int port = ipc_port_create();
    if (port < 0) {
        printf("fs_virtio_blk: ipc_port_create failed (%d)\n", port);
        thread_exit(1);
    }
    int ret = port_register("vfs.fs.virtio_blk", port);
    if (ret < 0) {
        printf("fs_virtio_blk: port_register('vfs.fs.virtio_blk') failed "
               "(%d)\n",
               ret);
        thread_exit(1);
    }
    printf("fs_virtio_blk: port %d registered as 'vfs.fs.virtio_blk'\n", port);

    /* ---- 6. MOUNT handshake: wait for the vfs_server, register the
     * Disk volume.  The server validates the row against its static
     * mount table.  A rejected mount (e.g. ERR_BUSY — already
     * mounted) leaves the driver alive in the degraded serve loop. */
    int vfs_port = -1;
    for (int i = 0; i < VBDK_MOUNT_WAIT && vfs_port < 0; i++) {
        vfs_port = port_get("vfs");
        if (vfs_port < 0)
            sleep(1);
    }
    if (vfs_port < 0) {
        printf("fs_virtio_blk: 'vfs' port never resolved\n");
        thread_exit(1);
    }
    printf("fs_virtio_blk: vfs_server port %d resolved\n", vfs_port);
    s_vfs_port = vfs_port;

    ret = vbdk_mount(s_vfs_port);
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
        ret         = ipc_recv_from(port, s_req, &msg_len, &token, &sender);
        if (ret < 0) {
            printf("fs_virtio_blk: ipc_recv failed (%d)\n", ret);
            thread_exit(1);
        }
        if (msg_len < (int)sizeof(u32)) { /* no op code: reject */
            drv_resp_t *resp = (drv_resp_t *)s_resp;
            resp->ret        = ERR_INVAL;
            (void)ipc_reply(token, resp, (int)sizeof(*resp));
            continue;
        }
        drv_handle(token, (drv_req_t *)s_req, sender);
    }
}
