/*
 * vfs.h - OpSys VFS protocol definitions
 * Copyright (c) 2026 OpSys Project
 *
 * Single source of truth for the VFS wire protocol (docs/vfs_design.md
 * §3/§4/§6/§7).  Included by ALL three VFS components:
 *   - vfs_server     (user/services/vfs/vfs_server.c)
 *   - fs_mem_driver  (user/services/vfs/fs_mem_driver.c)
 *   - libfs client   (user/lib/libfs/fs.c)
 *
 * Transport: flat structs over ipc_call()/ipc_recv()+ipc_reply(), raw
 * native-little-endian copy, req[0] = op code (keyboard.c style).
 * Message size limit: 4096 (kernel MAX_MSG_SIZE) — all read/write/enum
 * payloads are chunked to stay under it.
 *
 * User-space file: kernel/types.h must NOT be included here (its error
 * enum collides with the OK/ERR_* macros in syscalls.h) — hence the
 * fixed-width stdint typedefs below.
 */

#ifndef USER_SERVICES_VFS_VFS_H
#define USER_SERVICES_VFS_VFS_H

#include <stdint.h>

/* Fixed-width types (same convention as keyboard.c/manager.c) */
typedef uint8_t  u8;
typedef uint32_t u32;
typedef int32_t  i32;
typedef uint64_t u64;

/* ====================================================================
 * Object model (design §3.1)
 * ==================================================================== */

/* Volume UUID — 128 bit, generated at volume creation (vfs_server). */
typedef struct {
    u64 hi;
    u64 lo;
} vfs_uuid_t;

/* Volume-local stable ID: the inode equivalent.  Parent-ID chaining is
 * the foundation for "bookmark survives a move". */
typedef u64 vfs_item_id_t;

/* Global resource locator: unique, stable, persistable. */
typedef struct {
    vfs_uuid_t    vol;
    vfs_item_id_t id;
} vfs_resource_t;

/* ====================================================================
 * Item metadata (design §3.2)
 * ==================================================================== */

typedef enum {
    VFS_ITEM_FILE = 0,
    VFS_ITEM_DIR,
    VFS_ITEM_SYMLINK,     /* /System/usr compat layer only */
    VFS_ITEM_MOUNT_POINT, /* mount points under /Volumes */
} vfs_item_type_t;

typedef struct {
    vfs_item_id_t   parent_id; /* parent dir itemID (volume root = 0) */
    vfs_item_id_t   item_id;   /* own ID */
    vfs_item_type_t type;
    char            name[256]; /* name only, never a path */
    u64             size;
    u64             creation_date; /* RTC ticks */
    u64             mod_date;
    u32             posix_mode; /* /System/usr compat layer only */
    u32             uid;        /* compat layer; policy stays in user */
    u32             gid;
} vfs_item_info_t;

/* ====================================================================
 * FileHandle (design §3.3)
 * ==================================================================== */

typedef u32 vfs_handle_t; /* random 32-bit token; 0 invalid (handle_alloc) */

/* Open-time access rights — bit-aligned with kernel rights_t semantics */
#define VFS_ACCESS_READ  (1u << 0)
#define VFS_ACCESS_WRITE (1u << 1)
#define VFS_ACCESS_EXEC  (1u << 2)
#define VFS_ACCESS_COW   (1u << 3) /* copy-on-write clone (Phase 3) */

/* Open flags — Phase 0 defines all four; READONLY/CREATE/TRUNCATE are
 * enforced server-side, APPEND is accepted (offsets stay client-maintained) */
#define VFS_OPEN_READONLY 0x01
#define VFS_OPEN_CREATE   0x02
#define VFS_OPEN_TRUNCATE 0x04
#define VFS_OPEN_APPEND   0x08

typedef struct {
    vfs_handle_t   handle;
    vfs_resource_t target;
    u32            access; /* VFS_ACCESS_* mask */
    u64            offset; /* server-maintained r/w position */
    u32            flags;  /* VFS_OPEN_* */
} vfs_handle_info_t;

/* ====================================================================
 * Enumerator (design §3.4) — client-side aggregation buffer
 * ==================================================================== */

typedef struct {
    vfs_handle_t  handle;         /* server-side enumerator handle */
    u32           batch_count;    /* entries in this batch */
    char          batch[64][256]; /* up to 64 names/batch (client buf) */
    vfs_item_id_t batch_ids[64];
} vfs_enum_batch_t;

