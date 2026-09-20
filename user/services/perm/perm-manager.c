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
 * perm-manager.c - Powerbox permission manager (ring-3, independent process)
 * Copyright (c) 2026 OpSys Project
 *
 * Holds the SINGLE source of truth for access decisions (design §9.4,
 * docs/permission_model.md): the grant table, the role table and the
 * rule chains.  vfs_server consults it on every CREATE/RESOLVE_BOOKMARK;
 * the term service renders the text prompt pushed on "perm.ui".
 *
 * P1 地基 — role engine (docs/permission_model.md §二/§四):
 *   - Every subject maps to exactly one role (default: Standard).
 *   - Rule chains are keyed by (role, atom); FIRST match wins
 *     (override-first).  A chain verdict overrides the default.
 *   - Decision order (§四): 1) explicit grant (powerbox result) beats
 *     everything; 2) role-chain verdict (override-first); 3) default
 *     deny → Powerbox prompt (pending query + UI_SHOW push).
 *   - The check path is decision-encoding: ALLOW lands as an atom
 *     capability issued INTO the subject's kernel table via
 *     CapGrantToSubject() — the capability IS the encoded decision.
 *   - Requests carry the caller's kernel subject (ipc_recv_from for
 *     management ops; CHECK requests are filled by the trusted
 *     vfs_server from ITS recv — never app-supplied, unforgeable).
 *   - ROLE_SET is management-plane only (OWNER/ADMIN caller).  The
 *     bootstrap: subject 1 (init, the device owner) is seeded OWNER at
 *     startup; vfs_server's subject (learned via the WHOAMI handshake)
 *     is seeded ADMIN.
 *
 * v1.0 — P3/P4 从"预留"变为"生效" (docs/permission_model.md §十三):
 *   - 授权带生命周期: expiry_ticks（惰性过期 → PERM_EV_EXPIRE 审计）、
 *     scope_hash（作用域不符 → PERM_DEC_SCOPE_MISMATCH，继续角色链）、
 *     source（POWERBOX / DIRECT / POLICY）。
 *   - 上下文感知: 登记为后台的主体走到"默认拒绝"分支时**不建询问、不推
 *     UI_SHOW**，直接 VFS_ERR_ACCESS + PERM_DEC_BACKGROUND；未登记的主体
 *     一律按前台处理（P1/P2 语义逐字不变）。
 *   - 频率/隔离: (subject, atom) 统计命中/拒绝，滚动窗口 1000 tick，
 *     拒绝达 PERM_DENY_THRESHOLD(8) → 隔离 PERM_QUARANTINE_TICKS(3000)：
 *     期间一律拒绝且不弹窗（PERM_DEC_QUARANTINED）。
 *   - 审计全覆盖: CHECK/ANSWER/GRANT/REVOKE/ROLE_SET/CONTEXT/QUARANTINE/
 *     POLICY_SAVE/POLICY_LOAD/EXPIRE，PERM_OP_AUDIT 支持主体/裁决/时间/
 *     条数过滤（最旧在前）。
 *   - 策略快照 v2（grant 记录带 expiry/scope/source），仍能读 v1 快照。
 *
 * Ops (perm.h):
 *   CHECK   3   vfs_server → sync authz; not granted → rule chain →
 *               default deny → pending query + UI_SHOW → VFS_ERR_ACCESS.
 *   ANSWER  2   user verdict → grant upsert + atom issue / deny.
 *   QUERY   1   UI agent fetches a pending query.
 *   REVOKE  4   drop grants (default deny semantics).
 *   GRANT   5   direct grant, bypasses Powerbox (tests/management).
 *   ROLE_SET 7   management: set a subject's role (hot reload).
 *   DUMP    8   export role map + rule table (tests/management).
 *   CONTEXT  9 / CTX_QUERY 15  foreground table (CONTEXT writes; v1.0
 *               enforcement reads it), FREQ 10 counters + quarantine,
 *               POLICY_SAVE/LOAD 11/12 policy snapshots (v2), AUDIT 13
 *               filtered audit export, SET_QUIET 14 UI_SHOW switch.
 *
 * ------------------------------------------------------------------
 * Structure (PermManagerMain):
 *   main() -> registers "perm" port, ipc_recv dispatch loop
 *     grant table | role table | rule chains   (single source of truth)
 *     Powerbox panel: pending query queue + "perm.ui" push to term
 *   CHECK path: vfs_server fills caller subject -> decision encoding
 *     ALLOW = CapGrantToSubject() atom; else chain -> default deny
 * How it works:
 *   vfs_server consults it on CREATE/RESOLVE_BOOKMARK; the decision
 *   order is explicit grant, role-chain verdict (override-first),
 *   default deny -> Powerbox prompt.  ALLOW issues an atom capability
 *   into the subject's kernel table; term renders the "perm.ui" prompt.
 *   v1.0 adds four inputs to that order: a quarantined (subject, atom)
 *   is refused before anything else, a grant is only "found" while its
 *   TTL has not passed and its scope covers the request, a subject
 *   registered as BACKGROUND never reaches the Powerbox, and every
 *   branch also feeds the (subject, atom) frequency counters.
 * Purpose:
 *   The Powerbox permission manager: single authority for access
 *   decisions, encoding each ALLOW as a kernel atom capability.
 * Caveats:
 *   CHECK requests are filled by trusted vfs_server from its own recv
 *   (unforgeable); ROLE_SET is management-plane (OWNER/ADMIN) only.
 *   Quarantine is scoped to the (subject, atom) that tripped the
 *   threshold, so a subject that only misbehaves on one atom keeps the
 *   rest of its capability surface.
 * ------------------------------------------------------------------
 */

#include <stdint.h>
#include "../lib/libc/stdio.h"
#include "../lib/libc/string.h"
#include "../lib/libos/syscalls.h"
#include "perm.h"

/* ====================================================================
 * Constants
 * ==================================================================== */

#define PERM_MAX_GRANTS  64 /* grant table size */
#define PERM_MAX_QUERIES 16 /* query queue depth */
#define PERM_MAX_ROLES   64 /* role table size */
#define PERM_MAX_RULES   96 /* rule table size */
#define PERM_MAX_URL     VFS_PATH_MAX

/* v1.0 frequency/quarantine table: 48 B per slot × 64 slots ≈ 3 KiB —
 * enough for every (subject, atom) pair that ever reached DoCheck during
 * a boot, and small next to the query table (~19 KiB). */
#define PERM_FREQ_SLOTS 64

/* Policy snapshot versions.  perm.h keeps PERM_POLICY_VERSION = 1 for the
 * frozen interface; the writer emits v2 and the reader accepts BOTH
 * layouts (a v1 file is parsed with the legacy record and its new fields
 * default to 0 — see the policy section below). */
#define PERM_POLICY_VERSION_V1 PERM_POLICY_VERSION /* 1 = legacy layout */
#define PERM_POLICY_VERSION_V2 2u                  /* 2 = v1.0 layout  */

/* Rule-chain head index: [role][atom] → first rule, 0xFFFFFFFF = empty */
#define PERM_CHAIN_NONE 0xFFFFFFFFu

/* Bootstrap: subject 1 = init = the device owner (kernel assigns
 * subjects sequentially; init is the first user subject — see
 * kernel/process/process.c). */
#define PERM_BOOTSTRAP_SUBJECT 1u
#define PERM_BOOTSTRAP_ROLE    PERM_ROLE_OWNER

/* Request/response buffers (all perm messages < 4096) */
static u8 s_req[VFS_IPC_MAX];
static u8 s_resp[VFS_IPC_MAX];

/* Forward decls (defined later in this file) */
static u32  AtomFromAccess(u32 access);
static int  DecisionEncode(u64 subject_id, u32 atom);
static void FmtAppend(char *dst, int dst_len, int *pos, const char *s);
static void FmtUint(char *dst, int dst_len, int *pos, unsigned v, int base);
static void AuditAppend(u64 subject_id, u32 atom, u32 verdict, const vfs_resource_t *res,
                        u32 event);

/* Zeroed resource for audits that are not about one object (role change,
 * context switch, quarantine, policy I/O). */
static const vfs_resource_t s_res_none = {0};

static const vfs_resource_t *ResNone(void) {
    return &s_res_none;
}

/* ====================================================================
 * Grant table — (subject_id, resource, scope) → access mask + lifetime
 * ==================================================================== */

typedef struct {
    int in_use;
    u64 subject_id; /* 授权主体（0 = 任意发起者）— 授权表
                     * 的唯一身份键（不可伪造，内核填充） */
    vfs_resource_t resource;
    u32            access;
    u64            expiry_ticks; /* v1.0: 绝对 tick 截止；0 = 永久 */
    u32            scope_hash;   /* v1.0: 0 = 不限作用域 */
    u32            source;       /* v1.0: PERM_SRC_POWERBOX/DIRECT/POLICY */
} perm_grant_t;

static perm_grant_t s_grants[PERM_MAX_GRANTS];

/* ====================================================================
 * Role table — subject_id → role (single source of truth §二.2)
 * ==================================================================== */

typedef struct {
    int in_use;
    u64 subject_id;
    u32 role;
} perm_role_t;

static perm_role_t s_roles[PERM_MAX_ROLES];

/* ====================================================================
 * Rule table — (role, atom) chains, override-first (§四)
 * ==================================================================== */

typedef struct {
    int in_use;
    u32 role;
    u32 atom;
    i32 verdict; /* PERM_VERDICT_ALLOW / PERM_VERDICT_DENY */
    u32 next;    /* next rule index in the chain / CHAIN_NONE */
} perm_rule_t;

static perm_rule_t s_rules[PERM_MAX_RULES];
static u32         s_rule_head[PERM_ROLE_MAX][ATOM_MAX + 1];
static u32         s_rule_count;

/* ====================================================================
 * Query table — transient Powerbox consent requests
 * ==================================================================== */

typedef struct {
    int            in_use;
    u32            query_id;
    vfs_resource_t resource;
    u32            access;
    u32            atom;       /* P1: atom this decision encodes */
    u64            subject_id; /* P1: requesting subject (unforgeable) */
    u32            pid;        /* requesting process PID (display meta) */
    char           name[64];   /* requesting process name(NUL-terminated) */
    char           url[PERM_MAX_URL];
    u32            scope_hash; /* v1.0: scope of the check that asked */
    i32            state;      /* PERM_QUERY_* */
} perm_query_t;

static perm_query_t s_queries[PERM_MAX_QUERIES];
static u32          s_query_seq; /* monotonic query-id counter */

/* Lazily-resolved "perm.ui" port (term registers it at startup). */
static int s_ui_port = -1;

/* Quiet mode (v0.7.1): while set, Powerbox queries are created and can
 * be answered, but UI_SHOW is NOT pushed to term — no permission panel
 * flashes at the user.  init arms it around its P1 permission tests,
 * whose automatic answers would otherwise pop a panel the user cannot
 * act on (and a stray y/n would leak into the shell line). */
static int s_quiet;

/* ====================================================================
 * Grant table helpers
 * ==================================================================== */

/* v1.0 lazy expiry (§13.1): a grant whose absolute deadline passed is
 * not "found" any more — the slot is cleared in place and one
 * PERM_EV_EXPIRE audit entry (verdict = denied) records the reaping. */
