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
 *   - volumes: static mount table {System RO, Users RW, Disk RW}, filled
 *     by the driver-initiated MOUNT handshake (decision A1 — fs_mem_driver
 *     is manager-spawned and self-mounts; the virtio_blk driver mounts
 *     the "Disk" row once it registers).  The server-side spawn of §7.2
 *     is RESERVED as an A2 framework: no code, just comments.
 *   - handles: file handles AND enumerators live in two token tables;
 *     a handle is a random 32-bit token (decision Q1: fully
 *     userspace, kernel untouched).  VFS_OP_CLOSE releases either.
 *   - ops implemented: GET_ITEM, CREATE_DIR, DELETE_ITEM, OPEN_ITEM,
 *     READ, WRITE, CLOSE, ENUM_BEGIN, ENUM_NEXT, MOUNT, UNMOUNT (14),
 *     STAT_VOLUME, bookmark ops (10-12), MOVE (16) and WHOAMI (17).
 *     UNMOUNT drops the volume slot and stales its handles/enums/
 *     bookmarks; a dead driver (IPC transport error) is unmounted
 *     lazily the same way.
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

#define MAX_VOLS      VFS_MAX_VOLS /* mount slots (protocol constant) */
#define MAX_HANDLES   64  /* open file handles */
#define MAX_ENUMS     16  /* active enumerators */
#define MAX_BOOKMARKS 32  /* security-scoped bookmarks */
#define VFS_MAX_DEPTH 8   /* URL path segments */
#define VFS_SEG_MAX   256 /* per-segment buffer */

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

#define MOUNT_CFG_COUNT 3
static const vfs_mount_cfg_t s_mount_cfg[MOUNT_CFG_COUNT] = {
    {"System", "mem", 1},
    {"Users", "mem", 0},
    {"Disk", "virtio_blk", 0},
};

/* ====================================================================
 * Volume table (filled by MOUNT handshakes)
 * ==================================================================== */

typedef struct {
    int           mounted;
    char          mount_name[64];
    char          driver_name[64];
    vfs_uuid_t    uuid;
    vfs_item_id_t root_item_id;
    u32           read_only;
    u64           owner_subject; /* subject of the MOUNT handshake — only
                                  * it may UNMOUNT (§7.2 A1 binding) */
    int           drv_port; /* resolved lazily: "vfs.fs.<driver>" */
    u32           drv_vol;  /* driver-side volume index (§7.1:
                             * driver numbers its volumes 0,1,2…
                             * in MOUNT order; the server slot
                             * index differs once a driver mounts
                             * fewer volumes than the slots above
                             * it, so drv_req.volume must use
                             * THIS, not the slot index). */
} vfs_vol_t;

static vfs_vol_t s_vols[MAX_VOLS];

/* ====================================================================
 * File-handle table (decision Q1: fully userspace, random tokens)
 * ==================================================================== */

typedef struct {
    int           in_use;
    vfs_handle_t  token; /* random 32-bit value handed to client */
    u32           vol_index;
    vfs_item_id_t item_id;
    u32           access; /* VFS_ACCESS_* (perm-granted ∩ requested) */
    u32           flags;  /* VFS_OPEN_* */
    /* P1 authz: identity captured at open time so do_read/do_write can
     * re-run perm_check on every operation — a perm_revoke between open
     * and read/write takes effect immediately instead of being shadowed
     * by a stale handle.  resource = (volume UUID + itemID) lets the
     * perm-manager locate the grant without re-resolving the path. */
    u64            subject_id; /* opener's kernel subject (unforgeable) */
    vfs_resource_t resource;   /* (vol uuid, itemID) for perm_check */
} vfs_handle_ent_t;

static vfs_handle_ent_t s_handles[MAX_HANDLES];

/* ====================================================================
 * Enumerator table (design §3.4 — server-side iteration state)
 * ==================================================================== */

typedef struct {
    int           in_use;
    vfs_handle_t  token;
    u32           vol_index;
    vfs_item_id_t dir_id;
    u32           from; /* next child index */
    /* P2 authz: identity captured at ENUM_BEGIN so ENUM_NEXT can
     * re-run perm_check on every batch — a perm_revoke between
     * batches takes effect immediately.  The enumerator is bound to
     * its creator's subject (a different subject cannot step into a
     * stale enumerator). */
    u64            subject_id;
    vfs_resource_t resource; /* (vol uuid, dir itemID) */
} vfs_enum_ent_t;

static vfs_enum_ent_t s_enums[MAX_ENUMS];

/* ====================================================================
 * Bookmark table (design §5.2 — server-side authoritative record)
 *
 * The blob handed to the client is OPAQUE: it carries only denormalized
 * payload fields (resource, access, creator subject, timestamps) for
 * display — no server-side token, so nothing in the blob can mint
 * access on its own.  The record below is the source of truth —
 * validation matches the blob fields against a record and binds the
 * capability to its creator's subject, so a blob copied from another
 * app (or hand-crafted) cannot be used.
 * ==================================================================== */

