/*
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
 *     cap_grant_to_subject() — the capability IS the encoded decision.
 *   - Requests carry the caller's kernel subject (ipc_recv_from for
 *     management ops; CHECK requests are filled by the trusted
 *     vfs_server from ITS recv — never app-supplied, unforgeable).
 *   - ROLE_SET is management-plane only (OWNER/ADMIN caller).  The
 *     bootstrap: subject 1 (init, the device owner) is seeded OWNER at
 *     startup; vfs_server's subject (learned via the WHOAMI handshake)
 *     is seeded ADMIN.
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
 */

#include <stdint.h>
#include "../lib/libc/stdio.h"
#include "../lib/libc/string.h"
#include "../lib/libos/syscalls.h"
#include "perm.h"

/* ====================================================================
 * Constants
 * ==================================================================== */

#define PERM_MAX_GRANTS   64      /* grant table size */
#define PERM_MAX_QUERIES  16      /* query queue depth */
#define PERM_MAX_ROLES    64      /* role table size */
#define PERM_MAX_RULES    96      /* rule table size */
#define PERM_MAX_URL      VFS_PATH_MAX

/* Rule-chain head index: [role][atom] → first rule, 0xFFFFFFFF = empty */
#define PERM_CHAIN_NONE   0xFFFFFFFFu

/* Bootstrap: subject 1 = init = the device owner (kernel assigns
 * subjects sequentially; init is the first user subject — see
 * kernel/process/process.c). */
#define PERM_BOOTSTRAP_SUBJECT  1u
#define PERM_BOOTSTRAP_ROLE     PERM_ROLE_OWNER

/* Request/response buffers (all perm messages < 4096) */
static u8 s_req[VFS_IPC_MAX];
static u8 s_resp[VFS_IPC_MAX];

/* ====================================================================
 * Grant table — (subject_id, resource) → access mask
 * ==================================================================== */

typedef struct {
        int            in_use;
        u64            subject_id;   /* 授权主体（0 = 任意发起者）— 授权表
                                                                    * 的唯一身份键（不可伪造，内核填充） */
        vfs_resource_t resource;
        u32            access;
} perm_grant_t;

static perm_grant_t s_grants[PERM_MAX_GRANTS];

/* ====================================================================
 * Role table — subject_id → role (single source of truth §二.2)
 * ==================================================================== */

typedef struct {
        int  in_use;
        u64  subject_id;
        u32  role;
} perm_role_t;

static perm_role_t s_roles[PERM_MAX_ROLES];

/* ====================================================================
 * Rule table — (role, atom) chains, override-first (§四)
 * ==================================================================== */

typedef struct {
        int  in_use;
        u32  role;
        u32  atom;
        i32  verdict;          /* PERM_VERDICT_ALLOW / PERM_VERDICT_DENY */
        u32  next;             /* next rule index in the chain / CHAIN_NONE */
} perm_rule_t;

static perm_rule_t s_rules[PERM_MAX_RULES];
static u32 s_rule_head[PERM_ROLE_MAX][ATOM_MAX + 1];
static u32 s_rule_count;

/* ====================================================================
 * Query table — transient Powerbox consent requests
 * ==================================================================== */

typedef struct {
        int            in_use;
        u32            query_id;
        vfs_resource_t resource;
        u32            access;
        u32            atom;          /* P1: atom this decision encodes */
        u64            subject_id;    /* P1: requesting subject (unforgeable) */
        u32            pid;           /* requesting process PID (display meta) */
        char           name[64];      /* requesting process name (NUL-terminated) */
        char           url[PERM_MAX_URL];
        i32            state;         /* PERM_QUERY_* */
} perm_query_t;

static perm_query_t s_queries[PERM_MAX_QUERIES];
static u32 s_query_seq;           /* monotonic query-id counter */

/* Lazily-resolved "perm.ui" port (term registers it at startup). */
static int s_ui_port = -1;

/* ====================================================================
 * Grant table helpers
 * ==================================================================== */

static perm_grant_t *grant_find(u64 subject_id, const vfs_resource_t *res)
{
        for (int i = 0; i < PERM_MAX_GRANTS; i++) {
                perm_grant_t *g = &s_grants[i];
                if (!g->in_use)
                        continue;
                if (g->subject_id != 0 && g->subject_id != subject_id)
                        continue;               /* 0 = any subject（通配授权） */
                if (memcmp(&g->resource, res, sizeof(*res)) != 0)
                        continue;
                return g;
        }
        return NULL;
}

/* Upsert: extend the access mask of an existing grant, else allocate.
 * Keyed by (subject_id, resource); subject_id 0 = any subject. */
static perm_grant_t *grant_upsert(u64 subject_id,
                                                                    const vfs_resource_t *res, u32 access)
{
        perm_grant_t *g = grant_find(subject_id, res);
        if (g) {
                g->access |= access;
                if (g->subject_id == 0)
                        g->subject_id = subject_id;   /* narrow a wildcard grant */
                return g;
        }
        for (int i = 0; i < PERM_MAX_GRANTS; i++) {
                if (!s_grants[i].in_use) {
                        g = &s_grants[i];
                        g->in_use = 1;
                        g->subject_id = subject_id;
                        g->resource = *res;
                        g->access = access;
                        return g;
                }
        }
        return NULL;                    /* table full */
}

/* Count and drop grants matching subject (0 = all) and resource
 * (zero uuid = all).  Returns the number revoked. */
static u32 grant_revoke(u64 subject_id, const vfs_resource_t *res)
{
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
                g->in_use = 0;
                revoked++;
        }
        return revoked;
}

/* ====================================================================
 * Role table helpers (§二.2)
 * ==================================================================== */

static u32 role_of(u64 subject_id)
{
        for (int i = 0; i < PERM_MAX_ROLES; i++) {
                perm_role_t *r = &s_roles[i];
                if (r->in_use && r->subject_id == subject_id)
                        return r->role;
        }
        return PERM_ROLE_DEFAULT;
}