static int GrantExpired(const perm_grant_t *g, u64 now) {
    return g->expiry_ticks != 0 && now >= g->expiry_ticks;
}

static void GrantReap(perm_grant_t *g) {
    printf("perm: grant expired subject=%u access=%u\n",
           (unsigned)g->subject_id,
           (unsigned)g->access);
    AuditAppend(g->subject_id, AtomFromAccess(g->access), PERM_VERDICT_DENY, &g->resource,
                PERM_EV_EXPIRE);
    g->in_use = 0;
}

/* Decision-side lookup (the §13.1 "grant_find"): the first LIVE grant for
 * (subject, resource) whose scope covers the request and whose mask
 * covers `access`.
 *
 * Scope rules: a request with scope_hash == 0 (unscoped) accepts any
 * grant; a scoped request only accepts an identical scope.  A grant that
 * covers the requested bits but was issued for ANOTHER scope is reported
 * through *mismatch — the caller keeps walking the decision chain
 * (PERM_DEC_SCOPE_MISMATCH) and must never treat it as a hit.  Expired
 * grants are reaped here and reported through *expired. */
static perm_grant_t *grant_find(u64 subject_id, const vfs_resource_t *res, u32 scope_hash,
                                u32 access, u64 now, int *expired, int *mismatch) {
    perm_grant_t *hit = NULL;
    if (expired)
        *expired = 0;
    if (mismatch)
        *mismatch = 0;
    for (int i = 0; i < PERM_MAX_GRANTS; i++) {
        perm_grant_t *g = &s_grants[i];
        if (!g->in_use)
            continue;
        if (g->subject_id != 0 && g->subject_id != subject_id)
            continue; /* 0 = any subject（通配授权） */
        if (memcmp(&g->resource, res, sizeof(*res)) != 0)
            continue;
        if (GrantExpired(g, now)) {
            GrantReap(g);
            if (expired)
                *expired = 1;
            continue;
        }
        if (scope_hash == 0 || g->scope_hash == scope_hash) {
            if (hit == NULL && (g->access & access) != 0)
                hit = g;
            continue;
        }
        if ((g->access & access) != 0 && mismatch)
            *mismatch = 1; /* covers the bits, wrong scope */
    }
    return hit;
}

/* Upsert-side lookup: an existing live slot with the SAME
 * (subject, resource, scope) — a re-grant of the same tuple extends the
 * mask instead of eating a second slot. */
static perm_grant_t *grant_slot(u64 subject_id, const vfs_resource_t *res, u32 scope_hash,
                                u64 now) {
    for (int i = 0; i < PERM_MAX_GRANTS; i++) {
        perm_grant_t *g = &s_grants[i];
        if (!g->in_use)
            continue;
        if (g->subject_id != 0 && g->subject_id != subject_id)
            continue;
        if (memcmp(&g->resource, res, sizeof(*res)) != 0)
            continue;
        if (GrantExpired(g, now)) {
            GrantReap(g); /* a dead grant never absorbs a re-grant */
            continue;
        }
        if (g->scope_hash != scope_hash)
            continue;
        return g;
    }
    return NULL;
}

/* Upsert: extend the access mask of an existing grant, else allocate.
 * Keyed by (subject_id, resource, scope_hash); subject_id 0 = any
 * subject.  The newest decision refreshes the lifetime and the
 * provenance, so a permanent re-grant clears an earlier TTL (§13.1). */
static perm_grant_t *grant_upsert(u64 subject_id, const vfs_resource_t *res, u32 access,
                                  u64 expiry_ticks, u32 scope_hash, u32 source) {
    u64           now = (u64)GetTime();
    perm_grant_t *g   = grant_slot(subject_id, res, scope_hash, now);
    if (g) {
        g->access |= access;
        g->expiry_ticks = expiry_ticks;
        g->source       = source;
        if (g->subject_id == 0)
            g->subject_id = subject_id; /* narrow a wildcard grant */
        return g;
    }
    for (int i = 0; i < PERM_MAX_GRANTS; i++) {
        if (!s_grants[i].in_use) {
            g = &s_grants[i];
            memset(g, 0, sizeof(*g));
            g->in_use       = 1;
            g->subject_id   = subject_id;
            g->resource     = *res;
            g->access       = access;
            g->expiry_ticks = expiry_ticks;
            g->scope_hash   = scope_hash;
            g->source       = source;
            return g;
        }
    }
    return NULL; /* table full */
}

/* Count and drop grants matching subject (0 = all) and resource
 * (zero uuid = all).  Returns the number revoked. */
static u32 GrantRevoke(u64 subject_id, const vfs_resource_t *res) {
    u32 revoked = 0;
    for (int i = 0; i < PERM_MAX_GRANTS; i++) {
        perm_grant_t *g = &s_grants[i];
        if (!g->in_use)
            continue;
        if (subject_id != 0 && g->subject_id != subject_id)
            continue;
        if (res->vol.hi != 0 || res->vol.lo != 0 || res->id != 0) {
            if (memcmp(&g->resource, res, sizeof(*res)) != 0)
                continue;
        }
        AuditAppend(g->subject_id, AtomFromAccess(g->access), PERM_VERDICT_DENY, &g->resource,
                    PERM_EV_REVOKE);
        g->in_use = 0;
        revoked++;
    }
    return revoked;
}

/* ====================================================================
 * Role table helpers (§二.2)
 * ==================================================================== */

static u32 RoleOf(u64 subject_id) {
    for (int i = 0; i < PERM_MAX_ROLES; i++) {
        perm_role_t *r = &s_roles[i];
        if (r->in_use && r->subject_id == subject_id)
            return r->role;
    }
    return PERM_ROLE_DEFAULT;
}

/* Set (upsert) a subject's role — the ROLE_SET hot-reload write path. */
static int RoleSet(u64 subject_id, u32 role) {
    if (role >= PERM_ROLE_MAX)
        return ERR_INVAL;
    for (int i = 0; i < PERM_MAX_ROLES; i++) {
        perm_role_t *r = &s_roles[i];
        if (r->in_use && r->subject_id == subject_id) {
            r->role = role;
            return OK;
        }
    }
    for (int i = 0; i < PERM_MAX_ROLES; i++) {
        if (!s_roles[i].in_use) {
            s_roles[i].in_use     = 1;
            s_roles[i].subject_id = subject_id;
            s_roles[i].role       = role;
            return OK;
        }
    }
    return ERR_NOMEM; /* role table full */
}

/* Management plane: OWNER or ADMIN may change policy (ROLE_SET). */
static int RoleIsManagement(u64 subject_id) {
    u32 r = RoleOf(subject_id);
    return (r == PERM_ROLE_OWNER || r == PERM_ROLE_ADMIN) ? 1 : 0;
}

/* ====================================================================
 * Rule chain helpers (§四 — override-first)
 * ==================================================================== */

/* Seed a rule for (role, atom): push onto the chain head so the NEWEST
 * rule wins (override-first). */
static void RuleSeed(u32 role, u32 atom, i32 verdict) {
    if (role >= PERM_ROLE_MAX || atom > ATOM_MAX)
        return;
    if (s_rule_count >= PERM_MAX_RULES)
        return;
    perm_rule_t *r          = &s_rules[s_rule_count];
    r->in_use               = 1;
    r->role                 = role;
    r->atom                 = atom;
    r->verdict              = verdict;
    r->next                 = s_rule_head[role][atom];
    s_rule_head[role][atom] = s_rule_count;
    s_rule_count++;
}

/* Lookup: first matching rule in the (role, atom) chain.  Returns the
 * verdict, or -1 when the chain has no rule for (role, atom). */
static i32 RuleLookup(u32 role, u32 atom) {
    if (role >= PERM_ROLE_MAX || atom > ATOM_MAX)
        return -1;
    u32 idx = s_rule_head[role][atom];
    while (idx != PERM_CHAIN_NONE) {
        perm_rule_t *r = &s_rules[idx];
        if (r->in_use && r->verdict == PERM_VERDICT_ALLOW)
            return PERM_VERDICT_ALLOW;
        if (r->in_use && r->verdict == PERM_VERDICT_DENY)
            return PERM_VERDICT_DENY;
        idx = r->next;
    }
    return -1; /* no rule → default deny + Powerbox */
}

/* ====================================================================
 * P3: frequency counters + quarantine (§13.3) — a POLICY INPUT now
 *
 * One slot per (subject, atom): hits / denies inside a rolling window.
 * PERM_DENY_THRESHOLD denials within PERM_DENY_WINDOW_TICKS put that
 * (subject, atom) into quarantine for PERM_QUARANTINE_TICKS: every
 * further request is refused WITHOUT a Powerbox prompt, so a rogue or
 * background app cannot spam the user with panels.
 *
 * The quarantine is scoped to the (subject, atom) that tripped the
 * threshold — a subject that only misbehaves on WRITE keeps its READ
 * path (deliberate: the boot self-tests deny a handful of WRITE, READ
 * and EXEC checks for init and must keep the rest of the run working).
 * ==================================================================== */

typedef struct {
    int in_use;
    u64 subject_id;
    u32 atom;
    u32 hits;             /* granted decisions inside the window */
    u32 denies;           /* denied decisions inside the window  */
    u64 window_start;     /* start of the rolling deny window    */
    u64 quarantine_until; /* absolute tick; 0 = not quarantined  */
} perm_freq_ent_t;

static perm_freq_ent_t s_freq[PERM_FREQ_SLOTS];

static perm_freq_ent_t *FreqFind(u64 subject_id, u32 atom, int create) {
    for (int i = 0; i < PERM_FREQ_SLOTS; i++) {
        perm_freq_ent_t *f = &s_freq[i];
        if (f->in_use && f->subject_id == subject_id && f->atom == atom)
            return f;
    }
    if (!create)
        return NULL;
    for (int i = 0; i < PERM_FREQ_SLOTS; i++) {
        if (!s_freq[i].in_use) {
            perm_freq_ent_t *f = &s_freq[i];
            memset(f, 0, sizeof(*f));
            f->in_use     = 1;
            f->subject_id = subject_id;
            f->atom       = atom;
            return f;
        }
    }
    return NULL; /* table full — counting degrades, decisions do not */
}

/* Rolling window: a subject that behaves for PERM_DENY_WINDOW_TICKS
 * gets its denial count cleared (§13.3). */
static void FreqWindow(perm_freq_ent_t *f, u64 now) {
    if (f->window_start == 0 || now < f->window_start ||
        now - f->window_start > PERM_DENY_WINDOW_TICKS) {
        f->window_start = now;
        f->denies       = 0;
    }
}

/* Release a quarantine — deadline passed, or PERM_OP_FREQ
 * clear_quarantine=1.  The denial window restarts too, so a single
 * fresh denial cannot re-arm a 30-second quarantine. */
static void FreqRelease(perm_freq_ent_t *f, u64 now) {
    f->quarantine_until = 0;
    f->denies           = 0;
    f->window_start     = now;
    AuditAppend(f->subject_id, f->atom, PERM_VERDICT_ALLOW, ResNone(), PERM_EV_QUARANTINE);
}

/* 1 = that (subject, atom) is quarantined right now.  Expiry is lazy:
 * the deadline passing clears the state (and is audited) on this call. */
