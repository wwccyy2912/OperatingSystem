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
 * Transport: flat structs over IpcCall()/IpcRecv()+IpcReply() like
 * the VFS protocol (vfs.h), req[0] = op code, all messages < 4096.
 */

#ifndef USER_SERVICES_PERM_PERM_H
#define USER_SERVICES_PERM_PERM_H

#include <stdint.h>
#include "../vfs/vfs.h" /* vfs_resource_t, VFS_PATH_MAX, VFS_ERR_* */

/* Fixed-width types (same convention as vfs.h) */
typedef uint8_t  u8;
typedef uint32_t u32;
typedef int32_t  i32;
typedef uint64_t u64;

#define PERM_PORT_NAME    "perm"    /* service port  */
#define PERM_UI_PORT_NAME "perm.ui" /* term UI port  */

/* Query states (perm_resp_query_t.state / UI_SHOW state) */
#define PERM_QUERY_PENDING 0
#define PERM_QUERY_ALLOWED 1
#define PERM_QUERY_DENIED  2

/* P1 地基: roles (docs/permission_model.md §二.2).  Every subject maps
 * to exactly one role; unknown subjects get PERM_ROLE_DEFAULT (Standard). */
enum {
    PERM_ROLE_OWNER    = 0, /* 完全控制（系统安装者/设备所有者） */
    PERM_ROLE_ADMIN    = 1, /* 系统管理（无 Owner 级硬件/固件权限） */
    PERM_ROLE_STANDARD = 2, /* 默认角色：常规日常使用 */
    PERM_ROLE_CHILD    = 3, /* 儿童模式：受限 */
    PERM_ROLE_GUEST    = 4, /* 访客：最小权限 */
    PERM_ROLE_AUDITOR  = 5, /* 审计员：只读 + 日志 */
    PERM_ROLE_DEFAULT  = PERM_ROLE_STANDARD,
    PERM_ROLE_MAX      = 6,
};

/* P1 地基: rule verdict */
#define PERM_VERDICT_DENY  0
#define PERM_VERDICT_ALLOW 1

/* Op codes (req[0]) */
enum {
    PERM_OP_QUERY       = 1,  /* UI agent → perm-manager: fetch pending query */
    PERM_OP_ANSWER      = 2,  /* UI agent/shell → perm-manager: user verdict   */
    PERM_OP_CHECK       = 3,  /* vfs_server → perm-manager: synchronous authz  */
    PERM_OP_REVOKE      = 4,  /* shell → perm-manager: drop grants             */
    PERM_OP_GRANT       = 5,  /* shell/test → perm-manager: direct grant       */
    PERM_OP_UI_SHOW     = 6,  /* perm-manager → term("perm.ui"): push prompt   */
    PERM_OP_ROLE_SET    = 7,  /* P1: management → set subject role (hot reload) */
    PERM_OP_DUMP        = 8,  /* P1: management → export policy state          */
    PERM_OP_CONTEXT     = 9,  /* P2: P3 预留 — 前台/后台上下文切换通知          */
    PERM_OP_FREQ        = 10, /* P2: P3 预留 — 查询/清零频率计数器              */
    PERM_OP_POLICY_SAVE = 11, /* P2: P4 预留 — 导出策略二进制快照             */
    PERM_OP_POLICY_LOAD = 12, /* P2: P4 预留 — 导入策略二进制快照             */
    PERM_OP_AUDIT       = 13, /* P2: P3 预留 — 导出审计环形缓冲区               */
    PERM_OP_SET_QUIET   = 14, /* management: suppress UI_SHOW pushes (tests)  */
};

/* PERM_OP_SET_QUIET — management-only: when quiet=1 the perm-manager
 * creates/answers Powerbox queries WITHOUT pushing UI_SHOW to term.
 * init uses it around its P1 permission tests so their automatic
 * answers never flash a permission panel at the user (and the user's
 * stray y/n can never leak into the shell line).  Queries still exist
 * and ANSWER/QUERY work exactly as before. */
