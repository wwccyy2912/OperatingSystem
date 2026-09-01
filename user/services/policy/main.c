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
 *
 * ------------------------------------------------------------------
 * Structure (PolicyMain):
 *   main() spawns PolicyServerMain -> "policy" port, ipc_recv loop
 *     role -> command -> verdict table (in-memory)
 *     POLICY_OP_SET admin maintenance (OWNER/ADMIN, via user service)
 *   capability (kernel) -> policy DB -> shell override (3 tiers)
 * How it works:
 *   A command not in a role's table is POLICY_UNSET = default allow.
 *   POLICY_OP_SET updates a verdict; admin identity comes from the
 *   caller's role queried through the user service by subject.
 * Purpose:
 *   Command policy service: the middle tier of the three-tier command
 *   access architecture, consulted by the shell at startup.
 * Caveats:
 *   Table is in-memory only (VFS persistence is future work).  Never
 *   touches environment variables — those are process-local preferences.
 * ------------------------------------------------------------------
 */

#include "policy.h"

#include <libc/stdio.h>
#include <libc/string.h>
#include <stdint.h>
#include <libos/syscalls.h>

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

static void SeedTable(void) {
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
static u8 PolicyLookup(u32 role, const char *cmd) {
    if (role >= POLICY_ROLES)
        return POLICY_UNSET;
    for (u32 i = 0; i < s_count[role]; i++) {
        if (strcmp(s_table[role][i].cmd, cmd) == 0)
            return s_table[role][i].verdict;
    }
    return POLICY_UNSET;
}

/* Admin identity: the CALLER (via ipc_recv_from, unforgeable) must
 * hold the SERVICE_MANAGE atom — the same management gate the perm
 * service uses for ROLE_SET.  The user service's OWNER/ADMIN role is
 * synced into capabilities at login, and the atom is seeded to the
 * trusted services at spawn; a caller that can set policy is by
 * definition management-plane.  (Querying the user service's WHOAMI
 * here would resolve the policy service's OWN subject, not the
 * caller's — wrong.  Atom check is direct and unforgeable.) */
static int CallerIsAdmin(u64 subject) {
    return CapHasAtom(subject, ATOM_SERVICE_MANAGE) == 1;
}

/* ------------------------------------------------------------------ */
/*  Handlers                                                          */
/* ------------------------------------------------------------------ */

static void DoQuery(int token, int msg_len, policy_req_query_t *req) {
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
        resp.verdicts[i]                 = PolicyLookup(req->role, req->cmds[i]);
    }
    resp.ret = 0;
out:
    (void)IpcReply(token, &resp, (int)sizeof(resp));
}

static void DoSet(int token, int msg_len, u64 caller, policy_req_set_t *req) {
    policy_resp_set_t resp;
    memset(&resp, 0, sizeof(resp));
    if (msg_len < (int)sizeof(policy_req_set_t)) {
        resp.ret = -2;
        goto out;
    }
    if (!CallerIsAdmin(caller)) {
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
    (void)IpcReply(token, &resp, (int)sizeof(resp));
}

static void DoDump(int token, u64 caller, policy_req_dump_t *req) {
    (void)req;
    policy_resp_dump_t resp;
    memset(&resp, 0, sizeof(resp));
    if (!CallerIsAdmin(caller)) {
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
    (void)IpcReply(token, &resp, (int)sizeof(resp));
}

/* ------------------------------------------------------------------ */
/*  Server                                                            */
/* ------------------------------------------------------------------ */

static void PolicyServerMain(void *arg) {
    (void)arg;

    int port = IpcPortCreate();
    if (port < 0) {
        printf("policy: ipc_port_create failed (%d)\n", port);
        ThreadExit(1);
    }
    int ret = PortRegister(POLICY_PORT_NAME, port);
    if (ret < 0) {
        printf("policy: PortRegister('%s') failed (%d)\n", POLICY_PORT_NAME, ret);
        ThreadExit(1);
    }
    printf("policy: port %d registered as '%s'\n", port, POLICY_PORT_NAME);

    /* Largest request is the QUERY struct (op + role + count + cmds). */
    u8 s_req[sizeof(policy_req_query_t)];

    for (;;) {
        int msg_len = (int)sizeof(s_req);
        int token   = 0;
        u64 caller  = 0;
        int r       = IpcRecvFrom(port, s_req, &msg_len, &token, &caller);
        if (r < 0) {
            printf("policy: ipc_recv failed (%d)\n", r);
            continue;
        }
        u32 op = ((policy_req_query_t *)s_req)->op;
        switch (op) {
        case POLICY_OP_QUERY:
            DoQuery(token, msg_len, (policy_req_query_t *)s_req);
            break;
        case POLICY_OP_SET:
            DoSet(token, msg_len, caller, (policy_req_set_t *)s_req);
            break;
        case POLICY_OP_DUMP:
            DoDump(token, caller, (policy_req_dump_t *)s_req);
            break;
        default:
        {
            i32 err = -2;
            (void)IpcReply(token, &err, (int)sizeof(err));
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
    SeedTable();
    printf("policy: seeded %u entries\n",
           (u32)(sizeof(s_seeds) / sizeof(s_seeds[0])));

    if (ThreadCreate(PolicyServerMain, NULL, 10) < 0) {
        printf("policy: server thread_create failed\n");
        return 1;
    }

    for (;;)
        ThreadYield();
    return 0;
}
