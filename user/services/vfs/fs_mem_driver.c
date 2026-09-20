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
 * fs_mem_driver.c - In-memory filesystem driver (ring-3, independent process)
 * Copyright (c) 2026 OpSys Project
 *
 * Storage driver for the VFS (docs/vfs_design.md §7/§8).  Runs as its
 * own process spawned by the manager (decision A1 — driver-initiated
 * MOUNT handshake; the server-spawned driver of §7.2 is left as a
 * reserved A2 framework), owns two volumes backed by ordinary heap:
 *
 *   System  — read-only.  Exposes every kernel ELF blob (blob.c) as a
 *             file under /Kernel/<name>.elf — the boot filesystem.
 *   Users   — 32 MiB read-write scratch volume (capacity accounted;
 *             storage is malloc'd pages in Phase 0).
 *
 * Item space: each volume keeps its own dense item table; itemID is
 * the 1-based table index (never reused after delete).  The driver
 * owns volume internals (itemID ↔ memory block); the vfs_server owns
 * the namespace (volumes, handles, bookmarks).
 *
 * Registration: waits for the vfs_server's "vfs" port, then sends one
 * VFS_OP_MOUNT per volume.  The server validates each against its
 * static mount table (System/Users) and records the volume.
 *
 * Driver port: "vfs.fs.mem"   (driver_name "mem")
 * Protocol:    drv_req_t/drv_resp_t (vfs.h) — compact unions, every
 *              exchange < 4096 bytes.
 *
 * ------------------------------------------------------------------
 * Structure (volumes): two heap-backed volumes with per-volume dense
 *   item tables — System (RO, kernel blobs exposed as /Kernel/<name>.elf
 *   via s_sys_blobs) and Users (32 MiB RW scratch); driver port
 *   "vfs.fs.mem", protocol drv_req_t/drv_resp_t.
 * How it works:
 *   The manager-spawned process waits for the "vfs" port, builds the
 *   System volume from blob.c entries, then MemMount()s System and
 *   Users (VFS_OP_MOUNT handshake); DRV_OP_* requests from the
 *   vfs_server dispatch to helpers such as MemCreate; itemID is the
 *   1-based table index, never reused after delete.  DRV_OP_WRITE with
 *   len == 0 is the truncate form (MemTruncate): the offset field carries
 *   the target length, growing zero-fills, and the per-file cap
 *   (MEM_MAX_FILE) or the volume capacity answers VFS_ERR_NOSPC.
 * Purpose:
 *   In-RAM storage driver — the boot filesystem (kernel ELF blobs)
 *   plus a 32 MiB RW scratch volume for the running system.
 * Caveats:
 *   No persistence: a reboot loses Users data.  Item space is capped
 *   (MEM_MAX_ITEMS) and deleted items never recycle their ID; storage
 *   is malloc'd pages, so writes consume real RAM.
 * ------------------------------------------------------------------
 */

#include <stdint.h>
#include "../lib/libc/stdio.h"
#include "../lib/libc/string.h"
#include "../lib/libos/syscalls.h"
#include <malloc.h>
#include "vfs.h"

/* ====================================================================
 * Constants
 * ==================================================================== */

#define MEM_MAX_ITEMS   64                  /* item table per volume     */
#define MEM_USERS_CAP   (32u * 1024u * 1024u) /* 32 MiB RAM write volume  */
/* Per-file size cap (bytes): a single file may never exceed the writable
 * volume's capacity.  Checked on every grow path (MemWrite / MemTruncate)
 * before anything is allocated, so a runaway writer gets VFS_ERR_NOSPC
 * instead of an allocation failure. */
#define MEM_MAX_FILE    MEM_USERS_CAP
#define MEM_MOUNT_WAIT  200                 /* × 1 tick port_get retries */

/* Blob files exposed on /Kernel of the System volume (order = blob.c) */
static const char *s_sys_blobs[] = {
        "init", "manager", "serial", "keyboard", "term",
        "shell", "flaky", "hello", "vfs", "fs_mem_driver", "perm",
};
#define SYS_BLOB_COUNT  ((int)(sizeof(s_sys_blobs) / sizeof(s_sys_blobs[0])))

/* ====================================================================
 * Volume + item model
 * ==================================================================== */

typedef struct {
        int            in_use;
        u32            type;          /* VFS_ITEM_FILE / VFS_ITEM_DIR */
        vfs_item_id_t  parent;        /* 0 = volume root */
        char           name[256];
        u8            *data;          /* files only: malloc'd payload */
        u64            size;
        u64            created;       /* RTC ticks */
        u64            modified;
} mem_item_t;

typedef struct {
        char          mount_name[64];
        u32           read_only;
        mem_item_t    items[MEM_MAX_ITEMS];
        int           item_count;     /* next free index; itemID = index + 1 */
        u64           capacity;       /* volume capacity in bytes */
        u64           used;           /* sum of file sizes (capacity check) */
        vfs_item_id_t root;           /* volume root itemID (always 1) */
        u64           uuid_hi;        /* UUID sent in the MOUNT handshake,
                                       * recorded so DRV_OP_CTRL_INFO can
                                       * report the same identity the
                                       * vfs_server has for this volume */
        u64           uuid_lo;
} mem_vol_t;

static mem_vol_t s_sys;           /* System volume (read-only blobs) */
static mem_vol_t s_usr;           /* Users volume (read-write RAM)   */

/* ====================================================================
 * Shared page pool (Phase 3 zero-copy read path, kernel/mm/shm.c)
 * File data is preferentially allocated from this kernel-backed pool;
 * vfs_server maps pool pages READ-ONLY into authorized clients via
 * SYS_SHM_MAP, bypassing the 4096-byte IPC copy path.  A file whose
 * data is NOT pool-resident (pool exhausted, or it grew after
 * allocation — growth migrates to the heap) simply falls back to the
 * chunked read path.
 * ==================================================================== */

#define SHM_POOL_PAGES 256u /* 1 MiB pool */

static u8  *s_pool_virt; /* pool mapping in this process */
static u64  s_pool_phys; /* kernel-issued physical base (SYS_SHM_CREATE) */
static u32  s_pool_used; /* bump offset (page-aligned) */
static int  s_pool_ready;

/* Create the pool: vspace_alloc a range, then SYS_SHM_CREATE maps the
 * physical pages into it.  Returns 0, or -1 (pool stays disabled and
 * all files use heap storage — safe fallback). */
static int PoolInit(void) {
    void *virt = vspace_alloc(SHM_POOL_PAGES * PAGE_SIZE, 0);
    if (!virt)
        return -1;
    u64 phys = ShmCreate(SHM_POOL_PAGES, virt);
    if ((long long)phys <= 0)
        return -1;
    s_pool_virt  = (u8 *)virt;
    s_pool_phys  = phys;
    s_pool_used  = 0;
    s_pool_ready = 1;
    printf("fs_mem_driver: shm pool ready (phys=0x%lx, %u pages)\n",
           (unsigned long)phys,
           (unsigned)SHM_POOL_PAGES);
    return 0;
}

/* Page-aligned bump allocation; NULL when exhausted (heap fallback). */
static u8 *pool_alloc(u32 size) {
    if (!s_pool_ready)
        return NULL;
    u32 need = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (s_pool_used + need > SHM_POOL_PAGES * PAGE_SIZE)
        return NULL;
    u8 *p = s_pool_virt + s_pool_used;
    s_pool_used += need;
    return p;
}

static int PoolContains(const u8 *p) {
    return s_pool_ready && p >= s_pool_virt &&
           p < s_pool_virt + SHM_POOL_PAGES * PAGE_SIZE;
}

/* Backing physical range of a pool-resident buffer. */
static int PoolPhysRange(const u8 *p, u64 *phys_out) {
    if (!PoolContains(p))
        return 0;
    *phys_out = s_pool_phys + (u64)(p - s_pool_virt);
    return 1;
}

/* Free a data buffer: heap buffers are free()d, pool buffers are NOT
 * (the bump allocator never returns blocks; the 1 MiB pool is a
 * bounded, driver-lifetime cache). */
static void ItemFreeData(u8 *p) {
    if (p && !PoolContains(p))
        free(p);
}

/* Request/response buffers (drv_req_t/drv_resp_t both < 4096) */
static u8 s_req[DRV_REQ_MAX];
static u8 s_resp[DRV_RESP_MAX];

/* ====================================================================
 * Item table primitives
 * ==================================================================== */

static mem_item_t *mem_find(mem_vol_t *vol, vfs_item_id_t id)
{
        if (id == 0 || id > (vfs_item_id_t)vol->item_count)
                return NULL;
        mem_item_t *it = &vol->items[id - 1];
        return it->in_use ? it : NULL;
}

static i32 MemLookup(mem_vol_t *vol, vfs_item_id_t parent,
                                            const char *name, vfs_item_id_t *out)
{
        if (!name)
                return ERR_INVAL;
        size_t nlen = strlen(name);
        for (int i = 0; i < vol->item_count; i++) {
                mem_item_t *it = &vol->items[i];
                if (!it->in_use || it->parent != parent)
                        continue;
                if (strlen(it->name) == nlen && memcmp(it->name, name, nlen) == 0) {
                        *out = (vfs_item_id_t)(i + 1);
                        return 0;
                }
        }
        return ERR_NOENT;
}

/* Internal create primitive — no read-only check.  Used by the
 * RO-guarded protocol path (mem_create) and by System-volume setup,
 * which must build /Kernel on a volume that is RO to everyone else. */
static i32 MemCreateItem(mem_vol_t *vol, vfs_item_id_t parent,
                           const char *name, u32 type, vfs_item_id_t *out_id)
{
        if (!name || !name[0])
                return ERR_INVAL;
        if (strlen(name) >= 256)
                return ERR_OVERFLOW;

        mem_item_t *p = mem_find(vol, parent);
        if (!p || p->type != VFS_ITEM_DIR)
                return ERR_NOENT;

        vfs_item_id_t dup = 0;
        if (MemLookup(vol, parent, name, &dup) == 0)
                return VFS_ERR_EXISTS;

        /* Volume item cap: the per-volume item table (MEM_MAX_ITEMS) is
         * full.  That is an out-of-space condition for the volume, not a
         * failed allocation, so it reports VFS_ERR_NOSPC. */
        if (vol->item_count >= MEM_MAX_ITEMS)
                return VFS_ERR_NOSPC;

        /* Publish the slot LAST: item_count is what makes an entry
         * visible to mem_find/MemLookup, so building the record before
         * the bump means no failure can ever leave a half-created item
         * behind (nothing between here and the bump can fail, and no
         * allocation happens at create time — the file starts empty). */
        int idx = vol->item_count;
        mem_item_t *it = &vol->items[idx];
        memset(it, 0, sizeof(*it));
        it->type = type;
        it->parent = parent;
        strncpy(it->name, name, sizeof(it->name) - 1);
        it->name[sizeof(it->name) - 1] = '\0';
        it->created = (u64)GetTime();
        it->modified = it->created;
        it->in_use = 1;
        vol->item_count = idx + 1;
        *out_id = (vfs_item_id_t)(idx + 1);
        return 0;
}

/* Create a file or dir item under parent.  Returns 0 and sets *out_id,
 * or a negative error (VFS_ERR_READONLY / VFS_ERR_EXISTS / ERR_NOMEM). */
static i32 MemCreate(mem_vol_t *vol, vfs_item_id_t parent,
                                            const char *name, u32 type, vfs_item_id_t *out_id)
{
        if (vol->read_only)
                return VFS_ERR_READONLY;
        return MemCreateItem(vol, parent, name, type, out_id);
}

/* Recursive delete of item id (dir children first).  Frees file data. */
static i32 MemDelete(mem_vol_t *vol, vfs_item_id_t id, u32 recursive)
{
        if (vol->read_only)
                return VFS_ERR_READONLY;
        mem_item_t *it = mem_find(vol, id);
        if (!it)
                return ERR_NOENT;

        if (it->type == VFS_ITEM_DIR) {
                for (int i = 0; i < vol->item_count; i++) {
                        if (!vol->items[i].in_use || vol->items[i].parent != id)
                                continue;
                        if (!recursive)
                                return ERR_BUSY;            /* dir not empty */
                        i32 r = MemDelete(vol, (vfs_item_id_t)(i + 1), 1);
                        if (r < 0)
                                return r;
                        i = 0;                          /* table shrinks; rescan */
                }
        }

        if (it->data) {
                ItemFreeData(it->data);
                vol->used -= it->size;
        }
        memset(it, 0, sizeof(*it));             /* in_use = 0, id never reused */
        return 0;
}

/*
 * Move/rename item id under new_parent.  The itemID NEVER changes —
 * it is the 1-based table index, so a move only rewrites the parent
 * (and optionally the name) fields in place.  This is the foundation
 * for "bookmark survives a move" (design §5: 父级 ID 追踪 = 「文件移动
 * 后书签仍有效」的地基).  A non-empty payload.name renames the item;
 * an empty name keeps it.  Rejects moving a dir into its own subtree.
 */
static i32 MemMove(mem_vol_t *vol, vfs_item_id_t id,
                                        vfs_item_id_t new_parent, const char *name)
{
        if (vol->read_only)
                return VFS_ERR_READONLY;
        mem_item_t *it = mem_find(vol, id);
        if (!it)
                return ERR_NOENT;
        mem_item_t *p = mem_find(vol, new_parent);
        if (!p || p->type != VFS_ITEM_DIR)
                return ERR_NOENT;

        /* Rename semantics: empty name keeps the current one. */
        const char *final_name = (name && name[0]) ? name : it->name;

        /* Destination collision (same parent + same name = no-op). */
        if (it->parent != new_parent ||
                (name && name[0] && strcmp(name, it->name) != 0)) {
                vfs_item_id_t dup = 0;
                if (MemLookup(vol, new_parent, final_name, &dup) == 0)
                        return VFS_ERR_EXISTS;
        }

        /* A directory cannot be moved into its own subtree. */
        if (it->type == VFS_ITEM_DIR) {
                vfs_item_id_t cur = new_parent;
                while (cur != 0) {
                        if (cur == id)
                                return ERR_INVAL;           /* would create a cycle */
                        mem_item_t *a = mem_find(vol, cur);
                        if (!a)
                                break;
                        cur = a->parent;
                }
        }

        it->parent = new_parent;
        if (name && name[0]) {
                strncpy(it->name, name, sizeof(it->name) - 1);
                it->name[sizeof(it->name) - 1] = '\0';
        }
        it->modified = (u64)GetTime();
        return 0;                               /* itemID stays stable */
}

/* ====================================================================
 * Volume setup
 * ==================================================================== */

/* Fresh volume with a root dir. */
static void MemVolInit(mem_vol_t *vol, const char *mount_name,
                         u32 read_only, u64 capacity)
{
        memset(vol, 0, sizeof(*vol));
        strncpy(vol->mount_name, mount_name, sizeof(vol->mount_name) - 1);
        vol->read_only = read_only;
        vol->capacity = capacity;

        mem_item_t *root = &vol->items[0];
        root->in_use = 1;
        root->type = VFS_ITEM_DIR;
        root->parent = 0;
        root->created = (u64)GetTime();
        root->modified = root->created;
        vol->item_count = 1;
        vol->root = 1;
}

/*
 * Load the System volume: /Kernel/<blob>.elf for every registered blob.
 * Uses a 128 KB static staging buffer (init.elf — the largest service
 * ELF — exceeds 64 KB now that the P2V test suite is linked in), then
 * copies each payload into an exact-size heap allocation owned by the
 * item.  buf_size > BLOB_MAX_SIZE in the kernel is fine: the kernel
 * only refuses when the blob does not fit the caller's buffer.
 */
static int MemSysLoad(void)
{
        static char blob_buf[262144];

        MemVolInit(&s_sys, "System", 1, 0);

        /* Kernel directory under the root (System is RO to everyone else,
     * so setup uses the internal create primitive) */
        vfs_item_id_t kernel_id = 0;
        int r = MemCreateItem(&s_sys, s_sys.root, "Kernel", VFS_ITEM_DIR,
                                                        &kernel_id);
        if (r < 0) {
                printf("fs_mem_driver: System /Kernel create failed (%d)\n", r);
                return r;
        }

        int loaded = 0;
        for (int i = 0; i < SYS_BLOB_COUNT; i++) {
                const char *bname = s_sys_blobs[i];
                int n = BlobGet(bname, blob_buf, (int)sizeof(blob_buf));
                if (n < 0) {
                        printf("fs_mem_driver: blob '%s' fetch failed (%d)\n", bname, n);
                        continue;
                }

                /* file name = "<blob>.elf" */
                char fname[256];
                size_t l = strlen(bname);
                if (l + 5 > sizeof(fname)) {
                        printf("fs_mem_driver: blob name too long: '%s'\n", bname);
                        continue;
                }
                memcpy(fname, bname, l);
                fname[l] = '.';
                fname[l + 1] = 'e';
                fname[l + 2] = 'l';
                fname[l + 3] = 'f';
                fname[l + 4] = '\0';

                vfs_item_id_t fid = 0;
                r = MemCreateItem(&s_sys, kernel_id, fname, VFS_ITEM_FILE, &fid);
                if (r < 0) {
                        printf("fs_mem_driver: file '%s' create failed (%d)\n", fname, r);
                        continue;
                }

                mem_item_t *it = mem_find(&s_sys, fid);
                /* Prefer the zero-copy pool (read-only blobs, never
                 * freed); fall back to heap when the pool is full. */
                it->data = pool_alloc((u32)n);
                if (!it->data)
                        it->data = malloc((size_t)n);
                if (!it->data) {
                        printf("fs_mem_driver: OOM loading '%s' (%d bytes)\n", fname, n);
                        it->in_use = 0;
                        s_sys.item_count--;
                        continue;
                }
                memcpy(it->data, blob_buf, (size_t)n);
                it->size = (u64)n;
                it->modified = it->created;
                loaded++;
        }

        printf("fs_mem_driver: System volume ready - %d blob files on /Kernel\n",
           loaded);
        return 0;
}

/* ====================================================================
 * MOUNT handshake (A1: driver-initiated, design §7.2)
 * ==================================================================== */

static int MemMount(int vfs_port, mem_vol_t *vol, u64 uuid_lo)
{
        vfs_req_mount_t req;
        vfs_resp_mount_t resp;
        memset(&req, 0, sizeof(req));
        req.op = VFS_OP_MOUNT;
        strncpy(req.driver_name, "mem", sizeof(req.driver_name) - 1);
        strncpy(req.mount_name, vol->mount_name, sizeof(req.mount_name) - 1);
        req.uuid.hi = 0x6f707379732d7666ULL;        /* "opsys-vf" magic */
        req.uuid.lo = ((u64)(u32)GetTime() << 32) | uuid_lo;
        /* Remember the identity this volume just registered with, so
         * DRV_OP_CTRL_INFO reports the volume's real UUID (a RAM volume
         * gets a fresh lo half per mount — it is not persistent). */
        vol->uuid_hi = req.uuid.hi;
        vol->uuid_lo = req.uuid.lo;
        req.root_item_id = vol->root;
        req.read_only = vol->read_only;

        int resp_len = (int)sizeof(resp);
        int ret = IpcCall(vfs_port, &req, (int)sizeof(req), &resp, &resp_len);
        if (ret < 0)
                return ret;
        return resp.ret;
}

/* ====================================================================
 * Maintenance plane (v0.9): SYNC / CHECK / INFO / RAW_READ
 *
 * A memory volume has no medium: SYNC is a documented no-op and a raw
 * sector read is meaningless (there is no LBA space — ERR_INVAL rather
 * than invented bytes).  CHECK / INFO are read-only statistics over the
 * item table; neither writes a single byte of volume state.
 *
 * Gating: vfs.h:531 says the read-only CTRL diagnostics are gated on
 * ATOM_SERVICE_MANAGE "inside the driver".  This driver cannot do that
 * honestly — its serve loop uses IpcRecv (no caller subject, see
 * docs/service_reference.md 9.3) and the driver itself does not hold
 * that atom, so a CapHasAtom() query could only ever fail.  The two
 * diagnostics are therefore served ungated: they are read-only, expose
 * volume statistics only, and no in-tree component sends them to
 * "vfs.fs.mem" (the user-service disk proxy talks to the virtio driver,
 * where CHECK / INFO / RAW_READ ARE gated on the atom).
 * ==================================================================== */

/* Check-report error classes (drv_check_report_t.first_error). */
#define MEM_CHK_OK     0
#define MEM_CHK_ROOT   1 /* the root record is not a live directory    */
#define MEM_CHK_PARENT 2 /* parent is not a live directory (orphan)    */
#define MEM_CHK_TYPE   3 /* item type is neither file nor directory    */
#define MEM_CHK_DATA   4 /* file claims bytes but owns no data buffer  */

/* Bytes really owned by the volume's items.  Read from the item table
 * rather than vol->used: vol->used only tracks writes that go through
 * MemWrite, while the System volume is built at load time (MemSysLoad
 * sets item sizes directly), so the table is the authority for both
 * volumes.  Read-only. */
static u64 MemBytesUsed(const mem_vol_t *vol)
{
        u64 used = 0;
        for (int i = 0; i < vol->item_count; i++)
                if (vol->items[i].in_use)
                        used += vol->items[i].size;
        return used;
}

/* Record one problem: bump the count and keep the FIRST one (class +
 * human-readable phrase).  Read-only. */
static void MemChkError(drv_check_report_t *rep, u32 cls, const char *what, int idx)
{
        rep->errors++;
        if (rep->first_error != 0)
                return;
        rep->first_error = cls;
        if (idx >= 0)
                snprintf(rep->note, sizeof(rep->note), "%s (item %d)", what, idx + 1);
        else
                snprintf(rep->note, sizeof(rep->note), "%s", what);
}

/*
 * DRV_OP_SYNC — documented no-op.
 *
 * The volume lives in heap pages (plus the shm pool); there is no
 * medium and nothing is buffered, so the volume is always already
 * "flushed".  Returning 0 rather than ERR_INVAL keeps "sync" / "power
 * off" honest: the memory volume really is consistent, and reporting it
 * as unsupported would make a successful sync look partial.
 */
static i32 MemCtrlSync(void)
{
        printf("fs_mem_driver: sync - memory volume, nothing to flush\n");
        return 0;
}

/*
 * DRV_OP_CTRL_INFO — volume detail.  Every field is taken from the live
 * item tables (no device, no side effect).  "blocks" are 4 KB pages:
 * that is the granularity RAM is actually committed in (the shm pool
 * and the heap mapping), and persistent stays 0 because a reboot loses
 * the volume (file header, "Caveats").
 */
static i32 MemCtrlInfo(mem_vol_t *vol, drv_info_t *info)
{
        memset(info, 0, sizeof(*info));
        strncpy(info->driver, "mem", sizeof(info->driver) - 1);
        strncpy(info->mount, vol->mount_name, sizeof(info->mount) - 1);
        info->read_only = vol->read_only;
        info->block_size = PAGE_SIZE;
        info->total_blocks = (u32)(vol->capacity / PAGE_SIZE);
        info->used_blocks = (u32)((MemBytesUsed(vol) + PAGE_SIZE - 1) / PAGE_SIZE);

        u32 used_items = 0;
        for (int i = 0; i < vol->item_count; i++)
                if (vol->items[i].in_use)
                        used_items++;
        info->inode_total = MEM_MAX_ITEMS;
        info->inode_used = used_items;
        info->persistent = 0;                   /* RAM: gone after reboot */
        info->uuid_hi = vol->uuid_hi;
        info->uuid_lo = vol->uuid_lo;
        return 0;
}

/*
 * DRV_OP_CTRL_CHECK — read-only health statistics.
 *
 * There is no on-disk format to validate, so "consistency" here means
 * the item graph holds together: the root record is a live directory,
 * every other live item hangs off a live directory, the type is one of
 * the two defined kinds, and a file that claims bytes owns a buffer.
 * Nothing is repaired and nothing is written.
 */
static i32 MemCtrlCheck(mem_vol_t *vol, drv_check_report_t *rep)
{
        memset(rep, 0, sizeof(*rep));
        rep->inodes_total = MEM_MAX_ITEMS;

        mem_item_t *root = &vol->items[0];
        if (!root->in_use || root->type != VFS_ITEM_DIR || root->parent != 0) {
                MemChkError(rep, MEM_CHK_ROOT, "root item is not a live directory", -1);
        } else {
                /* No format signature exists: magic_ok reports that the
                 * volume's own root record validated. */
                rep->magic_ok = 1;
        }

        for (int i = 0; i < vol->item_count; i++) {
                mem_item_t *it = &vol->items[i];
                if (!it->in_use)
                        continue;
                rep->inodes_used++;
                if (it->type == VFS_ITEM_FILE)
                        rep->files++;
                else if (it->type == VFS_ITEM_DIR)
                        rep->dirs++;
                else
                        MemChkError(rep, MEM_CHK_TYPE, "unknown item type", i);

                mem_item_t *parent = NULL;
                if (it->parent >= 1 && it->parent <= (vfs_item_id_t)vol->item_count)
                        parent = &vol->items[it->parent - 1];
                if (parent && (!parent->in_use || parent->type != VFS_ITEM_DIR))
                        parent = NULL;
                if (i != 0 && !parent)
                        MemChkError(rep, MEM_CHK_PARENT,
                                        "parent is not a live directory", i);

                if (it->type == VFS_ITEM_FILE && it->size > 0 && !it->data)
                        MemChkError(rep, MEM_CHK_DATA,
                                        "file has size but no buffer", i);
        }

        u64 bytes = MemBytesUsed(vol);
        rep->used_blocks = (u32)((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
        u32 total_blocks = (u32)(vol->capacity / PAGE_SIZE);
        rep->free_blocks = (total_blocks > rep->used_blocks)
                                        ? total_blocks - rep->used_blocks : 0;

        if (rep->errors == 0)
                snprintf(rep->note, sizeof(rep->note),
                                "clean: %u files, %u dirs, %u bytes used",
                                (unsigned)rep->files, (unsigned)rep->dirs,
                                (unsigned)bytes);
        return 0;
}

/* ====================================================================
 * Driver protocol handlers
 * ==================================================================== */

static mem_vol_t *mem_vol_of(u32 v)
{
        if (v == 0)
                return &s_sys;
        if (v == 1)
                return &s_usr;
        return NULL;
}

static i32 MemGetattr(mem_vol_t *vol, vfs_item_id_t id, vfs_item_info_t *out)
{
        mem_item_t *it = mem_find(vol, id);
        if (!it)
                return ERR_NOENT;

        memset(out, 0, sizeof(*out));
        out->parent_id = it->parent;
        out->item_id = id;
        out->type = it->type;
        strncpy(out->name, it->name, sizeof(out->name) - 1);
        out->name[sizeof(out->name) - 1] = '\0';
        out->size = it->size;
        out->creation_date = it->created;
        out->mod_date = it->modified;
        out->posix_mode = 0;
        out->uid = 0;
        out->gid = 0;
        return 0;
}

static i32 MemRead(mem_vol_t *vol, vfs_item_id_t id, u64 offset,
                                        u32 len, u8 *out)
{
        mem_item_t *it = mem_find(vol, id);
        if (!it)
                return ERR_NOENT;
        if (it->type != VFS_ITEM_FILE)
                return ERR_INVAL;               /* dirs have no content */
        if (offset >= it->size)
                return 0;                       /* EOF */
        u64 avail = it->size - offset;
        if ((u64)len > avail)
                len = (u32)avail;
        if (len > DRV_MAX_PAYLOAD)
                len = DRV_MAX_PAYLOAD;
        memcpy(out, it->data + offset, len);
        return (i32)len;
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
 * Shrinking releases the tail: a heap buffer is reallocated to the new
 * length (best effort — the SIZE is authoritative, and a buffer that
 * cannot shrink simply keeps its pages), a pool-backed buffer is left to
 * the bump allocator (ItemFreeData) and only its size shrinks, and an
 * empty file drops its buffer entirely.  Growing allocates the new
 * length and zero-fills the added range, so the hole reads as NUL —
 * exactly what a sparse write past EOF leaves behind.
 *
 * Returns 0, or VFS_ERR_NOSPC (per-file cap / volume capacity) or
 * ERR_NOMEM (the allocator refused).
 */
static i32 MemTruncate(mem_vol_t *vol, mem_item_t *it, u64 newsize)
{
        if (newsize == it->size)
                return 0;                       /* already that length */

        if (newsize < it->size) {
                u64 old = it->size;
                if (newsize == 0) {
                        /* Truncate to zero: release the whole buffer. */
                        ItemFreeData(it->data);
                        it->data = NULL;
                } else if (it->data && !PoolContains(it->data)) {
                        u8 *nd = realloc(it->data, (size_t)newsize);
                        if (nd)
                                it->data = nd;  /* best effort shrink */
                }
                it->size = newsize;
                vol->used -= old - newsize;
                it->modified = (u64)GetTime();
                return 0;
        }

        /* Grow: the added range must read as zeros. */
        if (newsize > MEM_MAX_FILE)
                return VFS_ERR_NOSPC;           /* single-file limit */
        if (newsize > vol->capacity ||
                vol->used + (newsize - it->size) > vol->capacity)
                return VFS_ERR_NOSPC;

        if (!it->data) {
                /* First allocation: prefer the zero-copy pool, heap as
                 * the fallback.  The whole range is cleared because a
                 * file with no buffer has no bytes to preserve. */
                u8 *nd = pool_alloc((u32)newsize);
                if (!nd)
                        nd = malloc((size_t)newsize);
                if (!nd)
                        return ERR_NOMEM;
                memset(nd, 0, (size_t)newsize);
                it->data = nd;
        } else if (PoolContains(it->data)) {
                /* The pool bump allocator cannot grow a block in place:
                 * migrate to the heap (the file then uses the chunked
                 * read path). */
                u8 *nd = malloc((size_t)newsize);
                if (!nd)
                        return ERR_NOMEM;
                memcpy(nd, it->data, (size_t)it->size);
                memset(nd + it->size, 0, (size_t)(newsize - it->size));
                it->data = nd;
        } else {
                u8 *nd = realloc(it->data, (size_t)newsize);
                if (!nd)
                        return ERR_NOMEM;
                memset(nd + it->size, 0, (size_t)(newsize - it->size));
                it->data = nd;
        }
        vol->used += newsize - it->size;
        it->size = newsize;
        it->modified = (u64)GetTime();
        return 0;
}

static i32 MemWrite(mem_vol_t *vol, vfs_item_id_t id, u64 offset,
                     u32 len, const u8 *in)
{
        if (vol->read_only)
                return VFS_ERR_READONLY;
        mem_item_t *it = mem_find(vol, id);
        if (!it)
                return ERR_NOENT;
        if (it->type != VFS_ITEM_FILE)
                return ERR_INVAL;

        /* len == 0 is the truncate form (see MemTruncate): the offset
         * field carries the target length, offset 0 being the documented
         * OPEN+TRUNCATE clear. */
        if (len == 0)
                return MemTruncate(vol, it, offset);

        u64 need = offset + len;
        /* Per-file cap first, then the volume capacity: a single file may
         * never exceed MEM_MAX_FILE and the volume may never exceed its
         * configured capacity (Users: 32 MiB).  Both report NOSPC. */
        if (need > MEM_MAX_FILE)
                return VFS_ERR_NOSPC;           /* single-file limit */
        if (need > vol->capacity)
                return VFS_ERR_NOSPC;
        if (vol->used + (need - it->size) > vol->capacity)
                return VFS_ERR_NOSPC;           /* growth would exceed 32 MiB */

        if (need > it->size) {
                u8 *nd;
                if (!it->data) {
                        /* First allocation: prefer the zero-copy pool. */
                        nd = pool_alloc((u32)need);
                        if (nd && offset > 0)   /* hole: zero-fill 0..offset */
                                memset(nd, 0, (size_t)offset);
                } else if (PoolContains(it->data)) {
                        /* Pool-backed file growing: migrate to the heap.
                         * (The pool bump allocator cannot grow blocks in
                         * place; after migration the file is served by
                         * the chunked read path.) */
                        nd = malloc((size_t)need);
                        if (nd) {
                                memcpy(nd, it->data, (size_t)it->size);
                                if (offset > it->size)
                                        memset(nd + it->size, 0,
                                               (size_t)(offset - it->size));
                        }
                } else {
                        nd = realloc(it->data, (size_t)need);
                        if (nd && offset > it->size)
                                memset(nd + it->size, 0,
                                       (size_t)(offset - it->size));
                }
                if (!nd)
                        return ERR_NOMEM;
                it->data = nd;
                vol->used += need - it->size;
                it->size = need;
        }
        memcpy(it->data + offset, in, len);
        it->modified = (u64)GetTime();
        return (i32)len;
}

/* Collect up to VFS_ENUM_BATCH child names starting at index `from`. */
static i32 MemEnum(mem_vol_t *vol, vfs_item_id_t parent, u32 from,
                                        drv_resp_t *resp)
{
        u32 n = 0;
        for (int i = 0; i < vol->item_count && n < VFS_ENUM_BATCH; i++) {
                mem_item_t *it = &vol->items[i];
                if (!it->in_use || it->parent != parent)
                        continue;
                if (from > 0) {
                        from--;
                        continue;
                }
                strncpy(resp->u.en.items[n].name, it->name,
                                sizeof(resp->u.en.items[n].name) - 1);
                resp->u.en.items[n].name[sizeof(resp->u.en.items[n].name) - 1] = '\0';
                resp->u.en.items[n].id = (vfs_item_id_t)(i + 1);
                resp->u.en.items[n].type = it->type;
                n++;
        }
        resp->u.en.count = n;
        return (i32)n;
}

static void DrvHandle(int token, drv_req_t *req)
{
        drv_resp_t *resp = (drv_resp_t *)s_resp;
        memset(resp, 0, sizeof(*resp));

        /* v0.9 SYNC is dispatched before the volume lookup: it is legal
         * in any state and for whichever volume index the vfs_server
         * sends (the flush is driver-wide and, for RAM, a no-op). */
        switch (req->op) {
        case DRV_OP_SYNC:
                resp->ret = MemCtrlSync();
                goto out;
        default:
                break;
        }

        mem_vol_t *vol = mem_vol_of(req->volume);
        if (!vol) {
                resp->ret = ERR_INVAL;
                goto out;
        }

        switch (req->op) {
        case DRV_OP_GETATTR:
                resp->ret = MemGetattr(vol, req->item_id, &resp->u.item);
                break;
        case DRV_OP_LOOKUP:
                resp->ret = MemLookup(vol, req->parent_id, req->payload.name,
                               &resp->u.item_id);
                break;
        case DRV_OP_READ:
                resp->ret = MemRead(vol, req->item_id, req->offset, req->len,
                             resp->u.data);
                break;
        case DRV_OP_WRITE:
                resp->ret = MemWrite(vol, req->item_id, req->offset, req->len,
                                                            req->payload.data);
                break;
        case DRV_OP_CREATE_DIR:
                resp->ret = MemCreate(vol, req->parent_id, req->payload.name,
                               VFS_ITEM_DIR, &resp->u.item_id);
                break;
        case DRV_OP_MKFILE:
                resp->ret = MemCreate(vol, req->parent_id, req->payload.name,
                               VFS_ITEM_FILE, &resp->u.item_id);
                break;
        case DRV_OP_DELETE:
                resp->ret = MemDelete(vol, req->item_id, req->recursive);
                break;
        case DRV_OP_ENUM:
                resp->ret = MemEnum(vol, req->parent_id, req->from, resp);
                break;
        case DRV_OP_MOVE:
                resp->ret = MemMove(vol, req->item_id, req->parent_id,
                             req->payload.name);
                break;
        case DRV_OP_STAT:
                resp->u.stat.total_bytes = vol->capacity;
                resp->u.stat.used_bytes = vol->used;
                resp->u.stat.read_only = vol->read_only;
                resp->ret = 0;
                break;
        case DRV_OP_CTRL_CHECK:
                resp->ret = MemCtrlCheck(vol, &resp->u.check);
                break;
        case DRV_OP_CTRL_INFO:
                resp->ret = MemCtrlInfo(vol, &resp->u.info);
                break;
        case DRV_OP_CTRL_RAW_READ:
                /* A RAM volume has no sectors and no LBA space: fail
                 * honestly instead of fabricating bytes. */
                resp->ret = ERR_INVAL;
                break;
        case DRV_OP_PHYS_RANGE: {
                /* Zero-copy backing range of a pool-resident file. */
                mem_item_t *it = mem_find(vol, req->item_id);
                if (!it || it->type != VFS_ITEM_FILE || !it->data) {
                        resp->ret = ERR_NOENT;
                        break;
                }
                u64 phys = 0;
                if (!PoolPhysRange(it->data, &phys)) {
                        resp->ret = ERR_NOENT; /* heap-backed: fall back */
                        break;
                }
                resp->u.pr.phys_base = phys;
                resp->u.pr.size = (u32)it->size;
                resp->ret = 0;
                break;
        }
        default:
                resp->ret = ERR_INVAL;
                break;
        }

out:
        int r = IpcReply(token, resp, (int)sizeof(*resp));
        if (r < 0)
                printf("fs_mem_driver: ipc_reply failed (%d)\n", r);
}

/* ====================================================================
 * Entry point (fs_mem_driver process main)
 * ==================================================================== */

int main(void)
{
        printf("fs_mem_driver: starting in-memory filesystem driver\n");

        /* ---- 1. Zero-copy shared pool FIRST (best-effort: a failure
         * only disables the fast path; every file falls back to heap).
         * Created before the System volume loads so its blobs land in
         * the pool. ---- */
        (void)PoolInit();

        /* ---- 1. Volumes ---- */
        if (MemSysLoad() < 0) {
                printf("fs_mem_driver: System volume load FAILED\n");
                ThreadExit(1);
        }
        MemVolInit(&s_usr, "Users", 0, MEM_USERS_CAP);
        printf("fs_mem_driver: Users volume ready - %d MiB read-write\n",
           (int)(MEM_USERS_CAP / (1024u * 1024u)));

        /* ---- 2. Driver port ---- */
        int port = IpcPortCreate();
        if (port < 0) {
                printf("fs_mem_driver: ipc_port_create failed (%d)\n", port);
                ThreadExit(1);
        }
        int ret = PortRegister("vfs.fs.mem", port);
        if (ret < 0) {
                printf("fs_mem_driver: PortRegister('vfs.fs.mem') failed (%d)\n", ret);
                ThreadExit(1);
        }
        printf("fs_mem_driver: port %d registered as 'vfs.fs.mem'\n", port);

        /* ---- 3. MOUNT handshake: wait for the vfs_server, register both
     * volumes.  The server validates mount_name against its static
     * table (System/Users).  A2 (server-spawned driver) is reserved:
     * this process keeps the driver-initiated MOUNT path. */
        int vfs_port = -1;
        for (int i = 0; i < MEM_MOUNT_WAIT && vfs_port < 0; i++) {
                vfs_port = PortGet("vfs");
                if (vfs_port < 0)
                        Sleep(1);
        }
        if (vfs_port < 0) {
                printf("fs_mem_driver: 'vfs' port never resolved\n");
                ThreadExit(1);
        }
        printf("fs_mem_driver: vfs_server port %d resolved\n", vfs_port);

        ret = MemMount(vfs_port, &s_sys, 0x53595354u);      /* "SYST" */
        if (ret < 0)
                printf("fs_mem_driver: MOUNT System failed (%d)\n", ret);
        ret = MemMount(vfs_port, &s_usr, 0x55534552u);      /* "USER" */
        if (ret < 0)
                printf("fs_mem_driver: MOUNT Users failed (%d)\n", ret);
        printf("fs_mem_driver: volumes mounted (System RO, Users RW)\n");

        /* ---- 4. Serve the driver protocol ---- */
        for (;;) {
                int msg_len = (int)sizeof(s_req);
                int token = 0;
                ret = IpcRecv(port, s_req, &msg_len, &token);
                if (ret < 0) {
                        printf("fs_mem_driver: ipc_recv failed (%d)\n", ret);
                        ThreadExit(1);
                }
                if (msg_len < (int)sizeof(u32)) {       /* no op code: reject */
                        drv_resp_t *resp = (drv_resp_t *)s_resp;
                        resp->ret = ERR_INVAL;
                        (void)IpcReply(token, resp, (int)sizeof(*resp));
                        continue;
                }
                DrvHandle(token, (drv_req_t *)s_req);
        }
}