static int FreqQuarantined(perm_freq_ent_t *f, u64 now) {
    if (!f || f->quarantine_until == 0)
        return 0;
    if (now < f->quarantine_until)
        return 1;
    printf("perm: quarantine released subject=%u atom=%u\n",
           (unsigned)f->subject_id,
           f->atom);
    FreqRelease(f, now);
    return 0;
}

/* Granted decision (grant beat or role-chain ALLOW) → hits++. */
static void FreqHit(perm_freq_ent_t *f) {
    if (f)
        f->hits++;
}

/* Denied decision (chain deny / default deny / background / quarantine)
 * → denies++, arming the quarantine once the threshold is reached. */
static void FreqDeny(perm_freq_ent_t *f, u64 now) {
    if (!f)
        return;
    FreqWindow(f, now);
    f->denies++;
    if (f->quarantine_until == 0 && f->denies >= PERM_DENY_THRESHOLD) {
        f->quarantine_until = now + PERM_QUARANTINE_TICKS;
        AuditAppend(f->subject_id, f->atom, PERM_VERDICT_DENY, ResNone(), PERM_EV_QUARANTINE);
        printf("perm: subject=%u atom=%u quarantined for %u ticks (%u denials)\n",
               (unsigned)f->subject_id,
               f->atom,
               (unsigned)PERM_QUARANTINE_TICKS,
               (unsigned)f->denies);
    }
}

/* ====================================================================
 * P2: context tracking (P3 预留 — 前台/后台感知授权)
 *
 * Upsert-only; no enforcement yet (P3 will combine it with scope_hash).
 * ==================================================================== */

typedef struct {
    int in_use;
    u64 subject_id;
    u32 foreground; /* 1 = 前台, 0 = 后台 */
} perm_ctx_ent_t;

#define PERM_CTX_SLOTS 16

static perm_ctx_ent_t s_ctx[PERM_CTX_SLOTS];

static int CtxUpsert(u64 subject_id, u32 foreground) {
    if (subject_id == 0)
        return ERR_INVAL;
    for (int i = 0; i < PERM_CTX_SLOTS; i++) {
        perm_ctx_ent_t *c = &s_ctx[i];
        if (c->in_use && c->subject_id == subject_id) {
            c->foreground = foreground ? 1 : 0;
            return 0;
        }
    }
    for (int i = 0; i < PERM_CTX_SLOTS; i++) {
        if (!s_ctx[i].in_use) {
            perm_ctx_ent_t *c = &s_ctx[i];
            c->in_use         = 1;
            c->subject_id     = subject_id;
            c->foreground     = foreground ? 1 : 0;
            return 0;
        }
    }
    return ERR_NOMEM; /* context table full */
}

/* 0 = registered as BACKGROUND, 1 = foreground or UNREGISTERED.  The
 * unregistered default is what keeps the P1/P2 semantics ("default deny
 * → Powerbox prompt") identical for every subject that never declared a
 * context (§13.2). */
static int CtxForeground(u64 subject_id) {
    for (int i = 0; i < PERM_CTX_SLOTS; i++) {
        const perm_ctx_ent_t *c = &s_ctx[i];
        if (c->in_use && c->subject_id == subject_id)
            return c->foreground ? 1 : 0;
    }
    return 1;
}

/* 1 = the subject is quarantined for ANY atom (used by the context
 * listing; the decision path asks per (subject, atom)). */
static int CtxQuarantined(u64 subject_id, u64 now) {
    for (int i = 0; i < PERM_FREQ_SLOTS; i++) {
        perm_freq_ent_t *f = &s_freq[i];
        if (!f->in_use || f->subject_id != subject_id)
            continue;
        if (FreqQuarantined(f, now))
            return 1;
    }
    return 0;
}

/* Fill the shared perm_resp_context_t (CONTEXT list=1 / CTX_QUERY). */
static void CtxFill(perm_resp_context_t *resp, u64 now) {
    u32 n = 0;
    for (int i = 0; i < PERM_CTX_SLOTS && n < PERM_CTX_LIST_MAX; i++) {
        const perm_ctx_ent_t *c = &s_ctx[i];
        if (!c->in_use)
            continue;
        resp->entries[n].subject_id  = c->subject_id;
        resp->entries[n].foreground  = c->foreground;
        resp->entries[n].quarantined = (u32)CtxQuarantined(c->subject_id, now);
        n++;
    }
    resp->count = n;
}

/* ====================================================================
 * P2: audit ring (P3 预留 — 审计导出)
 *
 * Every do_check decision path appends one entry (ring of
 * PERM_AUDIT_MAX).  verdict: 0 = granted, 1 = denied.
 * ==================================================================== */

static perm_audit_ent_t s_audit[PERM_AUDIT_MAX];
static u32              s_audit_head;  /* next slot to write */
static u32              s_audit_count; /* valid entries (≤ PERM_AUDIT_MAX) */

/* Every state change lands here (§13.4).  `event` says WHAT happened
 * (PERM_EV_*) and `verdict` whether the outcome was permissive: a GRANT
 * is granted, a REVOKE/EXPIRE is denied, a quarantine release is
 * granted. */
static void AuditAppend(u64 subject_id, u32 atom, u32 verdict, const vfs_resource_t *res,
                        u32 event) {
    perm_audit_ent_t *e = &s_audit[s_audit_head];
    e->tick             = (u64)GetTime();
    e->subject_id       = subject_id;
    e->atom             = atom;
    e->verdict          = verdict;
    e->event            = event;
    e->resource         = *res;
    s_audit_head        = (s_audit_head + 1) % PERM_AUDIT_MAX;
    if (s_audit_count < PERM_AUDIT_MAX)
        s_audit_count++;
}

/* ====================================================================
 * P4: policy snapshot serialization (§13.5) — v2 writer, v1 reader
 *
 * v2 layout (packed, deterministic — table order, empty slots zeroed,
 * so save→load→save is byte-identical):
 *
 *   header  : magic u32, version u32, grant_count u32, role_count u32
 *   grants  : PERM_MAX_GRANTS × { subject_id u64, expiry_ticks u64,
 *              access u32, scope_hash u32, source u8,
 *              vfs_resource_t }                            (49 B each)
 *   roles   : PERM_MAX_ROLES  × { subject_id u64, role u8 }   (9 B each)
 *
 *   size = 16 + 64*49 + 64*9 = 3728 ≤ PERM_POLICY_MAX (3840) ✓
 *
 * v1 was 16 + 64*36 + 64*12 = 3088 B; the v2 grant record grows by 13 B,
 * so `source` and `role` ride as u8 — that keeps all 64 grant and 64
 * role slots inside the frozen PERM_POLICY_MAX / 4096-byte IPC message.
 *
 * v1 files (perm.h PERM_POLICY_VERSION = 1) are still READ: the legacy
 * record has no lifetime/scope/provenance record, so those fields come
 * back as 0 — except `source`, which becomes PERM_SRC_POLICY, because a
 * grant that only ever existed inside a policy file can only have
 * policy provenance.  A v2 record carries its own provenance and is
 * restored verbatim: re-stamping it on import would make save→load→save
 * differ (P2V test 3 asserts that round trip is byte-identical).
 * ==================================================================== */

typedef struct __attribute__((packed)) {
    u32 magic;
    u32 version;
    u32 grant_count;
    u32 role_count;
} perm_policy_hdr_t;

typedef struct __attribute__((packed)) {
    u64            subject_id; /* 0 + access 0 = empty slot */
    u64            expiry_ticks; /* absolute tick; 0 = permanent */
    u32            access;
    u32            scope_hash;
    u8             source; /* PERM_SRC_* */
    vfs_resource_t resource;
} perm_policy_grant_ent_t;

typedef struct __attribute__((packed)) {
    u64 subject_id;
    u8  role;
} perm_policy_role_ent_t;

typedef struct __attribute__((packed)) {
    perm_policy_hdr_t       hdr;
    perm_policy_grant_ent_t grants[PERM_MAX_GRANTS];
    perm_policy_role_ent_t  roles[PERM_MAX_ROLES];
} perm_policy_blob_t;

/* Legacy layout (v1) — accepted on LOAD only. */
typedef struct __attribute__((packed)) {
    u64            subject_id;
    u32            access;
    vfs_resource_t resource;
} perm_policy_grant_v1_t;

typedef struct __attribute__((packed)) {
    u64 subject_id;
    u32 role;
} perm_policy_role_v1_t;

typedef struct __attribute__((packed)) {
    perm_policy_hdr_t      hdr;
    perm_policy_grant_v1_t grants[PERM_MAX_GRANTS];
    perm_policy_role_v1_t  roles[PERM_MAX_ROLES];
} perm_policy_blob_v1_t;

/* SAVE: include_expired == 0 (default) exports only the grants that are
 * still live AND clears the dead slots out of the table (§13.5);
 * include_expired != 0 exports the table as it stands. */
static int PolicySerialize(u8 *out, int out_len, u32 include_expired) {
    if (out_len < (int)sizeof(perm_policy_blob_t))
        return ERR_NOMEM;
    u64                 now = (u64)GetTime();
    perm_policy_blob_t *b   = (perm_policy_blob_t *)out;
    memset(b, 0, sizeof(*b));
    b->hdr.magic   = PERM_POLICY_MAGIC;
    b->hdr.version = PERM_POLICY_VERSION_V2;
    u32 gn         = 0;
    for (int i = 0; i < PERM_MAX_GRANTS; i++) {
        perm_grant_t *g = &s_grants[i];
        if (!g->in_use)
            continue;
        if (!include_expired && GrantExpired(g, now)) {
            GrantReap(g); /* SAVE garbage-collects dead grants too */
            continue;
        }
        b->grants[i].subject_id   = g->subject_id;
        b->grants[i].expiry_ticks = g->expiry_ticks;
        b->grants[i].access       = g->access;
        b->grants[i].scope_hash   = g->scope_hash;
        b->grants[i].source       = (u8)g->source;
        b->grants[i].resource     = g->resource;
        gn++;
    }
    u32 rn = 0;
    for (int i = 0; i < PERM_MAX_ROLES; i++) {
        perm_role_t *r = &s_roles[i];
        if (!r->in_use)
            continue;
        b->roles[i].subject_id = r->subject_id;
        b->roles[i].role       = (u8)r->role;
        rn++;
    }
    b->hdr.grant_count = gn;
    b->hdr.role_count  = rn;
    return (int)sizeof(perm_policy_blob_t);
}

/* The v2 snapshot must fit the frozen PERM_POLICY_MAX (and therefore one
 * 4096-byte IPC message) — pinned at compile time so a future field
 * cannot silently overflow the bound the layout comment recalculated. */
_Static_assert(sizeof(perm_policy_blob_t) <= PERM_POLICY_MAX,
               "policy snapshot exceeds PERM_POLICY_MAX");

/* Staging blob for an import: the service is single-threaded, so one
 * static copy is safe and the ~3.7 KiB snapshot stays off the thread
 * stack. */
static perm_policy_blob_t s_policy_stage;

/* Shared tail of an import: validate the WHOLE blob, then — and only
 * then — apply it.  On success: roles first, then the grants restored
 * into their original table slots (byte-identical round trip), then the
 * kernel caps are hot-reloaded (补充六: revoke the old atom, re-issue
 * the new one).  One invalid record rejects the snapshot and leaves the
 * current policy untouched (§13.5 全有或全无). */