typedef struct {
    int           in_use;
    u32           token; /* random 32-bit value (informational; validation is field-match) */
    u32           vol_index;
    vfs_item_id_t item_id;
    vfs_item_id_t parent_id;  /* parent at creation (move tracking) */
    char          name[256];  /* name at creation (relocation) */
    u32           access;     /* granted VFS_ACCESS_* */
    u64           subject_id; /* creator's kernel subject (unforgeable) */
    u64           created_ticks;
    u64           expiry_ticks; /* 0 = never */
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

static u32 rng_next(void) {
    /* Simple LCG — tokens only need to be unpredictable-ish, not
     * cryptographically secure.  Seeded from time+pid at startup. */
    s_rng_state = s_rng_state * 1103515245u + 12345u;
    return s_rng_state;
}

/*
 * Drop a mounted volume: free its mount slot so a later re-MOUNT can
 * re-fill it, and stale EVERY handle / enumerator / bookmark that
 * points into it (later use returns VFS_ERR_STALE).  Zeroing the
 * bookmarks is essential — otherwise a re-MOUNT into the same slot
 * would silently alias the old Phase-2 bookmarks to the new volume.
 * Shared by VFS_OP_UNMOUNT and the lazy dead-driver cleanup below.
 */
static void vfs_vol_drop(u32 vol_index) {
    if (vol_index >= MAX_VOLS || !s_vols[vol_index].mounted)
        return;

    for (int i = 0; i < MAX_HANDLES; i++)
        if (s_handles[i].in_use && s_handles[i].vol_index == vol_index)
            memset(&s_handles[i], 0, sizeof(s_handles[i]));
    for (int i = 0; i < MAX_ENUMS; i++)
        if (s_enums[i].in_use && s_enums[i].vol_index == vol_index)
            memset(&s_enums[i], 0, sizeof(s_enums[i]));
    for (int i = 0; i < MAX_BOOKMARKS; i++)
        if (s_bookmarks[i].in_use && s_bookmarks[i].vol_index == vol_index)
            memset(&s_bookmarks[i], 0, sizeof(s_bookmarks[i]));

    memset(&s_vols[vol_index], 0, sizeof(s_vols[vol_index]));
}

/*
 * One synchronous call to the storage driver for vol_index.  Returns
 * the IPC transport error (<0), or 0 with the driver's business result
 * in s_drv_resp.ret.  The driver port is resolved lazily so mount
 * timing does not matter.
 */
static int vfs_drv_call(u32 vol_index, drv_req_t *req, drv_resp_t *resp) {
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
    int ret      = ipc_call(v->drv_port, req, (int)sizeof(*req), resp, &resp_len);
    if (ret < 0) {
        /* Lazy dead-driver cleanup: Phase 1 has no unmount initiator —
         * a driver process can simply die, so an IPC transport error is
         * the only unmount signal.  Drop EVERY volume owned by this
         * driver (one driver can mount several, e.g. "mem") and stale
         * their handles/enums/bookmarks. */
        printf("vfs: driver '%s' unreachable (%d) — dropping its "
               "volume(s)\n",
               v->driver_name,
               ret);
        for (u32 i = 0; i < MAX_VOLS; i++)
            if (s_vols[i].mounted && strcmp(s_vols[i].driver_name, v->driver_name) == 0)
                vfs_vol_drop(i);
    }
    return ret;
}

/*
 * Synchronous authorization check against the perm-manager (design
 * §9.4: "vfs_server 每次 open/resolve 校验").  Returns 0 when a grant
 * covering `access` exists; VFS_ERR_ACCESS when denied (perm-manager
 * also creates the PENDING query + UI_SHOW push); or the transport
 * error when "perm" is unreachable.  The perm port is resolved lazily
 * so service start order does not matter.
 *
 * P1 地基: `subject_id` is the REQUESTING subject, taken from this
 * server's ipc_recv_from of the client message — never from the app's
 * own bytes.  The perm-engine trusts it because it originates from the
 * kernel (unforgeable).
 *
 * P2 抹位: `granted` (optional, may be NULL) receives the access mask
 * the perm-manager actually approved — the intersection of the request
 * and the covering grant.  Callers that will hand the capability to
 * the client (open/create_bookmark) MUST pass a buffer and store the
 * result; the client must never see the requested mask when only part
 * of it was granted.
 */
static int
perm_check(const vfs_resource_t *res, u32 access, const char *url, u64 subject_id, u32 *granted) {
    if (s_perm_port < 0) {
        s_perm_port = port_get(PERM_PORT_NAME);
        if (s_perm_port < 0)
            return s_perm_port;
    }

    perm_req_check_t req;
    memset(&req, 0, sizeof(req));
    req.op         = PERM_OP_CHECK;
    req.resource   = *res;
    req.access     = access;
    req.subject_id = subject_id;
    strncpy(req.url, url, sizeof(req.url) - 1);
    req.url[sizeof(req.url) - 1] = '\0';

    perm_resp_check_t resp;
    int               resp_len = (int)sizeof(resp);
    int               r        = ipc_call(s_perm_port, &req, (int)sizeof(req), &resp, &resp_len);
    if (r < 0)
        return r;
    if (granted)
        *granted = (resp.ret == 0) ? resp.granted : 0;
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
static int vfs_parse_url(const char *url, char mount[64], char segs[][VFS_SEG_MAX], int *nsegs) {
    const char *p = url;
    while (*p == '/')
        p++;
    if (*p == '\0')
        return ERR_INVAL;

    char first[VFS_PATH_MAX];
    int  flen = 0;
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
        int   l   = 0;
        while (*p && *p != '/' && l < VFS_SEG_MAX - 1)
            seg[l++] = *p++;
        seg[l] = '\0';
        while (*p == '/')
            p++;
        if (l > 0) {
            /* Security: reject ".." (parent traversal) and "." (cwd)
             * path segments.  The current in-memory driver happens to
             * treat them as ordinary names, but a future real FS would
             * resolve them — allowing traversal outside the intended
             * sub-tree.  Fail early so the rule is uniform. */
            if (strcmp(seg, "..") == 0 || strcmp(seg, ".") == 0)
                return ERR_INVAL;
            (*nsegs)++;
        }
    }
    return 0;
}

static int vfs_find_vol(const char *mount, u32 *vol_index) {
    for (u32 i = 0; i < MAX_VOLS; i++) {
        if (s_vols[i].mounted && strcmp(s_vols[i].mount_name, mount) == 0) {
            *vol_index = i;
            return 0;
        }
    }
    return ERR_NOENT; /* volume not mounted */
}

/* Walk a segment chain from the volume root via parent-ID LOOKUPs. */
static int
vfs_lookup_path(u32 vol_index, char segs[][VFS_SEG_MAX], int nsegs, vfs_item_id_t *out_id) {
    vfs_vol_t    *v   = &s_vols[vol_index];
    vfs_item_id_t cur = v->root_item_id;

    for (int i = 0; i < nsegs; i++) {
        memset(&s_drv_req, 0, sizeof(s_drv_req));
        s_drv_req.op        = DRV_OP_LOOKUP;
        s_drv_req.volume    = s_vols[vol_index].drv_vol;
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

static int vfs_getattr(u32 vol_index, vfs_item_id_t id, vfs_item_info_t *out) {
    memset(&s_drv_req, 0, sizeof(s_drv_req));
    s_drv_req.op      = DRV_OP_GETATTR;
    s_drv_req.volume  = s_vols[vol_index].drv_vol;
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

static vfs_handle_ent_t *handle_alloc(u32                   vol_index,
                                      vfs_item_id_t         id,
                                      u32                   access,
                                      u32                   flags,
                                      u64                   subject_id,
                                      const vfs_resource_t *res,
                                      vfs_handle_t         *tok) {
    for (int i = 0; i < MAX_HANDLES; i++) {
        if (!s_handles[i].in_use) {
            vfs_handle_ent_t *h = &s_handles[i];
            do {
                h->token = rng_next();
            } while (h->token == 0);
            h->in_use     = 1;
            h->vol_index  = vol_index;
            h->item_id    = id;
            h->access     = access;
            h->flags      = flags;
            h->subject_id = subject_id;
            if (res)
                h->resource = *res;
            else {
                h->resource.vol = s_vols[vol_index].uuid;
                h->resource.id  = id;
            }
            *tok = h->token;
            return h;
        }
    }
    return NULL;
}

static vfs_handle_ent_t *handle_find(vfs_handle_t tok) {
    for (int i = 0; i < MAX_HANDLES; i++)
        if (s_handles[i].in_use && s_handles[i].token == tok)
            return &s_handles[i];
    return NULL;
}

static vfs_enum_ent_t *enum_alloc(u32                   vol_index,
                                  vfs_item_id_t         dir_id,
                                  u64                   subject_id,
                                  const vfs_resource_t *res,
                                  vfs_handle_t         *tok) {
    for (int i = 0; i < MAX_ENUMS; i++) {
        if (!s_enums[i].in_use) {
            vfs_enum_ent_t *e = &s_enums[i];
            do {
                e->token = rng_next();
            } while (e->token == 0);
            e->in_use     = 1;
            e->vol_index  = vol_index;
            e->dir_id     = dir_id;
            e->from       = 0;
            e->subject_id = subject_id;
            if (res)
                e->resource = *res;
            else {
                e->resource.vol = s_vols[vol_index].uuid;
                e->resource.id  = dir_id;
            }
            *tok = e->token;
            return e;
        }
    }
    return NULL;
}

static vfs_bookmark_ent_t *bookmark_alloc(
    u32 vol_index, vfs_item_id_t id, u32 access, u64 subject_id, u64 created, u64 expiry, u32 *tok) {
    for (int i = 0; i < MAX_BOOKMARKS; i++) {
        if (!s_bookmarks[i].in_use) {
            vfs_bookmark_ent_t *b = &s_bookmarks[i];
            do {
                b->token = rng_next();
            } while (b->token == 0);
            b->in_use        = 1;
            b->vol_index     = vol_index;
            b->item_id       = id;
            b->parent_id     = 0; /* filled by caller via getattr */
            b->access        = access;
            b->subject_id    = subject_id;
            b->created_ticks = created;
            b->expiry_ticks  = expiry;
            *tok             = b->token;
            return b;
        }
    }
    return NULL;
}

/*
 * Fill a vfs_bookmark_t blob from a server-side record.  Phase 2:
 * mac[16] stays zero (real MAC is Phase 3, docs §5 "阶段化说明");
 * validity comes from the server-side record, not the blob bytes.
 */
static void bookmark_fill_blob(const vfs_bookmark_ent_t *b,
                               const vfs_resource_t     *res,
                               vfs_item_id_t             parent_id,
                               vfs_bookmark_t           *blob) {
    memset(blob, 0, sizeof(*blob));
    blob->magic         = VFS_BOOKMARK_MAGIC;
    blob->version       = VFS_BOOKMARK_VERSION;
    blob->payload_len   = (u32)(sizeof(*blob) - offsetof(vfs_bookmark_t, resource));
    blob->subject_id    = b->subject_id;
    blob->resource      = *res;
    blob->parent_id     = parent_id;
    blob->access        = b->access;
    blob->created_ticks = b->created_ticks;
    blob->expiry_ticks  = b->expiry_ticks;
}

/*
 * Validate a client-supplied blob against the server-side record.
 * The blob is only a capability carrier: every payload field must
 * match a record (a hand-crafted blob cannot mint access).  The
 * blob carries no server-side token, so the record is found by field
 * match; identity is bound separately to the caller's ipc_recv_from
 * subject in do_resolve_bookmark (the blob's own subject field is
 * client-held and forgeable).  Returns the record, or NULL (blob
 * invalid).
 */
static vfs_bookmark_ent_t *bookmark_validate(const vfs_bookmark_t *blob) {
    if (blob->magic != VFS_BOOKMARK_MAGIC || blob->version != VFS_BOOKMARK_VERSION)
        return NULL;

    for (int i = 0; i < MAX_BOOKMARKS; i++) {
        vfs_bookmark_ent_t *b = &s_bookmarks[i];
        if (!b->in_use)
            continue;
        if (b->vol_index >= MAX_VOLS || !s_vols[b->vol_index].mounted)
            continue;
        if (s_vols[b->vol_index].uuid.hi != blob->resource.vol.hi ||
            s_vols[b->vol_index].uuid.lo != blob->resource.vol.lo)
            continue;
        if (b->item_id != blob->resource.id || b->parent_id != blob->parent_id ||
            b->access != blob->access || b->subject_id != blob->subject_id ||
            b->created_ticks != blob->created_ticks || b->expiry_ticks != blob->expiry_ticks)
            continue;
        if (b->expiry_ticks != 0 && (u64)get_time() > b->expiry_ticks) {
            memset(b, 0, sizeof(*b)); /* expired: drop the record */
            return NULL;
        }
        return b;
    }
    return NULL;
}

/* ====================================================================
 * Protocol handlers (each replies via the shared s_resp buffer)
 * ==================================================================== */

static void do_mount(int token, int msg_len, u64 caller_subject) {
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
        resp->ret = VFS_ERR_PERM; /* unknown/unexpected mount */
        goto out;
    }

    for (u32 i = 0; i < MAX_VOLS; i++) {
        if (s_vols[i].mounted && strcmp(s_vols[i].mount_name, req->mount_name) == 0) {
            resp->ret = ERR_BUSY; /* already mounted */
            goto out;
        }
    }
    for (u32 i = 0; i < MAX_VOLS; i++) {
        if (!s_vols[i].mounted) {
            vfs_vol_t *v = &s_vols[i];
            memset(v, 0, sizeof(*v));
            /* Driver-side volume index (§7.1): the driver numbers its
             * volumes 0,1,2… in MOUNT order, and the server slot index
             * can differ (e.g. "Disk" lands in slot 2 but is volume 0
             * of the virtio_blk driver).  Count volumes this driver
             * already has mounted — computed BEFORE v->mounted = 1 so
             * the current (still unmounted) entry is excluded and the
             * count is exactly the driver-side index for this volume. */
            v->drv_vol = 0;
            for (u32 j = 0; j < MAX_VOLS; j++)
                if (s_vols[j].mounted && strcmp(s_vols[j].driver_name, req->driver_name) == 0)
                    v->drv_vol++;
            v->mounted       = 1;
            v->owner_subject = caller_subject; /* A1 identity binding:
                                                * only this subject may
                                                * UNMOUNT the volume */
            strncpy(v->mount_name, req->mount_name, sizeof(v->mount_name) - 1);
            strncpy(v->driver_name, req->driver_name, sizeof(v->driver_name) - 1);
            v->uuid         = req->uuid;
            v->root_item_id = req->root_item_id;
            v->read_only    = req->read_only;
            v->drv_port     = -1; /* resolved lazily on first use */
            resp->ret       = 0;
            printf("vfs: volume '%s' mounted (driver '%s', %s)\n",
                   v->mount_name,
                   v->driver_name,
                   v->read_only ? "RO" : "RW");
            goto out;
        }
    }
    resp->ret = ERR_NOMEM; /* mount table full */

out:
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_unmount(int token, int msg_len, u64 caller_subject) {
    vfs_resp_unmount_t *resp = (vfs_resp_unmount_t *)s_resp;
    if (msg_len < (int)sizeof(vfs_req_unmount_t)) {
        resp->ret = ERR_INVAL;
        goto out;
    }
    vfs_req_unmount_t *req = (vfs_req_unmount_t *)s_req;

    /* Only a driver that could mount this row may unmount it (same
     * gate as do_mount: exact match against the static config). */
    int cfg_ok = 0;
    for (u32 i = 0; i < MOUNT_CFG_COUNT; i++) {
        if (strcmp(s_mount_cfg[i].mount_name, req->mount_name) == 0 &&
            strcmp(s_mount_cfg[i].driver_name, req->driver_name) == 0) {
            cfg_ok = 1;
            break;
        }
    }
    if (!cfg_ok) {
        resp->ret = VFS_ERR_PERM;
        goto out;
    }

    int vi = -1;
    for (u32 i = 0; i < MAX_VOLS; i++) {
        if (s_vols[i].mounted && strcmp(s_vols[i].mount_name, req->mount_name) == 0) {
            vi = (int)i;
            break;
        }
    }
    if (vi < 0) {
        resp->ret = ERR_NOENT; /* not mounted */
        goto out;
    }

    /* A1 identity binding: the volume was mounted by a specific kernel
     * subject (recorded in do_mount).  Only that subject may unmount
     * it — an arbitrary client must not be able to tear down a volume
     * (a denial-of-service vector).  The subject comes from
     * ipc_recv_from, never from the request bytes. */
    if (s_vols[vi].owner_subject != 0 && s_vols[vi].owner_subject != caller_subject) {
        resp->ret = VFS_ERR_ACCESS;
        goto out;
    }

    printf("vfs: volume '%s' unmounted (driver '%s')\n",
           s_vols[vi].mount_name,
           s_vols[vi].driver_name);
    vfs_vol_drop((u32)vi);
    resp->ret = 0;

out:
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_get_item(int token, int msg_len) {
    vfs_resp_get_item_t *resp = (vfs_resp_get_item_t *)s_resp;
    if (msg_len < (int)sizeof(vfs_req_get_item_t)) {
        resp->ret = ERR_INVAL;
        goto out;
    }
    vfs_req_get_item_t *req = (vfs_req_get_item_t *)s_req;

    char mount[64];
    char segs[VFS_MAX_DEPTH][VFS_SEG_MAX];
    int  nsegs;
    int  r = vfs_parse_url(req->path, mount, segs, &nsegs);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }
    u32 vi;
    r = vfs_find_vol(mount, &vi);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }

    vfs_item_id_t id;
    r = vfs_lookup_path(vi, segs, nsegs, &id);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }

    r = vfs_getattr(vi, id, &resp->item);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }
    resp->ret = 0;

out:
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_create_dir(int token, int msg_len, u64 caller_subject) {
    vfs_resp_create_dir_t *resp = (vfs_resp_create_dir_t *)s_resp;
    if (msg_len < (int)sizeof(vfs_req_create_dir_t)) {
        resp->ret = ERR_INVAL;
        goto out;
    }
    vfs_req_create_dir_t *req = (vfs_req_create_dir_t *)s_req;

    char mount[64];
    char segs[VFS_MAX_DEPTH][VFS_SEG_MAX];
    int  nsegs;
    int  r = vfs_parse_url(req->path, mount, segs, &nsegs);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }
    u32 vi;
    r = vfs_find_vol(mount, &vi);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }
    if (nsegs == 0) {
        resp->ret = ERR_INVAL;
        goto out;
    } /* no root mkdir */

    vfs_item_id_t id;
    r = vfs_lookup_path(vi, segs, nsegs - 1, &id);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }

    /* P2 authz gate: creating a dir writes into its parent directory.
     * 身份完全取自 ipc_recv_from 的 caller_subject（不可伪造）；
     * 仅角色链约束。在驱动改动文件系统之前校验。 */
    vfs_resource_t res;
    res.vol = s_vols[vi].uuid;
    res.id  = id;
    r       = perm_check(&res, VFS_ACCESS_WRITE, req->path, caller_subject, NULL);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }

    memset(&s_drv_req, 0, sizeof(s_drv_req));
    s_drv_req.op        = DRV_OP_CREATE_DIR;
    s_drv_req.volume    = s_vols[vi].drv_vol;
    s_drv_req.parent_id = id;
    strncpy(s_drv_req.payload.name, segs[nsegs - 1], DRV_PATH_MAX - 1);
    s_drv_req.payload.name[DRV_PATH_MAX - 1] = '\0';
    r                                        = vfs_drv_call(vi, &s_drv_req, &s_drv_resp);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }
    if (s_drv_resp.ret < 0) {
        resp->ret = s_drv_resp.ret;
        goto out;
    }

    r = vfs_getattr(vi, s_drv_resp.u.item_id, &resp->item);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }
    resp->ret = 0;