/* ====================================================================
 * Wire-size limits (kernel MAX_MSG_SIZE = 4096, types.h:82)
 * ==================================================================== */

#define VFS_IPC_MAX  4096
#define VFS_MAX_READ (VFS_IPC_MAX - 64) /* 4032: read resp payload */
/* Write is capped at the READ size too: the vfs_server re-wraps the
 * payload into a DRV request that carries its own header, so the client
 * payload must leave room for that (DRV_MAX_PAYLOAD == VFS_MAX_READ). */
#define VFS_MAX_WRITE  (VFS_IPC_MAX - 64) /* 4032: write req payload */
#define VFS_ENUM_BATCH 8                  /* items per ENUM_NEXT resp */

/* ====================================================================
 * VFS-specific error codes (design §4.1: "错误码沿用内核 ERR_* 约定")
 *
 * Kernel provides only OK(0)..ERR_INTERRUPTED(-10).  VFS semantics that
 * the kernel has no code for are defined here as negative values far
 * below the kernel range, so the two never collide.
 * ==================================================================== */

#define VFS_ERR_READONLY (-100) /* write to a read-only volume (EROFS) */
#define VFS_ERR_NOSPC    (-101) /* volume full (ENOSPC) */
#define VFS_ERR_STALE    (-102) /* handle/enumerator no longer valid (ESTALE) */
#define VFS_ERR_PERM     (-103) /* permission denied (EPERM) */
#define VFS_ERR_EXISTS   (-104) /* create_dir on existing dir (EEXIST) */
#define VFS_ERR_ACCESS   (-105) /* bookmark not authorized (EACCES) */

/* ====================================================================
 * Client protocol: vfs_server ↔ client (design §4.1/§4.2)
 * All requests start with a u32 op code.
 * ==================================================================== */

enum {
    VFS_OP_GET_ITEM         = 1, /* URL → metadata (no open) */
    VFS_OP_CREATE_DIR       = 2,
    VFS_OP_DELETE_ITEM      = 3,
    VFS_OP_OPEN_ITEM        = 4, /* open → FileHandle */
    VFS_OP_READ             = 5, /* handle + offset + len */
    VFS_OP_WRITE            = 6,
    VFS_OP_CLOSE            = 7,
    VFS_OP_ENUM_BEGIN       = 8,  /* dir iteration start */
    VFS_OP_ENUM_NEXT        = 9,  /* next batch */
    VFS_OP_CREATE_BOOKMARK  = 10, /* Phase 2 */
    VFS_OP_RESOLVE_BOOKMARK = 11, /* Phase 2 */
    VFS_OP_REVOKE_BOOKMARK  = 12, /* Phase 2 */
    VFS_OP_MOUNT            = 13, /* driver → vfs_server */
    VFS_OP_UNMOUNT          = 14,
    VFS_OP_STAT_VOLUME      = 15,
    VFS_OP_MOVE             = 16, /* Phase 2: move/rename item */
    VFS_OP_WHOAMI           = 17, /* P1 地基: caller → kernel subject_id */
    VFS_OP_LIST_VOLUMES     = 18, /* enumerate mounted volumes (root "/" view) */
};

#define VFS_PATH_MAX 1024 /* URL string field size */
#define VFS_MAX_VOLS 4    /* mount slots (matches vfs_server MAX_VOLS) */

/* VFS_OP_GET_ITEM */
typedef struct {
    u32  op;                 /* = VFS_OP_GET_ITEM */
    char path[VFS_PATH_MAX]; /* URL string — only this boundary sees a path */
} vfs_req_get_item_t;

typedef struct {
    i32             ret;
    vfs_item_info_t item;
} vfs_resp_get_item_t;

/* VFS_OP_CREATE_DIR */
typedef struct {
    u32  op;
    char path[VFS_PATH_MAX];
} vfs_req_create_dir_t;

typedef struct {
    i32             ret;
    vfs_item_info_t item;
} vfs_resp_create_dir_t;

/* VFS_OP_DELETE_ITEM */
typedef struct {
    u32  op;
    char path[VFS_PATH_MAX];
    u32  recursive; /* 1 = recursive delete */
} vfs_req_delete_t;