static int PolicyApply(const perm_policy_blob_t *b) {
    if (b->hdr.magic != PERM_POLICY_MAGIC || b->hdr.version != PERM_POLICY_VERSION_V2)
        return ERR_INVAL;
    if (b->hdr.grant_count > PERM_MAX_GRANTS || b->hdr.role_count > PERM_MAX_ROLES)
        return ERR_INVAL;

    /* Validate roles before mutating. */
    for (int i = 0; i < PERM_MAX_ROLES; i++) {
        const perm_policy_role_ent_t *r = &b->roles[i];
        if (r->subject_id == 0)
            continue; /* empty slot */
        if (r->role >= PERM_ROLE_MAX)
            return ERR_INVAL;
    }
    /* Validate grants before mutating. */
    for (int i = 0; i < PERM_MAX_GRANTS; i++) {
        const perm_policy_grant_ent_t *g = &b->grants[i];
        if (g->subject_id == 0 && g->access == 0)
            continue; /* empty slot */
        if (g->access == 0)
            return ERR_INVAL;
        if (g->source > PERM_SRC_POLICY)
            return ERR_INVAL;
    }

    /* Mutate: roles. */
    memset(s_roles, 0, sizeof(s_roles));
    for (int i = 0; i < PERM_MAX_ROLES; i++) {
        const perm_policy_role_ent_t *r = &b->roles[i];
        if (r->subject_id == 0)
            continue;
        s_roles[i].in_use     = 1;
        s_roles[i].subject_id = r->subject_id;
        s_roles[i].role       = r->role;
    }

    /* Mutate: grants + hot reload the kernel caps (补充六). */
    memset(s_grants, 0, sizeof(s_grants));
    for (int i = 0; i < PERM_MAX_GRANTS; i++) {
        const perm_policy_grant_ent_t *g = &b->grants[i];
        if (g->subject_id == 0 && g->access == 0)
            continue;
        perm_grant_t *d = &s_grants[i];
        d->in_use       = 1;
        d->subject_id   = g->subject_id;
        d->resource     = g->resource;
        d->access       = g->access;
        d->expiry_ticks = g->expiry_ticks;
        d->scope_hash   = g->scope_hash;
        d->source       = g->source;

        u32 atom = AtomFromAccess(g->access);
        if (d->subject_id != 0 && atom != ATOM_NONE && atom <= ATOM_MAX) {
            (void)CapRevokeByAtom(d->subject_id, atom, 0);
            (void)DecisionEncode(d->subject_id, atom);
        }
    }
    return 0;
}

/* LOAD entry point: read the version from the frozen 16-byte header,
 * normalise v1 into the v2 staging blob, then apply.  A snapshot that
 * is too short for the layout its version promises is rejected before
 * anything is touched. */
static int PolicyDeserialize(const u8 *in, int len) {
    if (len < (int)sizeof(perm_policy_hdr_t))
        return ERR_INVAL;
    const perm_policy_hdr_t *h = (const perm_policy_hdr_t *)in;
    if (h->magic != PERM_POLICY_MAGIC)
        return ERR_INVAL;

    if (h->version == PERM_POLICY_VERSION_V1) {
        if (len < (int)sizeof(perm_policy_blob_v1_t))
            return ERR_INVAL;
        const perm_policy_blob_v1_t *v1 = (const perm_policy_blob_v1_t *)in;
        if (v1->hdr.grant_count > PERM_MAX_GRANTS || v1->hdr.role_count > PERM_MAX_ROLES)
            return ERR_INVAL;

        perm_policy_blob_t *s = &s_policy_stage;
        memset(s, 0, sizeof(*s));
        s->hdr.magic       = PERM_POLICY_MAGIC;
        s->hdr.version     = PERM_POLICY_VERSION_V2;
        s->hdr.grant_count = v1->hdr.grant_count;
        s->hdr.role_count  = v1->hdr.role_count;
        for (int i = 0; i < PERM_MAX_GRANTS; i++) {
            const perm_policy_grant_v1_t *g = &v1->grants[i];
            if (g->subject_id == 0 && g->access == 0)
                continue; /* empty slot */
            s->grants[i].subject_id   = g->subject_id;
            s->grants[i].resource     = g->resource;
            s->grants[i].access       = g->access;
            s->grants[i].expiry_ticks = 0; /* v1 had no lifetime ...      */
            s->grants[i].scope_hash   = 0; /* ... and no scope (§13.5)    */
            s->grants[i].source       = (u8)PERM_SRC_POLICY; /* policy-only */
        }
        for (int i = 0; i < PERM_MAX_ROLES; i++) {
            const perm_policy_role_v1_t *r = &v1->roles[i];
            if (r->subject_id == 0)
                continue;
            if (r->role >= PERM_ROLE_MAX)
                return ERR_INVAL; /* checked before the u8 narrowing */
            s->roles[i].subject_id = r->subject_id;
            s->roles[i].role       = (u8)r->role;
        }
        return PolicyApply(s);
    }

    if (h->version == PERM_POLICY_VERSION_V2) {
        if (len < (int)sizeof(perm_policy_blob_t))
            return ERR_INVAL;
        memcpy(&s_policy_stage, in, sizeof(s_policy_stage));
        return PolicyApply(&s_policy_stage);
    }
    return ERR_INVAL; /* unknown snapshot version */
}

/* ====================================================================
 * Atom encoding — map a VFS_ACCESS_* mask to the atom it represents
 * ==================================================================== */

static u32 AtomFromAccess(u32 access) {
    if (access & VFS_ACCESS_WRITE)
        return ATOM_DATA_DOCS_WRITE;
    if (access & VFS_ACCESS_READ)
        return ATOM_DATA_DOCS_READ;
    return ATOM_NONE; /* EXEC/COW/… → no chain rule */
}

/* ====================================================================
 * Decision encoding — the capability IS the decision (§四)
 *
 * The perm-engine is the ONLY signer of atom capabilities.  When a
 * decision lands (ANSWER allow / direct GRANT), encode it into the
 * subject's kernel table via CapGrantToSubject() so the kernel can
 * enforce it independently of the engine (授予路径异步).
 * ==================================================================== */

/* Returns 0 on success, or a negative error code when the kernel-side
 * cap issuance failed (e.g. ERR_NOENT when the target subject's process
 * has already exited).  Callers MUST propagate this to the client so a
 * failed grant is not silently reported as success. */
static int DecisionEncode(u64 subject_id, u32 atom) {
    if (subject_id == 0 || atom == ATOM_NONE || atom > ATOM_MAX)
        return 0;
    int h = CapGrantToSubject(subject_id, atom, RIGHT_ALL, 0, 0);
    if (h < 0) {
        printf("perm: encode subject=%u atom=%u failed (%d)\n", (unsigned)subject_id, atom, h);
        return h;
    }
    printf("perm: encode subject=%u atom=%u handle=%d\n", (unsigned)subject_id, atom, h);
    return 0;
}

/* ====================================================================
 * Query table helpers
 * ==================================================================== */

static perm_query_t *query_find(u32 query_id) {
    for (int i = 0; i < PERM_MAX_QUERIES; i++) {
        perm_query_t *q = &s_queries[i];
        if (q->in_use && q->query_id == query_id)
            return q;
    }
    return NULL;
}

/* First PENDING query (FIFO), or NULL. */
static perm_query_t *query_first_pending(void) {
    for (int i = 0; i < PERM_MAX_QUERIES; i++) {
        perm_query_t *q = &s_queries[i];
        if (q->in_use && q->state == PERM_QUERY_PENDING)
            return q;
    }
    return NULL;
}

/* Reuse a PENDING query for the same (subject, resource, scope), else
 * allocate a fresh one.  The subject's display name/PID is resolved from
 * the kernel ONCE on the fresh-alloc path (never on a check that reuses
 * an identical pending query).  Returns NULL when the queue is full.
 * v1.0: the scope is part of the identity so an answer can never be
 * reused for a different scope. */
static perm_query_t *query_alloc(const vfs_resource_t *res, u64 subject_id, u32 atom,
                                 u32 scope_hash) {
    for (int i = 0; i < PERM_MAX_QUERIES; i++) {
        perm_query_t *q = &s_queries[i];
        if (!q->in_use)
            continue;
        if (q->state != PERM_QUERY_PENDING)
            continue;
        if (q->subject_id != subject_id)
            continue;
        if (q->scope_hash != scope_hash)
            continue;
        if (memcmp(&q->resource, res, sizeof(*res)) != 0)
            continue;
        return q; /* identical pending query */
    }
    for (int i = 0; i < PERM_MAX_QUERIES; i++) {
        if (!s_queries[i].in_use) {
            perm_query_t *q = &s_queries[i];
            memset(q, 0, sizeof(*q));
            q->in_use     = 1;
            q->query_id   = ++s_query_seq;
            q->resource   = *res;
            q->subject_id = subject_id;
            q->atom       = atom;
            q->scope_hash = scope_hash;
            q->state      = PERM_QUERY_PENDING;

            /* Resolve display identity once via the kernel
             * SYS_PROC_INFO_BY_SUBJECT wrapper.  If the subject already
             * died (ERR_NOENT), fall back to "subject <id>" so the UI
             * label is never empty. */
            proc_ident_t ident;
            if (ProcInfoBySubject(subject_id, &ident) == 0) {
                q->pid = (u32)ident.pid;
                strncpy(q->name, ident.name, sizeof(q->name) - 1);
                q->name[sizeof(q->name) - 1] = '\0';
                if (q->name[0] == '\0')
                    strcpy(q->name, "unknown");
            } else {
                q->pid  = 0;
                int pos = 0;
                FmtAppend(q->name, sizeof(q->name), &pos, "subject ");
                FmtUint(q->name, sizeof(q->name), &pos, (unsigned)subject_id, 10);
            }
            return q;
        }
    }
    return NULL;
}

/* ====================================================================
 * Minimal label formatting (the libc printf only supports
 * %d/%u/%x/%s/%c and writes to the debug log — there is no snprintf).
 * Enough to build the human-readable UI labels and the DUMP lines.
 * ==================================================================== */

static void FmtAppend(char *dst, int dst_len, int *pos, const char *s) {
    while (*s && *pos < dst_len - 1)
        dst[(*pos)++] = *s++;
}

static void FmtUint(char *dst, int dst_len, int *pos, unsigned v, int base) {
    char tmp[16];
    int  n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    } else {
        while (v > 0 && n < (int)sizeof(tmp) - 1) {
            unsigned d = v % (unsigned)base;
            tmp[n++]   = (char)(d < 10 ? '0' + d : 'a' + d - 10);
            v /= (unsigned)base;
        }
    }
    while (n > 0 && *pos < dst_len - 1)
        dst[(*pos)++] = tmp[--n];
}

static void AccessLabel(char *out, int out_len, u32 access) {
    int n = 0;
    (void)out_len; /* max 3 chars + NUL; callers pass >= 4-byte buffers */
    if (access & VFS_ACCESS_READ)
        out[n++] = 'R';
    if (access & VFS_ACCESS_WRITE)
        out[n++] = 'W';
    if (access & VFS_ACCESS_EXEC)
        out[n++] = 'X';
    if (n == 0)
        out[n++] = '?';
    out[n] = '\0';
}

/* Label = "perm: <name> (PID n) 请求访问 <url> (R) — 输入 perm_answer
 * q y/n"; the ALLOWED/DENIED update drops the prompt tail. */