out:
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_delete(int token, int msg_len, u64 caller_subject) {
    vfs_resp_delete_t *resp = (vfs_resp_delete_t *)s_resp;
    if (msg_len < (int)sizeof(vfs_req_delete_t)) {
        resp->ret = ERR_INVAL;
        goto out;
    }
    vfs_req_delete_t *req = (vfs_req_delete_t *)s_req;

    char mount[64];
    char segs[VFS_MAX_DEPTH][VFS_SEG_MAX];
    int  nsegs;
    int  r = vfs_parse_url(req->path, mount, segs, &nsegs);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }
    u32 vi;
    r = vfs_find_vol(mount, &vi);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }
    if (nsegs == 0) {
        resp->ret = ERR_INVAL;
        goto out;
    } /* no root delete */

    vfs_item_id_t id;
    r = vfs_lookup_path(vi, segs, nsegs, &id);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }

    /* P2 authz gate: delete mutates the volume.  身份完全取自
     * ipc_recv_from 的 caller_subject（不可伪造）；仅角色链约束。
     * 在驱动改动文件系统之前校验。 */
    vfs_resource_t res;
    res.vol = s_vols[vi].uuid;
    res.id  = id;
    r       = perm_check(&res, VFS_ACCESS_WRITE, req->path, caller_subject, NULL);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }

    memset(&s_drv_req, 0, sizeof(s_drv_req));
    s_drv_req.op        = DRV_OP_DELETE;
    s_drv_req.volume    = s_vols[vi].drv_vol;
    s_drv_req.item_id   = id;
    s_drv_req.recursive = req->recursive;
    r                   = vfs_drv_call(vi, &s_drv_req, &s_drv_resp);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }
    resp->ret = s_drv_resp.ret;

