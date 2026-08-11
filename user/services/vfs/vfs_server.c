/*
 * vfs_server.c - VFS namespace server (ring-3, independent process)
 * Copyright (c) 2026 OpSys Project
 *
 * Holds the system-wide namespace state (design §3.1: names are
 * "always resolvable" via the server; nothing else may store paths)
 * and serves the client protocol (vfs.h, VFS_OP_*) on the "vfs" port.
 * Volume internals stay with the storage drivers (§7.1) — the server
 * talks to them via drv_req_t/drv_resp_t on "vfs.fs.<driver>".
 *
 * Phase 0 scope (docs/vfs_design.md §8):
 *   - volumes: static mount table {System RO, Users RW}, filled by the
 *     driver-initiated MOUNT handshake (decision A1 — fs_mem_driver is
 *     manager-spawned and self-mounts).  The server-side spawn of
 *     §7.2 is RESERVED as an A2 framework: no code, just comments.
 *   - handles: file handles AND enumerators live in two token tables;
 *     a handle is a random 32-bit token (decision Q1: fully
 *     userspace, kernel untouched).  VFS_OP_CLOSE releases either.
 *   - ops implemented: GET_ITEM, CREATE_DIR, DELETE_ITEM, OPEN_ITEM,
 *     READ, WRITE, CLOSE, ENUM_BEGIN, ENUM_NEXT, MOUNT, STAT_VOLUME.
 *     Bookmark ops (10-12) and UNMOUNT (14) answer ERR_INVAL (Phase 2).
 *
 * URL grammar (design §6.1): "VolumeName/a/b/c" — the /Volumes view
 * layer is tolerated and stripped ("/Volumes/System/x" == "System/x").
 * Path resolution walks parent-ID chains via DRV_OP_LOOKUP (Phase 0
 * simple resolution).
 */

#include <stdint.h>
#include <stddef.h>
#include "../lib/libc/stdio.h"
#include "../lib/libc/string.h"
#include "../lib/libos/syscalls.h"
#include "vfs.h"
#include "../perm/perm.h"

/* ====================================================================
 * Constants
 * ==================================================================== */

#define MAX_VOLS      4       /* mount slots */
#define MAX_HANDLES   64      /* open file handles */
#define MAX_ENUMS     16      /* active enumerators */
#define MAX_BOOKMARKS 32      /* security-scoped bookmarks */
#define VFS_MAX_DEPTH 8       /* URL path segments */
#define VFS_SEG_MAX   256     /* per-segment buffer */

/* Request/response buffers (all messages < 4096) */
static u8 s_req[VFS_IPC_MAX];
static u8 s_resp[VFS_IPC_MAX];

/*
 * do_move scratch buffers (file scope, not stack): each user thread
 * gets a single 4 KB stack page (thread_create_user), and do_move
 * needs two URL-parse segment arrays (2 × 8 × 256 = 4 KB) plus mount
 * buffers.  The vfs server is single-threaded (ipc_recv loop), so
 * sharing one static scratch set is safe — same pattern as s_req/s_resp.
 */
static char s_mount[64], d_mount[64];
static char s_segs[VFS_MAX_DEPTH][VFS_SEG_MAX];
static char d_segs[VFS_MAX_DEPTH][VFS_SEG_MAX];

/* ====================================================================
 * Static mount configuration (Phase 0 — mirrors the fs_mem_driver
 * volumes).  A MOUNT from a driver is accepted only when it matches
 * one of these rows exactly (mount_name + driver_name + read_only).
 * ==================================================================== */

typedef struct {
    const char *mount_name;
    const char *driver_name;
    u32         read_only;
} vfs_mount_cfg_t;

#define MOUNT_CFG_COUNT 2
static const vfs_mount_cfg_t s_mount_cfg[MOUNT_CFG_COUNT] = {
    { "System", "mem", 1 },
    { "Users",  "mem", 0 },
};

/* ====================================================================
 * Volume table (filled by MOUNT handshakes)
 * ==================================================================== */

typedef struct {
    int            mounted;
    char           mount_name[64];
    char           driver_name[64];
    vfs_uuid_t     uuid;
    vfs_item_id_t  root_item_id;
    u32            read_only;
    int            drv_port;       /* resolved lazily: "vfs.fs.<driver>" */
} vfs_vol_t;

static vfs_vol_t s_vols[MAX_VOLS];

/* ====================================================================
 * File-handle table (decision Q1: fully userspace, random tokens)
 * ==================================================================== */

typedef struct {
    int            in_use;
    vfs_handle_t   token;          /* random 32-bit value handed to client */
    u32            vol_index;
    vfs_item_id_t  item_id;
    u32            access;         /* VFS_ACCESS_* */
    u32            flags;          /* VFS_OPEN_* */
    u64            offset;         /* server-maintained r/w position */
} vfs_handle_ent_t;

static vfs_handle_ent_t s_handles[MAX_HANDLES];

/* ====================================================================
 * Enumerator table (design §3.4 — server-side iteration state)
 * ==================================================================== */

typedef struct {
    int            in_use;
    vfs_handle_t   token;
    u32            vol_index;
    vfs_item_id_t  dir_id;
    u32            from;           /* next child index */
} vfs_enum_ent_t;

static vfs_enum_ent_t s_enums[MAX_ENUMS];

/* ====================================================================
 * Bookmark table (design §5.2 — server-side authoritative record)
 *
 * The blob handed to the client is OPAQUE: it only carries a random
 * server-side token (vfs_bookmark_t.reserved) plus denormalized
 * payload fields for display.  The record below is the source of
 * truth — validation always goes through it, so a blob copied from
 * another app (or hand-crafted) cannot mint access.
 * ==================================================================== */

typedef struct {
    int            in_use;
    u32            token;          /* random 32-bit value (blob.reserved) */
    u32            vol_index;
    vfs_item_id_t  item_id;
    vfs_item_id_t  parent_id;      /* parent at creation (move tracking) */
    char           name[256];      /* name at creation (relocation) */
    u32            access;         /* granted VFS_ACCESS_* */
    u32            app_id_hash;
    u64            created_ticks;
    u64            expiry_ticks;   /* 0 = never */
} vfs_bookmark_ent_t;

static vfs_bookmark_ent_t s_bookmarks[MAX_BOOKMARKS];

/* Lazy-resolved perm-manager port ("perm") — like driver ports. */
static int s_perm_port = -1;