static void BuildLabel(char *dst, int dst_len, const perm_query_t *q) {
    int pos = 0;
    dst[0]  = '\0';
    if (q->state == PERM_QUERY_PENDING) {
        FmtAppend(dst, dst_len, &pos, "perm: ");
        FmtAppend(dst, dst_len, &pos, q->name);
        FmtAppend(dst, dst_len, &pos, " (PID ");
        FmtUint(dst, dst_len, &pos, (unsigned)q->pid, 10);
        FmtAppend(dst, dst_len, &pos, ") 请求访问 ");
        FmtAppend(dst, dst_len, &pos, q->url);
        FmtAppend(dst, dst_len, &pos, " (");
        char acc[8];
        AccessLabel(acc, sizeof(acc), q->access);
        FmtAppend(dst, dst_len, &pos, acc);
        FmtAppend(dst, dst_len, &pos, ") — 输入 perm_answer ");
        FmtUint(dst, dst_len, &pos, q->query_id, 10);
        FmtAppend(dst, dst_len, &pos, " y/n");
    } else {
        FmtAppend(dst, dst_len, &pos, "perm: 查询 ");
        FmtUint(dst, dst_len, &pos, q->query_id, 10);
        FmtAppend(dst, dst_len, &pos, q->state == PERM_QUERY_ALLOWED ? " 已允许" : " 已拒绝");
    }
    dst[pos] = '\0';
}

/* ====================================================================
 * UI notification — push UI_SHOW to term("perm.ui")
 *
 * Best effort: if the UI port is not up yet, drop the notification
 * silently (the query stays PENDING and the shell can list it with
 * perm_query).  The port is cached on first success; a failed port_get
 * is retried on the next notification.
 * ==================================================================== */

static void NotifyUi(const perm_query_t *q) {
    if (s_ui_port < 0) {
        s_ui_port = PortGet(PERM_UI_PORT_NAME);
        if (s_ui_port < 0)
            return;
    }

    perm_req_ui_t *req = (perm_req_ui_t *)s_req;
    memset(req, 0, sizeof(*req));
    req->op         = PERM_OP_UI_SHOW;
    req->query_id   = q->query_id;
    req->subject_id = q->subject_id;
    req->pid        = q->pid;
    strncpy(req->name, q->name, sizeof(req->name) - 1);
    req->name[sizeof(req->name) - 1] = '\0';
    strncpy(req->url, q->url, sizeof(req->url) - 1);
    req->access = q->access;
    req->state  = q->state;

    /* P1: perm 聚合完整提示文本，term 原样展示 */
    BuildLabel(req->label, sizeof(req->label), q);

    perm_resp_ui_t resp;
    int            resp_len = (int)sizeof(resp);
    (void)IpcCall(s_ui_port, req, (int)sizeof(*req), &resp, &resp_len);
}

/* ====================================================================
 * Operation handlers
 * ==================================================================== */

/* (The old GrantedForAtom() single-atom masker lived here.  It was
 * replaced by the per-bit chain evaluation inside DoCheck: deciding on
 * one "primary" atom dropped rights from multi-right requests.) */

/* CHECK: vfs_server → synchronous authorization check.
 * Decision order (§四): 1) explicit grant beat, 2) role-chain verdict
 * (override-first), 3) default deny → Powerbox.
 *
 * P2 抹位: the grant beat fires on PARTIAL intersection
 * ((g->access & req->access) != 0) and resp->granted carries ONLY the
 * covered bits — a READ grant never yields a WRITE-carrying handle.
 *
 * v1.0 (§13) adds one shortcut and three inputs to that order; every
 * branch also feeds the (subject, atom) counters and the audit ring:
 *   0) the (subject, atom) is QUARANTINED → refuse, never prompt;
 *   1) the grant must be LIVE (TTL) and scope-compatible;
 *   2) the role chain (unchanged);
 *   3) default deny → a BACKGROUND subject is refused without creating
 *      a query or pushing UI_SHOW; a foreground one gets the prompt. */
static void DoCheck(int token, int msg_len, u64 caller_subject) {
    perm_resp_check_t *resp = (perm_resp_check_t *)s_resp;
    memset(resp, 0, sizeof(*resp));
    resp->ret = ERR_INVAL;
    if (msg_len < (int)sizeof(perm_req_check_t))
        goto out;
    /* GATED (docs/ops_format.md §6): CHECK answers for the subject in
     * the REQUEST, which only the trusted vfs_server proxy may fill.
     * An ungated CHECK would let any Ring-3 process probe arbitrary
     * subjects' grants/roles (authorization oracle). */
    if (CapHasAtom(caller_subject, ATOM_SERVICE_MANAGE) != 1) {
        resp->ret = ERR_DENIED;
        goto out;
    }
    perm_req_check_t *req     = (perm_req_check_t *)s_req;
    u64               subject = req->subject_id; /* filled by trusted vfs_server */
    u32               atom    = AtomFromAccess(req->access);
    u64               now     = (u64)GetTime();
    perm_freq_ent_t  *fe      = FreqFind(subject, atom, 1);

    /* 0) Quarantine (§13.3): a subject that burned through
     * PERM_DENY_THRESHOLD denials on this atom is refused outright —
     * no Powerbox query, no UI_SHOW. */
    if (FreqQuarantined(fe, now)) {
        resp->ret   = VFS_ERR_ACCESS;
        resp->flags = PERM_DEC_QUARANTINED;
        FreqDeny(fe, now); /* the attempt is a denial too */
        AuditAppend(subject, atom, PERM_VERDICT_DENY, &req->resource, PERM_EV_CHECK_DENY);
        goto out;
    }

    /* 1) Grant beat — a LIVE (non-expired), scope-compatible grant
     * (powerbox result) wins.  A grant that covers the requested bits
     * under ANOTHER scope is not a hit: the chain decides instead
     * (§13.1, 能力化抹位 untouched). */
    int           expired = 0, mismatch = 0;
    perm_grant_t *g = grant_find(subject, &req->resource, req->scope_hash, req->access, now,
                                 &expired, &mismatch);
    if (expired)
        resp->flags |= PERM_DEC_EXPIRED;
    if (g) {
        resp->ret     = 0;                       /* granted — proceed */
        resp->granted = g->access & req->access; /* 抹位掩码 */
        resp->flags |= PERM_DEC_GRANT_BEAT;
        FreqHit(fe);
        AuditAppend(subject, atom, PERM_VERDICT_ALLOW, &req->resource, PERM_EV_CHECK_ALLOW);
        goto out;
    }
    if (mismatch)
        resp->flags |= PERM_DEC_SCOPE_MISMATCH;

    /* 2) Role chain — override-first, evaluated PER REQUESTED BIT.
     *
     * A request can carry more than one right (a stdio "r+" opens
     * READ|WRITE), and each right has its own atom.  Deciding on a
     * single "primary" atom used to lose rights: with WRITE tested
     * first, an OWNER asking for READ|WRITE received a WRITE-only
     * handle — the READ half of an authorized request silently
     * disappeared, and the handle could no longer read.
     *
     * The chain is now evaluated for every requested bit:
     *   - every bit allowed            → allow, with exactly those bits
     *   - any bit explicitly denied    → deny, and NO prompt (policy
     *                                    said no, so asking the user
     *                                    would be meaningless)
     *   - some bit allowed, some with
     *     no rule at all               → fall through to the Powerbox
     *   - nothing allowed              → fall through to the Powerbox
     * Untyped bits (EXEC/COW, atom == ATOM_NONE) are never granted by
     * the chain — this is the 抹位 rule that keeps a READ|EXEC request
     * from producing an executable handle. */
    {
        u32 role    = RoleOf(subject);
        u32 typed   = req->access & (VFS_ACCESS_READ | VFS_ACCESS_WRITE);
        u32 granted = 0;
        int denied  = 0;

        if (typed & VFS_ACCESS_READ) {
            i32 v = RuleLookup(role, ATOM_DATA_DOCS_READ);
            if (v == PERM_VERDICT_ALLOW)
                granted |= VFS_ACCESS_READ;
            else if (v == PERM_VERDICT_DENY)
                denied = 1;
        }
        if (typed & VFS_ACCESS_WRITE) {
            i32 v = RuleLookup(role, ATOM_DATA_DOCS_WRITE);
            if (v == PERM_VERDICT_ALLOW)
                granted |= VFS_ACCESS_WRITE;
            else if (v == PERM_VERDICT_DENY)
                denied = 1;
        }

        if (granted != 0 && granted == typed) {
            resp->ret     = 0;
            resp->granted = granted; /* 抹位: untyped bits are dropped */
            resp->flags |= PERM_DEC_ROLE_CHAIN;
            FreqHit(fe);
            AuditAppend(subject, atom, PERM_VERDICT_ALLOW, &req->resource, PERM_EV_CHECK_ALLOW);
            goto out;
        }
        if (denied) {
            /* Chain deny: policy says no — no Powerbox prompt. */
            resp->ret = VFS_ERR_ACCESS;
            resp->flags |= PERM_DEC_ROLE_CHAIN;
            FreqDeny(fe, now);
            AuditAppend(subject, atom, PERM_VERDICT_DENY, &req->resource, PERM_EV_CHECK_DENY);
            goto out;
        }
        /* Partial (some bit has no rule) or nothing allowed: fall
         * through to the default-deny / Powerbox path below. */
    }

    /* 3) Default deny.  A BACKGROUND subject is refused WITHOUT the
     * prompt — the point of the P3 context binding (§13.2): an app the
     * user cannot see must not be able to raise a panel.  Subjects that
     * never declared a context count as FOREGROUND. */
    resp->flags |= PERM_DEC_DEFAULT_DENY;
    if (!CtxForeground(subject)) {
        resp->ret = VFS_ERR_ACCESS;
        resp->flags |= PERM_DEC_BACKGROUND;
        FreqDeny(fe, now);
        AuditAppend(subject, atom, PERM_VERDICT_DENY, &req->resource, PERM_EV_CHECK_DENY);
        goto out;
    }

    /* Default deny → Powerbox.  Create/reuse a pending query and tell
     * the UI to show the prompt.  vfs_server translates this into
     * -EACCES for the client (design §8: 授权前 → -EACCES). */
    perm_query_t *q = query_alloc(&req->resource, subject, atom, req->scope_hash);
    if (!q) {
        resp->ret = ERR_NOMEM; /* query queue full */
        goto out;
    }
    strncpy(q->url, req->url, sizeof(q->url) - 1);
    q->url[sizeof(q->url) - 1] = '\0';
    q->access                  = req->access;

    resp->ret      = VFS_ERR_ACCESS;
    resp->query_id = q->query_id;
    FreqDeny(fe, now);
    AuditAppend(subject, atom, PERM_VERDICT_DENY, &req->resource, PERM_EV_CHECK_DENY);
    AuditAppend(subject, atom, PERM_VERDICT_DENY, &req->resource, PERM_EV_POWERBOX);

    if (!s_quiet)
        NotifyUi(q);

out:
    (void)IpcReply(token, resp, (int)sizeof(*resp));
}