out:
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_open(int token, int msg_len, u64 caller_subject) {
    vfs_resp_open_t *resp = (vfs_resp_open_t *)s_resp;
    if (msg_len < (int)sizeof(vfs_req_open_t)) {
        resp->ret = ERR_INVAL;
        goto out;
    }
    vfs_req_open_t *req = (vfs_req_open_t *)s_req;

    char mount[64];
    char segs[VFS_MAX_DEPTH][VFS_SEG_MAX];
    int  nsegs;
    int  r = vfs_parse_url(req->path, mount, segs, &nsegs);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }
    u32 vi;
    r = vfs_find_vol(mount, &vi);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }
    vfs_vol_t *v = &s_vols[vi];
    if (nsegs == 0) {
        resp->ret = ERR_INVAL;
        goto out;
    } /* no root open */

    /* Resolve the parent directory. */
    vfs_item_id_t id;
    r = vfs_lookup_path(vi, segs, nsegs - 1, &id);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }

    /* Look up (or CREATE) the final component. */
    vfs_item_id_t fid;
    memset(&s_drv_req, 0, sizeof(s_drv_req));
    s_drv_req.op        = DRV_OP_LOOKUP;
    s_drv_req.volume    = s_vols[vi].drv_vol;
    s_drv_req.parent_id = id;
    strncpy(s_drv_req.payload.name, segs[nsegs - 1], DRV_PATH_MAX - 1);
    s_drv_req.payload.name[DRV_PATH_MAX - 1] = '\0';
    r                                        = vfs_drv_call(vi, &s_drv_req, &s_drv_resp);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }

    if (s_drv_resp.ret < 0) {
        if (s_drv_resp.ret == ERR_NOENT && (req->flags & VFS_OPEN_CREATE)) {
            /* Create the file (drivers enforce volume read-only). */
            memset(&s_drv_req, 0, sizeof(s_drv_req));
            s_drv_req.op        = DRV_OP_MKFILE;
            s_drv_req.volume    = s_vols[vi].drv_vol;
            s_drv_req.parent_id = id;
            strncpy(s_drv_req.payload.name, segs[nsegs - 1], DRV_PATH_MAX - 1);
            s_drv_req.payload.name[DRV_PATH_MAX - 1] = '\0';
            r                                        = vfs_drv_call(vi, &s_drv_req, &s_drv_resp);
            if (r < 0) {
                resp->ret = r;
                goto out;
            }
            if (s_drv_resp.ret < 0) {
                resp->ret = s_drv_resp.ret;
                goto out;
            }
        } else {
            resp->ret = s_drv_resp.ret;
            goto out;
        }
    }
    fid = s_drv_resp.u.item_id;

    /* OPEN_ITEM is a file open: directories are enumerated via
     * VFS_OP_ENUM_BEGIN, never opened as file handles (a dir handle
     * would carry READ/WRITE access against a non-file).  Reject a
     * directory target with ERR_INVAL, matching do_enum_begin's
     * file-vs-dir rule. */
    vfs_item_info_t oinfo;
    r = vfs_getattr(vi, fid, &oinfo);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }
    if (oinfo.type != VFS_ITEM_FILE) {
        resp->ret = ERR_INVAL;
        goto out;
    }

    /* Read-only volume rejects any WRITE access at open time. */
    if (v->read_only && (req->access & VFS_ACCESS_WRITE)) {
        resp->ret = VFS_ERR_READONLY;
        goto out;
    }

    /* P1 authz gate (design §9.4: "vfs_server 每次 open 校验").  After
     * resolving the path to a stable itemID, ask the perm-manager to
     * authorize the requested access for the REAL caller subject (taken
     * from ipc_recv_from — never from the request bytes).  A denial
     * (VFS_ERR_ACCESS) is returned to the client; the perm-manager also
     * creates a pending Powerbox query + UI_SHOW push so the user can
     * consent.  Storing the granted access (intersected with the
     * requested mask) into the handle lets do_read/do_write re-check
     * it on every operation so a perm_revoke takes effect immediately. */
    vfs_resource_t res;
    res.vol     = v->uuid;
    res.id      = fid;
    u32 granted = 0;
    r           = perm_check(&res, req->access, req->path, caller_subject, &granted);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }
    /* P2 抹位: 句柄只携带 perm-manager 实际批准的位。旧 perm-manager
     * （无 granted 字段）回复时该值为 0 → 回退为请求掩码以保持兼容。 */
    if (granted == 0)
        granted = req->access;

    /* Truncate on open (VFS_OPEN_TRUNCATE → driver truncate op).  This
     * is a write; it runs AFTER perm_check so an unauthorized open
     * never mutates the file. */
    if (req->flags & VFS_OPEN_TRUNCATE) {
        memset(&s_drv_req, 0, sizeof(s_drv_req));
        s_drv_req.op      = DRV_OP_WRITE;
        s_drv_req.volume  = s_vols[vi].drv_vol;
        s_drv_req.item_id = fid;
        s_drv_req.offset  = 0;
        s_drv_req.len     = 0;
        r                 = vfs_drv_call(vi, &s_drv_req, &s_drv_resp);
        if (r < 0) {
            resp->ret = r;
            goto out;
        }
        if (s_drv_resp.ret < 0) {
            resp->ret = s_drv_resp.ret;
            goto out;
        }
    }

    vfs_handle_t      htok;
    vfs_handle_ent_t *h = handle_alloc(vi, fid, granted, req->flags, caller_subject, &res, &htok);
    if (!h) {
        resp->ret = ERR_NOMEM;
        goto out;
    }
    resp->handle = htok;

    r = vfs_getattr(vi, fid, &resp->item);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }

    resp->ret = 0;