/* ====================================================================
 * Driver call channel + RNG
 * ==================================================================== */

static drv_req_t  s_drv_req;
static drv_resp_t s_drv_resp;

static u32 s_rng_state;

static u32 rng_next(void)
{
    /* Simple LCG — tokens only need to be unpredictable-ish, not
     * cryptographically secure.  Seeded from time+pid at startup. */
    s_rng_state = s_rng_state * 1103515245u + 12345u;
    return s_rng_state;
}

/*
 * One synchronous call to the storage driver for vol_index.  Returns
 * the IPC transport error (<0), or 0 with the driver's business result
 * in s_drv_resp.ret.  The driver port is resolved lazily so mount
 * timing does not matter.
 */
static int vfs_drv_call(u32 vol_index, drv_req_t *req, drv_resp_t *resp)
{
    vfs_vol_t *v = &s_vols[vol_index];
    if (!v->mounted)
        return ERR_NOENT;

    if (v->drv_port < 0) {
        char pname[96];
        strncpy(pname, "vfs.fs.", sizeof(pname) - 1);
        pname[sizeof(pname) - 1] = '\0';
        strncat(pname, v->driver_name, sizeof(pname) - 1 - 7);
        v->drv_port = port_get(pname);
        if (v->drv_port < 0)
            return v->drv_port;
    }

    int resp_len = (int)sizeof(*resp);
    int ret = ipc_call(v->drv_port, req, (int)sizeof(*req), resp, &resp_len);
    return ret;
}

/*
 * Synchronous authorization check against the perm-manager (design
 * §9.4: "vfs_server 每次 open/resolve 校验").  Returns 0 when a grant
 * covering `access` exists; VFS_ERR_ACCESS when denied (perm-manager
 * also creates the PENDING query + UI_SHOW push); or the transport
 * error when "perm" is unreachable.  The perm port is resolved lazily
 * so service start order does not matter.
 */
static int perm_check(u32 app_id_hash, const vfs_resource_t *res,
                      u32 access, const char *url)
{
    if (s_perm_port < 0) {
        s_perm_port = port_get(PERM_PORT_NAME);
        if (s_perm_port < 0)
            return s_perm_port;
    }

    perm_req_check_t req;
    memset(&req, 0, sizeof(req));
    req.op = PERM_OP_CHECK;
    req.app_id_hash = app_id_hash;
    req.resource = *res;
    req.access = access;
    strncpy(req.url, url, sizeof(req.url) - 1);
    req.url[sizeof(req.url) - 1] = '\0';

    perm_resp_check_t resp;
    int resp_len = (int)sizeof(resp);
    int r = ipc_call(s_perm_port, &req, (int)sizeof(req), &resp, &resp_len);
    if (r < 0)
        return r;
    return resp.ret;
}

/* ====================================================================
 * URL resolution (design §6.1)
 * ==================================================================== */

/*
 * Parse a URL into a mount name + path segments.  Accepts both
 * "Volume/a/b" and "/Volumes/Volume/a/b" (view layer stripped).
 * Returns 0, or a negative error (ERR_INVAL malformed, ERR_OVERFLOW
 * too deep).
 */
static int vfs_parse_url(const char *url, char mount[64],
                         char segs[][VFS_SEG_MAX], int *nsegs)
{
    const char *p = url;
    while (*p == '/')
        p++;
    if (*p == '\0')
        return ERR_INVAL;

    char first[VFS_PATH_MAX];
    int flen = 0;
    while (*p && *p != '/' && flen < (int)sizeof(first) - 1)
        first[flen++] = *p++;
    first[flen] = '\0';
    while (*p == '/')
        p++;

    if (strcmp(first, "Volumes") == 0) {
        /* View layer: the real mount name is the next segment. */
        if (*p == '\0')
            return ERR_INVAL;
        flen = 0;
        while (*p && *p != '/' && flen < (int)sizeof(first) - 1)
            first[flen++] = *p++;
        first[flen] = '\0';
        while (*p == '/')
            p++;
    }

    if (flen == 0 || flen >= 64)
        return ERR_INVAL;
    memcpy(mount, first, (size_t)flen + 1);

    *nsegs = 0;
    while (*p) {
        if (*nsegs >= VFS_MAX_DEPTH)
            return ERR_OVERFLOW;
        char *seg = segs[*nsegs];
        int l = 0;
        while (*p && *p != '/' && l < VFS_SEG_MAX - 1)
            seg[l++] = *p++;
        seg[l] = '\0';
        while (*p == '/')
            p++;
        if (l > 0)
            (*nsegs)++;
    }
    return 0;
}

static int vfs_find_vol(const char *mount, u32 *vol_index)
{
    for (u32 i = 0; i < MAX_VOLS; i++) {
        if (s_vols[i].mounted && strcmp(s_vols[i].mount_name, mount) == 0) {
            *vol_index = i;
            return 0;
        }
    }
    return ERR_NOENT;       /* volume not mounted */
}

/* Walk a segment chain from the volume root via parent-ID LOOKUPs. */
static int vfs_lookup_path(u32 vol_index, char segs[][VFS_SEG_MAX],
                           int nsegs, vfs_item_id_t *out_id)
{
    vfs_vol_t *v = &s_vols[vol_index];
    vfs_item_id_t cur = v->root_item_id;

    for (int i = 0; i < nsegs; i++) {
        memset(&s_drv_req, 0, sizeof(s_drv_req));
        s_drv_req.op = DRV_OP_LOOKUP;
        s_drv_req.volume = vol_index;
        s_drv_req.parent_id = cur;
        strncpy(s_drv_req.payload.name, segs[i], DRV_PATH_MAX - 1);
        s_drv_req.payload.name[DRV_PATH_MAX - 1] = '\0';

        int r = vfs_drv_call(vol_index, &s_drv_req, &s_drv_resp);
        if (r < 0)
            return r;
        if (s_drv_resp.ret < 0)
            return s_drv_resp.ret;
        cur = s_drv_resp.u.item_id;
    }
    *out_id = cur;
    return 0;
}

static int vfs_getattr(u32 vol_index, vfs_item_id_t id, vfs_item_info_t *out)
{
    memset(&s_drv_req, 0, sizeof(s_drv_req));
    s_drv_req.op = DRV_OP_GETATTR;
    s_drv_req.volume = vol_index;
    s_drv_req.item_id = id;

    int r = vfs_drv_call(vol_index, &s_drv_req, &s_drv_resp);
    if (r < 0)
        return r;
    if (s_drv_resp.ret < 0)
        return s_drv_resp.ret;
    *out = s_drv_resp.u.item;
    return 0;
}

