/*
 * user.h - User account service protocol
 * Copyright (c) 2026 OpSys Project
 *
 * The user service owns the ACCOUNT layer: usernames, password hashes
 * and roles.  Permissions stay in the perm-manager (ABAC engine); a
 * successful login BINDS the caller's kernel-issued subject to an
 * account and syncs the account role into perm (ROLE_SET), so the
 * existing authorization model applies unchanged.
 *
 * Security notes:
 *   - Password hashes are FNV-1a-64 + per-account salt (no crypto
 *     library in-tree).  This is an integrity check, NOT production
 *     password storage — documented limitation.
 *   - Account management ops (USERADD/USERDEL/PASSWD-other/STOP/HALT)
 *     require the caller's bound account role to be OWNER/ADMIN.
 *   - The caller identity always comes from ipc_recv_from (kernel-
 *     filled subject), never from request bytes.
 *
 * Port name: "user"
 */

#ifndef USER_H
#define USER_H

#include <stdint.h>

#define USER_NAME_MAX   32
#define USER_PW_MAX     64
#define USER_MAX_ACCOUNTS 16
#define USER_PORT_NAME  "user"

/* Ops */
enum {
    USER_OP_LOGIN     = 1, /* bind caller subject to account + sync role */
    USER_OP_LOGOUT    = 2, /* unbind caller subject */
    USER_OP_PASSWD    = 3, /* change password (self, or other if admin) */
    USER_OP_USERADD   = 4, /* create account (admin) */
    USER_OP_USERDEL   = 5, /* delete account (admin; never self/last-admin) */
    USER_OP_USERS     = 6, /* list accounts (admin) */
    USER_OP_WHOAMI    = 7, /* caller's bound account + role */
    USER_OP_VERIFY    = 8, /* verify name+password (used by exit guard) */
    USER_OP_STOP      = 9, /* stop a user process (admin + verified) */
    USER_OP_LOCK      = 10, /* disable an account (admin; never self/last-admin) */
    USER_OP_UNLOCK    = 11, /* re-enable an account (admin) */
    USER_OP_POLICY_SET = 12, /* admin proxy: hot-update command policy */
    USER_OP_POLICY_DUMP = 13, /* admin proxy: dump command policy table */
    USER_OP_KILL      = 14, /* admin proxy: kill a process by PID */
};

/* Account lockout policy: failed logins before auto-lock. */
#define USER_MAX_LOGIN_ATTEMPTS 5

typedef struct {
    uint32_t op;
    char     name[USER_NAME_MAX];
    char     password[USER_PW_MAX];
    uint32_t role; /* USERADD: target role; PASSWD: unused */
} user_req_login_t; /* also USERADD / VERIFY */

typedef struct {
    int32_t ret;
    uint32_t role;      /* LOGIN/WHOAMI/VERIFY: account role */
    char     name[USER_NAME_MAX]; /* WHOAMI */
    char     reason[64]; /* USERS: account lines; STOP: error detail */
    uint32_t count;     /* USERS: number of account lines in reason */
} user_resp_login_t;

typedef struct {
    uint32_t op;
    char     old_password[USER_PW_MAX];
    char     new_password[USER_PW_MAX];
    char     name[USER_NAME_MAX]; /* PASSWD: target (self if empty) */
} user_req_passwd_t;

typedef struct {
    int32_t ret;
} user_resp_passwd_t;

typedef struct {
    uint32_t op;
    char     svc[USER_NAME_MAX]; /* STOP: service/process name to stop */
} user_req_stop_t;

typedef struct {
    int32_t ret;
    char     detail[64];
} user_resp_stop_t;

/* POLICY_SET/DUMP: the user service proxies command-policy updates to
 * the policy service.  The user service holds ATOM_SERVICE_MANAGE and
 * can resolve the CALLER's account role (OWNER/ADMIN), so it is the
 * trusted management proxy for policy mutation — the shell does not
 * hold the management atom and must not mutate policy directly. */
typedef struct {
    uint32_t op;      /* USER_OP_POLICY_SET */
    uint32_t role;    /* PERM_ROLE_* target role */
    uint32_t verdict; /* POLICY_ALLOW / POLICY_DENY / POLICY_UNSET */
    char     cmd[32]; /* command name */
} user_req_policy_t;

typedef struct {
    int32_t  ret;
    uint32_t count; /* DUMP: number of policy lines */
    char     lines[64][48]; /* DUMP: "ROLE cmd verdict" */
} user_resp_policy_t;

/* KILL: admin proxy to SIGKILL a process by PID.  The shell cannot
 * pass the kernel's kill gate (no ATOM_SERVICE_MANAGE); the user
 * service holds it and re-checks the caller is OWNER/ADMIN. */
typedef struct {
    uint32_t op;   /* USER_OP_KILL */
    int32_t  pid;  /* target PID */
} user_req_kill_t;

typedef struct {
    int32_t ret;
    char    detail[64];
} user_resp_kill_t;

/* Compile-time guard: every message fits the 4096-byte IPC limit. */
#define USER_IPC_MAX 4096

#endif /* USER_H */