/* Set (upsert) a subject's role — the ROLE_SET hot-reload write path. */
static int role_set(u64 subject_id, u32 role)
{
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
                        s_roles[i].in_use = 1;
                        s_roles[i].subject_id = subject_id;
                        s_roles[i].role = role;
                        return OK;
                }
        }
        return ERR_NOMEM;               /* role table full */
}

/* Management plane: OWNER or ADMIN may change policy (ROLE_SET). */
static int role_is_management(u64 subject_id)
{
        u32 r = role_of(subject_id);
        return (r == PERM_ROLE_OWNER || r == PERM_ROLE_ADMIN) ? 1 : 0;
}

/* ====================================================================
 * Rule chain helpers (§四 — override-first)
 * ==================================================================== */

/* Seed a rule for (role, atom): push onto the chain head so the NEWEST
 * rule wins (override-first). */
static void rule_seed(u32 role, u32 atom, i32 verdict)
{
        if (role >= PERM_ROLE_MAX || atom > ATOM_MAX)
                return;
        if (s_rule_count >= PERM_MAX_RULES)
                return;
        perm_rule_t *r = &s_rules[s_rule_count];
        r->in_use = 1;
        r->role = role;
        r->atom = atom;
        r->verdict = verdict;
        r->next = s_rule_head[role][atom];
        s_rule_head[role][atom] = s_rule_count;
        s_rule_count++;
}

/* Lookup: first matching rule in the (role, atom) chain.  Returns the
 * verdict, or -1 when the chain has no rule for (role, atom). */
static i32 rule_lookup(u32 role, u32 atom)
{
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
        return -1;                      /* no rule → default deny + Powerbox */
}

/* ====================================================================
 * P2: frequency counters (P3 预留 — 频率阈值策略)
 *
 * freq_bump() is the single hook called on every grant-beat in
 * do_check (spec: "授权命中时计数器+1").  do_freq queries/resets.
 * ==================================================================== */

typedef struct {
        int  in_use;
        u64  subject_id;
        u32  atom;
        u32  count;
} perm_freq_ent_t;

#define PERM_FREQ_SLOTS  32

static perm_freq_ent_t s_freq[PERM_FREQ_SLOTS];

static void freq_bump(u64 subject_id, u32 atom)
{
        for (int i = 0; i < PERM_FREQ_SLOTS; i++) {
                perm_freq_ent_t *f = &s_freq[i];
                if (f->in_use && f->subject_id == subject_id && f->atom == atom) {
                        f->count++;
                        return;
                }
        }
        for (int i = 0; i < PERM_FREQ_SLOTS; i++) {
                if (!s_freq[i].in_use) {
                        perm_freq_ent_t *f = &s_freq[i];
                        f->in_use = 1;
                        f->subject_id = subject_id;
                        f->atom = atom;
                        f->count = 1;
                        return;
                }
        }
}

/* ====================================================================
 * P2: context tracking (P3 预留 — 前台/后台感知授权)
 *
 * Upsert-only; no enforcement yet (P3 will combine it with scope_hash).
 * ==================================================================== */

typedef struct {
        int  in_use;
        u64  subject_id;
        u32  foreground;    /* 1 = 前台, 0 = 后台 */
} perm_ctx_ent_t;

#define PERM_CTX_SLOTS  16

static perm_ctx_ent_t s_ctx[PERM_CTX_SLOTS];

static int ctx_upsert(u64 subject_id, u32 foreground)
{
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
                        c->in_use = 1;
                        c->subject_id = subject_id;
                        c->foreground = foreground ? 1 : 0;
                        return 0;
                }
        }
        return ERR_NOMEM;               /* context table full */
}

/* ====================================================================
 * P2: audit ring (P3 预留 — 审计导出)
 *
 * Every do_check decision path appends one entry (ring of
 * PERM_AUDIT_MAX).  verdict: 0 = granted, 1 = denied.
 * ==================================================================== */

static perm_audit_ent_t s_audit[PERM_AUDIT_MAX];
static u32 s_audit_head;            /* next slot to write */
static u32 s_audit_count;           /* valid entries (≤ PERM_AUDIT_MAX) */

static void audit_append(u64 subject_id, u32 atom, u32 verdict,
                         const vfs_resource_t *res)
{
        perm_audit_ent_t *e = &s_audit[s_audit_head];
        e->tick = (u64)get_time();
        e->subject_id = subject_id;
        e->atom = atom;
        e->verdict = verdict;
        e->resource = *res;
        s_audit_head = (s_audit_head + 1) % PERM_AUDIT_MAX;
        if (s_audit_count < PERM_AUDIT_MAX)
                s_audit_count++;
}

/* ====================================================================
 * P2: policy snapshot serialization (P4 预留 — 持久化/热更新)
 *
 * v1 layout (packed, deterministic — table order, empty slots zeroed,
 * so save→load→save is byte-identical):
 *
 *   header  : magic u32, version u32, grant_count u32, role_count u32
 *   grants  : PERM_MAX_GRANTS × { subject_id u64, access u32,
 *              vfs_resource_t }   (36 B each)
 *   roles   : PERM_MAX_ROLES  × { subject_id u64, role u32 }  (12 B each)
 *
 *   size = 16 + 64*36 + 64*12 = 3088 ≤ PERM_POLICY_MAX (3840) ✓
 * ==================================================================== */

/* Forward decls (defined later in this file) */
static u32 atom_from_access(u32 access);
static int decision_encode(u64 subject_id, u32 atom);
static void fmt_append(char *dst, int dst_len, int *pos, const char *s);
static void fmt_uint(char *dst, int dst_len, int *pos, unsigned v, int base);

typedef struct __attribute__((packed)) {
        u64 subject_id;      /* enforcement identity (0 = any subject) */
        u32 access;
        vfs_resource_t resource;
} perm_policy_grant_ent_t;

typedef struct __attribute__((packed)) {
        u64 subject_id;
        u32 role;
} perm_policy_role_ent_t;