out:
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_read(int token, int msg_len, u64 caller_subject) {
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

    /* P1 authz re-check (design §9.4): the handle's access bits were
     * captured at open time and never updated.  A perm_revoke between
     * open and read would otherwise leave this handle silently able to
     * read.  Re-run perm_check on every operation using the identity
     * stored in the handle; a denial (VFS_ERR_ACCESS) fails the read
     * immediately so revocation takes effect without reopening.  The
     * caller subject is also checked against the handle's opener to
     * reject a handle token leaked to another process. */
    if (caller_subject != h->subject_id) {
        resp->ret = VFS_ERR_ACCESS;
        goto out;
    }
    int r = perm_check(&h->resource, VFS_ACCESS_READ, "", h->subject_id, NULL);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }

    memset(&s_drv_req, 0, sizeof(s_drv_req));
    s_drv_req.op      = DRV_OP_READ;
    s_drv_req.volume  = s_vols[h->vol_index].drv_vol;
    s_drv_req.item_id = h->item_id;
    s_drv_req.offset  = req->offset;
    s_drv_req.len     = req->len;
    r                 = vfs_drv_call(h->vol_index, &s_drv_req, &s_drv_resp);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }
    if (s_drv_resp.ret < 0) {
        resp->ret = s_drv_resp.ret;
        goto out;
    }

    i32 n = s_drv_resp.ret;
    memcpy(resp->data, s_drv_resp.u.data, (size_t)n);
    resp->ret = n;