/* ANSWER: user verdict → grant upsert + decision encode (allow) or
 * deny.  The Powerbox verdict lands as a kernel-enforceable atom cap
 * issued into the requesting subject's table (§四).
 * GATED (docs/ops_format.md §6, capability-based like do_grant): the
 * CALLER must hold ATOM_SERVICE_MANAGE.  The legitimate answerers are
 * kernel-endorsed services — term (UI agent, user y/n through the
 * panel) and init/perm/pkg (management plane; init keeps the atom
 * even after ROLE_SET hot-reloads it to GUEST).  Sandbox apps never
 * hold the atom, so they cannot auto-approve their own pending
 * Powerbox queries (privilege escalation). */
static void DoAnswer(int token, int msg_len, u64 caller_subject) {
    perm_resp_answer_t *resp = (perm_resp_answer_t *)s_resp;
    memset(resp, 0, sizeof(*resp));
    resp->ret = ERR_INVAL;
    if (msg_len < (int)sizeof(perm_req_answer_t))
        goto out;
    perm_req_answer_t *req = (perm_req_answer_t *)s_req;

    if (CapHasAtom(caller_subject, ATOM_SERVICE_MANAGE) != 1) {
        resp->ret = ERR_DENIED; /* apps cannot answer queries */
        goto out;
    }

    perm_query_t *q = query_find(req->query_id);
    if (!q) {
        resp->ret = ERR_NOENT;
        goto out;
    }

    if (req->allow) {
        q->state = PERM_QUERY_ALLOWED;
        /* v1.0 (§13.1): the panel may answer with a lifetime
         * (ttl_ticks, 0 = permanent) and a scope.  An answer NEVER
         * widens the scope the check asked for: without an explicit
         * scope_hash it inherits the pending query's. */
        u64 now    = (u64)GetTime();
        u64 expiry = 0;
        if (req->ttl_ticks != 0) {
            expiry = now + req->ttl_ticks;
            if (expiry < now)
                expiry = ~0ULL; /* saturate instead of wrapping */
        }
        u32 scope = req->scope_hash ? req->scope_hash : q->scope_hash;
        if (!grant_upsert(q->subject_id, &q->resource, q->access, expiry, scope,
                          PERM_SRC_POWERBOX))
            resp->ret = ERR_NOMEM;
        else {
            /* Encode the decision into the subject's kernel cap table.
             * If the target process has exited (ERR_NOENT) the grant
             * table entry still stands for a future re-spawn, but we
             * report the failure so the caller knows the kernel cap
             * was not issued. */
            int enc   = DecisionEncode(q->subject_id, q->atom);
            resp->ret = (enc < 0) ? enc : 0;
        }
        AuditAppend(q->subject_id, q->atom, PERM_VERDICT_ALLOW, &q->resource, PERM_EV_ANSWER);
    } else {
        q->state  = PERM_QUERY_DENIED;
        resp->ret = 0;
        AuditAppend(q->subject_id, q->atom, PERM_VERDICT_DENY, &q->resource, PERM_EV_ANSWER);
    }

    if (!s_quiet)
        NotifyUi(q);

out:
    (void)IpcReply(token, resp, (int)sizeof(*resp));
}

/* QUERY: UI agent fetches a pending query (decision 2).
 * GATED: the response carries the query's subject/name/url — only the
 * trusted UI agent (term) and the management plane may enumerate
 * pending authorizations. */
static void DoQuery(int token, int msg_len, u64 caller_subject) {
    perm_resp_query_t *resp = (perm_resp_query_t *)s_resp;
    if (msg_len < (int)sizeof(perm_req_query_t)) {
        resp->ret = ERR_INVAL;
        goto out;
    }
    if (CapHasAtom(caller_subject, ATOM_SERVICE_MANAGE) != 1) {
        resp->ret = ERR_DENIED; /* apps cannot enumerate queries */
        goto out;
    }
    perm_req_query_t *req = (perm_req_query_t *)s_req;

    perm_query_t *q = (req->query_id == 0) ? query_first_pending() : query_find(req->query_id);
    if (!q) {
        resp->ret = ERR_NOENT;
        goto out;
    }

    resp->ret      = 0;
    resp->query_id = q->query_id;
    resp->pid      = q->pid;
    strncpy(resp->name, q->name, sizeof(resp->name) - 1);
    resp->name[sizeof(resp->name) - 1] = '\0';
    strncpy(resp->url, q->url, sizeof(resp->url) - 1);
    resp->url[sizeof(resp->url) - 1] = '\0';
    resp->access                     = q->access;
    resp->subject_id                 = q->subject_id;
    resp->state                      = q->state;
    resp->label[0]                   = '\0';
    {
        int pos = 0;
        FmtAppend(resp->label, sizeof(resp->label), &pos, q->name);
        FmtAppend(resp->label, sizeof(resp->label), &pos, " (PID ");
        FmtUint(resp->label, sizeof(resp->label), &pos, (unsigned)q->pid, 10);
        FmtAppend(resp->label, sizeof(resp->label), &pos, ") → ");
        FmtAppend(resp->label, sizeof(resp->label), &pos, q->url);
        FmtAppend(resp->label, sizeof(resp->label), &pos, ", subject ");
        FmtUint(resp->label, sizeof(resp->label), &pos, (unsigned)q->subject_id, 10);
        FmtAppend(resp->label,
                   sizeof(resp->label),
                   &pos,
                   q->state == PERM_QUERY_PENDING ? " (待决定)" : " (已决定)");
    }

out:
    (void)IpcReply(token, resp, (int)sizeof(*resp));
}

/* REVOKE: drop grants (default deny).
 * GATED (docs/ops_format.md §6): self-revoke is always allowed (a
 * process dropping its OWN grants is harmless); revoking ANOTHER
 * subject's grants requires ATOM_SERVICE_MANAGE.  Without the
 * cross-subject gate any Ring-3 process could revoke other subjects'
 * grants (DoS / privilege-removal). */
static void DoRevoke(int token, int msg_len, u64 caller_subject) {
    perm_resp_revoke_t *resp = (perm_resp_revoke_t *)s_resp;
    if (msg_len < (int)sizeof(perm_req_revoke_t)) {
        resp->ret = ERR_INVAL;
        goto out;
    }
    perm_req_revoke_t *req = (perm_req_revoke_t *)s_req;

    if (req->subject_id != caller_subject &&
        CapHasAtom(caller_subject, ATOM_SERVICE_MANAGE) != 1) {
        resp->ret = ERR_DENIED;
        goto out;
    }

    resp->revoked = GrantRevoke(req->subject_id, &req->resource);
    resp->ret     = 0;

out:
    (void)IpcReply(token, resp, (int)sizeof(*resp));
}

/* GRANT: direct grant, bypasses the Powerbox (tests/management).
 * P1: carries the target subject + atom; the decision is encoded into
 * the subject's kernel table via CapGrantToSubject().
 * GATED (docs/ops_format.md §6): the CALLER must hold the
 * ATOM_SERVICE_MANAGE atom — grants beat role defaults (§四), so a
 * management-CAPABLE caller may grant even when its ROLE is not
 * management (e.g. init hot-reloaded to GUEST).  Apps can never hold
 * the management atom, so they cannot grant. */
static void DoGrant(int token, int msg_len, u64 caller_subject) {
    perm_resp_grant_t *resp = (perm_resp_grant_t *)s_resp;
    memset(resp, 0, sizeof(*resp));
    resp->ret = ERR_INVAL;
    if (msg_len < (int)sizeof(perm_req_grant_t))
        goto out;
    perm_req_grant_t *req = (perm_req_grant_t *)s_req;

    if (CapHasAtom(caller_subject, ATOM_SERVICE_MANAGE) != 1) {
        resp->ret = ERR_DENIED; /* apps cannot grant */
        goto out;
    }

    /* v1.0: the caller names the lifetime (expiry_ticks, absolute tick,
     * 0 = permanent), the scope and the provenance.  An out-of-range
     * source is normalised to DIRECT — the only way a request reaches
     * this handler is the management plane (§13.1). */
    u32 source = req->source;
    if (source > PERM_SRC_POLICY)
        source = PERM_SRC_DIRECT;

    perm_grant_t *g = grant_upsert(req->subject_id, &req->resource, req->access,
                                   req->expiry_ticks, req->scope_hash, source);
    resp->ret       = g ? 0 : ERR_NOMEM;
    if (g) {
        /* Echo what actually took effect (upsert refreshes the tuple). */
        resp->expiry_ticks = g->expiry_ticks;
        resp->scope_hash   = g->scope_hash;

        /* Encode the decision into the target subject's kernel cap table.
         * A failure here (e.g. ERR_NOENT — target process already exited)
         * must be propagated: otherwise the client believes the grant
         * succeeded but no kernel-enforceable cap was issued. */
        int enc = DecisionEncode(req->subject_id, req->atom);
        if (enc < 0)
            resp->ret = enc;
        AuditAppend(req->subject_id, req->atom, PERM_VERDICT_ALLOW, &req->resource,
                    PERM_EV_GRANT);
    }

out:
    (void)IpcReply(token, resp, (int)sizeof(*resp));
}

/* ROLE_SET: management-plane hot reload (§二.2).  The caller's subject
 * comes from IpcRecvFrom(the kernel — unforgeable); callers may
 * change roles when they are OWNER/ADMIN by role (the classic
 * management plane: init is seeded OWNER), or when they are the user
 * account service itself (SERVICE_MANAGE atom + kernel-issued process
 * name "user" — init, which also holds the atom, stays gated by role
 * so a demoted init cannot self-repromote, P1 test 8).  Applied
 * immediately; grants are NOT rewritten (grants beat role defaults —
 * §四). */
static int CallerIsUserService(u64 subject) {
    if (CapHasAtom(subject, ATOM_SERVICE_MANAGE) != 1)
        return 0;
    proc_ident_t ident;
    if (ProcInfoBySubject(subject, &ident) != 0)
        return 0;
    return strcmp(ident.name, "user") == 0;
}

static void DoRoleSet(int token, int msg_len, u64 caller_subject) {
    perm_resp_role_set_t *resp = (perm_resp_role_set_t *)s_resp;
    if (msg_len < (int)sizeof(perm_req_role_set_t)) {
        resp->ret = ERR_INVAL;
        goto out;
    }
    perm_req_role_set_t *req = (perm_req_role_set_t *)s_req;

    if (!RoleIsManagement(caller_subject) && !CallerIsUserService(caller_subject)) {
        resp->ret = ERR_DENIED; /* apps cannot change policy */
        goto out;
    }
    resp->ret = RoleSet(req->subject_id, req->role);
    if (resp->ret == 0) {
        resp->role = RoleOf(req->subject_id);
        AuditAppend(req->subject_id, ATOM_NONE, PERM_VERDICT_ALLOW, ResNone(), PERM_EV_ROLE_SET);
    }

out:
    (void)IpcReply(token, resp, (int)sizeof(*resp));
}

/* DUMP: export policy state (roles + rules + grants) for tests and
 * management.  GATED (docs/ops_format.md §6, capability-based): the
 * snapshot reveals the whole policy — management plane only. */