typedef struct {
    u32 op;   /* = PERM_OP_SET_QUIET */
    u32 quiet; /* 1 = suppress UI_SHOW, 0 = normal */
} perm_req_set_quiet_t;

typedef struct {
    i32 ret;
} perm_resp_set_quiet_t;

/* ====================================================================
 * PERM_OP_CHECK — synchronous authorization check (vfs_server)
 *
 * vfs_server calls this for every CREATE/RESOLVE_BOOKMARK.  If a grant
 * for (subject_id, resource) covering `access` exists, ret = 0 and the
 * operation may proceed.  Otherwise perm-manager creates a PENDING
 * query (reusing an identical pending one), pushes a UI_SHOW to
 * "perm.ui", and returns VFS_ERR_ACCESS (-105) so vfs_server answers
 * the client with -EACCES (design §8: 授权前 → -EACCES).
 * ==================================================================== */

typedef struct {
    u32            op;                /* = PERM_OP_CHECK */
    vfs_resource_t resource;          /* 目标资源（卷 UUID + itemID） */
    u32            access;            /* 请求的 VFS_ACCESS_* 位 */
    char           url[VFS_PATH_MAX]; /* 人类可读 URL，仅询问展示用 */
    u64            subject_id;        /* P1: 请求方真实主体 — 由 vfs_server
                                       * 用 ipc_recv_from 取得后填入，绝不
                                       * 接受 app 自报值（不可伪造）。 */
    u64 scope_hash;                   /* P2: 作用域哈希（0 = 无作用域）。
                                       * 预留：P3 起用于细化同一资源在不同
                                       * 上下文下的授权。do_check 当前忽略。 */
} perm_req_check_t;

typedef struct {
    i32 ret;      /* 0 = granted; VFS_ERR_ACCESS = denied */
    u32 query_id; /* pending query (valid when ret < 0) */
    u32 granted;  /* P2: 实际被批准的 VFS_ACCESS_* 位掩码
                   * （能力化抹位）：= grant 覆盖的位，
                   * 未覆盖位一律抹去。ret=0 时有效，
                   * 调用方（vfs_server）必须用该掩码
                   * 约束后续操作，不得使用请求掩码。 */
} perm_resp_check_t;

/* ====================================================================
 * PERM_OP_ANSWER — user verdict on a query (UI agent / shell)
 *
 * allow=1 → grant record upserted (subject_id + resource → access),
 *           query → ALLOWED; allow=0 → query → DENIED (default deny).
 * A UI_SHOW update is pushed to "perm.ui" either way.
 * ==================================================================== */

typedef struct {
    u32 op; /* = PERM_OP_ANSWER */
    u32 query_id;
    i32 allow; /* 1 = 允许, 0 = 拒绝 */
} perm_req_answer_t;

typedef struct {
    i32 ret;
} perm_resp_answer_t;

/* ====================================================================
 * PERM_OP_QUERY — fetch a pending query (UI agent, decision 2)
 *
 * query_id == 0 → the first PENDING query (FIFO); otherwise the query
 * with that exact id.  Returns ERR_NOENT when none matches.
 * ==================================================================== */

typedef struct {
    u32 op;       /* = PERM_OP_QUERY */
    u32 query_id; /* 0 = first pending */
} perm_req_query_t;

typedef struct {
    i32  ret;
    u32  query_id;
    u32  pid;      /* 发起进程 PID（展示元数据，内核解析） */
    char name[64]; /* 发起进程名（NUL 结尾，内核解析） */
    char url[VFS_PATH_MAX];
    u32  access;
    u64  subject_id; /* P1: 发起主体（不可伪造，内核填充） */
    i32  state;      /* PERM_QUERY_* */
    char label[128]; /* P1: perm 聚合的人类可读描述（UI 直接展示） */
} perm_resp_query_t;

/* ====================================================================
 * PERM_OP_REVOKE — drop grants (shell / management)
 *
 * subject_id == 0 → match all subjects; resource with zero uuid → match
 * any resource for the subject.  Returns the number of grants revoked.
 * ==================================================================== */

