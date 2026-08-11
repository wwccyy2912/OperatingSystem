/*
 * perm.h - Powerbox permission-manager protocol (docs/vfs_design.md §8 决策 2)
 * Copyright (c) 2026 OpSys Project
 *
 * perm-manager owns the SINGLE source of truth for access grants
 * (design §9.4: "权限单一事实源在 perm-manager，vfs_server 每次
 * open/resolve 校验").  It exposes two port names:
 *
 *   "perm"     — the service port (CHECK from vfs_server, ANSWER/QUERY/
 *                REVOKE/GRANT from UI agents and the shell).
 *   "perm.ui"  — registered by the term service at startup; perm-manager
 *                PUSHES UI_SHOW notifications there so the Powerbox text
 *                prompt appears on screen without polling.
 *
 * Ops (decision 2 defines QUERY/ANSWER; CHECK/REVOKE/GRANT/UI_SHOW are
 * the Phase 2 additions that make the "single source of truth" model
 * work end to end):
 *
 *   CHECK    3   vfs_server → perm-manager   synchronous authz check;
 *                                          if not granted, creates a
 *                                          pending query + UI_SHOW push,
 *                                          returns VFS_ERR_ACCESS.
 *   ANSWER   2   UI agent/shell → perm-manager  user verdict on a query.
 *   QUERY    1   UI agent → perm-manager   fetch a pending query.
 *   REVOKE   4   shell → perm-manager      drop grants (default deny).
 *   GRANT    5   shell/test → perm-manager direct grant (bypasses the
 *                                          Powerbox — test/management).
 *   UI_SHOW  6   perm-manager → term("perm.ui")   push prompt text.
 *
 * Transport: flat structs over ipc_call()/ipc_recv()+ipc_reply() like
 * the VFS protocol (vfs.h), req[0] = op code, all messages < 4096.
 */

#ifndef USER_SERVICES_PERM_PERM_H
#define USER_SERVICES_PERM_PERM_H

#include <stdint.h>
#include "../vfs/vfs.h"          /* vfs_resource_t, VFS_PATH_MAX, VFS_ERR_* */

/* Fixed-width types (same convention as vfs.h) */
typedef uint8_t     u8;
typedef uint32_t    u32;
typedef int32_t     i32;
typedef uint64_t    u64;

#define PERM_PORT_NAME     "perm"       /* service port  */
#define PERM_UI_PORT_NAME  "perm.ui"    /* term UI port  */

/* Query states (perm_resp_query_t.state / UI_SHOW state) */
#define PERM_QUERY_PENDING   0
#define PERM_QUERY_ALLOWED   1
#define PERM_QUERY_DENIED    2

/* Op codes (req[0]) */
enum {
    PERM_OP_QUERY   = 1,   /* UI agent → perm-manager: fetch pending query */
    PERM_OP_ANSWER  = 2,   /* UI agent/shell → perm-manager: user verdict   */
    PERM_OP_CHECK   = 3,   /* vfs_server → perm-manager: synchronous authz  */
    PERM_OP_REVOKE  = 4,   /* shell → perm-manager: drop grants             */
    PERM_OP_GRANT   = 5,   /* shell/test → perm-manager: direct grant       */
    PERM_OP_UI_SHOW = 6,   /* perm-manager → term("perm.ui"): push prompt   */
};

/* ====================================================================
 * PERM_OP_CHECK — synchronous authorization check (vfs_server)
 *
 * vfs_server calls this for every CREATE/RESOLVE_BOOKMARK.  If a grant
 * for (app_id_hash, resource) covering `access` exists, ret = 0 and the
 * operation may proceed.  Otherwise perm-manager creates a PENDING
 * query (reusing an identical pending one), pushes a UI_SHOW to
 * "perm.ui", and returns VFS_ERR_ACCESS (-105) so vfs_server answers
 * the client with -EACCES (design §8: 授权前 → -EACCES).
 * ==================================================================== */

