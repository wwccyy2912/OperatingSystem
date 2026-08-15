/*
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
} mem_vol_t;

static mem_vol_t s_sys;           /* System volume (read-only blobs) */
static mem_vol_t s_usr;           /* Users volume (read-write RAM)   */

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

static i32 mem_lookup(mem_vol_t *vol, vfs_item_id_t parent,
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
static i32 mem_create_item(mem_vol_t *vol, vfs_item_id_t parent,
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
        if (mem_lookup(vol, parent, name, &dup) == 0)
                return VFS_ERR_EXISTS;

        if (vol->item_count >= MEM_MAX_ITEMS)
                return ERR_NOMEM;

        int idx = vol->item_count++;
        mem_item_t *it = &vol->items[idx];
        memset(it, 0, sizeof(*it));
        it->in_use = 1;
        it->type = type;
        it->parent = parent;
        strncpy(it->name, name, sizeof(it->name) - 1);
        it->name[sizeof(it->name) - 1] = '\0';
        it->created = (u64)get_time();
        it->modified = it->created;
        *out_id = (vfs_item_id_t)(idx + 1);
        return 0;
}

/* Create a file or dir item under parent.  Returns 0 and sets *out_id,
 * or a negative error (VFS_ERR_READONLY / VFS_ERR_EXISTS / ERR_NOMEM). */
static i32 mem_create(mem_vol_t *vol, vfs_item_id_t parent,
                                            const char *name, u32 type, vfs_item_id_t *out_id)
{
        if (vol->read_only)
                return VFS_ERR_READONLY;
        return mem_create_item(vol, parent, name, type, out_id);
}

/* Recursive delete of item id (dir children first).  Frees file data. */
static i32 mem_delete(mem_vol_t *vol, vfs_item_id_t id, u32 recursive)
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
                        i32 r = mem_delete(vol, (vfs_item_id_t)(i + 1), 1);
                        if (r < 0)
                                return r;
                        i = 0;                          /* table shrinks; rescan */
                }
        }

        if (it->data) {
                free(it->data);
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
static i32 mem_move(mem_vol_t *vol, vfs_item_id_t id,
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
                if (mem_lookup(vol, new_parent, final_name, &dup) == 0)
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
        it->modified = (u64)get_time();
        return 0;                               /* itemID stays stable */
}

/* ====================================================================
 * Volume setup
 * ==================================================================== */

/* Fresh volume with a root dir. */
static void mem_vol_init(mem_vol_t *vol, const char *mount_name,
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
        root->created = (u64)get_time();
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
static int mem_sys_load(void)
{
        static char blob_buf[131072];

        mem_vol_init(&s_sys, "System", 1, 0);

        /* Kernel directory under the root (System is RO to everyone else,
     * so setup uses the internal create primitive) */
        vfs_item_id_t kernel_id = 0;
        int r = mem_create_item(&s_sys, s_sys.root, "Kernel", VFS_ITEM_DIR,
                                                        &kernel_id);
        if (r < 0) {
                printf("fs_mem_driver: System /Kernel create failed (%d)\n", r);
                return r;
        }

        int loaded = 0;
        for (int i = 0; i < SYS_BLOB_COUNT; i++) {
                const char *bname = s_sys_blobs[i];
                int n = blob_get(bname, blob_buf, (int)sizeof(blob_buf));
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
                r = mem_create_item(&s_sys, kernel_id, fname, VFS_ITEM_FILE, &fid);
                if (r < 0) {
                        printf("fs_mem_driver: file '%s' create failed (%d)\n", fname, r);
                        continue;
                }

                mem_item_t *it = mem_find(&s_sys, fid);
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

static int mem_mount(int vfs_port, mem_vol_t *vol, u64 uuid_lo)
{
        vfs_req_mount_t req;
        vfs_resp_mount_t resp;
        memset(&req, 0, sizeof(req));
        req.op = VFS_OP_MOUNT;
        strncpy(req.driver_name, "mem", sizeof(req.driver_name) - 1);
        strncpy(req.mount_name, vol->mount_name, sizeof(req.mount_name) - 1);
        req.uuid.hi = 0x6f707379732d7666ULL;        /* "opsys-vf" magic */
        req.uuid.lo = ((u64)(u32)get_time() << 32) | uuid_lo;
        req.root_item_id = vol->root;
        req.read_only = vol->read_only;

        int resp_len = (int)sizeof(resp);
        int ret = ipc_call(vfs_port, &req, (int)sizeof(req), &resp, &resp_len);
        if (ret < 0)
                return ret;
        return resp.ret;
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

static i32 mem_getattr(mem_vol_t *vol, vfs_item_id_t id, vfs_item_info_t *out)
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

static i32 mem_read(mem_vol_t *vol, vfs_item_id_t id, u64 offset,
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

static i32 mem_write(mem_vol_t *vol, vfs_item_id_t id, u64 offset,
                     u32 len, const u8 *in)
{
        if (vol->read_only)
                return VFS_ERR_READONLY;
        mem_item_t *it = mem_find(vol, id);
        if (!it)
                return ERR_NOENT;
        if (it->type != VFS_ITEM_FILE)
                return ERR_INVAL;

        /* Truncate (OPEN+TRUNCATE): len==0 && offset==0 clears the file. */
        if (len == 0) {
                if (offset == 0 && it->size > 0) {
                        free(it->data);
                        it->data = NULL;
                        vol->used -= it->size;
                        it->size = 0;
                        it->modified = (u64)get_time();
                }
                return 0;
        }

        u64 need = offset + len;
        if (need > vol->capacity)
                return VFS_ERR_NOSPC;
        if (vol->used + (need - it->size) > vol->capacity)
                return VFS_ERR_NOSPC;           /* growth would exceed 32 MiB */

        if (need > it->size) {
                u8 *nd = realloc(it->data, (size_t)need);
                if (!nd)
                        return ERR_NOMEM;
                if (offset > it->size)          /* hole: zero-fill size..offset */
                        memset(nd + it->size, 0, (size_t)(offset - it->size));
                it->data = nd;
                vol->used += need - it->size;
                it->size = need;
        }
        memcpy(it->data + offset, in, len);
        it->modified = (u64)get_time();
        return (i32)len;
}

/* Collect up to VFS_ENUM_BATCH child names starting at index `from`. */
static i32 mem_enum(mem_vol_t *vol, vfs_item_id_t parent, u32 from,
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

static void drv_handle(int token, drv_req_t *req)
{
        drv_resp_t *resp = (drv_resp_t *)s_resp;
        memset(resp, 0, sizeof(*resp));

        mem_vol_t *vol = mem_vol_of(req->volume);
        if (!vol) {
                resp->ret = ERR_INVAL;
                goto out;
        }

        switch (req->op) {
        case DRV_OP_GETATTR:
                resp->ret = mem_getattr(vol, req->item_id, &resp->u.item);
                break;
        case DRV_OP_LOOKUP:
                resp->ret = mem_lookup(vol, req->parent_id, req->payload.name,
                               &resp->u.item_id);
                break;
        case DRV_OP_READ:
                resp->ret = mem_read(vol, req->item_id, req->offset, req->len,
                             resp->u.data);
                break;
        case DRV_OP_WRITE:
                resp->ret = mem_write(vol, req->item_id, req->offset, req->len,
                                                            req->payload.data);
                break;
        case DRV_OP_CREATE_DIR:
                resp->ret = mem_create(vol, req->parent_id, req->payload.name,
                               VFS_ITEM_DIR, &resp->u.item_id);
                break;
        case DRV_OP_MKFILE:
                resp->ret = mem_create(vol, req->parent_id, req->payload.name,
                               VFS_ITEM_FILE, &resp->u.item_id);
                break;
        case DRV_OP_DELETE:
                resp->ret = mem_delete(vol, req->item_id, req->recursive);
                break;
        case DRV_OP_ENUM:
                resp->ret = mem_enum(vol, req->parent_id, req->from, resp);
                break;
        case DRV_OP_MOVE:
                resp->ret = mem_move(vol, req->item_id, req->parent_id,
                             req->payload.name);
                break;
        case DRV_OP_STAT:
                resp->u.stat.total_bytes = vol->capacity;
                resp->u.stat.used_bytes = vol->used;
                resp->u.stat.read_only = vol->read_only;
                resp->ret = 0;
                break;
        default:
                resp->ret = ERR_INVAL;
                break;
        }

out:
        int r = ipc_reply(token, resp, (int)sizeof(*resp));
        if (r < 0)
                printf("fs_mem_driver: ipc_reply failed (%d)\n", r);
}

/* ====================================================================
 * Entry point (fs_mem_driver process main)
 * ==================================================================== */

int main(void)
{
        printf("fs_mem_driver: starting in-memory filesystem driver\n");

        /* ---- 1. Volumes ---- */
        if (mem_sys_load() < 0) {
                printf("fs_mem_driver: System volume load FAILED\n");
                thread_exit(1);
        }
        mem_vol_init(&s_usr, "Users", 0, MEM_USERS_CAP);
        printf("fs_mem_driver: Users volume ready - %d MiB read-write\n",
           (int)(MEM_USERS_CAP / (1024u * 1024u)));

        /* ---- 2. Driver port ---- */
        int port = ipc_port_create();
        if (port < 0) {
                printf("fs_mem_driver: ipc_port_create failed (%d)\n", port);
                thread_exit(1);
        }
        int ret = port_register("vfs.fs.mem", port);
        if (ret < 0) {
                printf("fs_mem_driver: port_register('vfs.fs.mem') failed (%d)\n", ret);
                thread_exit(1);
        }
        printf("fs_mem_driver: port %d registered as 'vfs.fs.mem'\n", port);

        /* ---- 3. MOUNT handshake: wait for the vfs_server, register both
     * volumes.  The server validates mount_name against its static
     * table (System/Users).  A2 (server-spawned driver) is reserved:
     * this process keeps the driver-initiated MOUNT path. */
        int vfs_port = -1;
        for (int i = 0; i < MEM_MOUNT_WAIT && vfs_port < 0; i++) {
                vfs_port = port_get("vfs");
                if (vfs_port < 0)
                        sleep(1);
        }
        if (vfs_port < 0) {
                printf("fs_mem_driver: 'vfs' port never resolved\n");
                thread_exit(1);
        }
        printf("fs_mem_driver: vfs_server port %d resolved\n", vfs_port);

        ret = mem_mount(vfs_port, &s_sys, 0x53595354u);      /* "SYST" */
        if (ret < 0)
                printf("fs_mem_driver: MOUNT System failed (%d)\n", ret);
        ret = mem_mount(vfs_port, &s_usr, 0x55534552u);      /* "USER" */
        if (ret < 0)
                printf("fs_mem_driver: MOUNT Users failed (%d)\n", ret);
        printf("fs_mem_driver: volumes mounted (System RO, Users RW)\n");

        /* ---- 4. Serve the driver protocol ---- */
        for (;;) {
                int msg_len = (int)sizeof(s_req);
                int token = 0;
                ret = ipc_recv(port, s_req, &msg_len, &token);
                if (ret < 0) {
                        printf("fs_mem_driver: ipc_recv failed (%d)\n", ret);
                        thread_exit(1);
                }
                if (msg_len < (int)sizeof(u32)) {       /* no op code: reject */
                        drv_resp_t *resp = (drv_resp_t *)s_resp;
                        resp->ret = ERR_INVAL;
                        (void)ipc_reply(token, resp, (int)sizeof(*resp));
                        continue;
                }
                drv_handle(token, (drv_req_t *)s_req);
        }
}
