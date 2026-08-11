/*
 * perm-manager.c - Powerbox permission manager (ring-3, independent process)
 * Copyright (c) 2026 OpSys Project
 *
 * Holds the SINGLE source of truth for access grants (design §9.4):
 *   (app_id_hash, resource) -> access mask
 * and the Powerbox query queue (§8 Phase 2 / 决策 2).  vfs_server
 * consults it on every CREATE/RESOLVE_BOOKMARK; the term service
 * renders the text prompt pushed on "perm.ui".
 *
 * Ops (perm.h):
 *   CHECK   3   vfs_server → sync authz; not granted → create pending
 *               query + UI_SHOW push → VFS_ERR_ACCESS (-EACCES).
 *   ANSWER  2   user verdict → grant upsert / deny; UI_SHOW update.
 *   QUERY   1   UI agent fetches a pending query.
 *   REVOKE  4   drop grants (default deny semantics).
 *   GRANT   5   direct grant, bypasses Powerbox (tests/management).
 *
 * State model:
 *   - A grant is a persistent (app, resource) -> access record.
 *   - A query is a transient request for consent; PENDING until the
 *     user answers, then ALLOWED (→ grant upserted) or DENIED.
 *   - Query ids are a monotonic counter (never reused).
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
#define PERM_MAX_URL      VFS_PATH_MAX

/* Request/response buffers (all perm messages < 4096) */
static u8 s_req[VFS_IPC_MAX];
static u8 s_resp[VFS_IPC_MAX];

/* ====================================================================
 * Grant table — (app_id_hash, resource) → access mask
 * ==================================================================== */

typedef struct {
    int            in_use;
    u32            app_id_hash;
    vfs_resource_t resource;
    u32            access;
} perm_grant_t;

static perm_grant_t s_grants[PERM_MAX_GRANTS];

/* ====================================================================
 * Query table — transient Powerbox consent requests
 * ==================================================================== */

typedef struct {
    int            in_use;
    u32            query_id;
    u32            app_id_hash;
    vfs_resource_t resource;
    u32            access;
    char           url[PERM_MAX_URL];
    i32            state;       /* PERM_QUERY_* */
} perm_query_t;

static perm_query_t s_queries[PERM_MAX_QUERIES];
static u32 s_query_seq;         /* monotonic query-id counter */

/* Lazily-resolved "perm.ui" port (term registers it at startup). */
static int s_ui_port = -1;

/* ====================================================================
 * Grant table helpers
 * ==================================================================== */

static perm_grant_t *grant_find(u32 app_id_hash, const vfs_resource_t *res)
{
    for (int i = 0; i < PERM_MAX_GRANTS; i++) {
        perm_grant_t *g = &s_grants[i];
        if (!g->in_use)
            continue;
        if (g->app_id_hash != app_id_hash)
            continue;
        if (memcmp(&g->resource, res, sizeof(*res)) != 0)
            continue;
        return g;
    }
    return NULL;
}

/* Upsert: extend the access mask of an existing grant, else allocate. */
static perm_grant_t *grant_upsert(u32 app_id_hash,
                                  const vfs_resource_t *res, u32 access)
{
    perm_grant_t *g = grant_find(app_id_hash, res);
    if (g) {
        g->access |= access;
        return g;
    }
    for (int i = 0; i < PERM_MAX_GRANTS; i++) {
        if (!s_grants[i].in_use) {
            g = &s_grants[i];
            g->in_use = 1;
            g->app_id_hash = app_id_hash;
            g->resource = *res;
            g->access = access;
            return g;
        }
    }
    return NULL;                    /* table full */
}

/* Count and drop grants matching app (0 = all) and resource
 * (zero uuid = all).  Returns the number revoked. */