/* ====================================================================
 * Handle / enumerator allocation
 * ==================================================================== */

static vfs_handle_ent_t *handle_alloc(u32 vol_index, vfs_item_id_t id,
                                      u32 access, u32 flags,
                                      vfs_handle_t *tok)
{
    for (int i = 0; i < MAX_HANDLES; i++) {
        if (!s_handles[i].in_use) {
            vfs_handle_ent_t *h = &s_handles[i];
            do {
                h->token = rng_next();
            } while (h->token == 0);
            h->in_use = 1;
            h->vol_index = vol_index;
            h->item_id = id;
            h->access = access;
            h->flags = flags;
            h->offset = 0;
            *tok = h->token;
            return h;
        }
    }
    return NULL;
}

static vfs_handle_ent_t *handle_find(vfs_handle_t tok)
{
    for (int i = 0; i < MAX_HANDLES; i++)
        if (s_handles[i].in_use && s_handles[i].token == tok)
            return &s_handles[i];
    return NULL;
}

static vfs_enum_ent_t *enum_alloc(u32 vol_index, vfs_item_id_t dir_id,
                                  vfs_handle_t *tok)
{
    for (int i = 0; i < MAX_ENUMS; i++) {
        if (!s_enums[i].in_use) {
            vfs_enum_ent_t *e = &s_enums[i];
            do {
                e->token = rng_next();
            } while (e->token == 0);
            e->in_use = 1;
            e->vol_index = vol_index;
            e->dir_id = dir_id;
            e->from = 0;
            *tok = e->token;
            return e;
        }
    }
    return NULL;
}

static vfs_bookmark_ent_t *bookmark_alloc(u32 vol_index, vfs_item_id_t id,
                                          u32 access, u32 app_id_hash,
                                          u64 created, u64 expiry,
                                          u32 *tok)
{
    for (int i = 0; i < MAX_BOOKMARKS; i++) {
        if (!s_bookmarks[i].in_use) {
            vfs_bookmark_ent_t *b = &s_bookmarks[i];
            do {
                b->token = rng_next();
            } while (b->token == 0);
            b->in_use = 1;
            b->vol_index = vol_index;
            b->item_id = id;
            b->parent_id = 0;       /* filled by caller via getattr */
            b->access = access;
            b->app_id_hash = app_id_hash;
            b->created_ticks = created;
            b->expiry_ticks = expiry;
            *tok = b->token;
            return b;
        }
    }
    return NULL;
}

static vfs_bookmark_ent_t *bookmark_find(u32 tok)
{
    for (int i = 0; i < MAX_BOOKMARKS; i++)
        if (s_bookmarks[i].in_use && s_bookmarks[i].token == tok)
            return &s_bookmarks[i];
    return NULL;
}

/*
 * Fill a vfs_bookmark_t blob from a server-side record.  Phase 2:
 * mac[16] stays zero (real MAC is Phase 3, docs §5 "阶段化说明");
 * validity comes from the server-side record, not the blob bytes.
 */
static void bookmark_fill_blob(const vfs_bookmark_ent_t *b,
                               const vfs_resource_t *res,
                               vfs_item_id_t parent_id,
                               vfs_bookmark_t *blob)
{
    memset(blob, 0, sizeof(*blob));
    blob->magic = VFS_BOOKMARK_MAGIC;
    blob->version = 1;
    blob->payload_len = (u32)(sizeof(*blob) -
                              offsetof(vfs_bookmark_t, reserved));
    blob->reserved = b->token;
    blob->resource = *res;
    blob->parent_id = parent_id;
    blob->access = b->access;
    blob->app_id_hash = b->app_id_hash;
    blob->created_ticks = b->created_ticks;
    blob->expiry_ticks = b->expiry_ticks;
}

/*
 * Validate a client-supplied blob against the server-side record.
 * The blob is only a capability carrier: token → record, then every
 * payload field must match the record (a hand-crafted blob cannot
 * mint access).  Returns the record, or NULL (blob invalid).
 */
static vfs_bookmark_ent_t *bookmark_validate(const vfs_bookmark_t *blob)
{
    if (blob->magic != VFS_BOOKMARK_MAGIC || blob->version != 1)
        return NULL;
    vfs_bookmark_ent_t *b = bookmark_find(blob->reserved);
    if (!b)
        return NULL;
    if (blob->access != b->access || blob->app_id_hash != b->app_id_hash)
        return NULL;
    if (blob->expiry_ticks != b->expiry_ticks)
        return NULL;
    if (b->expiry_ticks != 0 && (u64)get_time() > b->expiry_ticks) {
        memset(b, 0, sizeof(*b));   /* expired: drop the record */
        return NULL;
    }
    return b;
}

/* ====================================================================
 * Protocol handlers (each replies via the shared s_resp buffer)
 * ==================================================================== */