out:
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_write(int token, int msg_len, u64 caller_subject) {
    vfs_resp_write_t *resp = (vfs_resp_write_t *)s_resp;
    if (msg_len < (int)sizeof(u32) * 3 + (int)sizeof(u64)) { /* header */
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

    /* P1 authz re-check — same rationale as do_read: a perm_revoke
     * must take effect on the next write, not on the next open.  The
     * caller must also be the handle's original opener. */
    if (caller_subject != h->subject_id) {
        resp->ret = VFS_ERR_ACCESS;
        goto out;
    }
    int r = perm_check(&h->resource, VFS_ACCESS_WRITE, "", h->subject_id, NULL);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }

    memset(&s_drv_req, 0, sizeof(s_drv_req));
    s_drv_req.op      = DRV_OP_WRITE;
    s_drv_req.volume  = s_vols[h->vol_index].drv_vol;
    s_drv_req.item_id = h->item_id;
    s_drv_req.offset  = req->offset;
    s_drv_req.len     = req->len;
    memcpy(s_drv_req.payload.data, req->data, req->len);
    r = vfs_drv_call(h->vol_index, &s_drv_req, &s_drv_resp);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }
    if (s_drv_resp.ret < 0) {
        resp->ret = s_drv_resp.ret;
        goto out;
    }

    resp->ret = s_drv_resp.ret;

out:
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_close(int token, int msg_len, u64 caller_subject) {
    vfs_resp_close_t *resp = (vfs_resp_close_t *)s_resp;
    if (msg_len < (int)sizeof(vfs_req_close_t)) {
        resp->ret = ERR_INVAL;
        goto out;
    }
    vfs_req_close_t *req = (vfs_req_close_t *)s_req;

    /* A leaked handle token must not let another process close the
     * handle: bind CLOSE to the opener's kernel subject (same rule as
     * do_read/do_write/do_enum_next).  The subject comes from
     * ipc_recv_from, never from the request bytes. */
    vfs_handle_ent_t *h = handle_find(req->handle);
    if (h) {
        if (caller_subject != h->subject_id) {
            resp->ret = VFS_ERR_ACCESS;
            goto out;
        }
        memset(h, 0, sizeof(*h));
        resp->ret = 0;
        goto out;
    }
    for (int i = 0; i < MAX_ENUMS; i++) {
        if (s_enums[i].in_use && s_enums[i].token == req->handle) {
            if (caller_subject != s_enums[i].subject_id) {
                resp->ret = VFS_ERR_ACCESS;
                goto out;
            }
            memset(&s_enums[i], 0, sizeof(s_enums[i]));
            resp->ret = 0;
            goto out;
        }
    }
    resp->ret = VFS_ERR_STALE;

out:
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_enum_begin(int token, int msg_len, u64 caller_subject) {
    vfs_resp_enum_begin_t *resp = (vfs_resp_enum_begin_t *)s_resp;
    if (msg_len < (int)sizeof(vfs_req_enum_begin_t)) {
        resp->ret = ERR_INVAL;
        goto out;
    }
    vfs_req_enum_begin_t *req = (vfs_req_enum_begin_t *)s_req;

    char mount[64];
    char segs[VFS_MAX_DEPTH][VFS_SEG_MAX];
    int  nsegs;
    int  r = vfs_parse_url(req->path, mount, segs, &nsegs);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }
    u32 vi;
    r = vfs_find_vol(mount, &vi);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }

    vfs_item_id_t id;
    r = vfs_lookup_path(vi, segs, nsegs, &id);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }

    vfs_item_info_t info;
    r = vfs_getattr(vi, id, &info);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }
    if (info.type != VFS_ITEM_DIR) {
        resp->ret = ERR_INVAL; /* can only enumerate dirs */
        goto out;
    }

    /* P2 authz gate: enumerating a directory is a READ on it.  The
     * enumerator carries the creator's identity (subject + resource)
     * so every ENUM_NEXT batch re-checks — a perm_revoke between
     * batches takes effect immediately. */
    vfs_resource_t res;
    res.vol = s_vols[vi].uuid;
    res.id  = id;
    r       = perm_check(&res, VFS_ACCESS_READ, req->path, caller_subject, NULL);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }

    vfs_handle_t htok;
    if (!enum_alloc(vi, id, caller_subject, &res, &htok)) {
        resp->ret = ERR_NOMEM;
        goto out;
    }
    resp->handle = htok;
    resp->ret    = 0;

out:
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_enum_next(int token, int msg_len, u64 caller_subject) {
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

    /* P2 authz re-check: the enumerator is bound to its creator (a
     * handle token leaked to another subject cannot step in), and
     * every batch re-checks the grant — a perm_revoke between batches
     * takes effect immediately. */
    if (caller_subject != e->subject_id) {
        resp->ret = VFS_ERR_ACCESS;
        goto out;
    }
    int r = perm_check(&e->resource, VFS_ACCESS_READ, "", e->subject_id, NULL);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }

    memset(&s_drv_req, 0, sizeof(s_drv_req));
    s_drv_req.op        = DRV_OP_ENUM;
    s_drv_req.volume    = s_vols[e->vol_index].drv_vol;
    s_drv_req.parent_id = e->dir_id;
    s_drv_req.from      = e->from;
    r                   = vfs_drv_call(e->vol_index, &s_drv_req, &s_drv_resp);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }
    if (s_drv_resp.ret < 0) {
        resp->ret = s_drv_resp.ret;
        goto out;
    }

    resp->count = s_drv_resp.u.en.count;
    for (u32 i = 0; i < resp->count; i++)
        resp->items[i] = s_drv_resp.u.en.items[i];
    e->from += resp->count;
    resp->ret = (i32)resp->count;

out:
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_stat_volume(int token, int msg_len) {
    vfs_resp_stat_volume_t *resp = (vfs_resp_stat_volume_t *)s_resp;
    if (msg_len < (int)sizeof(vfs_req_stat_volume_t)) {
        resp->ret = ERR_INVAL;
        goto out;
    }
    vfs_req_stat_volume_t *req = (vfs_req_stat_volume_t *)s_req;

    char mount[64];
    char segs[VFS_MAX_DEPTH][VFS_SEG_MAX];
    int  nsegs;
    int  r = vfs_parse_url(req->path, mount, segs, &nsegs);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }
    u32 vi;
    r = vfs_find_vol(mount, &vi);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }

    memset(&s_drv_req, 0, sizeof(s_drv_req));
    s_drv_req.op     = DRV_OP_STAT;
    s_drv_req.volume = s_vols[vi].drv_vol;
    r                = vfs_drv_call(vi, &s_drv_req, &s_drv_resp);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }
    if (s_drv_resp.ret < 0) {
        resp->ret = s_drv_resp.ret;
        goto out;
    }

    resp->total_bytes = s_drv_resp.u.stat.total_bytes;
    resp->used_bytes  = s_drv_resp.u.stat.used_bytes;
    resp->read_only   = s_drv_resp.u.stat.read_only;
    resp->ret         = 0;

out:
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

/* VFS_OP_LIST_VOLUMES — enumerate currently-mounted volumes.  There is
 * no root item to enum_begin("/") on: URLs start at a volume name, so
 * the "/" root view is served from the mount table itself. */