static u32 grant_revoke(u32 app_id_hash, const vfs_resource_t *res)
{
    u32 revoked = 0;
    for (int i = 0; i < PERM_MAX_GRANTS; i++) {
        perm_grant_t *g = &s_grants[i];
        if (!g->in_use)
            continue;
        if (app_id_hash != 0 && g->app_id_hash != app_id_hash)
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

/* Reuse a PENDING query for the same (app, resource), else allocate a
 * fresh one.  Returns NULL when the queue is full. */
static perm_query_t *query_alloc(u32 app_id_hash, const vfs_resource_t *res)
{
    for (int i = 0; i < PERM_MAX_QUERIES; i++) {
        perm_query_t *q = &s_queries[i];
        if (!q->in_use)
            continue;
        if (q->state != PERM_QUERY_PENDING)
            continue;
        if (q->app_id_hash != app_id_hash)
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
            q->app_id_hash = app_id_hash;
            q->resource = *res;
            q->state = PERM_QUERY_PENDING;
            return q;
        }
    }
    return NULL;
}

/* ====================================================================
 * UI notification — push UI_SHOW to term("perm.ui")
 *
 * Best effort: if the UI port is not up yet, drop the notification
 * silently (the query stays PENDING and the shell can list it with
 * perm_query).  The port is cached on first success; a failed port_get
 * is retried on the next notification.
 * ==================================================================== */

static void notify_ui(u32 query_id, u32 app_id_hash, const char *url,
                      u32 access, i32 state)
{
    if (s_ui_port < 0) {
        s_ui_port = port_get(PERM_UI_PORT_NAME);
        if (s_ui_port < 0)
            return;
    }

    perm_req_ui_t *req = (perm_req_ui_t *)s_req;
    memset(req, 0, sizeof(*req));
    req->op = PERM_OP_UI_SHOW;
    req->query_id = query_id;
    req->app_id_hash = app_id_hash;
    strncpy(req->url, url, sizeof(req->url) - 1);
    req->access = access;
    req->state = state;

    perm_resp_ui_t resp;
    int resp_len = (int)sizeof(resp);
    (void)ipc_call(s_ui_port, req, (int)sizeof(*req), &resp, &resp_len);
}

/* ====================================================================
 * Operation handlers
 * ==================================================================== */

/* CHECK: vfs_server → synchronous authorization check. */
static void do_check(int token, int msg_len)
{
    perm_resp_check_t *resp = (perm_resp_check_t *)s_resp;
    if (msg_len < (int)sizeof(perm_req_check_t)) {
        resp->ret = ERR_INVAL;
        goto out;
    }
    perm_req_check_t *req = (perm_req_check_t *)s_req;

    perm_grant_t *g = grant_find(req->app_id_hash, &req->resource);
    if (g && (g->access & req->access) == req->access) {
        resp->ret = 0;                  /* granted — proceed */
        resp->query_id = 0;
        goto out;
    }

    /* Not granted → Powerbox.  Create/reuse a pending query and tell
     * the UI to show the prompt.  vfs_server translates this into
     * -EACCES for the client (design §8: 授权前 → -EACCES). */
    perm_query_t *q = query_alloc(req->app_id_hash, &req->resource);
    if (!q) {
        resp->ret = ERR_NOMEM;          /* query queue full */
        resp->query_id = 0;
        goto out;
    }
    strncpy(q->url, req->url, sizeof(q->url) - 1);
    q->url[sizeof(q->url) - 1] = '\0';
    q->access = req->access;

    resp->ret = VFS_ERR_ACCESS;
    resp->query_id = q->query_id;

    notify_ui(q->query_id, q->app_id_hash, q->url, q->access,
              PERM_QUERY_PENDING);

out:
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

/* ANSWER: user verdict → grant upsert (allow) or deny. */
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
        if (!grant_upsert(q->app_id_hash, &q->resource, q->access))
            resp->ret = ERR_NOMEM;
        else
            resp->ret = 0;
    } else {
        q->state = PERM_QUERY_DENIED;
        resp->ret = 0;
    }

    notify_ui(q->query_id, q->app_id_hash, q->url, q->access, q->state);

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
    resp->app_id_hash = q->app_id_hash;
    strncpy(resp->url, q->url, sizeof(resp->url) - 1);
    resp->url[sizeof(resp->url) - 1] = '\0';
    resp->access = q->access;
    resp->state = q->state;

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

    resp->revoked = grant_revoke(req->app_id_hash, &req->resource);
    resp->ret = 0;

out:
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

/* GRANT: direct grant, bypasses the Powerbox (tests/management). */
static void do_grant(int token, int msg_len)
{
    perm_resp_grant_t *resp = (perm_resp_grant_t *)s_resp;
    if (msg_len < (int)sizeof(perm_req_grant_t)) {
        resp->ret = ERR_INVAL;
        goto out;
    }
    perm_req_grant_t *req = (perm_req_grant_t *)s_req;

    resp->ret = grant_upsert(req->app_id_hash, &req->resource,
                             req->access) ? 0 : ERR_NOMEM;

out:
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

/* ====================================================================
 * Dispatch
 * ==================================================================== */

static void perm_handle_request(int token, u32 op, int msg_len)
{
    switch (op) {
    case PERM_OP_CHECK:   do_check(token, msg_len);   break;
    case PERM_OP_ANSWER:  do_answer(token, msg_len);  break;
    case PERM_OP_QUERY:   do_query(token, msg_len);   break;
    case PERM_OP_REVOKE:  do_revoke(token, msg_len);  break;
    case PERM_OP_GRANT:   do_grant(token, msg_len);   break;
    default: {
        i32 *resp = (i32 *)s_resp;
        *resp = ERR_INVAL;
        (void)ipc_reply(token, resp, (int)sizeof(i32));
        break;
    }
    }
}

/* ====================================================================
 * Entry point
 * ==================================================================== */

int main(void)
{
    printf("perm: starting permission manager\n");

    memset(s_grants, 0, sizeof(s_grants));
    memset(s_queries, 0, sizeof(s_queries));
    s_query_seq = (u32)get_time() & 0x7FFFFFFFu;

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

    printf("perm: serving (grant table %d, query queue %d)\n",
           PERM_MAX_GRANTS, PERM_MAX_QUERIES);

    for (;;) {
        int msg_len = (int)sizeof(s_req);
        int token = 0;
        ret = ipc_recv(port, s_req, &msg_len, &token);
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
        perm_handle_request(token, op, msg_len);
    }
}