typedef struct {
    u32            op;             /* = PERM_OP_CHECK */
    u32            app_id_hash;    /* 沙盒 app id 的 32 位哈希 */
    vfs_resource_t resource;       /* 目标资源（卷 UUID + itemID） */
    u32            access;         /* 请求的 VFS_ACCESS_* 位 */
    char           url[VFS_PATH_MAX]; /* 人类可读 URL，仅询问展示用 */
} perm_req_check_t;

typedef struct {
    i32  ret;                      /* 0 = granted; VFS_ERR_ACCESS = denied */
    u32  query_id;                 /* pending query (valid when ret < 0) */
} perm_resp_check_t;

/* ====================================================================
 * PERM_OP_ANSWER — user verdict on a query (UI agent / shell)
 *
 * allow=1 → grant record upserted (app_id_hash + resource → access),
 *           query → ALLOWED; allow=0 → query → DENIED (default deny).
 * A UI_SHOW update is pushed to "perm.ui" either way.
 * ==================================================================== */

typedef struct {
    u32  op;             /* = PERM_OP_ANSWER */
    u32  query_id;
    i32  allow;          /* 1 = 允许, 0 = 拒绝 */
} perm_req_answer_t;

typedef struct {
    i32  ret;
} perm_resp_answer_t;

/* ====================================================================
 * PERM_OP_QUERY — fetch a pending query (UI agent, decision 2)
 *
 * query_id == 0 → the first PENDING query (FIFO); otherwise the query
 * with that exact id.  Returns ERR_NOENT when none matches.
 * ==================================================================== */

typedef struct {
    u32  op;             /* = PERM_OP_QUERY */
    u32  query_id;       /* 0 = first pending */
} perm_req_query_t;

typedef struct {
    i32  ret;
    u32  query_id;
    u32  app_id_hash;
    char url[VFS_PATH_MAX];
    u32  access;
    i32  state;          /* PERM_QUERY_* */
} perm_resp_query_t;

/* ====================================================================
 * PERM_OP_REVOKE — drop grants (shell / management)
 *
 * app_id_hash == 0 → match all apps; resource with zero uuid → match
 * any resource for the app.  Returns the number of grants revoked.
 * ==================================================================== */

typedef struct {
    u32            op;          /* = PERM_OP_REVOKE */
    u32            app_id_hash; /* 0 = all */
    vfs_resource_t resource;    /* zero uuid = all resources */
} perm_req_revoke_t;

typedef struct {
    i32  ret;
    u32  revoked;               /* grants dropped */
} perm_resp_revoke_t;

/* ====================================================================
 * PERM_OP_GRANT — direct grant, bypasses the Powerbox (test only)
 *
 * Upserts (app_id_hash, resource) → access.  NOT used by the normal
 * flow — the Powerbox is the only authorization entry (§9.3); GRANT
 * exists so acceptance tests and management tools can seed grants.
 * ==================================================================== */

typedef struct {
    u32            op;          /* = PERM_OP_GRANT */
    u32            app_id_hash;
    vfs_resource_t resource;
    u32            access;
} perm_req_grant_t;

typedef struct {
    i32  ret;
} perm_resp_grant_t;

/* ====================================================================
 * PERM_OP_UI_SHOW — push prompt text to term("perm.ui") (perm-manager)
 *
 * The receiver renders one status line, e.g.
 *   "perm: app 0x1234 请求访问 /Users/a.txt (R) — perm_answer 3 y/n"
 * and updates it when state changes (ALLOWED/DENIED).
 * ==================================================================== */

typedef struct {
    u32  op;             /* = PERM_OP_UI_SHOW */
    u32  query_id;
    u32  app_id_hash;
    char url[VFS_PATH_MAX];
    u32  access;
    i32  state;          /* PERM_QUERY_* */
} perm_req_ui_t;

typedef struct {
    i32  ret;
} perm_resp_ui_t;

#endif /* USER_SERVICES_PERM_PERM_H */