typedef struct {
    u32            op;         /* = PERM_OP_REVOKE */
    u64            subject_id; /* 0 = all subjects */
    vfs_resource_t resource;   /* zero uuid = all resources */
} perm_req_revoke_t;

typedef struct {
    i32 ret;
    u32 revoked; /* grants dropped */
} perm_resp_revoke_t;

/* ====================================================================
 * PERM_OP_GRANT — direct grant, bypasses the Powerbox (test only)
 *
 * Upserts (subject_id, resource) → access.  NOT used by the normal
 * flow — the Powerbox is the only authorization entry (§9.3); GRANT
 * exists so acceptance tests and management tools can seed grants.
 * ==================================================================== */

typedef struct {
    u32            op; /* = PERM_OP_GRANT */
    vfs_resource_t resource;
    u32            access;
    u64            subject_id; /* P1: 目标主体（0 = 任意发起者） */
    u32            atom;       /* P1: 授权时签发的权限 atom */
} perm_req_grant_t;

typedef struct {
    i32 ret;
} perm_resp_grant_t;

/* ====================================================================
 * P1: PERM_OP_ROLE_SET — set a subject's role (management hot reload)
 *
 * Requests must be signed by a subject whose role is OWNER or ADMIN
 * (management plane).  Unknown subject_id (0 or not live) → ERR_NOENT;
 * invalid role → ERR_INVAL.  Applies immediately to all subsequent
 * checks — grants are NOT rewritten (grants beat role defaults, see
 * docs/permission_model.md §四).
 * ==================================================================== */

typedef struct {
    u32 op;         /* = PERM_OP_ROLE_SET */
    u64 subject_id; /* target subject */
    u32 role;       /* PERM_ROLE_* */
} perm_req_role_set_t;

typedef struct {
    i32 ret;
    u32 role; /* current role on success */
} perm_resp_role_set_t;

/* ====================================================================
 * P1: PERM_OP_DUMP — export policy state (management / tests)
 *
 * Returns the role map and the full rule table so tests can assert on
 * policy without probing behavior.  Dump stays flat and small: roles
 * first, then one rule per line.
 * ==================================================================== */

#define PERM_DUMP_LINE_MAX 96

typedef struct {
    u32 op; /* = PERM_OP_DUMP */
} perm_req_dump_t;

typedef struct {
    i32  ret;
    u32  role_count;
    u32  rule_count;
    u32  grant_count;
    char lines[8][PERM_DUMP_LINE_MAX]; /* text dump (roles+rules) */
} perm_resp_dump_t;

/* ====================================================================
 * PERM_OP_UI_SHOW — push prompt text to term("perm.ui") (perm-manager)
 *
 * The receiver renders one status line, e.g.
 *   "perm: init (PID 1) 请求访问 /Users/a.txt (R) — perm_answer 3 y/n"
 * and updates it when state changes (ALLOWED/DENIED).
 * ==================================================================== */

typedef struct {
    u32  op; /* = PERM_OP_UI_SHOW */
    u32  query_id;
    u64  subject_id; /* 发起主体（不可伪造） */
    u32  pid;        /* 发起进程 PID（展示元数据） */
    char name[64];   /* 发起进程名（NUL 结尾） */
    char url[VFS_PATH_MAX];
    u32  access;
    i32  state;      /* PERM_QUERY_* */
    char label[128]; /* P1: perm 聚合的完整提示文本，term 原样展示 */
} perm_req_ui_t;

typedef struct {
    i32 ret;
} perm_resp_ui_t;

/* ====================================================================
 * P2: P3 预留 — PERM_OP_CONTEXT — 前台/后台上下文切换通知
 *
 * 界面层在应用切入/切出前台时通知 perm-manager。预留接口：P3 将
 * 结合 scope_hash 与 freq 计数器实现"前台可访问、后台受限"的
 * 上下文感知授权。当前实现仅维护每主体前台/后台标记（upsert），
 * 不做强制。
 * ==================================================================== */