typedef struct {
    i32 ret;
} vfs_resp_delete_t;

/* VFS_OP_OPEN_ITEM */
typedef struct {
    u32  op;
    char path[VFS_PATH_MAX];
    u32  flags;  /* VFS_OPEN_* */
    u32  access; /* VFS_ACCESS_* */
} vfs_req_open_t;

typedef struct {
    i32             ret;
    vfs_handle_t    handle;
    vfs_item_info_t item;
} vfs_resp_open_t;

/* VFS_OP_READ — chunked, single read ≤ VFS_MAX_READ */
typedef struct {
    u32          op;
    vfs_handle_t handle;
    u64          offset; /* absolute; client-maintained */
    u32          len;    /* ≤ VFS_MAX_READ */
} vfs_req_read_t;

typedef struct {
    i32 ret; /* bytes read, 0 = EOF, negative = error */
    u8  data[VFS_MAX_READ];
} vfs_resp_read_t;

/* VFS_OP_WRITE */
typedef struct {
    u32          op;
    vfs_handle_t handle;
    u64          offset; /* absolute */
    u32          len;    /* ≤ VFS_MAX_WRITE */
    u8           data[VFS_MAX_WRITE];
} vfs_req_write_t;

typedef struct {
    i32 ret; /* bytes written, negative = error */
} vfs_resp_write_t;

/* VFS_OP_CLOSE — closes file handles AND enumerators (shared space) */
typedef struct {
    u32          op;
    vfs_handle_t handle;
} vfs_req_close_t;

typedef struct {
    i32 ret;
} vfs_resp_close_t;

/* VFS_OP_ENUM_BEGIN / VFS_OP_ENUM_NEXT — paginated (design §6.4) */
typedef struct {
    u32  op;
    char path[VFS_PATH_MAX];
} vfs_req_enum_begin_t;

typedef struct {
    i32          ret;
    vfs_handle_t handle; /* enumerator handle */
} vfs_resp_enum_begin_t;

typedef struct {
    u32          op;
    vfs_handle_t handle;
} vfs_req_enum_next_t;

/* One fixed-size entry: name[256] + id (design §6.4) */
typedef struct {
    char          name[256];
    vfs_item_id_t id;
    u32           type;
} vfs_enum_item_t;

typedef struct {
    i32             ret;   /* entries in this batch (0 = iteration end) */
    u32             count; /* ≤ VFS_ENUM_BATCH */
    vfs_enum_item_t items[VFS_ENUM_BATCH];
} vfs_resp_enum_next_t;

/* ====================================================================
 * Security-Scoped Bookmark (design §5) — Phase 2
 *
 * The bookmark blob is an OPAQUE capability handed to the client; the
 * vfs_server holds the authoritative record table (design §5.2:
 * "Phase 2 的书签有效性以 vfs_server 服务端记录表为准") and binds every
 * bookmark to its creator's kernel subject.  The blob layout mirrors
 * vfs_bookmark_t below; mac[16] stays zero until Phase 3 adds a real
 * HMAC.
 * ==================================================================== */

#define VFS_BOOKMARK_MAGIC   0x4B4D4256 /* 'VB MK' */
#define VFS_BOOKMARK_VERSION 2
#define VFS_BOOKMARK_MAX     256 /* blob buffer in msgs */

typedef struct {
    u32 magic;       /* VFS_BOOKMARK_MAGIC */
    u32 version;     /* = 2 */
    u32 payload_len; /* bytes from resource (payload start) to end */
    /* --- payload (version 2) --- */
    vfs_resource_t resource;      /* volume UUID + itemID (stable) */
    vfs_item_id_t  parent_id;     /* parent at creation (move track) */
    u32            access;        /* granted VFS_ACCESS_* */
    u64            subject_id;    /* creator's kernel subject (unforgeable) */
    u64            created_ticks; /* RTC timestamp */
    u64            expiry_ticks;  /* 0 = never expires */
    /* --- signature area (Phase 3) --- */
    u8 mac[16]; /* zero in Phase 2 */
} vfs_bookmark_t;

_Static_assert(sizeof(vfs_bookmark_t) <= VFS_BOOKMARK_MAX, "bookmark too big");