static void do_mount(int token, int msg_len)
{
    vfs_resp_mount_t *resp = (vfs_resp_mount_t *)s_resp;
    if (msg_len < (int)sizeof(vfs_req_mount_t)) {
        resp->ret = ERR_INVAL;
        goto out;
    }
    vfs_req_mount_t *req = (vfs_req_mount_t *)s_req;

    /* Must match a static mount-config row exactly (A1 handshake). */
    int cfg_ok = 0;
    for (u32 i = 0; i < MOUNT_CFG_COUNT; i++) {
        if (strcmp(s_mount_cfg[i].mount_name, req->mount_name) == 0 &&
            strcmp(s_mount_cfg[i].driver_name, req->driver_name) == 0 &&
            s_mount_cfg[i].read_only == req->read_only) {
            cfg_ok = 1;
            break;
        }
    }
    if (!cfg_ok) {
        resp->ret = VFS_ERR_PERM;       /* unknown/unexpected mount */
        goto out;
    }

    for (u32 i = 0; i < MAX_VOLS; i++) {
        if (s_vols[i].mounted &&
            strcmp(s_vols[i].mount_name, req->mount_name) == 0) {
            resp->ret = ERR_BUSY;       /* already mounted */
            goto out;
        }
    }
    for (u32 i = 0; i < MAX_VOLS; i++) {
        if (!s_vols[i].mounted) {
            vfs_vol_t *v = &s_vols[i];
            memset(v, 0, sizeof(*v));
            v->mounted = 1;
            strncpy(v->mount_name, req->mount_name, sizeof(v->mount_name) - 1);
            strncpy(v->driver_name, req->driver_name,
                    sizeof(v->driver_name) - 1);
            v->uuid = req->uuid;
            v->root_item_id = req->root_item_id;
            v->read_only = req->read_only;
            v->drv_port = -1;           /* resolved lazily on first use */
            resp->ret = 0;
            printf("vfs: volume '%s' mounted (driver '%s', %s)\n",
                   v->mount_name, v->driver_name,
                   v->read_only ? "RO" : "RW");
            goto out;
        }
    }
    resp->ret = ERR_NOMEM;              /* mount table full */

out:
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_get_item(int token, int msg_len)
{
    vfs_resp_get_item_t *resp = (vfs_resp_get_item_t *)s_resp;
    if (msg_len < (int)sizeof(vfs_req_get_item_t)) {
        resp->ret = ERR_INVAL;
        goto out;
    }
    vfs_req_get_item_t *req = (vfs_req_get_item_t *)s_req;

    char mount[64];
    char segs[VFS_MAX_DEPTH][VFS_SEG_MAX];
    int nsegs;
    int r = vfs_parse_url(req->path, mount, segs, &nsegs);
    if (r < 0) { resp->ret = r; goto out; }
    u32 vi;
    r = vfs_find_vol(mount, &vi);
    if (r < 0) { resp->ret = r; goto out; }

    vfs_item_id_t id;
    r = vfs_lookup_path(vi, segs, nsegs, &id);
    if (r < 0) { resp->ret = r; goto out; }

    r = vfs_getattr(vi, id, &resp->item);
    if (r < 0) { resp->ret = r; goto out; }
    resp->ret = 0;

out:
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_create_dir(int token, int msg_len)
{
    vfs_resp_create_dir_t *resp = (vfs_resp_create_dir_t *)s_resp;
    if (msg_len < (int)sizeof(vfs_req_create_dir_t)) {
        resp->ret = ERR_INVAL;
        goto out;
    }
    vfs_req_create_dir_t *req = (vfs_req_create_dir_t *)s_req;

    char mount[64];
    char segs[VFS_MAX_DEPTH][VFS_SEG_MAX];
    int nsegs;
    int r = vfs_parse_url(req->path, mount, segs, &nsegs);
    if (r < 0) { resp->ret = r; goto out; }
    u32 vi;
    r = vfs_find_vol(mount, &vi);
    if (r < 0) { resp->ret = r; goto out; }
    if (nsegs == 0) { resp->ret = ERR_INVAL; goto out; }    /* no root mkdir */

    vfs_item_id_t id;
    r = vfs_lookup_path(vi, segs, nsegs - 1, &id);
    if (r < 0) { resp->ret = r; goto out; }

    memset(&s_drv_req, 0, sizeof(s_drv_req));
    s_drv_req.op = DRV_OP_CREATE_DIR;
    s_drv_req.volume = vi;
    s_drv_req.parent_id = id;
    strncpy(s_drv_req.payload.name, segs[nsegs - 1], DRV_PATH_MAX - 1);
    s_drv_req.payload.name[DRV_PATH_MAX - 1] = '\0';
    r = vfs_drv_call(vi, &s_drv_req, &s_drv_resp);
    if (r < 0) { resp->ret = r; goto out; }
    if (s_drv_resp.ret < 0) { resp->ret = s_drv_resp.ret; goto out; }

    r = vfs_getattr(vi, s_drv_resp.u.item_id, &resp->item);
    if (r < 0) { resp->ret = r; goto out; }
    resp->ret = 0;

out:
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_delete(int token, int msg_len)
{
    vfs_resp_delete_t *resp = (vfs_resp_delete_t *)s_resp;
    if (msg_len < (int)sizeof(vfs_req_delete_t)) {
        resp->ret = ERR_INVAL;
        goto out;
    }
    vfs_req_delete_t *req = (vfs_req_delete_t *)s_req;

    char mount[64];
    char segs[VFS_MAX_DEPTH][VFS_SEG_MAX];
    int nsegs;
    int r = vfs_parse_url(req->path, mount, segs, &nsegs);
    if (r < 0) { resp->ret = r; goto out; }
    u32 vi;
    r = vfs_find_vol(mount, &vi);
    if (r < 0) { resp->ret = r; goto out; }
    if (nsegs == 0) { resp->ret = ERR_INVAL; goto out; }    /* no root delete */

    vfs_item_id_t id;
    r = vfs_lookup_path(vi, segs, nsegs, &id);
    if (r < 0) { resp->ret = r; goto out; }

    memset(&s_drv_req, 0, sizeof(s_drv_req));
    s_drv_req.op = DRV_OP_DELETE;
    s_drv_req.volume = vi;
    s_drv_req.item_id = id;
    s_drv_req.recursive = req->recursive;
    r = vfs_drv_call(vi, &s_drv_req, &s_drv_resp);
    if (r < 0) { resp->ret = r; goto out; }
    resp->ret = s_drv_resp.ret;

out:
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_open(int token, int msg_len)
{
    vfs_resp_open_t *resp = (vfs_resp_open_t *)s_resp;
    if (msg_len < (int)sizeof(vfs_req_open_t)) {
        resp->ret = ERR_INVAL;
        goto out;
    }
    vfs_req_open_t *req = (vfs_req_open_t *)s_req;

    char mount[64];
    char segs[VFS_MAX_DEPTH][VFS_SEG_MAX];
    int nsegs;
    int r = vfs_parse_url(req->path, mount, segs, &nsegs);
    if (r < 0) { resp->ret = r; goto out; }
    u32 vi;
    r = vfs_find_vol(mount, &vi);
    if (r < 0) { resp->ret = r; goto out; }
    vfs_vol_t *v = &s_vols[vi];
    if (nsegs == 0) { resp->ret = ERR_INVAL; goto out; }    /* no root open */

    /* Resolve the parent directory. */
    vfs_item_id_t id;
    r = vfs_lookup_path(vi, segs, nsegs - 1, &id);
    if (r < 0) { resp->ret = r; goto out; }

    /* Look up (or CREATE) the final component. */
    vfs_item_id_t fid;
    memset(&s_drv_req, 0, sizeof(s_drv_req));
    s_drv_req.op = DRV_OP_LOOKUP;
    s_drv_req.volume = vi;
    s_drv_req.parent_id = id;
    strncpy(s_drv_req.payload.name, segs[nsegs - 1], DRV_PATH_MAX - 1);
    s_drv_req.payload.name[DRV_PATH_MAX - 1] = '\0';
    r = vfs_drv_call(vi, &s_drv_req, &s_drv_resp);
    if (r < 0) { resp->ret = r; goto out; }

    if (s_drv_resp.ret < 0) {
        if (s_drv_resp.ret == ERR_NOENT && (req->flags & VFS_OPEN_CREATE)) {
            /* Create the file (drivers enforce volume read-only). */
            memset(&s_drv_req, 0, sizeof(s_drv_req));
            s_drv_req.op = DRV_OP_MKFILE;
            s_drv_req.volume = vi;
            s_drv_req.parent_id = id;
            strncpy(s_drv_req.payload.name, segs[nsegs - 1],
                    DRV_PATH_MAX - 1);
            s_drv_req.payload.name[DRV_PATH_MAX - 1] = '\0';
            r = vfs_drv_call(vi, &s_drv_req, &s_drv_resp);
            if (r < 0) { resp->ret = r; goto out; }
            if (s_drv_resp.ret < 0) { resp->ret = s_drv_resp.ret; goto out; }
        } else {
            resp->ret = s_drv_resp.ret;
            goto out;
        }
    }
    fid = s_drv_resp.u.item_id;

    /* Read-only volume rejects any WRITE access at open time. */
    if (v->read_only && (req->access & VFS_ACCESS_WRITE)) {
        resp->ret = VFS_ERR_READONLY;
        goto out;
    }

    /* Truncate on open (VFS_OPEN_TRUNCATE → driver truncate op). */
    if (req->flags & VFS_OPEN_TRUNCATE) {
        memset(&s_drv_req, 0, sizeof(s_drv_req));
        s_drv_req.op = DRV_OP_WRITE;
        s_drv_req.volume = vi;
        s_drv_req.item_id = fid;
        s_drv_req.offset = 0;
        s_drv_req.len = 0;
        r = vfs_drv_call(vi, &s_drv_req, &s_drv_resp);
        if (r < 0) { resp->ret = r; goto out; }
        if (s_drv_resp.ret < 0) { resp->ret = s_drv_resp.ret; goto out; }
    }

    vfs_handle_t htok;
    vfs_handle_ent_t *h = handle_alloc(vi, fid, req->access, req->flags,
                                       &htok);
    if (!h) {
        resp->ret = ERR_NOMEM;
        goto out;
    }
    resp->handle = htok;

    r = vfs_getattr(vi, fid, &resp->item);
    if (r < 0) { resp->ret = r; goto out; }

    /* APPEND (Phase 0 extra): start the r/w position at EOF. */
    if (req->flags & VFS_OPEN_APPEND)
        h->offset = resp->item.size;

    resp->ret = 0;

out:
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_read(int token, int msg_len)
{
    vfs_resp_read_t *resp = (vfs_resp_read_t *)s_resp;
    if (msg_len < (int)sizeof(vfs_req_read_t)) {
        resp->ret = ERR_INVAL;
        goto out;
    }
    vfs_req_read_t *req = (vfs_req_read_t *)s_req;
    if (req->len > VFS_MAX_READ) {
        resp->ret = ERR_INVAL;
        goto out;
    }

    vfs_handle_ent_t *h = handle_find(req->handle);
    if (!h) {
        resp->ret = VFS_ERR_STALE;
        goto out;
    }
    if (!(h->access & VFS_ACCESS_READ)) {
        resp->ret = VFS_ERR_PERM;
        goto out;
    }

    memset(&s_drv_req, 0, sizeof(s_drv_req));
    s_drv_req.op = DRV_OP_READ;
    s_drv_req.volume = h->vol_index;
    s_drv_req.item_id = h->item_id;
    s_drv_req.offset = req->offset;
    s_drv_req.len = req->len;
    int r = vfs_drv_call(h->vol_index, &s_drv_req, &s_drv_resp);
    if (r < 0) { resp->ret = r; goto out; }
    if (s_drv_resp.ret < 0) { resp->ret = s_drv_resp.ret; goto out; }

    i32 n = s_drv_resp.ret;
    memcpy(resp->data, s_drv_resp.u.data, (size_t)n);
    h->offset = req->offset + (u64)n;
    resp->ret = n;

out:
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_write(int token, int msg_len)
{
    vfs_resp_write_t *resp = (vfs_resp_write_t *)s_resp;
    if (msg_len < (int)sizeof(u32) * 3 + (int)sizeof(u64)) {   /* header */
        resp->ret = ERR_INVAL;
        goto out;
    }
    vfs_req_write_t *req = (vfs_req_write_t *)s_req;
    if (req->len > VFS_MAX_WRITE) {
        resp->ret = ERR_INVAL;
        goto out;
    }
    if (msg_len < (int)sizeof(u32) * 3 + (int)sizeof(u64) + (int)req->len) {
        resp->ret = ERR_INVAL;
        goto out;
    }

    vfs_handle_ent_t *h = handle_find(req->handle);
    if (!h) {
        resp->ret = VFS_ERR_STALE;
        goto out;
    }
    if (!(h->access & VFS_ACCESS_WRITE)) {
        resp->ret = VFS_ERR_PERM;
        goto out;
    }

    memset(&s_drv_req, 0, sizeof(s_drv_req));
    s_drv_req.op = DRV_OP_WRITE;
    s_drv_req.volume = h->vol_index;
    s_drv_req.item_id = h->item_id;
    s_drv_req.offset = req->offset;
    s_drv_req.len = req->len;
    memcpy(s_drv_req.payload.data, req->data, req->len);
    int r = vfs_drv_call(h->vol_index, &s_drv_req, &s_drv_resp);
    if (r < 0) { resp->ret = r; goto out; }
    if (s_drv_resp.ret < 0) { resp->ret = s_drv_resp.ret; goto out; }

    h->offset = req->offset + (u64)s_drv_resp.ret;
    resp->ret = s_drv_resp.ret;

out:
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_close(int token, int msg_len)
{
    vfs_resp_close_t *resp = (vfs_resp_close_t *)s_resp;
    if (msg_len < (int)sizeof(vfs_req_close_t)) {
        resp->ret = ERR_INVAL;
        goto out;
    }
    vfs_req_close_t *req = (vfs_req_close_t *)s_req;

    vfs_handle_ent_t *h = handle_find(req->handle);
    if (h) {
        memset(h, 0, sizeof(*h));
        resp->ret = 0;
        goto out;
    }
    for (int i = 0; i < MAX_ENUMS; i++) {
        if (s_enums[i].in_use && s_enums[i].token == req->handle) {
            memset(&s_enums[i], 0, sizeof(s_enums[i]));
            resp->ret = 0;
            goto out;
        }
    }
    resp->ret = VFS_ERR_STALE;

out:
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_enum_begin(int token, int msg_len)
{
    vfs_resp_enum_begin_t *resp = (vfs_resp_enum_begin_t *)s_resp;
    if (msg_len < (int)sizeof(vfs_req_enum_begin_t)) {
        resp->ret = ERR_INVAL;
        goto out;
    }
    vfs_req_enum_begin_t *req = (vfs_req_enum_begin_t *)s_req;

    char mount[64];
    char segs[VFS_MAX_DEPTH][VFS_SEG_MAX];
    int nsegs;
    int r = vfs_parse_url(req->path, mount, segs, &nsegs);
    if (r < 0) { resp->ret = r; goto out; }
    u32 vi;
    r = vfs_find_vol(mount, &vi);
    if (r < 0) { resp->ret = r; goto out; }

    vfs_item_id_t id;
    r = vfs_lookup_path(vi, segs, nsegs, &id);
    if (r < 0) { resp->ret = r; goto out; }

    vfs_item_info_t info;
    r = vfs_getattr(vi, id, &info);
    if (r < 0) { resp->ret = r; goto out; }
    if (info.type != VFS_ITEM_DIR) {
        resp->ret = ERR_INVAL;          /* can only enumerate dirs */
        goto out;
    }

    vfs_handle_t htok;
    if (!enum_alloc(vi, id, &htok)) {
        resp->ret = ERR_NOMEM;
        goto out;
    }
    resp->handle = htok;
    resp->ret = 0;

out:
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_enum_next(int token, int msg_len)
{
    vfs_resp_enum_next_t *resp = (vfs_resp_enum_next_t *)s_resp;
    if (msg_len < (int)sizeof(vfs_req_enum_next_t)) {
        resp->ret = ERR_INVAL;
        goto out;
    }
    vfs_req_enum_next_t *req = (vfs_req_enum_next_t *)s_req;

    vfs_enum_ent_t *e = NULL;
    for (int i = 0; i < MAX_ENUMS; i++) {
        if (s_enums[i].in_use && s_enums[i].token == req->handle) {
            e = &s_enums[i];
            break;
        }
    }
    if (!e) {
        resp->ret = VFS_ERR_STALE;
        goto out;
    }

    memset(&s_drv_req, 0, sizeof(s_drv_req));
    s_drv_req.op = DRV_OP_ENUM;
    s_drv_req.volume = e->vol_index;
    s_drv_req.parent_id = e->dir_id;
    s_drv_req.from = e->from;
    int r = vfs_drv_call(e->vol_index, &s_drv_req, &s_drv_resp);
    if (r < 0) { resp->ret = r; goto out; }
    if (s_drv_resp.ret < 0) { resp->ret = s_drv_resp.ret; goto out; }

    resp->count = s_drv_resp.u.en.count;
    for (u32 i = 0; i < resp->count; i++)
        resp->items[i] = s_drv_resp.u.en.items[i];
    e->from += resp->count;
    resp->ret = (i32)resp->count;

out:
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_stat_volume(int token, int msg_len)
{
    vfs_resp_stat_volume_t *resp = (vfs_resp_stat_volume_t *)s_resp;
    if (msg_len < (int)sizeof(vfs_req_stat_volume_t)) {
        resp->ret = ERR_INVAL;
        goto out;
    }
    vfs_req_stat_volume_t *req = (vfs_req_stat_volume_t *)s_req;

    char mount[64];
    char segs[VFS_MAX_DEPTH][VFS_SEG_MAX];
    int nsegs;
    int r = vfs_parse_url(req->path, mount, segs, &nsegs);
    if (r < 0) { resp->ret = r; goto out; }
    u32 vi;
    r = vfs_find_vol(mount, &vi);
    if (r < 0) { resp->ret = r; goto out; }

    memset(&s_drv_req, 0, sizeof(s_drv_req));
    s_drv_req.op = DRV_OP_STAT;
    s_drv_req.volume = vi;
    r = vfs_drv_call(vi, &s_drv_req, &s_drv_resp);
    if (r < 0) { resp->ret = r; goto out; }
    if (s_drv_resp.ret < 0) { resp->ret = s_drv_resp.ret; goto out; }

    resp->total_bytes = s_drv_resp.u.stat.total_bytes;
    resp->used_bytes = s_drv_resp.u.stat.used_bytes;
    resp->read_only = s_drv_resp.u.stat.read_only;
    resp->ret = 0;

out:
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

/* ====================================================================
 * Bookmark handlers (design §5 — security-scoped bookmarks, Phase 2)
 *
 * Create flow:   URL → itemID → perm-manager CHECK (denied ⇒ -EACCES
 *                + pending Powerbox query) → server record + opaque blob.
 * Resolve flow:  blob → record (authoritative) → item still exists
 *                (parent_id chain relocation when the driver moved it
 *                with a new ID) → perm-manager CHECK again (§9.4:
 *                vfs_server 每次 resolve 校验 — revocation is immediate)
 *                → temporary FileHandle.
 * Revoke flow:   blob → drop the server record.  The next resolve
 *                returns -EACCES (record gone ⇒ VFS_ERR_ACCESS).
 * ==================================================================== */

static void do_create_bookmark(int token, int msg_len)
{
    vfs_resp_create_bookmark_t *resp = (vfs_resp_create_bookmark_t *)s_resp;
    if (msg_len < (int)sizeof(vfs_req_create_bookmark_t)) {
        resp->ret = ERR_INVAL;
        goto out;
    }
    vfs_req_create_bookmark_t *req = (vfs_req_create_bookmark_t *)s_req;

    char mount[64];
    char segs[VFS_MAX_DEPTH][VFS_SEG_MAX];
    int nsegs;
    int r = vfs_parse_url(req->path, mount, segs, &nsegs);
    if (r < 0) { resp->ret = r; goto out; }
    u32 vi;
    r = vfs_find_vol(mount, &vi);
    if (r < 0) { resp->ret = r; goto out; }

    vfs_item_id_t id;
    r = vfs_lookup_path(vi, segs, nsegs, &id);
    if (r < 0) { resp->ret = r; goto out; }

    vfs_item_info_t info;
    r = vfs_getattr(vi, id, &info);
    if (r < 0) { resp->ret = r; goto out; }

    /* Authorization gate (Powerbox): denied ⇒ -EACCES (VFS_ERR_ACCESS),
     * with a pending query + UI_SHOW pushed by the perm-manager. */
    vfs_resource_t res;
    memset(&res, 0, sizeof(res));
    res.vol = s_vols[vi].uuid;
    res.id = id;
    r = perm_check(req->app_id_hash, &res, req->access, req->path);
    if (r < 0) { resp->ret = r; goto out; }

    u32 tok;
    vfs_bookmark_ent_t *b = bookmark_alloc(vi, id, req->access,
                                           req->app_id_hash,
                                           (u64)get_time(),
                                           req->expiry_ticks, &tok);
    if (!b) {
        resp->ret = ERR_NOMEM;
        goto out;
    }
    b->parent_id = info.parent_id;
    strncpy(b->name, info.name, sizeof(b->name) - 1);
    b->name[sizeof(b->name) - 1] = '\0';

    vfs_bookmark_t blob;
    bookmark_fill_blob(b, &res, info.parent_id, &blob);

    resp->bk_len = (u32)sizeof(blob);
    memcpy(resp->data, &blob, sizeof(blob));
    resp->ret = 0;

out:
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_resolve_bookmark(int token, int msg_len)
{
    vfs_resp_resolve_bookmark_t *resp = (vfs_resp_resolve_bookmark_t *)s_resp;
    if (msg_len < (int)sizeof(vfs_req_resolve_bookmark_t)) {
        resp->ret = ERR_INVAL;
        goto out;
    }
    vfs_req_resolve_bookmark_t *req = (vfs_req_resolve_bookmark_t *)s_req;
    if (req->bk_len < sizeof(vfs_bookmark_t) ||
        req->bk_len > VFS_BOOKMARK_MAX) {
        resp->ret = ERR_INVAL;
        goto out;
    }

    vfs_bookmark_t blob;
    memcpy(&blob, req->data, sizeof(blob));

    /* The server-side record is the source of truth.  A forged/foreign/
     * expired blob validates to NULL ⇒ -EACCES (VFS_ERR_ACCESS). */
    vfs_bookmark_ent_t *b = bookmark_validate(&blob);
    if (!b) {
        resp->ret = VFS_ERR_ACCESS;
        goto out;
    }

    /* Locate the item.  itemID is stable across DRV_OP_MOVE, but a
     * driver may re-ID on move (copy+delete): fall back to the
     * parent_id chain + stored name (design §5: "用 parent_id 链重新
     * 定位"). */
    vfs_item_info_t info;
    int r = vfs_getattr(b->vol_index, b->item_id, &info);
    if (r == ERR_NOENT && b->parent_id != 0) {
        memset(&s_drv_req, 0, sizeof(s_drv_req));
        s_drv_req.op = DRV_OP_LOOKUP;
        s_drv_req.volume = b->vol_index;
        s_drv_req.parent_id = b->parent_id;
        strncpy(s_drv_req.payload.name, b->name, DRV_PATH_MAX - 1);
        s_drv_req.payload.name[DRV_PATH_MAX - 1] = '\0';
        int r2 = vfs_drv_call(b->vol_index, &s_drv_req, &s_drv_resp);
        if (r2 >= 0 && s_drv_resp.ret >= 0) {
            b->item_id = s_drv_resp.u.item_id;  /* re-anchor record */
            r = vfs_getattr(b->vol_index, b->item_id, &info);
        }
    }
    if (r < 0) { resp->ret = r; goto out; }

    /* Re-authorize on EVERY resolve (§9.4) — a REVOKE'd grant takes
     * effect immediately, before any handle is handed out. */
    vfs_resource_t res;
    memset(&res, 0, sizeof(res));
    res.vol = s_vols[b->vol_index].uuid;
    res.id = b->item_id;
    r = perm_check(b->app_id_hash, &res, b->access, "");
    if (r < 0) { resp->ret = r; goto out; }

    vfs_handle_t htok;
    vfs_handle_ent_t *h = handle_alloc(b->vol_index, b->item_id,
                                       b->access, 0, &htok);
    if (!h) {
        resp->ret = ERR_NOMEM;
        goto out;
    }

    resp->handle = htok;
    resp->access = b->access;
    resp->item = info;
    resp->ret = 0;

out:
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_revoke_bookmark(int token, int msg_len)
{
    vfs_resp_revoke_bookmark_t *resp = (vfs_resp_revoke_bookmark_t *)s_resp;
    if (msg_len < (int)sizeof(vfs_req_revoke_bookmark_t)) {
        resp->ret = ERR_INVAL;
        goto out;
    }
    vfs_req_revoke_bookmark_t *req = (vfs_req_revoke_bookmark_t *)s_req;
    if (req->bk_len < sizeof(vfs_bookmark_t) ||
        req->bk_len > VFS_BOOKMARK_MAX) {
        resp->ret = ERR_INVAL;
        goto out;
    }

    vfs_bookmark_t blob;
    memcpy(&blob, req->data, sizeof(blob));

    vfs_bookmark_ent_t *b = bookmark_validate(&blob);
    if (b)
        memset(b, 0, sizeof(*b));   /* drop the server-side record */
    resp->ret = 0;                  /* idempotent: revoke is revoke */

out:
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

/* ====================================================================
 * VFS_OP_MOVE — move/rename an item (Phase 2)
 *
 * The server resolves src → itemID and dst_dir → parent itemID, then
 * hands both (plus an optional rename) to the driver.  The driver
 * keeps the itemID stable (design §5: 父级 ID 追踪 = 「文件移动后书签
 * 仍有效」的地基), so existing bookmarks keep working after a move.
 * ==================================================================== */

static void do_move(int token, int msg_len)
{
    vfs_resp_move_t *resp = (vfs_resp_move_t *)s_resp;
    if (msg_len < (int)sizeof(vfs_req_move_t)) {
        resp->ret = ERR_INVAL;
        goto out;
    }
    vfs_req_move_t *req = (vfs_req_move_t *)s_req;

    /* Source item. (scratch buffers are file-scope statics — see top of
     * file: do_move's 4 KB segment arrays don't fit the 4 KB user stack) */
    int snsegs, dnsegs;
    int r = vfs_parse_url(req->src, s_mount, s_segs, &snsegs);
    if (r < 0) { resp->ret = r; goto out; }
    r = vfs_parse_url(req->dst_dir, d_mount, d_segs, &dnsegs);
    if (r < 0) { resp->ret = r; goto out; }

    u32 svi, dvi;
    r = vfs_find_vol(s_mount, &svi);
    if (r < 0) { resp->ret = r; goto out; }
    r = vfs_find_vol(d_mount, &dvi);
    if (r < 0) { resp->ret = r; goto out; }
    if (svi != dvi) {                   /* Phase 2: no cross-volume move */
        resp->ret = VFS_ERR_PERM;
        goto out;
    }

    vfs_item_id_t sid;
    r = vfs_lookup_path(svi, s_segs, snsegs, &sid);
    if (r < 0) { resp->ret = r; goto out; }
    if (snsegs == 0) {
        resp->ret = ERR_INVAL;          /* can't move the volume root */
        goto out;
    }

    vfs_item_id_t did;
    r = vfs_lookup_path(dvi, d_segs, dnsegs, &did);
    if (r < 0) { resp->ret = r; goto out; }

    vfs_item_info_t dinfo;
    r = vfs_getattr(dvi, did, &dinfo);
    if (r < 0) { resp->ret = r; goto out; }
    if (dinfo.type != VFS_ITEM_DIR) {
        resp->ret = ERR_INVAL;          /* dst must be a directory */
        goto out;
    }

    memset(&s_drv_req, 0, sizeof(s_drv_req));
    s_drv_req.op = DRV_OP_MOVE;
    s_drv_req.volume = svi;
    s_drv_req.item_id = sid;
    s_drv_req.parent_id = did;
    if (req->new_name[0] != '\0') {     /* optional rename */
        strncpy(s_drv_req.payload.name, req->new_name, DRV_PATH_MAX - 1);
        s_drv_req.payload.name[DRV_PATH_MAX - 1] = '\0';
    }
    r = vfs_drv_call(svi, &s_drv_req, &s_drv_resp);
    if (r < 0) { resp->ret = r; goto out; }
    if (s_drv_resp.ret < 0) { resp->ret = s_drv_resp.ret; goto out; }

    r = vfs_getattr(svi, sid, &resp->item);
    if (r < 0) { resp->ret = r; goto out; }
    resp->ret = 0;

out:
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void vfs_handle_request(int token, u32 op, int msg_len)
{
    switch (op) {
    case VFS_OP_GET_ITEM:       do_get_item(token, msg_len); break;
    case VFS_OP_CREATE_DIR:     do_create_dir(token, msg_len); break;
    case VFS_OP_DELETE_ITEM:    do_delete(token, msg_len); break;
    case VFS_OP_OPEN_ITEM:      do_open(token, msg_len); break;
    case VFS_OP_READ:           do_read(token, msg_len); break;
    case VFS_OP_WRITE:          do_write(token, msg_len); break;
    case VFS_OP_CLOSE:          do_close(token, msg_len); break;
    case VFS_OP_ENUM_BEGIN:     do_enum_begin(token, msg_len); break;
    case VFS_OP_ENUM_NEXT:      do_enum_next(token, msg_len); break;
    case VFS_OP_MOUNT:          do_mount(token, msg_len); break;
    case VFS_OP_STAT_VOLUME:    do_stat_volume(token, msg_len); break;
    case VFS_OP_CREATE_BOOKMARK:  do_create_bookmark(token, msg_len); break;
    case VFS_OP_RESOLVE_BOOKMARK: do_resolve_bookmark(token, msg_len); break;
    case VFS_OP_REVOKE_BOOKMARK:  do_revoke_bookmark(token, msg_len); break;
    case VFS_OP_MOVE:             do_move(token, msg_len); break;
    /* VFS_OP_UNMOUNT (14): Phase 2 — answer a clean ERR_INVAL. */
    default: {
        i32 *resp = (i32 *)s_resp;
        *resp = ERR_INVAL;
        (void)ipc_reply(token, resp, (int)sizeof(i32));
        break;
    }
    }
}

/* ====================================================================
 * Entry point (vfs_server process main)
 *
 * A2 framework note (RESERVED — design §7.2): server-spawned drivers
 * would create the driver process here (process_create + blob fetch),
 * wait for its "vfs.fs.<driver>" port, then hand it a MOUNT config.
 * Phase 0 keeps A1: the manager spawns fs_mem_driver, which performs
 * the driver-initiated MOUNT handshake against this server.
 * ==================================================================== */

int main(void)
{
    printf("vfs: starting VFS server\n");

    memset(s_vols, 0, sizeof(s_vols));
    memset(s_handles, 0, sizeof(s_handles));
    memset(s_enums, 0, sizeof(s_enums));
    memset(s_bookmarks, 0, sizeof(s_bookmarks));
    s_perm_port = -1;
    s_rng_state = (u32)get_time() ^ ((u32)get_pid() << 16) ^ 0x9e3779b9u;

    int port = ipc_port_create();
    if (port < 0) {
        printf("vfs: ipc_port_create failed (%d)\n", port);
        thread_exit(1);
    }
    int ret = port_register("vfs", port);
    if (ret < 0) {
        printf("vfs: port_register('vfs') failed (%d)\n", ret);
        thread_exit(1);
    }
    printf("vfs: port %d registered as 'vfs'\n", port);

    printf("vfs: serving (%d mounts configured; A2 spawn framework "
           "reserved)\n", MOUNT_CFG_COUNT);

    for (;;) {
        int msg_len = (int)sizeof(s_req);
        int token = 0;
        ret = ipc_recv(port, s_req, &msg_len, &token);
        if (ret < 0) {
            printf("vfs: ipc_recv failed (%d)\n", ret);
            thread_exit(1);
        }
        if (msg_len < (int)sizeof(u32)) {       /* no op code */
            i32 *resp = (i32 *)s_resp;
            *resp = ERR_INVAL;
            (void)ipc_reply(token, resp, (int)sizeof(i32));
            continue;
        }
        u32 op = *(u32 *)s_req;
        vfs_handle_request(token, op, msg_len);
    }
}