static void do_list_volumes(int token, int msg_len) {
    vfs_resp_list_volumes_t *resp = (vfs_resp_list_volumes_t *)s_resp;
    (void)msg_len;
    resp->ret   = 0;
    resp->count = 0;
    for (u32 i = 0; i < MAX_VOLS; i++) {
        if (!s_vols[i].mounted)
            continue;
        if (resp->count >= VFS_MAX_VOLS)
            break;
        strncpy(resp->vols[resp->count].mount_name, s_vols[i].mount_name,
                sizeof(resp->vols[resp->count].mount_name) - 1);
        resp->vols[resp->count].mount_name[sizeof(resp->vols[resp->count].mount_name) - 1] = '\0';
        strncpy(resp->vols[resp->count].driver_name, s_vols[i].driver_name,
                sizeof(resp->vols[resp->count].driver_name) - 1);
        resp->vols[resp->count].driver_name[sizeof(resp->vols[resp->count].driver_name) - 1] = '\0';
        resp->vols[resp->count].read_only = s_vols[i].read_only;
        resp->count++;
    }
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

static void do_create_bookmark(int token, int msg_len, u64 subject_id) {
    vfs_resp_create_bookmark_t *resp = (vfs_resp_create_bookmark_t *)s_resp;
    if (msg_len < (int)sizeof(vfs_req_create_bookmark_t)) {
        resp->ret = ERR_INVAL;
        goto out;
    }
    vfs_req_create_bookmark_t *req = (vfs_req_create_bookmark_t *)s_req;

    char mount[64];
    char segs[VFS_MAX_DEPTH][VFS_SEG_MAX];
    int  nsegs;
    int  r = vfs_parse_url(req->path, mount, segs, &nsegs);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }
    u32 vi;
    r = vfs_find_vol(mount, &vi);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }

    vfs_item_id_t id;
    r = vfs_lookup_path(vi, segs, nsegs, &id);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }

    vfs_item_info_t info;
    r = vfs_getattr(vi, id, &info);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }

    /* Authorization gate (Powerbox): denied ⇒ -EACCES (VFS_ERR_ACCESS),
     * with a pending query + UI_SHOW pushed by the perm-manager.  The
     * subject is the real requester from ipc_recv_from (unforgeable).
     * P2 抹位: the bookmark stores ONLY the granted bits (a READ-only
     * grant can never mint a WRITE-carrying bookmark). */
    vfs_resource_t res;
    memset(&res, 0, sizeof(res));
    res.vol     = s_vols[vi].uuid;
    res.id      = id;
    u32 granted = 0;
    r           = perm_check(&res, req->access, req->path, subject_id, &granted);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }
    if (granted == 0)
        granted = req->access; /* 兼容旧 perm-manager（无 granted） */

    u32                 tok;
    vfs_bookmark_ent_t *b =
        bookmark_alloc(vi, id, granted, subject_id, (u64)get_time(), req->expiry_ticks, &tok);
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

static void do_resolve_bookmark(int token, int msg_len, u64 subject_id) {
    vfs_resp_resolve_bookmark_t *resp = (vfs_resp_resolve_bookmark_t *)s_resp;
    if (msg_len < (int)sizeof(vfs_req_resolve_bookmark_t)) {
        resp->ret = ERR_INVAL;
        goto out;
    }
    vfs_req_resolve_bookmark_t *req = (vfs_req_resolve_bookmark_t *)s_req;
    if (req->bk_len < sizeof(vfs_bookmark_t) || req->bk_len > VFS_BOOKMARK_MAX) {
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

    /* Identity binding: a bookmark capability is tied to its creating
     * subject (matches the handle-ownership model at do_read/do_write).
     * The blob's own subject_id field is client-held and forgeable, so
     * the authoritative compare is the caller's ipc_recv_from subject
     * against the server record. */
    if (subject_id != b->subject_id) {
        resp->ret = VFS_ERR_ACCESS;
        goto out;
    }

    /* Locate the item.  itemID is stable across DRV_OP_MOVE, but a
     * driver may re-ID on move (copy+delete): fall back to the
     * parent_id chain + stored name (design §5: "用 parent_id 链重新
     * 定位").  The relocated id is used for THIS resolve only — the
     * record's item_id stays anchored to the original id so the
     * client's blob (carrying the original resource.id) keeps matching
     * bookmark_validate on every later resolve.  Mutating the record
     * here would desync it from the blob and break the bookmark from
     * the second resolve onward. */
    vfs_item_id_t live_id = b->item_id;
    vfs_item_info_t info;
    int             r = vfs_getattr(b->vol_index, live_id, &info);
    if (r == ERR_NOENT && b->parent_id != 0) {
        memset(&s_drv_req, 0, sizeof(s_drv_req));
        s_drv_req.op        = DRV_OP_LOOKUP;
        s_drv_req.volume    = s_vols[b->vol_index].drv_vol;
        s_drv_req.parent_id = b->parent_id;
        strncpy(s_drv_req.payload.name, b->name, DRV_PATH_MAX - 1);
        s_drv_req.payload.name[DRV_PATH_MAX - 1] = '\0';
        int r2 = vfs_drv_call(b->vol_index, &s_drv_req, &s_drv_resp);
        if (r2 >= 0 && s_drv_resp.ret >= 0) {
            live_id = s_drv_resp.u.item_id;
            r       = vfs_getattr(b->vol_index, live_id, &info);
        }
    }
    if (r < 0) {
        resp->ret = r;
        goto out;
    }

    /* Re-authorize on EVERY resolve (§9.4) — a REVOKE'd grant takes
     * effect immediately, before any handle is handed out.  P1: the
     * subject is the real requester from ipc_recv_from.  P2 抹位: the
     * temporary handle carries ONLY the bits granted at resolve time. */
    vfs_resource_t res;
    memset(&res, 0, sizeof(res));
    res.vol = s_vols[b->vol_index].uuid;
    res.id  = live_id;
    u32 granted = 0;
    r           = perm_check(&res, b->access, "", subject_id, &granted);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }
    if (granted == 0)
        granted = b->access; /* 兼容旧 perm-manager（无 granted） */

    vfs_handle_t      htok;
    vfs_handle_ent_t *h =
        handle_alloc(b->vol_index, live_id, granted, 0, subject_id, &res, &htok);
    if (!h) {
        resp->ret = ERR_NOMEM;
        goto out;
    }

    resp->handle = htok;
    resp->access = granted;
    resp->item   = info;
    resp->ret    = 0;

out:
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_revoke_bookmark(int token, int msg_len, u64 caller_subject) {
    vfs_resp_revoke_bookmark_t *resp = (vfs_resp_revoke_bookmark_t *)s_resp;
    if (msg_len < (int)sizeof(vfs_req_revoke_bookmark_t)) {
        resp->ret = ERR_INVAL;
        goto out;
    }
    vfs_req_revoke_bookmark_t *req = (vfs_req_revoke_bookmark_t *)s_req;
    if (req->bk_len < sizeof(vfs_bookmark_t) || req->bk_len > VFS_BOOKMARK_MAX) {
        resp->ret = ERR_INVAL;
        goto out;
    }

    vfs_bookmark_t blob;
    memcpy(&blob, req->data, sizeof(blob));

    /* A bookmark is bound to its creator's kernel subject (the record
     * field is authoritative; the blob's own copy is client-held).  A
     * leaked/copied blob must not let another process revoke it — that
     * would be a denial-of-service on a still-valid grant.  Same
     * identity rule as do_resolve_bookmark. */
    vfs_bookmark_ent_t *b = bookmark_validate(&blob);
    if (b) {
        if (caller_subject != b->subject_id) {
            resp->ret = VFS_ERR_ACCESS;
            goto out;
        }
        memset(b, 0, sizeof(*b)); /* drop the server-side record */
    }
    resp->ret = 0; /* idempotent: revoke is revoke */

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

static void do_move(int token, int msg_len, u64 caller_subject) {
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
    if (r < 0) {
        resp->ret = r;
        goto out;
    }
    r = vfs_parse_url(req->dst_dir, d_mount, d_segs, &dnsegs);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }

    u32 svi, dvi;
    r = vfs_find_vol(s_mount, &svi);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }
    r = vfs_find_vol(d_mount, &dvi);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }
    if (svi != dvi) { /* Phase 2: no cross-volume move */
        resp->ret = VFS_ERR_PERM;
        goto out;
    }

    vfs_item_id_t sid;
    r = vfs_lookup_path(svi, s_segs, snsegs, &sid);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }
    if (snsegs == 0) {
        resp->ret = ERR_INVAL; /* can't move the volume root */
        goto out;
    }

    vfs_item_id_t did;
    r = vfs_lookup_path(dvi, d_segs, dnsegs, &did);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }

    vfs_item_info_t dinfo;
    r = vfs_getattr(dvi, did, &dinfo);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }
    if (dinfo.type != VFS_ITEM_DIR) {
        resp->ret = ERR_INVAL; /* dst must be a directory */
        goto out;
    }

    /* P2 authz gate: move mutates both the source item and the
     * destination directory.  身份完全取自 ipc_recv_from 的
     * caller_subject（不可伪造）；仅角色链约束。在驱动改动文件系统
     * 之前校验。 */
    vfs_resource_t res;
    res.vol = s_vols[svi].uuid;
    res.id  = sid;
    r       = perm_check(&res, VFS_ACCESS_WRITE, req->src, caller_subject, NULL);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }
    res.vol = s_vols[dvi].uuid;
    res.id  = did;
    r       = perm_check(&res, VFS_ACCESS_WRITE, req->dst_dir, caller_subject, NULL);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }

    memset(&s_drv_req, 0, sizeof(s_drv_req));
    s_drv_req.op        = DRV_OP_MOVE;
    s_drv_req.volume    = s_vols[svi].drv_vol;
    s_drv_req.item_id   = sid;
    s_drv_req.parent_id = did;
    if (req->new_name[0] != '\0') { /* optional rename */
        strncpy(s_drv_req.payload.name, req->new_name, DRV_PATH_MAX - 1);
        s_drv_req.payload.name[DRV_PATH_MAX - 1] = '\0';
    }
    r = vfs_drv_call(svi, &s_drv_req, &s_drv_resp);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }
    if (s_drv_resp.ret < 0) {
        resp->ret = s_drv_resp.ret;
        goto out;
    }

    r = vfs_getattr(svi, sid, &resp->item);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }
    resp->ret = 0;