static void DoDump(int token, int msg_len, u64 caller_subject) {
    (void)msg_len; /* fixed-size response; length already validated by caller */
    perm_resp_dump_t *resp = (perm_resp_dump_t *)s_resp;
    memset(resp, 0, sizeof(*resp));
    if (CapHasAtom(caller_subject, ATOM_SERVICE_MANAGE) != 1) {
        resp->ret = ERR_DENIED; /* apps cannot export policy */
        goto out;
    }

    int n            = 0;
    resp->role_count = 0;
    for (int i = 0; i < PERM_MAX_ROLES && n < 8; i++) {
        perm_role_t *r = &s_roles[i];
        if (!r->in_use)
            continue;
        resp->role_count++;
        resp->lines[n][0] = '\0';
        {
            int pos = 0;
            FmtAppend(resp->lines[n], PERM_DUMP_LINE_MAX, &pos, "role: subject=");
            FmtUint(resp->lines[n], PERM_DUMP_LINE_MAX, &pos, (unsigned)r->subject_id, 10);
            FmtAppend(resp->lines[n], PERM_DUMP_LINE_MAX, &pos, " role=");
            FmtUint(resp->lines[n], PERM_DUMP_LINE_MAX, &pos, r->role, 10);
        }
        n++;
    }
    resp->rule_count  = s_rule_count;
    resp->grant_count = 0;
    for (int i = 0; i < PERM_MAX_GRANTS; i++)
        if (s_grants[i].in_use)
            resp->grant_count++;

    if (n < 8) {
        for (u32 role = 0; role < PERM_ROLE_MAX && n < 8; role++) {
            for (u32 atom = 0; atom <= ATOM_MAX && n < 8; atom++) {
                i32 v = RuleLookup(role, atom);
                if (v < 0)
                    continue;
                resp->lines[n][0] = '\0';
                {
                    int pos = 0;
                    FmtAppend(resp->lines[n], PERM_DUMP_LINE_MAX, &pos, "rule: role=");
                    FmtUint(resp->lines[n], PERM_DUMP_LINE_MAX, &pos, role, 10);
                    FmtAppend(resp->lines[n], PERM_DUMP_LINE_MAX, &pos, " atom=");
                    FmtUint(resp->lines[n], PERM_DUMP_LINE_MAX, &pos, atom, 10);
                    FmtAppend(resp->lines[n], PERM_DUMP_LINE_MAX, &pos, " ");
                    FmtAppend(resp->lines[n],
                               PERM_DUMP_LINE_MAX,
                               &pos,
                               v == PERM_VERDICT_ALLOW ? "ALLOW" : "DENY");
                }
                n++;
            }
        }
    }
    resp->ret = 0;

out:
    (void)IpcReply(token, resp, (int)sizeof(*resp));
}

/* ====================================================================
 * P3/P4 handlers — v1.0: context, frequency/quarantine, policy, audit
 *
 * 管理面 gate 用**能力制**（ATOM_SERVICE_MANAGE，docs/ops_format.md §6）
 * 而非角色制：init 即使被热切换为 GUEST 仍持管理原子，因此回归
 * （P2V test 3 以 GUEST 调用 context/freq/policy/audit）保持绿；
 * 沙盒应用永不可持该原子。
 * ==================================================================== */

/* CONTEXT: 前台/后台切换通知 —— v1.0 起这条状态真正参与决策（§13.2）。
 * 写模式要求管理面；list=1 是同一入口的只读查询（填 entries[]）。 */
static void DoContext(int token, int msg_len, u64 caller_subject) {
    perm_resp_context_t *resp = (perm_resp_context_t *)s_resp;
    memset(resp, 0, sizeof(*resp));
    resp->ret = ERR_INVAL;
    if (msg_len < (int)sizeof(perm_req_context_t))
        goto out;
    if (CapHasAtom(caller_subject, ATOM_SERVICE_MANAGE) != 1) {
        resp->ret = ERR_DENIED; /* apps cannot manipulate context */
        goto out;
    }
    perm_req_context_t *req = (perm_req_context_t *)s_req;
    if (req->list) {
        CtxFill(resp, (u64)GetTime());
        resp->ret = 0;
        goto out;
    }
    resp->ret = CtxUpsert(req->subject_id, req->foreground);
    if (resp->ret == 0)
        AuditAppend(req->subject_id, ATOM_NONE, PERM_VERDICT_ALLOW, ResNone(), PERM_EV_CONTEXT);
out:
    (void)IpcReply(token, resp, (int)sizeof(*resp));
}

/* v1.0: PERM_OP_CTX_QUERY — the dedicated read-only view of the same
 * table.  No management gate: it mutates nothing and reports exactly
 * what the focus owner (term/gui) already knows; the WRITE path above
 * stays management-plane only. */
static void DoCtxQuery(int token, int msg_len, u64 caller_subject) {
    (void)caller_subject;
    perm_resp_context_t *resp = (perm_resp_context_t *)s_resp;
    memset(resp, 0, sizeof(*resp));
    resp->ret = ERR_INVAL;
    if (msg_len < (int)sizeof(perm_req_ctx_query_t))
        goto out;
    CtxFill(resp, (u64)GetTime());
    resp->ret = 0;
out:
    (void)IpcReply(token, resp, (int)sizeof(*resp));
}

/* FREQ: 查询/清零授权命中频率计数器 + v1.0 的拒绝次数/隔离状态。
 * clear_quarantine=1 手动解除隔离（subject_id != 0 只解该主体，
 * subject_id == 0 解全部）；解除在统计之前完成，因此响应里的
 * quarantined/quarantine_ticks 是调用者下一步真正会看到的状态。
 * 计数查询/reset 的既有语义不变（reset 清零 hits 但保留槽位）。
 * GATED: frequency counters are per-subject telemetry — management
 * plane only. */
static void DoFreq(int token, int msg_len, u64 caller_subject) {
    perm_resp_freq_t *resp = (perm_resp_freq_t *)s_resp;
    memset(resp, 0, sizeof(*resp));
    resp->ret = ERR_INVAL;
    if (msg_len < (int)sizeof(perm_req_freq_t))
        goto out;
    if (CapHasAtom(caller_subject, ATOM_SERVICE_MANAGE) != 1) {
        resp->ret = ERR_DENIED; /* apps cannot read frequency telemetry */
        goto out;
    }
    perm_req_freq_t *req = (perm_req_freq_t *)s_req;
    u64              now = (u64)GetTime();

    if (req->clear_quarantine) {
        for (int i = 0; i < PERM_FREQ_SLOTS; i++) {
            perm_freq_ent_t *f = &s_freq[i];
            if (!f->in_use || f->quarantine_until == 0)
                continue;
            if (req->subject_id != 0 && f->subject_id != req->subject_id)
                continue;
            printf("perm: quarantine cleared subject=%u atom=%u\n",
                   (unsigned)f->subject_id,
                   f->atom);
            FreqRelease(f, now);
        }
    }

    u32 total = 0, slots = 0, denies = 0, quarantined = 0;
    u64 qleft = 0;
    for (int i = 0; i < PERM_FREQ_SLOTS; i++) {
        perm_freq_ent_t *f = &s_freq[i];
        if (!f->in_use)
            continue;
        if (req->subject_id != 0 && f->subject_id != req->subject_id)
            continue;
        if (req->atom != 0 && f->atom != req->atom)
            continue;
        total += f->hits;
        denies += f->denies;
        slots++;
        if (FreqQuarantined(f, now)) {
            u64 left = f->quarantine_until - now;
            if (left > qleft)
                qleft = left;
            quarantined = 1;
        }
        if (req->reset)
            f->hits = 0; /* keep the slot */
    }
    resp->count            = total;
    resp->slots            = slots;
    resp->denies           = denies;
    resp->quarantined      = quarantined;
    resp->quarantine_ticks = (u32)qleft;
    resp->ret              = 0;
out:
    (void)IpcReply(token, resp, (int)sizeof(*resp));
}

/* POLICY_SAVE: 导出策略二进制快照（v2）。
 * 只要求 op 字段即可（size/data 是 LOAD 方向用的）；include_expired=1
 * 时才连过期授权一起导出，缺省 0 只导出仍然有效的授权并顺手回收死槽。
 * GATED (docs/ops_format.md §6): the snapshot reveals the whole policy
 * (roles, rules, grants) — management plane only. */
static void DoPolicySave(int token, int msg_len, u64 caller_subject) {
    perm_resp_policy_t *resp = (perm_resp_policy_t *)s_resp;
    memset(resp, 0, sizeof(*resp));
    resp->ret = ERR_INVAL;
    if (msg_len < (int)sizeof(u32))
        goto out;
    if (CapHasAtom(caller_subject, ATOM_SERVICE_MANAGE) != 1) {
        resp->ret = ERR_DENIED; /* apps cannot export policy */
        goto out;
    }
    /* include_expired lives at the end of a large request struct; a
     * short (op-only) request means "default export" (0). */
    u32 include_expired = 0;
    if (msg_len >= (int)sizeof(perm_req_policy_t))
        include_expired = ((perm_req_policy_t *)s_req)->include_expired;
    int n = PolicySerialize(resp->data, (int)sizeof(resp->data), include_expired);
    if (n < 0) {
        resp->ret = n;
        goto out;
    }
    resp->size = (u32)n;
    resp->ret  = 0;
    AuditAppend(caller_subject, ATOM_NONE, PERM_VERDICT_ALLOW, ResNone(), PERM_EV_POLICY_SAVE);
out:
    (void)IpcReply(token, resp, (int)sizeof(*resp));
}

/* POLICY_LOAD: 导入策略二进制快照（全有或全无 + 热更新 + 审计）。
 * v2 快照原样恢复来源；v1 快照按旧布局解析、来源标记为 POLICY。
 * GATED (docs/ops_format.md §6, CRITICAL): the import REPLACES the
 * engine's whole state (roles/rules/grants).  An ungated load would
 * let any Ring-3 process inject a policy that promotes itself to
 * OWNER with full-allow rules — complete permission takeover. */
static void DoPolicyLoad(int token, int msg_len, u64 caller_subject) {
    perm_resp_policy_t *resp = (perm_resp_policy_t *)s_resp;
    memset(resp, 0, sizeof(*resp));
    resp->ret = ERR_INVAL;
    if (msg_len < (int)sizeof(perm_req_policy_t))
        goto out;
    if (CapHasAtom(caller_subject, ATOM_SERVICE_MANAGE) != 1) {
        resp->ret = ERR_DENIED; /* apps cannot import policy */
        goto out;
    }
    perm_req_policy_t *req = (perm_req_policy_t *)s_req;
    if (req->size == 0 || req->size > (u32)sizeof(req->data)) {
        resp->ret = ERR_INVAL;
        goto out;
    }
    resp->ret = PolicyDeserialize(req->data, (int)req->size);
    if (resp->ret == 0)
        AuditAppend(caller_subject, ATOM_NONE, PERM_VERDICT_ALLOW, ResNone(),
                    PERM_EV_POLICY_LOAD);
out:
    (void)IpcReply(token, resp, (int)sizeof(*resp));
}

/* SET_QUIET: management-only switch that suppresses UI_SHOW pushes
 * (init's P1 permission tests).  GATED like do_grant/do_role_set:
 * the caller must hold ATOM_SERVICE_MANAGE. */
static void DoSetQuiet(int token, int msg_len, u64 caller_subject) {
    perm_resp_set_quiet_t *resp = (perm_resp_set_quiet_t *)s_resp;
    memset(resp, 0, sizeof(*resp));
    resp->ret = ERR_INVAL;
    if (msg_len < (int)sizeof(perm_req_set_quiet_t))
        goto out;
    perm_req_set_quiet_t *req = (perm_req_set_quiet_t *)s_req;

    if (CapHasAtom(caller_subject, ATOM_SERVICE_MANAGE) != 1) {
        resp->ret = ERR_DENIED; /* management plane only */
        goto out;
    }

    s_quiet   = (req->quiet != 0);
    resp->ret = 0;
    printf("perm: UI_SHOW %s\n", s_quiet ? "suppressed (quiet)" : "enabled");
out:
    (void)IpcReply(token, resp, (int)sizeof(*resp));
}