/* VFS_OP_CREATE_BOOKMARK */
typedef struct {
    u32  op;
    char path[VFS_PATH_MAX]; /* URL to bookmark */
    u32  access;             /* requested VFS_ACCESS_* */
    u64  expiry_ticks;       /* 0 = never */
} vfs_req_create_bookmark_t;

typedef struct {
    i32 ret;
    u32 bk_len;
    u8  data[VFS_BOOKMARK_MAX]; /* vfs_bookmark_t blob */
} vfs_resp_create_bookmark_t;

/* VFS_OP_RESOLVE_BOOKMARK — blob → temporary FileHandle */
typedef struct {
    u32 op;
    u32 bk_len;
    u8  data[VFS_BOOKMARK_MAX];
} vfs_req_resolve_bookmark_t;

typedef struct {
    i32             ret;
    vfs_handle_t    handle;
    vfs_item_info_t item;
    u32             access; /* granted access (blob) */
} vfs_resp_resolve_bookmark_t;

/* VFS_OP_REVOKE_BOOKMARK — drop a bookmark server-side */
typedef struct {
    u32 op;
    u32 bk_len;
    u8  data[VFS_BOOKMARK_MAX];
} vfs_req_revoke_bookmark_t;

typedef struct {
    i32 ret;
} vfs_resp_revoke_bookmark_t;

/* VFS_OP_MOVE — move/rename an item (parent_id stays valid; the
 * driver keeps the itemID stable, so bookmarks survive the move) */
typedef struct {
    u32  op;
    char src[VFS_PATH_MAX];     /* source URL */
    char dst_dir[VFS_PATH_MAX]; /* destination directory URL */
    char new_name[256];         /* optional rename; "" = keep name */
} vfs_req_move_t;

typedef struct {
    i32             ret;
    vfs_item_info_t item;
} vfs_resp_move_t;

/* VFS_OP_STAT_VOLUME */
typedef struct {
    u32  op;
    char path[VFS_PATH_MAX]; /* volume URL, e.g. "/Volumes/Users" */
} vfs_req_stat_volume_t;

typedef struct {
    i32 ret;
    u64 total_bytes;
    u64 used_bytes;
    u32 read_only;
} vfs_resp_stat_volume_t;

/* VFS_OP_WHOAMI — P1 地基: caller asks the service layer for its
 * kernel-issued subject_id (proxies SYS_GET_SUBJECT through a trusted
 * server so sandboxed clients never talk to the kernel directly). */
typedef struct {
    u32 op; /* = VFS_OP_WHOAMI */
} vfs_req_whoami_t;

typedef struct {
    i32 ret;
    u64 subject_id; /* 0 = error/unknown */
} vfs_resp_whoami_t;

/* VFS_OP_LIST_VOLUMES — one mounted volume entry (root "/" view) */
typedef struct {
    char mount_name[64];   /* e.g. "System" */
    char driver_name[64];  /* e.g. "mem" */
    u32  read_only;
} vfs_vol_info_t;

typedef struct {
    u32 op; /* = VFS_OP_LIST_VOLUMES */
} vfs_req_list_volumes_t;

typedef struct {
    i32            ret;   /* 0 = ok */
    u32            count; /* mounted volumes, ≤ VFS_MAX_VOLS */
    vfs_vol_info_t vols[VFS_MAX_VOLS];
} vfs_resp_list_volumes_t;

/* VFS_OP_MOUNT — driver → vfs_server registration (design §7.2) */
typedef struct {
    u32           op;
    char          driver_name[64]; /* e.g. "mem" (fs_mem_driver) */
    char          mount_name[64];  /* e.g. "System" — vfs_server validates */
    vfs_uuid_t    uuid;
    vfs_item_id_t root_item_id;
    u32           read_only;
} vfs_req_mount_t;

typedef struct {
    i32 ret;
} vfs_resp_mount_t;

/* VFS_OP_UNMOUNT — driver → vfs_server deregistration (design §7.2).
 * Mirrors vfs_req_mount_t's name fields (same types/sizes, same order):
 * the server matches the mounted volume by mount_name + driver_name,
 * drops it, and stales every handle/enumerator/bookmark on it. */
typedef struct {
    u32  op;
    char driver_name[64]; /* e.g. "mem" (fs_mem_driver) */
    char mount_name[64];  /* e.g. "System" — vfs_server validates */
} vfs_req_unmount_t;