typedef struct {
    u32 op;         /* = PERM_OP_CONTEXT */
    u64 subject_id; /* 目标主体 */
    u32 foreground; /* 1 = 前台, 0 = 后台 */
} perm_req_context_t;

typedef struct {
    i32 ret;
} perm_resp_context_t;

/* ====================================================================
 * P2: P3 预留 — PERM_OP_FREQ — 授权命中频率计数器
 *
 * perm-manager 在每次 grant 命中（do_check 的授权分支）时对
 * (subject_id, atom) 计数递增。本接口供测试/管理查询（reset=1 时
 * 清零并保留槽位）。P3 将用它实现频率阈值策略。
 * ==================================================================== */

#define PERM_FREQ_MAX 32

typedef struct {
    u32 op;         /* = PERM_OP_FREQ */
    u64 subject_id; /* 0 = 全部主体 */
    u32 atom;       /* 0 = 全部 atom */
    u32 reset;      /* 1 = 查询后清零计数 */
} perm_req_freq_t;

typedef struct {
    i32 ret;
    u32 count; /* 命中总数（reset=1 时返回清零前的值） */
    u32 slots; /* 占用的计数器槽位数 */
} perm_resp_freq_t;

/* ====================================================================
 * P2: P4 预留 — PERM_OP_POLICY_SAVE / PERM_OP_POLICY_LOAD
 *
 * 策略二进制快照：16 字节头部（magic "POLY" = 0x504F4C59, version=1,
 * grant_count, role_count）+ 按表序排列的 grants + roles。
 * 表序填充保证 save→load→save 字节一致。LOAD 为全有或全无：
 * 校验失败则完全不动当前策略；成功后按"热更新"语义对每条 grant
 * 先 revoke 原 atom 再签发新 atom（见补充六）。
 * ==================================================================== */

#define PERM_POLICY_MAGIC   0x504F4C59 /* "POLY" */
#define PERM_POLICY_VERSION 1
#define PERM_POLICY_MAX     3840 /* 头部16 + 快照内容，< 4096 报文 */

typedef struct {
    u32 op;                    /* = PERM_OP_POLICY_SAVE / PERM_OP_POLICY_LOAD */
    u32 size;                  /* LOAD: 快照字节数（SAVE 忽略，置 0） */
    u8  data[PERM_POLICY_MAX]; /* LOAD: 待导入快照（SAVE 忽略） */
} perm_req_policy_t;

typedef struct {
    i32 ret;
    u32 size;                  /* 快照字节数（0 = 无内容） */
    u8  data[PERM_POLICY_MAX]; /* 序列化快照 */
} perm_resp_policy_t;

/* ====================================================================
 * P2: P3 预留 — PERM_OP_AUDIT — 导出审计环形缓冲区
 *
 * perm-manager 对每一次 do_check 判定（授权/拒绝/能力抹位/缺省）
 * 都追加一条审计记录（环形，PERM_AUDIT_MAX 条）。测试/审计员可
 * 导出校验。verdict: 0=授权 1=拒绝（含缺省拒绝）。
 * ==================================================================== */

#define PERM_AUDIT_MAX 64

typedef struct {
    u64            tick;       /* 内核 tick */
    u64            subject_id; /* 发起主体 */
    u32            atom;       /* 命中的权限 atom（拒绝时为 0） */
    u32            verdict;    /* 0 = granted, 1 = denied */
    vfs_resource_t resource;
} perm_audit_ent_t;

typedef struct {
    u32 op; /* = PERM_OP_AUDIT */
} perm_req_audit_t;

typedef struct {
    i32              ret;
    u32              count; /* 有效审计条目数（≤ PERM_AUDIT_MAX） */
    perm_audit_ent_t entries[PERM_AUDIT_MAX];
} perm_resp_audit_t;

#endif /* USER_SERVICES_PERM_PERM_H */