typedef struct __attribute__((packed)) {
        u32 magic;
        u32 version;
        u32 grant_count;
        u32 role_count;
        perm_policy_grant_ent_t grants[PERM_MAX_GRANTS];
        perm_policy_role_ent_t  roles[PERM_MAX_ROLES];
} perm_policy_blob_t;

static int policy_serialize(u8 *out, int out_len)
{
        if (out_len < (int)sizeof(perm_policy_blob_t))
                return ERR_NOMEM;
        perm_policy_blob_t *b = (perm_policy_blob_t *)out;
        memset(b, 0, sizeof(*b));
        b->magic = PERM_POLICY_MAGIC;
        b->version = PERM_POLICY_VERSION;
        u32 gn = 0;
        for (int i = 0; i < PERM_MAX_GRANTS; i++) {
                perm_grant_t *g = &s_grants[i];
                if (!g->in_use)
                        continue;
                b->grants[i].subject_id = g->subject_id;
                b->grants[i].access = g->access;
                b->grants[i].resource = g->resource;
                gn++;
        }
        u32 rn = 0;
        for (int i = 0; i < PERM_MAX_ROLES; i++) {
                perm_role_t *r = &s_roles[i];
                if (!r->in_use)
                        continue;
                b->roles[i].subject_id = r->subject_id;
                b->roles[i].role = r->role;
                rn++;
        }
        b->grant_count = gn;
        b->role_count = rn;
        return (int)sizeof(perm_policy_blob_t);
}

/* All-or-nothing load: validate the ENTIRE blob before mutating any
 * state.  On success: roles first, then grants restored into their
 * original table slots (byte-identical round trip), then hot-reload
 * the kernel caps: revoke the old atom, re-issue the new one. */
static int policy_deserialize(const u8 *in, int len)
{
        if (len < (int)sizeof(perm_policy_blob_t))
                return ERR_INVAL;
        const perm_policy_blob_t *b = (const perm_policy_blob_t *)in;
        if (b->magic != PERM_POLICY_MAGIC)
                return ERR_INVAL;
        if (b->version != PERM_POLICY_VERSION)
                return ERR_INVAL;
        if (b->grant_count > PERM_MAX_GRANTS || b->role_count > PERM_MAX_ROLES)
                return ERR_INVAL;

        /* Validate roles before mutating. */
        for (int i = 0; i < PERM_MAX_ROLES; i++) {
                const perm_policy_role_ent_t *r = &b->roles[i];
                if (r->subject_id == 0)
                        continue;               /* empty slot */
                if (r->role >= PERM_ROLE_MAX)
                        return ERR_INVAL;
        }
        /* Validate grants before mutating. */
        for (int i = 0; i < PERM_MAX_GRANTS; i++) {
                const perm_policy_grant_ent_t *g = &b->grants[i];
                if (g->subject_id == 0 && g->access == 0)
                        continue;               /* empty slot */
                if (g->access == 0)
                        return ERR_INVAL;
        }

        /* Mutate: roles. */
        memset(s_roles, 0, sizeof(s_roles));
        for (int i = 0; i < PERM_MAX_ROLES; i++) {
                const perm_policy_role_ent_t *r = &b->roles[i];
                if (r->subject_id == 0)
                        continue;
                s_roles[i].in_use = 1;
                s_roles[i].subject_id = r->subject_id;
                s_roles[i].role = r->role;
        }

        /* Mutate: grants + hot reload the kernel caps (补充六). */
        memset(s_grants, 0, sizeof(s_grants));
        for (int i = 0; i < PERM_MAX_GRANTS; i++) {
                const perm_policy_grant_ent_t *g = &b->grants[i];
                if (g->subject_id == 0 && g->access == 0)
                        continue;
                perm_grant_t *d = &s_grants[i];
                d->in_use = 1;
                d->subject_id = g->subject_id;
                d->access = g->access;
                d->resource = g->resource;

                u32 atom = atom_from_access(g->access);
                if (d->subject_id != 0 && atom != ATOM_NONE && atom <= ATOM_MAX) {
                        (void)cap_revoke_by_atom(d->subject_id, atom, 0);
                        (void)decision_encode(d->subject_id, atom);
                }
        }
        return 0;
}

/* ====================================================================
 * Atom encoding — map a VFS_ACCESS_* mask to the atom it represents
 * ==================================================================== */

static u32 atom_from_access(u32 access)
{
        if (access & VFS_ACCESS_WRITE)
                return ATOM_DATA_DOCS_WRITE;
        if (access & VFS_ACCESS_READ)
                return ATOM_DATA_DOCS_READ;
        return ATOM_NONE;               /* EXEC/COW/… → no chain rule */
}

/* ====================================================================
 * Decision encoding — the capability IS the decision (§四)
 *
 * The perm-engine is the ONLY signer of atom capabilities.  When a
 * decision lands (ANSWER allow / direct GRANT), encode it into the
 * subject's kernel table via cap_grant_to_subject() so the kernel can
 * enforce it independently of the engine (授予路径异步).
 * ==================================================================== */

/* Returns 0 on success, or a negative error code when the kernel-side
 * cap issuance failed (e.g. ERR_NOENT when the target subject's process
 * has already exited).  Callers MUST propagate this to the client so a
 * failed grant is not silently reported as success. */
static int decision_encode(u64 subject_id, u32 atom)
{
        if (subject_id == 0 || atom == ATOM_NONE || atom > ATOM_MAX)
                return 0;
        int h = cap_grant_to_subject(subject_id, atom, RIGHT_ALL, 0, 0);
        if (h < 0) {
                printf("perm: encode subject=%u atom=%u failed (%d)\n",
               (unsigned)subject_id, atom, h);
                return h;
        }
        printf("perm: encode subject=%u atom=%u handle=%d\n",
           (unsigned)subject_id, atom, h);
        return 0;
}

/* ====================================================================
 * Query table helpers
 * ==================================================================== */

static perm_query_t *query_find(u32 query_id)
{
        for (int i = 0; i < PERM_MAX_QUERIES; i++) {
                perm_query_t *q = &s_queries[i];
                if (q->in_use && q->query_id == query_id)
                        return q;
        }
        return NULL;
}