typedef struct {
    i32 ret;
} vfs_resp_unmount_t;

/* ====================================================================
 * Driver protocol: vfs_server ↔ fs_<format>_driver (design §7.1)
 *
 * The driver owns volume internals: itemID ↔ (memory block).  The
 * server owns namespace state (volumes, handles, bookmarks).  Volume
 * IDs are the driver's own per-volume index space (0-based, assigned
 * by the driver in MOUNT order).
 *
 * Messages are compact unions so every exchange stays under the
 * 4096-byte IPC limit even with a full read/write payload:
 *   req  = 48-byte header + 4032-byte payload union   (4080 bytes)
 *   resp = 4-byte ret + 4032-byte payload union        (4040 bytes)
 * ==================================================================== */

#define DRV_NAME_MAX    64
#define DRV_PATH_MAX    256
#define DRV_MAX_PAYLOAD VFS_MAX_READ /* 4032 — fits header + payload */

enum {
    DRV_OP_GETATTR = 1, /* item_id → vfs_item_info_t */
    DRV_OP_LOOKUP  = 2, /* parent_id + name → item_id */
    DRV_OP_READ    = 3, /* item_id + offset + len → bytes */
    DRV_OP_WRITE =
        4,                  /* item_id + offset + data → bytes;
                                                           len==0 && offset==0 = truncate (OPEN+TRUNCATE) */
    DRV_OP_CREATE_DIR = 5,  /* parent_id + name → item_id */
    DRV_OP_MKFILE     = 6,  /* parent_id + name → item_id */
    DRV_OP_DELETE     = 7,  /* item_id [+ recursive] */
    DRV_OP_ENUM       = 8,  /* parent_id + from → batch (≤ VFS_ENUM_BATCH) */
    DRV_OP_STAT       = 9,  /* volume → capacity/used/read-only */
    DRV_OP_MOVE       = 10, /* Phase 2: move/rename, itemID stays stable */
};

typedef struct {
    u32           op;
    u32           volume;    /* driver-side volume index */
    vfs_item_id_t item_id;   /* GETATTR/READ/WRITE/DELETE target */
    vfs_item_id_t parent_id; /* LOOKUP/CREATE_DIR/MKFILE/ENUM parent */
    u32           from;      /* ENUM: start index */
    u32           recursive; /* DELETE: recurse into subdirs */
    u32           len;       /* READ/WRITE byte count */
    u64           offset;    /* READ/WRITE byte offset */
    union {
        char name[DRV_PATH_MAX];    /* LOOKUP/CREATE_DIR/MKFILE */
        u8   data[DRV_MAX_PAYLOAD]; /* WRITE payload */
    } payload;
} drv_req_t;

typedef struct {
    i32 ret;
    union {
        vfs_item_info_t item;    /* GETATTR */
        vfs_item_id_t   item_id; /* LOOKUP/CREATE_DIR/MKFILE */
        struct {
            u32             count; /* ENUM: entries in this batch */
            vfs_enum_item_t items[VFS_ENUM_BATCH];
        } en; /* ENUM */
        struct {
            u64 total_bytes; /* STAT */
            u64 used_bytes;
            u32 read_only;
        } stat;                   /* STAT */
        u8 data[DRV_MAX_PAYLOAD]; /* READ payload */
    } u;
} drv_resp_t;

#define DRV_REQ_MAX  (sizeof(drv_req_t))
#define DRV_RESP_MAX (sizeof(drv_resp_t))

/* Compile-time guard: every message must fit the 4096-byte IPC limit. */
_Static_assert(DRV_REQ_MAX <= VFS_IPC_MAX, "drv_req_t exceeds IPC limit");
_Static_assert(DRV_RESP_MAX <= VFS_IPC_MAX, "drv_resp_t exceeds IPC limit");
_Static_assert(sizeof(vfs_resp_read_t) <= VFS_IPC_MAX, "read resp exceeds IPC limit");
_Static_assert(sizeof(vfs_req_write_t) <= VFS_IPC_MAX, "write req exceeds IPC limit");
_Static_assert(sizeof(vfs_resp_enum_next_t) <= VFS_IPC_MAX, "enum resp exceeds IPC limit");

#endif /* USER_SERVICES_VFS_VFS_H */
