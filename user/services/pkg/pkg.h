/*
 * pkg.h - pkg-manager protocol (v0.3 沙盒应用容器, docs/ops_format.md)
 * Copyright (c) 2026 OpSys Project
 *
 * pkg-manager owns application installation and sandbox capability
 * issuance (docs/ops_format.md §5): apps declare permissions in a .ops
 * manifest; pkg-manager (kernel-endorsed via ATOM_SERVICE_MANAGE seed)
 * signs atom capabilities into the app's kernel cap table on APP_READY.
 *
 * It exposes one service port:
 *
 *   "pkg" — INSTALL/LIST/RUN/REMOVE from the shell; APP_READY handshake
 *           from freshly spawned apps (identity = ipc_recv_from subject,
 *           cross-checked with proc_info_by_subject).
 *
 * Ops (docs/ops_format.md §7):
 *
 *   INSTALL    1   shell → pkg    install a kernel blob as a .ops app.
 *   LIST       2   shell → pkg    enumerate installed apps.
 *   RUN        3   shell → pkg    spawn an app, return pid.
 *   REMOVE     4   shell → pkg    delete an installed app (recursive).
 *   APP_READY  5   app   → pkg    handshake; pkg signs manifest atoms
 *                                 into the caller's kernel cap table,
 *                                 then replies 0.
 *
 * Contract status: FROZEN for Phase A (see docs/ops_format.md header).
 * Do not modify fields/ops in this file without orchestrator approval.
 *
 * Transport: flat structs over ipc_call()/ipc_recv()+ipc_reply() like
 * the VFS/perm protocols; req[0] = op code, all messages < 4096.
 */

#ifndef USER_SERVICES_PKG_PKG_H
#define USER_SERVICES_PKG_PKG_H

#include <stdint.h>

/* Fixed-width types (same convention as perm.h) */
typedef uint8_t  u8;
typedef uint32_t u32;
typedef int32_t  i32;
typedef uint64_t u64;

#define PKG_PORT_NAME "pkg" /* service port */

/* Bounds (docs/ops_format.md §2/§3) */
#define PKG_NAME_MAX     64  /* app_id / blob name (incl NUL) */
#define PKG_PERMS_MAX    256 /* comma-separated atom names     */
#define PKG_MANIFEST_MAX 512 /* manifest text length           */
#define PKG_MAX_APPS     8   /* installed app listing cap      */
#define PKG_MAX_ATOMS    8   /* permissions per manifest       */
#define PKG_PENDING_MAX  4   /* concurrent run handshakes      */

/* Op codes (req[0]) */
enum {
    PKG_OP_INSTALL   = 1, /* shell → pkg: install kernel blob     */
    PKG_OP_LIST      = 2, /* shell → pkg: enumerate installed     */
    PKG_OP_RUN       = 3, /* shell → pkg: spawn app → pid         */
    PKG_OP_REMOVE    = 4, /* shell → pkg: delete installed app    */
    PKG_OP_APP_READY = 5, /* app → pkg: ready; sign manifest atoms */
};

/* ====================================================================
 * INSTALL — install a kernel blob (SYS_BLOB_GET name) as a .ops app.
 *
 * pkg-manager fetches the blob, builds the manifest (app_id = name,
 * permissions from `perms`, which may be "" = none), packs the .ops in
 * memory, and writes /Volumes/Users/Apps/<name>/app.ops (Users RW).
 * Returns 0 on success; ERR_INVAL for unknown perms (incl. forbidden
 * management atoms, docs/ops_format.md §4); VFS errors propagate.
 * ==================================================================== */

typedef struct {
    u32  op;                   /* = PKG_OP_INSTALL */
    char name[PKG_NAME_MAX];   /* kernel blob name (app_id) */
    char perms[PKG_PERMS_MAX]; /* comma-separated atom names, "" = none */
} pkg_req_install_t;

typedef struct {
    i32 ret;
} pkg_resp_install_t;

/* ====================================================================
 * LIST — enumerate installed apps under /Volumes/Users/Apps/.
 * ==================================================================== */

typedef struct {
    u32 op; /* = PKG_OP_LIST */
} pkg_req_list_t;

typedef struct {
    i32  ret;
    u32  count; /* apps listed */
    char apps[PKG_MAX_APPS][PKG_NAME_MAX];
} pkg_resp_list_t;

/* ====================================================================
 * RUN — spawn an installed app.
 *
 * pkg-manager reads the .ops back from VFS, parses the manifest, records
 * s_pending{pid → manifest atoms}, then process_create()s the payload
 * ELF and returns the pid.  Grant issuance happens on APP_READY (the
 * app cannot receive atoms before it runs).
 * ==================================================================== */

typedef struct {
    u32  op; /* = PKG_OP_RUN */
    char app_id[PKG_NAME_MAX];
} pkg_req_run_t;

typedef struct {
    i32 ret;
    i32 pid; /* spawned pid on success */
} pkg_resp_run_t;

/* ====================================================================
 * REMOVE — delete an installed app (recursive delete of its directory).
 * ==================================================================== */

typedef struct {
    u32  op; /* = PKG_OP_REMOVE */
    char app_id[PKG_NAME_MAX];
} pkg_req_remove_t;

typedef struct {
    i32 ret;
} pkg_resp_remove_t;

/* ====================================================================
 * APP_READY — sandbox handshake from a freshly spawned app.
 *
 * pkg-manager derives the caller's REAL subject from ipc_recv_from (the
 * kernel — unforgeable), looks it up via proc_info_by_subject() to get
 * pid + name, and cross-checks against the pending RUN record.  Only on
 * match does it sign the manifest atoms into that subject's kernel cap
 * table (cap_grant_to_subject, gated on pkg's ATOM_SERVICE_MANAGE).
 * Replies 0 on success; ERR_NOENT (no pending record / name mismatch)
 * or ERR_NOCAP otherwise — the app must treat non-zero as "no rights".
 * ==================================================================== */

typedef struct {
    u32  op;                   /* = PKG_OP_APP_READY */
    char app_id[PKG_NAME_MAX]; /* the app's own app_id */
} pkg_req_app_ready_t;

typedef struct {
    i32 ret;
} pkg_resp_app_ready_t;

#endif /* USER_SERVICES_PKG_PKG_H */