out:
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

/* WHOAMI — P1 地基: a client asks the service layer for its
 * kernel-issued subject_id.  Proxies SYS_GET_SUBJECT through the
 * trusted server (sandboxed clients never talk to the kernel directly);
 * the subject comes from THIS server's ipc_recv_from, never from the
 * request bytes. */
static void do_whoami(int token, int msg_len, u64 subject_id) {
    vfs_resp_whoami_t *resp = (vfs_resp_whoami_t *)s_resp;
    (void)msg_len;
    resp->ret        = 0;
    resp->subject_id = subject_id;
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void vfs_handle_request(int token, u32 op, int msg_len, u64 caller_subject) {
    switch (op) {
    case VFS_OP_GET_ITEM:
        do_get_item(token, msg_len);
        break;
    case VFS_OP_CREATE_DIR:
        do_create_dir(token, msg_len, caller_subject);
        break;
    case VFS_OP_DELETE_ITEM:
        do_delete(token, msg_len, caller_subject);
        break;
    case VFS_OP_OPEN_ITEM:
        do_open(token, msg_len, caller_subject);
        break;
    case VFS_OP_READ:
        do_read(token, msg_len, caller_subject);
        break;
    case VFS_OP_WRITE:
        do_write(token, msg_len, caller_subject);
        break;
    case VFS_OP_CLOSE:
        do_close(token, msg_len, caller_subject);
        break;
    case VFS_OP_ENUM_BEGIN:
        do_enum_begin(token, msg_len, caller_subject);
        break;
    case VFS_OP_ENUM_NEXT:
        do_enum_next(token, msg_len, caller_subject);
        break;
    case VFS_OP_MOUNT:
        do_mount(token, msg_len, caller_subject);
        break;
    case VFS_OP_UNMOUNT:
        do_unmount(token, msg_len, caller_subject);
        break;
    case VFS_OP_STAT_VOLUME:
        do_stat_volume(token, msg_len);
        break;
    case VFS_OP_CREATE_BOOKMARK:
        do_create_bookmark(token, msg_len, caller_subject);
        break;
    case VFS_OP_RESOLVE_BOOKMARK:
        do_resolve_bookmark(token, msg_len, caller_subject);
        break;
    case VFS_OP_REVOKE_BOOKMARK:
        do_revoke_bookmark(token, msg_len, caller_subject);
        break;
    case VFS_OP_MOVE:
        do_move(token, msg_len, caller_subject);
        break;
    case VFS_OP_WHOAMI:
        do_whoami(token, msg_len, caller_subject);
        break;
    case VFS_OP_LIST_VOLUMES:
        do_list_volumes(token, msg_len);
        break;
    default: {
        i32 *resp = (i32 *)s_resp;
        *resp     = ERR_INVAL;
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

int main(void) {
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
           "reserved)\n",
           MOUNT_CFG_COUNT);

    for (;;) {
        int msg_len        = (int)sizeof(s_req);
        int token          = 0;
        u64 caller_subject = 0;
        /* P1 地基: ipc_recv_from gives the kernel-filled unforgeable
         * sender subject — the basis for every authz decision and for
         * the WHOAMI op. */
        ret = ipc_recv_from(port, s_req, &msg_len, &token, &caller_subject);
        if (ret < 0) {
            printf("vfs: ipc_recv failed (%d)\n", ret);
            thread_exit(1);
        }
        if (msg_len < (int)sizeof(u32)) { /* no op code */
            i32 *resp = (i32 *)s_resp;
            *resp     = ERR_INVAL;
            (void)ipc_reply(token, resp, (int)sizeof(i32));
            continue;
        }
        u32 op = *(u32 *)s_req;
        vfs_handle_request(token, op, msg_len, caller_subject);
    }
}
