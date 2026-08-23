/*
 * policy.c - Command policy service (v0.5)
 * Copyright (c) 2026 OpSys Project
 *
 * Middle tier of the three-tier command-access architecture:
 *
 *   Capability (kernel)  ->  Policy DB (this service)  ->  Shell override
 *
 * The service keeps a role -> {command -> verdict} table.  Roles are
 * the perm-manager's PERM_ROLE_* values (OWNER/ADMIN/STANDARD/CHILD/
 * GUEST/AUDITOR).  A command not present in a role's table is
 * POLICY_UNSET = default allow (the capability layer still gates what
 * the process can actually do).
 *
 * Admin maintenance: POLICY_OP_SET lets an OWNER/ADMIN caller update a
 * role's verdict (admin identity comes from the caller's role, queried
 * through the user service by subject — the user service is the only
 * authority on account roles).  The table is in-memory (persistence
 * via the VFS policy file is future work; documented).
 *
 * The service never touches environment variables: env vars are
 * process-local user preferences only (PS1/EDITOR/LANG), never policy.
 *
 * Spawned by the manager before the shell; the shell queries it at
 * startup to build its command filter.
 */

#include "policy.h"

#include <libc/stdio.h>
#include <libc/string.h>
#include <stdint.h>
#include <libos/syscalls.h>
#include "../user/user.h"  /* USER_OP_WHOAMI + user protocol */

typedef uint8_t  u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t  i32;

/* Table dimensions: roles (perm.h) x command entries. */
#define POLICY_ROLES 6 /* OWNER..AUDITOR */

typedef struct {
    char cmd[POLICY_CMD_MAX];
    u8   verdict; /* POLICY_* */
} policy_entry_t;

static policy_entry_t s_table[POLICY_ROLES][POLICY_MAX_CMDS];
static u32            s_count[POLICY_ROLES];

/* Default seeds: sensible per-role command policy.  The capability
 * layer remains the hard gate; this list is the "policy" decision. */
static const struct {
    u32         role;
    const char *cmd;
    u8          verdict;
} s_seeds[] = {
    /* GUEST: bare minimum — no process/account/system commands. */
    {4, "exec", POLICY_DENY},
    {4, "kill", POLICY_DENY},
    {4, "stop", POLICY_DENY},
    {4, "useradd", POLICY_DENY},
    {4, "userdel", POLICY_DENY},
    {4, "passwd", POLICY_DENY},
    {4, "users", POLICY_DENY},
    {4, "perm_answer", POLICY_DENY},
    {4, "perm_revoke", POLICY_DENY},
    {4, "reboot", POLICY_DENY},
    /* CHILD: same restrictions as GUEST plus no package install. */
    {3, "exec", POLICY_DENY},
    {3, "kill", POLICY_DENY},
    {3, "stop", POLICY_DENY},
    {3, "useradd", POLICY_DENY},
    {3, "userdel", POLICY_DENY},
    {3, "users", POLICY_DENY},
    {3, "pkg", POLICY_DENY},
    {3, "reboot", POLICY_DENY},
    /* STANDARD: deny nothing extra (default allow covers the rest);
     * sensitive ops are still capability-gated. */
};

static void seed_table(void) {
    memset(s_table, 0, sizeof(s_table));
    memset(s_count, 0, sizeof(s_count));
    for (u32 i = 0; i < sizeof(s_seeds) / sizeof(s_seeds[0]); i++) {
        u32 role = s_seeds[i].role;
        if (role >= POLICY_ROLES || s_count[role] >= POLICY_MAX_CMDS)
            continue;
        policy_entry_t *e = &s_table[role][s_count[role]++];
        strncpy(e->cmd, s_seeds[i].cmd, sizeof(e->cmd) - 1);
        e->cmd[sizeof(e->cmd) - 1] = '\0';
        e->verdict = s_seeds[i].verdict;
    }
}

/* Verdict for (role, cmd); POLICY_UNSET when not in the table. */
static u8 policy_lookup(u32 role, const char *cmd) {
    if (role >= POLICY_ROLES)
        return POLICY_UNSET;
    for (u32 i = 0; i < s_count[role]; i++) {
        if (strcmp(s_table[role][i].cmd, cmd) == 0)
            return s_table[role][i].verdict;
    }
    return POLICY_UNSET;
}

/* Admin identity: the user service resolves the CALLER's subject to an
 * account role (WHOAMI keys on the caller server-side).  Returns 1 when
 * OWNER/ADMIN. */