/* AUDIT: 导出审计环形缓冲区（最旧在前）。
 * v1.0: subject_id / verdict_filter(0 全部,1 granted,2 denied) /
 * since_tick / max_entries(0 = PERM_AUDIT_MAX) 过滤后再按时间序返回，
 * 过滤在拷贝时完成，因此 count 永远 ≤ PERM_AUDIT_MAX。
 * GATED: the audit log records who accessed what — management plane
 * only (an app must not learn other subjects' access history). */
static void DoAudit(int token, int msg_len, u64 caller_subject) {
    perm_resp_audit_t *resp = (perm_resp_audit_t *)s_resp;
    memset(resp, 0, sizeof(*resp));
    resp->ret = ERR_INVAL;
    if (msg_len < (int)sizeof(perm_req_audit_t))
        goto out;
    if (CapHasAtom(caller_subject, ATOM_SERVICE_MANAGE) != 1) {
        resp->ret = ERR_DENIED; /* apps cannot read the audit log */
        goto out;
    }
    perm_req_audit_t *req = (perm_req_audit_t *)s_req;

    u32 max = req->max_entries;
    if (max == 0 || max > PERM_AUDIT_MAX)
        max = PERM_AUDIT_MAX;

    u32 n     = s_audit_count;
    u32 start = (s_audit_head + PERM_AUDIT_MAX - n) % PERM_AUDIT_MAX;
    u32 out   = 0;
    for (u32 i = 0; i < n && out < max; i++) {
        const perm_audit_ent_t *e = &s_audit[(start + i) % PERM_AUDIT_MAX];
        if (req->subject_id != 0 && e->subject_id != req->subject_id)
            continue;
        if (req->since_tick != 0 && e->tick < req->since_tick)
            continue;
        if (req->verdict_filter == 1 && e->verdict != PERM_VERDICT_ALLOW)
            continue;
        if (req->verdict_filter == 2 && e->verdict != PERM_VERDICT_DENY)
            continue;
        resp->entries[out++] = *e;
    }
    resp->count = out;
    resp->ret   = 0;
out:
    (void)IpcReply(token, resp, (int)sizeof(*resp));
}

static void PermHandleRequest(int token, u32 op, int msg_len, u64 caller_subject) {
    switch (op) {
    case PERM_OP_CHECK:
        DoCheck(token, msg_len, caller_subject);
        break;
    case PERM_OP_ANSWER:
        DoAnswer(token, msg_len, caller_subject);
        break;
    case PERM_OP_QUERY:
        DoQuery(token, msg_len, caller_subject);
        break;
    case PERM_OP_REVOKE:
        DoRevoke(token, msg_len, caller_subject);
        break;
    case PERM_OP_GRANT:
        DoGrant(token, msg_len, caller_subject);
        break;
    case PERM_OP_ROLE_SET:
        DoRoleSet(token, msg_len, caller_subject);
        break;
    case PERM_OP_DUMP:
        DoDump(token, msg_len, caller_subject);
        break;
    case PERM_OP_CONTEXT:
        DoContext(token, msg_len, caller_subject);
        break;
    case PERM_OP_FREQ:
        DoFreq(token, msg_len, caller_subject);
        break;
    case PERM_OP_POLICY_SAVE:
        DoPolicySave(token, msg_len, caller_subject);
        break;
    case PERM_OP_POLICY_LOAD:
        DoPolicyLoad(token, msg_len, caller_subject);
        break;
    case PERM_OP_AUDIT:
        DoAudit(token, msg_len, caller_subject);
        break;
    case PERM_OP_SET_QUIET:
        DoSetQuiet(token, msg_len, caller_subject);
        break;
    case PERM_OP_CTX_QUERY:
        DoCtxQuery(token, msg_len, caller_subject);
        break;
    default: {
        i32 *resp = (i32 *)s_resp;
        *resp     = ERR_INVAL;
        (void)IpcReply(token, resp, (int)sizeof(i32));
        break;
    }
    }
}

/* ====================================================================
 * Startup — bootstrap roles and rule chains
 * ==================================================================== */

/* Seed the rule chains with the P1 role policy (§二.2):
 *   ADMIN    — system management: docs fully allowed; management atoms
 *              fall through to the Powerbox.
 *   STANDARD — default: read allowed; write/exec has no chain rule and
 *              falls through to the Powerbox (default-deny + user consent,
 *              per §九 acceptance: bm_create/vfs_write -> -105 + prompt ->
 *              perm_answer -> grant -> retry succeeds).
 *   CHILD    — restricted: read allowed, write/exec denied.
 *   GUEST    — minimal: everything denied.
 *   AUDITOR  — read-only + audit: read allowed, write denied. */
static void SeedRules(void) {
    static const struct {
        u32 role;
        u32 atom;
        i32 verdict;
    } seeds[] = {
        /* OWNER: complete control */
        {PERM_ROLE_OWNER, ATOM_DATA_DOCS_READ, PERM_VERDICT_ALLOW},
        {PERM_ROLE_OWNER, ATOM_DATA_DOCS_WRITE, PERM_VERDICT_ALLOW},
        {PERM_ROLE_OWNER, ATOM_DATA_DL_WRITE, PERM_VERDICT_ALLOW},
        {PERM_ROLE_OWNER, ATOM_NET_CONNECT, PERM_VERDICT_ALLOW},
        {PERM_ROLE_OWNER, ATOM_SERVICE_MANAGE, PERM_VERDICT_ALLOW},
        {PERM_ROLE_OWNER, ATOM_PKG_INSTALL, PERM_VERDICT_ALLOW},
        {PERM_ROLE_OWNER, ATOM_SYS_DEBUG, PERM_VERDICT_ALLOW},
        {PERM_ROLE_OWNER, ATOM_CAP_GRANT_SELF, PERM_VERDICT_ALLOW},
        /* ADMIN: system management */
        {PERM_ROLE_ADMIN, ATOM_DATA_DOCS_READ, PERM_VERDICT_ALLOW},
        {PERM_ROLE_ADMIN, ATOM_DATA_DOCS_WRITE, PERM_VERDICT_ALLOW},
        {PERM_ROLE_ADMIN, ATOM_DATA_DL_WRITE, PERM_VERDICT_ALLOW},
        {PERM_ROLE_ADMIN, ATOM_NET_CONNECT, PERM_VERDICT_ALLOW},
        {PERM_ROLE_ADMIN, ATOM_SERVICE_MANAGE, PERM_VERDICT_ALLOW},
        {PERM_ROLE_ADMIN, ATOM_PKG_INSTALL, PERM_VERDICT_ALLOW},
        /* STANDARD: default role — docs read allowed; docs write has NO
         * chain rule so it falls through to default-deny -> Powerbox
         * (user consent via perm_answer, then grant beats the default). */
        {PERM_ROLE_STANDARD, ATOM_DATA_DOCS_READ, PERM_VERDICT_ALLOW},
        /* CHILD: restricted */
        {PERM_ROLE_CHILD, ATOM_DATA_DOCS_READ, PERM_VERDICT_ALLOW},
        {PERM_ROLE_CHILD, ATOM_DATA_DOCS_WRITE, PERM_VERDICT_DENY},
        /* GUEST: minimal */
        {PERM_ROLE_GUEST, ATOM_DATA_DOCS_READ, PERM_VERDICT_DENY},
        {PERM_ROLE_GUEST, ATOM_DATA_DOCS_WRITE, PERM_VERDICT_DENY},
        {PERM_ROLE_GUEST, ATOM_NET_CONNECT, PERM_VERDICT_DENY},
        /* AUDITOR: read-only + audit */
        {PERM_ROLE_AUDITOR, ATOM_DATA_DOCS_READ, PERM_VERDICT_ALLOW},
        {PERM_ROLE_AUDITOR, ATOM_DATA_DOCS_WRITE, PERM_VERDICT_DENY},
        {PERM_ROLE_AUDITOR, ATOM_DATA_SYS_LOGS_READ, PERM_VERDICT_ALLOW},
    };
    for (u32 i = 0; i < sizeof(seeds) / sizeof(seeds[0]); i++)
        RuleSeed(seeds[i].role, seeds[i].atom, seeds[i].verdict);
}

/* ====================================================================
 * Entry point
 * ==================================================================== */

int main(void) {
    printf("perm: starting permission manager\n");

    memset(s_grants, 0, sizeof(s_grants));
    memset(s_roles, 0, sizeof(s_roles));
    memset(s_rules, 0, sizeof(s_rules));
    memset(s_queries, 0, sizeof(s_queries));
    memset(s_freq, 0, sizeof(s_freq));
    memset(s_ctx, 0, sizeof(s_ctx));
    memset(s_audit, 0, sizeof(s_audit));
    s_audit_head  = 0;
    s_audit_count = 0;
    for (u32 role = 0; role < PERM_ROLE_MAX; role++)
        for (u32 atom = 0; atom <= ATOM_MAX; atom++)
            s_rule_head[role][atom] = PERM_CHAIN_NONE;
    s_query_seq = (u32)GetTime() & 0x7FFFFFFFu;

    /* Bootstrap roles (§二.2): the policy engine itself is the top
     * authority (OWNER); init (kernel subject 1, the device owner) is
     * seeded OWNER too.  Both come from unforgeable kernel sources:
     * GetSubject() and the documented kernel subject numbering. */
    RoleSet(GetSubject(), PERM_ROLE_OWNER);
    RoleSet(PERM_BOOTSTRAP_SUBJECT, PERM_BOOTSTRAP_ROLE);
    SeedRules();

    int port = IpcPortCreate();
    if (port < 0) {
        printf("perm: ipc_port_create failed (%d)\n", port);
        ThreadExit(1);
    }
    int ret = PortRegister(PERM_PORT_NAME, port);
    if (ret < 0) {
        printf("perm: PortRegister('%s') failed (%d)\n", PERM_PORT_NAME, ret);
        ThreadExit(1);
    }
    printf("perm: port %d registered as '%s'\n", port, PERM_PORT_NAME);

    printf("perm: serving (grants %d, roles %d, rules %d, queries %d)\n",
           PERM_MAX_GRANTS,
           PERM_MAX_ROLES,
           PERM_MAX_RULES,
           PERM_MAX_QUERIES);

    for (;;) {
        int msg_len        = (int)sizeof(s_req);
        int token          = 0;
        u64 caller_subject = 0;
        ret                = IpcRecvFrom(port, s_req, &msg_len, &token, &caller_subject);
        if (ret < 0) {
            printf("perm: ipc_recv failed (%d)\n", ret);
            ThreadExit(1);
        }
        if (msg_len < (int)sizeof(u32)) { /* no op code */
            i32 *resp = (i32 *)s_resp;
            *resp     = ERR_INVAL;
            (void)IpcReply(token, resp, (int)sizeof(i32));
            continue;
        }
        u32 op = *(u32 *)s_req;
        PermHandleRequest(token, op, msg_len, caller_subject);
    }
}