/* First PENDING query (FIFO), or NULL. */
static perm_query_t *query_first_pending(void)
{
        for (int i = 0; i < PERM_MAX_QUERIES; i++) {
                perm_query_t *q = &s_queries[i];
                if (q->in_use && q->state == PERM_QUERY_PENDING)
                        return q;
        }
        return NULL;
}

/* Reuse a PENDING query for the same (subject, resource), else allocate
 * a fresh one.  The subject's display name/PID is resolved from the
 * kernel ONCE on the fresh-alloc path (never on a check that reuses an
 * identical pending query).  Returns NULL when the queue is full. */
static perm_query_t *query_alloc(const vfs_resource_t *res, u64 subject_id,
                                 u32 atom)
{
        for (int i = 0; i < PERM_MAX_QUERIES; i++) {
                perm_query_t *q = &s_queries[i];
                if (!q->in_use)
                        continue;
                if (q->state != PERM_QUERY_PENDING)
                        continue;
                if (q->subject_id != subject_id)
                        continue;
                if (memcmp(&q->resource, res, sizeof(*res)) != 0)
                        continue;
                return q;                   /* identical pending query */
        }
        for (int i = 0; i < PERM_MAX_QUERIES; i++) {
                if (!s_queries[i].in_use) {
                        perm_query_t *q = &s_queries[i];
                        memset(q, 0, sizeof(*q));
                        q->in_use = 1;
                        q->query_id = ++s_query_seq;
                        q->resource = *res;
                        q->subject_id = subject_id;
                        q->atom = atom;
                        q->state = PERM_QUERY_PENDING;

                        /* Resolve display identity once via the kernel
             * SYS_PROC_INFO_BY_SUBJECT wrapper.  If the subject already
             * died (ERR_NOENT), fall back to "subject <id>" so the UI
             * label is never empty. */
                        proc_ident_t ident;
                        if (proc_info_by_subject(subject_id, &ident) == 0) {
                                q->pid = (u32)ident.pid;
                                strncpy(q->name, ident.name, sizeof(q->name) - 1);
                                q->name[sizeof(q->name) - 1] = '\0';
                                if (q->name[0] == '\0')
                                        strcpy(q->name, "unknown");
                        } else {
                                q->pid = 0;
                                int pos = 0;
                                fmt_append(q->name, sizeof(q->name), &pos, "subject ");
                                fmt_uint(q->name, sizeof(q->name), &pos,
                         (unsigned)subject_id, 10);
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

static void fmt_append(char *dst, int dst_len, int *pos, const char *s)
{
        while (*s && *pos < dst_len - 1)
                dst[(*pos)++] = *s++;
}

static void fmt_uint(char *dst, int dst_len, int *pos, unsigned v, int base)
{
        char tmp[16];
        int n = 0;
        if (v == 0) {
                tmp[n++] = '0';
        } else {
                while (v > 0 && n < (int)sizeof(tmp) - 1) {
                        unsigned d = v % (unsigned)base;
                        tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
                        v /= (unsigned)base;
                }
        }
        while (n > 0 && *pos < dst_len - 1)
                dst[(*pos)++] = tmp[--n];
}

static void access_label(char *out, int out_len, u32 access)
{
        int n = 0;
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
static void build_label(char *dst, int dst_len, const perm_query_t *q)
{
        int pos = 0;
        dst[0] = '\0';
        if (q->state == PERM_QUERY_PENDING) {
                fmt_append(dst, dst_len, &pos, "perm: ");
                fmt_append(dst, dst_len, &pos, q->name);
                fmt_append(dst, dst_len, &pos, " (PID ");
                fmt_uint(dst, dst_len, &pos, (unsigned)q->pid, 10);
                fmt_append(dst, dst_len, &pos, ") 请求访问 ");
                fmt_append(dst, dst_len, &pos, q->url);
                fmt_append(dst, dst_len, &pos, " (");
                char acc[8];
                access_label(acc, sizeof(acc), q->access);
                fmt_append(dst, dst_len, &pos, acc);
                fmt_append(dst, dst_len, &pos, ") — 输入 perm_answer ");
                fmt_uint(dst, dst_len, &pos, q->query_id, 10);
                fmt_append(dst, dst_len, &pos, " y/n");
        } else {
                fmt_append(dst, dst_len, &pos, "perm: 查询 ");
                fmt_uint(dst, dst_len, &pos, q->query_id, 10);
                fmt_append(dst, dst_len, &pos,
                   q->state == PERM_QUERY_ALLOWED ? " 已允许" : " 已拒绝");
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

static void notify_ui(const perm_query_t *q)
{
        if (s_ui_port < 0) {
                s_ui_port = port_get(PERM_UI_PORT_NAME);
                if (s_ui_port < 0)
                        return;
        }

        perm_req_ui_t *req = (perm_req_ui_t *)s_req;
        memset(req, 0, sizeof(*req));
        req->op = PERM_OP_UI_SHOW;
        req->query_id = q->query_id;
        req->subject_id = q->subject_id;
        req->pid = q->pid;
        strncpy(req->name, q->name, sizeof(req->name) - 1);
        req->name[sizeof(req->name) - 1] = '\0';
        strncpy(req->url, q->url, sizeof(req->url) - 1);
        req->access = q->access;
        req->state = q->state;

        /* P1: perm 聚合完整提示文本，term 原样展示 */
        build_label(req->label, sizeof(req->label), q);

        perm_resp_ui_t resp;
        int resp_len = (int)sizeof(resp);
        (void)ipc_call(s_ui_port, req, (int)sizeof(*req), &resp, &resp_len);
}

/* ====================================================================
 * Operation handlers
 * ==================================================================== */

/* P2: role-chain ALLOW grants a capability, not a raw access mask:
 * READ allows → VFS_ACCESS_READ, WRITE allows → VFS_ACCESS_WRITE;
 * untyped atoms (EXEC/COW/…) fall back to the requested mask.  This is
 * the capability-side 抹位 for the chain path. */
static u32 granted_for_atom(u32 atom, u32 access)
{
        if (atom == ATOM_DATA_DOCS_READ)
                return VFS_ACCESS_READ;
        if (atom == ATOM_DATA_DOCS_WRITE)
                return VFS_ACCESS_WRITE;
        return access;
}

/* CHECK: vfs_server → synchronous authorization check.
 * Decision order (§四): 1) explicit grant beat, 2) role-chain verdict
 * (override-first), 3) default deny → Powerbox.
 *
 * P2 抹位: the grant beat now fires on PARTIAL intersection
 * ((g->access & req->access) != 0) and resp->granted carries ONLY the
 * covered bits — a READ grant never yields a WRITE-carrying handle. */
static void do_check(int token, int msg_len)
{
        perm_resp_check_t *resp = (perm_resp_check_t *)s_resp;
        resp->granted = 0;
        resp->query_id = 0;
        if (msg_len < (int)sizeof(perm_req_check_t)) {
                resp->ret = ERR_INVAL;
                goto out;
        }
        perm_req_check_t *req = (perm_req_check_t *)s_req;
        u64 subject = req->subject_id;  /* filled by trusted vfs_server */
        u32 atom = atom_from_access(req->access);

        /* 1) Grant beat — an explicit grant (powerbox result) wins. */
        perm_grant_t *g = grant_find(req->subject_id, &req->resource);
        if (g && (g->access & req->access) != 0) {
                resp->ret = 0;              /* granted — proceed */
                resp->granted = g->access & req->access;   /* 抹位掩码 */
                freq_bump(subject, atom);
                audit_append(subject, atom, PERM_VERDICT_ALLOW, &req->resource);
                goto out;
        }

        /* 2) Role chain — override-first on (role, atom). */
        i32 verdict = rule_lookup(role_of(subject), atom);
        if (verdict == PERM_VERDICT_ALLOW) {
                resp->ret = 0;
                resp->granted = granted_for_atom(atom, req->access);
                audit_append(subject, atom, PERM_VERDICT_ALLOW, &req->resource);
                goto out;
        }
        if (verdict == PERM_VERDICT_DENY) {
                /* Chain deny: policy says no — no Powerbox prompt. */
                resp->ret = VFS_ERR_ACCESS;
                audit_append(subject, atom, PERM_VERDICT_DENY, &req->resource);
                goto out;
        }

        /* 3) Default deny → Powerbox.  Create/reuse a pending query and
     * tell the UI to show the prompt.  vfs_server translates this into
     * -EACCES for the client (design §8: 授权前 → -EACCES). */
        perm_query_t *q = query_alloc(&req->resource, subject, atom);
        if (!q) {
                resp->ret = ERR_NOMEM;      /* query queue full */
                goto out;
        }
        strncpy(q->url, req->url, sizeof(q->url) - 1);
        q->url[sizeof(q->url) - 1] = '\0';
        q->access = req->access;

        resp->ret = VFS_ERR_ACCESS;
        resp->query_id = q->query_id;
        audit_append(subject, atom, PERM_VERDICT_DENY, &req->resource);

        notify_ui(q);

out:
        (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

/* ANSWER: user verdict → grant upsert + decision encode (allow) or
 * deny.  The Powerbox verdict lands as a kernel-enforceable atom cap
 * issued into the requesting subject's table (§四). */
static void do_answer(int token, int msg_len)
{
        perm_resp_answer_t *resp = (perm_resp_answer_t *)s_resp;
        if (msg_len < (int)sizeof(perm_req_answer_t)) {
                resp->ret = ERR_INVAL;
                goto out;
        }
        perm_req_answer_t *req = (perm_req_answer_t *)s_req;

        perm_query_t *q = query_find(req->query_id);
        if (!q) {
                resp->ret = ERR_NOENT;
                goto out;
        }

        if (req->allow) {
                q->state = PERM_QUERY_ALLOWED;
                if (!grant_upsert(q->subject_id, &q->resource, q->access))
                        resp->ret = ERR_NOMEM;
                else {
                        /* Encode the decision into the subject's kernel cap table.
             * If the target process has exited (ERR_NOENT) the grant
             * table entry still stands for a future re-spawn, but we
             * report the failure so the caller knows the kernel cap
             * was not issued. */
                        int enc = decision_encode(q->subject_id, q->atom);
                        resp->ret = (enc < 0) ? enc : 0;
                }
        } else {
                q->state = PERM_QUERY_DENIED;
                resp->ret = 0;
        }

        notify_ui(q);

out:
        (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

/* QUERY: UI agent fetches a pending query (decision 2). */
static void do_query(int token, int msg_len)
{
        perm_resp_query_t *resp = (perm_resp_query_t *)s_resp;
        if (msg_len < (int)sizeof(perm_req_query_t)) {
                resp->ret = ERR_INVAL;
                goto out;
        }
        perm_req_query_t *req = (perm_req_query_t *)s_req;

        perm_query_t *q = (req->query_id == 0) ? query_first_pending()
                                           : query_find(req->query_id);
        if (!q) {
                resp->ret = ERR_NOENT;
                goto out;
        }

        resp->ret = 0;
        resp->query_id = q->query_id;
        resp->pid = q->pid;
        strncpy(resp->name, q->name, sizeof(resp->name) - 1);
        resp->name[sizeof(resp->name) - 1] = '\0';
        strncpy(resp->url, q->url, sizeof(resp->url) - 1);
        resp->url[sizeof(resp->url) - 1] = '\0';
        resp->access = q->access;
        resp->subject_id = q->subject_id;
        resp->state = q->state;
        resp->label[0] = '\0';
        {
                int pos = 0;
                fmt_append(resp->label, sizeof(resp->label), &pos, q->name);
                fmt_append(resp->label, sizeof(resp->label), &pos, " (PID ");
                fmt_uint(resp->label, sizeof(resp->label), &pos, (unsigned)q->pid, 10);
                fmt_append(resp->label, sizeof(resp->label), &pos, ") → ");
                fmt_append(resp->label, sizeof(resp->label), &pos, q->url);
                fmt_append(resp->label, sizeof(resp->label), &pos, ", subject ");
                fmt_uint(resp->label, sizeof(resp->label), &pos,
                 (unsigned)q->subject_id, 10);
                fmt_append(resp->label, sizeof(resp->label), &pos,
                   q->state == PERM_QUERY_PENDING ? " (待决定)" : " (已决定)");
        }

out:
        (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

/* REVOKE: drop grants (default deny). */
static void do_revoke(int token, int msg_len)
{
        perm_resp_revoke_t *resp = (perm_resp_revoke_t *)s_resp;
        if (msg_len < (int)sizeof(perm_req_revoke_t)) {
                resp->ret = ERR_INVAL;
                goto out;
        }
        perm_req_revoke_t *req = (perm_req_revoke_t *)s_req;

        resp->revoked = grant_revoke(req->subject_id, &req->resource);
        resp->ret = 0;

out:
        (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

/* GRANT: direct grant, bypasses the Powerbox (tests/management).
 * P1: carries the target subject + atom; the decision is encoded into
 * the subject's kernel table via cap_grant_to_subject().
 * GATED (docs/ops_format.md §6): the CALLER must hold the
 * ATOM_SERVICE_MANAGE atom — grants beat role defaults (§四), so a
 * management-CAPABLE caller may grant even when its ROLE is not
 * management (e.g. init hot-reloaded to GUEST).  Apps can never hold
 * the management atom, so they cannot grant. */
static void do_grant(int token, int msg_len, u64 caller_subject)
{
        perm_resp_grant_t *resp = (perm_resp_grant_t *)s_resp;
        if (msg_len < (int)sizeof(perm_req_grant_t)) {
                resp->ret = ERR_INVAL;
                goto out;
        }
        perm_req_grant_t *req = (perm_req_grant_t *)s_req;

        if (cap_has_atom(caller_subject, ATOM_SERVICE_MANAGE) != 1) {
                resp->ret = ERR_DENIED;         /* apps cannot grant */
                goto out;
        }

        resp->ret = grant_upsert(req->subject_id, &req->resource,
                             req->access) ? 0 : ERR_NOMEM;
        if (resp->ret == 0) {
                /* Encode the decision into the target subject's kernel cap table.
         * A failure here (e.g. ERR_NOENT — target process already exited)
         * must be propagated: otherwise the client believes the grant
         * succeeded but no kernel-enforceable cap was issued. */
                int enc = decision_encode(req->subject_id, req->atom);
                if (enc < 0)
                        resp->ret = enc;
        }

out:
        (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

/* ROLE_SET: management-plane hot reload (§二.2).  The caller's subject
 * comes from ipc_recv_from (the kernel — unforgeable); only OWNER/ADMIN
 * callers may change roles.  Applied immediately; grants are NOT
 * rewritten (grants beat role defaults — §四). */
static void do_role_set(int token, int msg_len, u64 caller_subject)
{
        perm_resp_role_set_t *resp = (perm_resp_role_set_t *)s_resp;
        if (msg_len < (int)sizeof(perm_req_role_set_t)) {
                resp->ret = ERR_INVAL;
                goto out;
        }
        perm_req_role_set_t *req = (perm_req_role_set_t *)s_req;

        if (!role_is_management(caller_subject)) {
                resp->ret = ERR_DENIED;         /* apps cannot change policy */
                goto out;
        }
        resp->ret = role_set(req->subject_id, req->role);
        if (resp->ret == 0)
                resp->role = role_of(req->subject_id);

out:
        (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

/* DUMP: export policy state (roles + rules) for tests/management. */
static void do_dump(int token, int msg_len)
{
        perm_resp_dump_t *resp = (perm_resp_dump_t *)s_resp;
        memset(resp, 0, sizeof(*resp));

        int n = 0;
        resp->role_count = 0;
        for (int i = 0; i < PERM_MAX_ROLES && n < 8; i++) {
                perm_role_t *r = &s_roles[i];
                if (!r->in_use)
                        continue;
                resp->role_count++;
                resp->lines[n][0] = '\0';
                {
                        int pos = 0;
                        fmt_append(resp->lines[n], PERM_DUMP_LINE_MAX, &pos, "role: subject=");
                        fmt_uint(resp->lines[n], PERM_DUMP_LINE_MAX, &pos,
                     (unsigned)r->subject_id, 10);
                        fmt_append(resp->lines[n], PERM_DUMP_LINE_MAX, &pos, " role=");
                        fmt_uint(resp->lines[n], PERM_DUMP_LINE_MAX, &pos, r->role, 10);
                }
                n++;
        }
        resp->rule_count = s_rule_count;
        resp->grant_count = 0;
        for (int i = 0; i < PERM_MAX_GRANTS; i++)
                if (s_grants[i].in_use)
                        resp->grant_count++;

        if (n < 8) {
                for (u32 role = 0; role < PERM_ROLE_MAX && n < 8; role++) {
                        for (u32 atom = 0; atom <= ATOM_MAX && n < 8; atom++) {
                                i32 v = rule_lookup(role, atom);
                                if (v < 0)
                                        continue;
                                resp->lines[n][0] = '\0';
                                {
                                        int pos = 0;
                                        fmt_append(resp->lines[n], PERM_DUMP_LINE_MAX, &pos,
                               "rule: role=");
                                        fmt_uint(resp->lines[n], PERM_DUMP_LINE_MAX, &pos, role, 10);
                                        fmt_append(resp->lines[n], PERM_DUMP_LINE_MAX, &pos,
                               " atom=");
                                        fmt_uint(resp->lines[n], PERM_DUMP_LINE_MAX, &pos, atom, 10);
                                        fmt_append(resp->lines[n], PERM_DUMP_LINE_MAX, &pos, " ");
                                        fmt_append(resp->lines[n], PERM_DUMP_LINE_MAX, &pos,
                               v == PERM_VERDICT_ALLOW ? "ALLOW" : "DENY");
                                }
                                n++;
                        }
                }
        }
        resp->ret = 0;

        (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

/* ====================================================================
 * P2: P3/P4 预留 handlers
 *
 * 这些接口当前只维护状态、不做强制（测试以 GUEST 角色运行，若做
 * 管理面 gate 将全部被拒）。P4 起应对 ROLE_SET 一样要求调用者为
 * OWNER/ADMIN（role_is_management），并纳入策略快照。
 * ==================================================================== */

/* CONTEXT (P3 预留): 前台/后台切换通知。 */
static void do_context(int token, int msg_len)
{
        perm_resp_context_t *resp = (perm_resp_context_t *)s_resp;
        resp->ret = ERR_INVAL;
        if (msg_len < (int)sizeof(perm_req_context_t))
                goto out;
        perm_req_context_t *req = (perm_req_context_t *)s_req;
        resp->ret = ctx_upsert(req->subject_id, req->foreground);
out:
        (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

/* FREQ (P3 预留): 查询/清零授权命中频率计数器。 */
static void do_freq(int token, int msg_len)
{
        perm_resp_freq_t *resp = (perm_resp_freq_t *)s_resp;
        memset(resp, 0, sizeof(*resp));
        resp->ret = ERR_INVAL;
        if (msg_len < (int)sizeof(perm_req_freq_t))
                goto out;
        perm_req_freq_t *req = (perm_req_freq_t *)s_req;

        u32 total = 0, slots = 0;
        for (int i = 0; i < PERM_FREQ_SLOTS; i++) {
                perm_freq_ent_t *f = &s_freq[i];
                if (!f->in_use)
                        continue;
                if (req->subject_id != 0 && f->subject_id != req->subject_id)
                        continue;
                if (req->atom != 0 && f->atom != req->atom)
                        continue;
                total += f->count;
                slots++;
                if (req->reset)
                        f->count = 0;           /* keep the slot */
        }
        resp->count = total;
        resp->slots = slots;
        resp->ret = 0;
out:
        (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

/* POLICY_SAVE (P4 预留): 导出策略二进制快照。
 * 只要求 op 字段即可（size/data 是 LOAD 方向用的）。 */
static void do_policy_save(int token, int msg_len)
{
        perm_resp_policy_t *resp = (perm_resp_policy_t *)s_resp;
        memset(resp, 0, sizeof(*resp));
        resp->ret = ERR_INVAL;
        if (msg_len < (int)sizeof(u32))
                goto out;
        int n = policy_serialize(resp->data, (int)sizeof(resp->data));
        if (n < 0) {
                resp->ret = n;
                goto out;
        }
        resp->size = (u32)n;
        resp->ret = 0;
out:
        (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

/* POLICY_LOAD (P4 预留): 导入策略二进制快照（全有或全无 + 热更新）。 */
static void do_policy_load(int token, int msg_len)
{
        perm_resp_policy_t *resp = (perm_resp_policy_t *)s_resp;
        memset(resp, 0, sizeof(*resp));
        resp->ret = ERR_INVAL;
        if (msg_len < (int)sizeof(perm_req_policy_t))
                goto out;
        perm_req_policy_t *req = (perm_req_policy_t *)s_req;
        if (req->size == 0 || req->size > (u32)sizeof(req->data)) {
                resp->ret = ERR_INVAL;
                goto out;
        }
        resp->ret = policy_deserialize(req->data, (int)req->size);
out:
        (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

/* AUDIT (P3 预留): 导出审计环形缓冲区（最旧在前）。 */
static void do_audit(int token, int msg_len)
{
        perm_resp_audit_t *resp = (perm_resp_audit_t *)s_resp;
        memset(resp, 0, sizeof(*resp));
        resp->ret = ERR_INVAL;
        if (msg_len < (int)sizeof(perm_req_audit_t))
                goto out;
        u32 n = s_audit_count;
        u32 start = (s_audit_head + PERM_AUDIT_MAX - n) % PERM_AUDIT_MAX;
        for (u32 i = 0; i < n; i++)
                resp->entries[i] = s_audit[(start + i) % PERM_AUDIT_MAX];
        resp->count = n;
        resp->ret = 0;
out:
        (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void perm_handle_request(int token, u32 op, int msg_len,
                                                                u64 caller_subject)
{
        switch (op) {
        case PERM_OP_CHECK:   do_check(token, msg_len);       break;
        case PERM_OP_ANSWER:  do_answer(token, msg_len);      break;
        case PERM_OP_QUERY:   do_query(token, msg_len);       break;
        case PERM_OP_REVOKE:  do_revoke(token, msg_len);      break;
        case PERM_OP_GRANT:   do_grant(token, msg_len, caller_subject); break;
        case PERM_OP_ROLE_SET: do_role_set(token, msg_len, caller_subject); break;
        case PERM_OP_DUMP:    do_dump(token, msg_len);        break;
        case PERM_OP_CONTEXT: do_context(token, msg_len);     break;
        case PERM_OP_FREQ:    do_freq(token, msg_len);        break;
        case PERM_OP_POLICY_SAVE: do_policy_save(token, msg_len); break;
        case PERM_OP_POLICY_LOAD: do_policy_load(token, msg_len); break;
        case PERM_OP_AUDIT:   do_audit(token, msg_len);       break;
        default: {
                i32 *resp = (i32 *)s_resp;
                *resp = ERR_INVAL;
                (void)ipc_reply(token, resp, (int)sizeof(i32));
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
static void seed_rules(void)
{
        static const struct {
                u32 role;
                u32 atom;
                i32 verdict;
        } seeds[] = {
                /* OWNER: complete control */
                { PERM_ROLE_OWNER, ATOM_DATA_DOCS_READ,  PERM_VERDICT_ALLOW },
                { PERM_ROLE_OWNER, ATOM_DATA_DOCS_WRITE, PERM_VERDICT_ALLOW },
                { PERM_ROLE_OWNER, ATOM_DATA_DL_WRITE,   PERM_VERDICT_ALLOW },
                { PERM_ROLE_OWNER, ATOM_NET_CONNECT,     PERM_VERDICT_ALLOW },
                { PERM_ROLE_OWNER, ATOM_SERVICE_MANAGE,  PERM_VERDICT_ALLOW },
                { PERM_ROLE_OWNER, ATOM_PKG_INSTALL,     PERM_VERDICT_ALLOW },
                { PERM_ROLE_OWNER, ATOM_SYS_DEBUG,       PERM_VERDICT_ALLOW },
                { PERM_ROLE_OWNER, ATOM_CAP_GRANT_SELF,  PERM_VERDICT_ALLOW },
                /* ADMIN: system management */
                { PERM_ROLE_ADMIN, ATOM_DATA_DOCS_READ,  PERM_VERDICT_ALLOW },
                { PERM_ROLE_ADMIN, ATOM_DATA_DOCS_WRITE, PERM_VERDICT_ALLOW },
                { PERM_ROLE_ADMIN, ATOM_DATA_DL_WRITE,   PERM_VERDICT_ALLOW },
                { PERM_ROLE_ADMIN, ATOM_NET_CONNECT,     PERM_VERDICT_ALLOW },
                { PERM_ROLE_ADMIN, ATOM_SERVICE_MANAGE,  PERM_VERDICT_ALLOW },
                { PERM_ROLE_ADMIN, ATOM_PKG_INSTALL,     PERM_VERDICT_ALLOW },
                /* STANDARD: default role — docs read allowed; docs write has NO
         * chain rule so it falls through to default-deny -> Powerbox
         * (user consent via perm_answer, then grant beats the default). */
                { PERM_ROLE_STANDARD, ATOM_DATA_DOCS_READ,  PERM_VERDICT_ALLOW },
                /* CHILD: restricted */
                { PERM_ROLE_CHILD, ATOM_DATA_DOCS_READ,  PERM_VERDICT_ALLOW },
                { PERM_ROLE_CHILD, ATOM_DATA_DOCS_WRITE, PERM_VERDICT_DENY },
                /* GUEST: minimal */
                { PERM_ROLE_GUEST, ATOM_DATA_DOCS_READ,  PERM_VERDICT_DENY },
                { PERM_ROLE_GUEST, ATOM_DATA_DOCS_WRITE, PERM_VERDICT_DENY },
                { PERM_ROLE_GUEST, ATOM_NET_CONNECT,     PERM_VERDICT_DENY },
                /* AUDITOR: read-only + audit */
                { PERM_ROLE_AUDITOR, ATOM_DATA_DOCS_READ,  PERM_VERDICT_ALLOW },
                { PERM_ROLE_AUDITOR, ATOM_DATA_DOCS_WRITE, PERM_VERDICT_DENY },
                { PERM_ROLE_AUDITOR, ATOM_DATA_SYS_LOGS_READ, PERM_VERDICT_ALLOW },
        };
        for (u32 i = 0; i < sizeof(seeds) / sizeof(seeds[0]); i++)
                rule_seed(seeds[i].role, seeds[i].atom, seeds[i].verdict);
}

/* ====================================================================
 * Entry point
 * ==================================================================== */

int main(void)
{
        printf("perm: starting permission manager\n");

        memset(s_grants, 0, sizeof(s_grants));
        memset(s_roles, 0, sizeof(s_roles));
        memset(s_rules, 0, sizeof(s_rules));
        memset(s_queries, 0, sizeof(s_queries));
        memset(s_freq, 0, sizeof(s_freq));
        memset(s_ctx, 0, sizeof(s_ctx));
        memset(s_audit, 0, sizeof(s_audit));
        s_audit_head = 0;
        s_audit_count = 0;
        for (u32 role = 0; role < PERM_ROLE_MAX; role++)
                for (u32 atom = 0; atom <= ATOM_MAX; atom++)
                        s_rule_head[role][atom] = PERM_CHAIN_NONE;
        s_query_seq = (u32)get_time() & 0x7FFFFFFFu;

        /* Bootstrap roles (§二.2): the policy engine itself is the top
     * authority (OWNER); init (kernel subject 1, the device owner) is
     * seeded OWNER too.  Both come from unforgeable kernel sources:
     * get_subject() and the documented kernel subject numbering. */
        role_set(get_subject(), PERM_ROLE_OWNER);
        role_set(PERM_BOOTSTRAP_SUBJECT, PERM_BOOTSTRAP_ROLE);
        seed_rules();

        int port = ipc_port_create();
        if (port < 0) {
                printf("perm: ipc_port_create failed (%d)\n", port);
                thread_exit(1);
        }
        int ret = port_register(PERM_PORT_NAME, port);
        if (ret < 0) {
                printf("perm: port_register('%s') failed (%d)\n",
               PERM_PORT_NAME, ret);
                thread_exit(1);
        }
        printf("perm: port %d registered as '%s'\n", port, PERM_PORT_NAME);

        printf("perm: serving (grants %d, roles %d, rules %d, queries %d)\n",
           PERM_MAX_GRANTS, PERM_MAX_ROLES, PERM_MAX_RULES,
           PERM_MAX_QUERIES);

        for (;;) {
                int msg_len = (int)sizeof(s_req);
                int token = 0;
                u64 caller_subject = 0;
                ret = ipc_recv_from(port, s_req, &msg_len, &token, &caller_subject);
                if (ret < 0) {
                        printf("perm: ipc_recv failed (%d)\n", ret);
                        thread_exit(1);
                }
                if (msg_len < (int)sizeof(u32)) {   /* no op code */
                        i32 *resp = (i32 *)s_resp;
                        *resp = ERR_INVAL;
                        (void)ipc_reply(token, resp, (int)sizeof(i32));
                        continue;
                }
                u32 op = *(u32 *)s_req;
                perm_handle_request(token, op, msg_len, caller_subject);
        }
}