static int caller_is_admin(void) {
    int port = port_get(USER_PORT_NAME);
    if (port < 0)
        return 0;
    user_req_login_t req;
    memset(&req, 0, sizeof(req));
    req.op = USER_OP_WHOAMI;
    user_resp_login_t resp;
    memset(&resp, 0, sizeof(resp));
    int rlen = (int)sizeof(resp);
    if (ipc_call(port, &req, (int)sizeof(req), &resp, &rlen) < 0)
        return 0;
    /* WHOAMI keys on the CALLER's subject server-side, so this is the
     * subject's own role. */
    if (resp.ret < 0)
        return 0;
    return (resp.role == 1 /* ADMIN */ || resp.role == 0 /* OWNER */) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/*  Handlers                                                          */
/* ------------------------------------------------------------------ */

static void do_query(int token, int msg_len, policy_req_query_t *req) {
    policy_resp_query_t resp;
    memset(&resp, 0, sizeof(resp));
    if (msg_len < (int)sizeof(policy_req_query_t)) {
        resp.ret = -2; /* ERR_INVAL */
        goto out;
    }
    if (req->count > POLICY_MAX_CMDS) {
        resp.ret = -2;
        goto out;
    }
    resp.count = req->count;
    for (u32 i = 0; i < req->count; i++) {
        req->cmds[i][POLICY_CMD_MAX - 1] = '\0';
        resp.verdicts[i]                 = policy_lookup(req->role, req->cmds[i]);
    }
    resp.ret = 0;
out:
    (void)ipc_reply(token, &resp, (int)sizeof(resp));
}

static void do_set(int token, int msg_len, policy_req_set_t *req) {
    policy_resp_set_t resp;
    memset(&resp, 0, sizeof(resp));
    if (msg_len < (int)sizeof(policy_req_set_t)) {
        resp.ret = -2;
        goto out;
    }
    if (!caller_is_admin()) {
        resp.ret = -9; /* ERR_DENIED: admin only */
        goto out;
    }
    if (req->role >= POLICY_ROLES || req->cmd[0] == '\0' ||
        req->verdict > POLICY_DENY) {
        resp.ret = -2;
        goto out;
    }
    req->cmd[POLICY_CMD_MAX - 1] = '\0';

    if (req->verdict == POLICY_UNSET) {
        /* Remove the entry. */
        for (u32 i = 0; i < s_count[req->role]; i++) {
            if (strcmp(s_table[req->role][i].cmd, req->cmd) == 0) {
                for (u32 j = i; j + 1 < s_count[req->role]; j++)
                    s_table[req->role][j] = s_table[req->role][j + 1];
                s_count[req->role]--;
                break;
            }
        }
        resp.ret = 0;
        goto out;
    }

    /* Upsert. */
    for (u32 i = 0; i < s_count[req->role]; i++) {
        if (strcmp(s_table[req->role][i].cmd, req->cmd) == 0) {
            s_table[req->role][i].verdict = req->verdict;
            resp.ret                     = 0;
            goto out;
        }
    }
    if (s_count[req->role] >= POLICY_MAX_CMDS) {
        resp.ret = -1; /* ERR_NOMEM: table full */
        goto out;
    }
    policy_entry_t *e = &s_table[req->role][s_count[req->role]++];
    strncpy(e->cmd, req->cmd, sizeof(e->cmd) - 1);
    e->cmd[sizeof(e->cmd) - 1] = '\0';
    e->verdict                 = req->verdict;
    resp.ret                   = 0;
out:
    (void)ipc_reply(token, &resp, (int)sizeof(resp));
}

static void do_dump(int token, policy_req_dump_t *req) {
    (void)req;
    policy_resp_dump_t resp;
    memset(&resp, 0, sizeof(resp));
    if (!caller_is_admin()) {
        resp.ret = -9;
        goto out;
    }
    static const char *const role_names[POLICY_ROLES] = {
        "OWNER", "ADMIN", "STANDARD", "CHILD", "GUEST", "AUDITOR"};
    u32 n = 0;
    for (u32 r = 0; r < POLICY_ROLES && n < POLICY_MAX_CMDS; r++) {
        for (u32 i = 0; i < s_count[r] && n < POLICY_MAX_CMDS; i++) {
            snprintf(resp.lines[n], sizeof(resp.lines[0]), "%s %s %u",
                     role_names[r], s_table[r][i].cmd, s_table[r][i].verdict);
            n++;
        }
    }
    resp.count = n;
    resp.ret   = 0;
out:
    (void)ipc_reply(token, &resp, (int)sizeof(resp));
}

/* ------------------------------------------------------------------ */
/*  Server                                                            */
/* ------------------------------------------------------------------ */

static void policy_server_main(void *arg) {
    (void)arg;

    int port = ipc_port_create();
    if (port < 0) {
        printf("policy: ipc_port_create failed (%d)\n", port);
        thread_exit(1);
    }
    int ret = port_register(POLICY_PORT_NAME, port);
    if (ret < 0) {
        printf("policy: port_register('%s') failed (%d)\n", POLICY_PORT_NAME, ret);
        thread_exit(1);
    }
    printf("policy: port %d registered as '%s'\n", port, POLICY_PORT_NAME);

    /* Largest request is the QUERY struct (op + role + count + cmds). */
    u8 s_req[sizeof(policy_req_query_t)];

    for (;;) {
        int msg_len = (int)sizeof(s_req);
        int token   = 0;
        u64 caller  = 0;
        int r       = ipc_recv_from(port, s_req, &msg_len, &token, &caller);
        if (r < 0) {
            printf("policy: ipc_recv failed (%d)\n", r);
            continue;
        }
        u32 op = ((policy_req_query_t *)s_req)->op;
        switch (op) {
        case POLICY_OP_QUERY:
            do_query(token, msg_len, (policy_req_query_t *)s_req);
            break;
        case POLICY_OP_SET:
            do_set(token, msg_len, (policy_req_set_t *)s_req);
            break;
        case POLICY_OP_DUMP:
            do_dump(token, (policy_req_dump_t *)s_req);
            break;
        default:
        {
            i32 err = -2;
            (void)ipc_reply(token, &err, (int)sizeof(err));
            break;
        }
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Entry                                                             */
/* ------------------------------------------------------------------ */

int main(void) {
    printf("policy: starting command policy service\n");
    seed_table();
    printf("policy: seeded %u entries\n",
           (u32)(sizeof(s_seeds) / sizeof(s_seeds[0])));

    if (thread_create(policy_server_main, NULL, 10) < 0) {
        printf("policy: server thread_create failed\n");
        return 1;
    }

    for (;;)
        thread_yield();
    return 0;
}
