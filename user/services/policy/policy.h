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
 * policy.h - Command policy service protocol (v0.5)
 * Copyright (c) 2026 OpSys Project
 *
 * The policy service implements the MIDDLE layer of the three-tier
 * command-access architecture (Capability -> Policy DB -> Shell
 * override; see docs/permission_model.md §九·五):
 *
 *   - Capability (kernel): a process may only EXECUTE a command it has
 *     the capability for — hard limit, unforgeable.
 *   - Policy DB (this service): decides which commands a given ROLE
 *     may or may not use, by name.  Admin-maintained, per-role.
 *   - Shell override: the shell queries this service at startup and
 *     filters its command table; a hardcoded rescue list keeps the
 *     shell usable if the policy service is down.
 *
 * The policy service is deliberately small: a role -> allow/deny
 * command-name table.  It does NOT evaluate capabilities (that stays
 * in the kernel/perm) and it does NOT use environment variables for
 * anything (env vars carry only user preferences).
 *
 * Port name: "policy"
 */

#ifndef POLICY_H
#define POLICY_H

#include <stdint.h>

#define POLICY_PORT_NAME "policy"

/* Max command names a query response can carry. */
#define POLICY_MAX_CMDS 64
#define POLICY_CMD_MAX  32 /* per command name, incl. NUL */

/* Ops */
#define POLICY_OP_QUERY 1 /* shell -> policy: fetch role's command policy */
#define POLICY_OP_SET   2 /* admin  -> policy: update a role's policy     */
#define POLICY_OP_DUMP  3 /* admin  -> policy: export the whole table     */

/* Verdict for a single command. */
enum {
    POLICY_UNSET  = 0, /* not in this role's table -> default allow */
    POLICY_ALLOW  = 1,
    POLICY_DENY   = 2,
};

/* QUERY: role + list of command names the shell cares about; the
 * service answers with a verdict per command (compressed into one
 * byte each).  The shell's registry is small (~40), so one round trip
 * covers everything. */
typedef struct {
    uint32_t op;          /* = POLICY_OP_QUERY */
    uint32_t role;        /* PERM_ROLE_* of the caller (from user svc) */
    uint32_t count;       /* number of names in cmds[] */
    char     cmds[POLICY_MAX_CMDS][POLICY_CMD_MAX];
} policy_req_query_t;

typedef struct {
    int32_t  ret;
    uint32_t count;                       /* verdicts returned */
    uint8_t  verdicts[POLICY_MAX_CMDS];   /* POLICY_* per cmds[i] */
} policy_resp_query_t;

/* SET: admin updates one role's command verdict. */
typedef struct {
    uint32_t op;      /* = POLICY_OP_SET */
    uint32_t role;    /* PERM_ROLE_* */
    char     cmd[POLICY_CMD_MAX];
    uint8_t  verdict; /* POLICY_ALLOW / POLICY_DENY / POLICY_UNSET */
} policy_req_set_t;

typedef struct {
    int32_t ret;
} policy_resp_set_t;

/* DUMP: export the full table ("role cmd verdict" lines). */
typedef struct {
    uint32_t op; /* = POLICY_OP_DUMP */
} policy_req_dump_t;

typedef struct {
    int32_t  ret;
    uint32_t count;
    char     lines[POLICY_MAX_CMDS][48];
} policy_resp_dump_t;

/* Compile-time guard: every message fits the 4096-byte IPC limit. */
#define POLICY_IPC_MAX 4096

#endif /* POLICY_H */
