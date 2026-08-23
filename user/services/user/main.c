/*
 * user.c - User account service (independent process)
 * Copyright (c) 2026 OpSys Project
 *
 * Owns the account layer: usernames, password hashes, roles and the
 * subject→account binding used by the exit guard.  See user.h.
 *
 * Bootstrap: on first start (no accounts) a default "admin" account is
 * created with role OWNER and password "admin".  CHANGE IT with
 * `passwd` after login — the default is printed to the serial log.
 *
 * Role sync: LOGIN sets the caller's perm role to the account role via
 * PERM_OP_ROLE_SET (the user service holds ATOM_SERVICE_MANAGE via
 * blob-identity seeding, so the management-plane gate passes).
 *
 * Exit guard: USER_OP_STOP stops a user-level process (pkg/device_mgr/
 * shell/demos...) after the shell has shown a TUI confirm dialog and
 * verified an OWNER/ADMIN password (USER_OP_VERIFY).  System-critical
 * services (serial/term/keyboard/vfs/fs drivers/perm/manager) are
 * refused.  The kill itself is executed here because the shell is NOT
 * management-plane (SYS_KILL is atom-gated).
 *
 * Password hashing: FNV-1a-64 + per-account salt.  Integrity-only —
 * documented limitation (no crypto library in-tree).
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <libos/syscalls.h>
#include "../lib/libc/stdio.h"
#include "../perm/perm.h"
#include "../vfs/vfs.h" /* VFS_ERR_EXISTS */
#include "../policy/policy.h" /* command policy proxy (v0.5) */
#include "user.h"

/* Request/response buffers (single-threaded service) */
static u8 s_req[USER_IPC_MAX];
static u8 s_resp[USER_IPC_MAX];

/* ------------------------------------------------------------------ */
/*  Account table                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    int      in_use;
    char     name[USER_NAME_MAX];
    uint64_t salt;    /* per-account random salt */
    uint64_t pw_hash; /* FNV-1a(name? no: password) with salt */
    uint32_t role;    /* PERM_ROLE_* */
    int      disabled;   /* 1 = account locked (admin or auto-lockout) */
    int      fail_count; /* consecutive failed logins (lockout counter) */
} user_acct_t;

typedef struct {
    uint64_t subject; /* kernel-issued subject of the logged-in process */
    int      acct;    /* index into s_accts */
} user_bind_t;

static user_acct_t s_accts[USER_MAX_ACCOUNTS];
static user_bind_t s_binds[USER_MAX_ACCOUNTS];

/* ------------------------------------------------------------------ */
/*  Password hashing (FNV-1a-64 + salt) — integrity only              */
/* ------------------------------------------------------------------ */

static uint64_t fnv1a64(const char *s) {
    uint64_t h = 0xcbf29ce484222325ULL;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 0x100000001b3ULL;
    }
    return h;
}

static uint64_t pw_hash(const char *pw, uint64_t salt) {
    return fnv1a64(pw) ^ (salt * 0x9E3779B97F4A7C15ULL);
}

/* Not cryptographically random, but enough to decorrelate salts. */
static uint64_t salt_gen(void) {
    uint64_t t = (uint64_t)get_time();
    uint64_t p = (uint64_t)get_pid();
    return t ^ (p << 32) ^ 0xA5A5A5A5A5A5A5A5ULL;
}

/* ------------------------------------------------------------------ */
/*  Account helpers                                                   */
/* ------------------------------------------------------------------ */

static user_acct_t *acct_find(const char *name) {
    for (int i = 0; i < USER_MAX_ACCOUNTS; i++) {
        if (s_accts[i].in_use && strcmp(s_accts[i].name, name) == 0)
            return &s_accts[i];
    }
    return NULL;
}

static int acct_verify(const user_acct_t *a, const char *pw) {
    return a && pw_hash(pw, a->salt) == a->pw_hash;
}

static int acct_count(void) {
    int n = 0;
    for (int i = 0; i < USER_MAX_ACCOUNTS; i++)
        if (s_accts[i].in_use)
            n++;
    return n;
}

static int acct_admin_count(void) {
    int n = 0;
    for (int i = 0; i < USER_MAX_ACCOUNTS; i++)
        if (s_accts[i].in_use &&
            (s_accts[i].role == PERM_ROLE_OWNER || s_accts[i].role == PERM_ROLE_ADMIN))
            n++;
    return n;
}

/* Admin+: OWNER or ADMIN account role. */
static int acct_is_admin(const user_acct_t *a) {
    return a && (a->role == PERM_ROLE_OWNER || a->role == PERM_ROLE_ADMIN);
}

/* Bound account of a subject (or NULL). */
static user_acct_t *acct_of_subject(uint64_t subject) {
    for (int i = 0; i < USER_MAX_ACCOUNTS; i++) {
        if (s_binds[i].subject == subject && s_binds[i].acct >= 0)
            return &s_accts[s_binds[i].acct];
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Role sync into perm (PERM_OP_ROLE_SET)                            */
/* ------------------------------------------------------------------ */

static int perm_role_set(uint64_t subject, uint32_t role) {
    int port = port_get(PERM_PORT_NAME);
    if (port < 0)
        return port;
    perm_req_role_set_t req;
    memset(&req, 0, sizeof(req));
    req.op         = PERM_OP_ROLE_SET;
    req.subject_id = subject;
    req.role       = role;
    perm_resp_role_set_t resp;
    memset(&resp, 0, sizeof(resp));
    int rlen = (int)sizeof(resp);
    if (ipc_call(port, &req, (int)sizeof(req), &resp, &rlen) < 0)
        return ERR_NOCAP;
    return resp.ret;
}

/* ------------------------------------------------------------------ */
/*  Handlers                                                          */
/* ------------------------------------------------------------------ */

static void do_login(int token, int msg_len, uint64_t caller) {
    user_resp_login_t *resp = (user_resp_login_t *)s_resp;
    memset(resp, 0, sizeof(*resp));
    resp->ret = ERR_INVAL;
    if (msg_len < (int)sizeof(user_req_login_t))
        goto out;
    user_req_login_t *req = (user_req_login_t *)s_req;
    req->name[USER_NAME_MAX - 1] = '\0';
    req->password[USER_PW_MAX - 1] = '\0';

    user_acct_t *a = acct_find(req->name);
    if (!a) {
        resp->ret = ERR_DENIED; /* bad name */
        goto out;
    }
    if (a->disabled) {
        resp->ret = ERR_DENIED; /* account locked */
        printf("user: login '%s' rejected (account disabled)\n", a->name);
        goto out;
    }
    if (!acct_verify(a, req->password)) {
        /* Lockout: N consecutive failures disables the account. */
        a->fail_count++;
        if (a->fail_count >= USER_MAX_LOGIN_ATTEMPTS) {
            a->disabled = 1;
            printf("user: account '%s' auto-locked after %d failed logins\n",
                   a->name, a->fail_count);
        } else {
            printf("user: bad password for '%s' (%d/%d)\n",
                   a->name, a->fail_count, USER_MAX_LOGIN_ATTEMPTS);
        }
        resp->ret = ERR_DENIED; /* bad password */
        goto out;
    }
    a->fail_count = 0; /* successful login resets the counter */

    /* (Re)bind: replace any existing binding for this subject. */
    for (int i = 0; i < USER_MAX_ACCOUNTS; i++) {
        if (s_binds[i].subject == caller) {
            s_binds[i].acct = (int)(a - s_accts);
            break;
        }
    }
    /* fresh slot if not bound yet */
    int bound = 0;
    for (int i = 0; i < USER_MAX_ACCOUNTS; i++) {
        if (s_binds[i].subject == caller) {
            bound = 1;
            break;
        }
    }
    if (!bound) {
        for (int i = 0; i < USER_MAX_ACCOUNTS; i++) {
            if (s_binds[i].subject == 0) {
                s_binds[i].subject = caller;
                s_binds[i].acct    = (int)(a - s_accts);
                break;
            }
        }
    }

    /* Sync the account role into the permission engine. */
    int r = perm_role_set(caller, a->role);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }
    resp->role = a->role;
    strncpy(resp->name, a->name, sizeof(resp->name) - 1);
    resp->name[sizeof(resp->name) - 1] = '\0';
    resp->ret = 0;
    printf("user: login '%s' (role=%u) subject=%llu\n",
           a->name,
           a->role,
           (unsigned long long)caller);
out:
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_logout(int token, int msg_len, uint64_t caller) {
    user_resp_login_t *resp = (user_resp_login_t *)s_resp;
    memset(resp, 0, sizeof(*resp));
    (void)msg_len;
    user_acct_t *a = acct_of_subject(caller);
    if (!a) {
        resp->ret = ERR_NOENT;
        goto out;
    }
    for (int i = 0; i < USER_MAX_ACCOUNTS; i++) {
        if (s_binds[i].subject == caller) {
            s_binds[i].subject = 0;
            s_binds[i].acct    = -1;
            break;
        }
    }
    /* Fall back to the default role. */
    (void)perm_role_set(caller, PERM_ROLE_DEFAULT);
    resp->ret = 0;
out:
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_whoami(int token, int msg_len, uint64_t caller) {
    user_resp_login_t *resp = (user_resp_login_t *)s_resp;
    memset(resp, 0, sizeof(*resp));
    (void)msg_len;
    user_acct_t *a = acct_of_subject(caller);
    if (!a) {
        resp->ret = ERR_NOENT;
        goto out;
    }
    resp->role = a->role;
    strncpy(resp->name, a->name, sizeof(resp->name) - 1);
    resp->name[sizeof(resp->name) - 1] = '\0';
    resp->ret = 0;
out:
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

/* Verify a name+password pair WITHOUT binding (exit guard pre-check).
 * The shell shows the TUI prompt, then calls this before stopping. */
static void do_verify(int token, int msg_len) {
    user_resp_login_t *resp = (user_resp_login_t *)s_resp;
    memset(resp, 0, sizeof(*resp));
    resp->ret = ERR_INVAL;
    if (msg_len < (int)sizeof(user_req_login_t))
        goto out;
    user_req_login_t *req = (user_req_login_t *)s_req;
    req->name[USER_NAME_MAX - 1]     = '\0';
    req->password[USER_PW_MAX - 1]   = '\0';
    user_acct_t *a = acct_find(req->name);
    if (!a || !acct_verify(a, req->password)) {
        resp->ret = ERR_DENIED;
        goto out;
    }
    resp->role = a->role;
    resp->ret  = 0;
out:
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_passwd(int token, int msg_len, uint64_t caller) {
    user_resp_passwd_t *resp = (user_resp_passwd_t *)s_resp;
    resp->ret = ERR_INVAL;
    if (msg_len < (int)sizeof(user_req_passwd_t))
        goto out;
    user_req_passwd_t *req = (user_req_passwd_t *)s_req;
    req->old_password[USER_PW_MAX - 1] = '\0';
    req->new_password[USER_PW_MAX - 1] = '\0';
    req->name[USER_NAME_MAX - 1]       = '\0';

    user_acct_t *me = acct_of_subject(caller);
    if (!me) {
        resp->ret = ERR_DENIED; /* must be logged in */
        goto out;
    }

    user_acct_t *target = NULL;
    if (req->name[0] == '\0') {
        target = me; /* change own password */
    } else {
        target = acct_find(req->name);
        if (!target) {
            resp->ret = ERR_NOENT;
            goto out;
        }
        if (target != me && !acct_is_admin(me)) {
            resp->ret = ERR_DENIED; /* only admin may change others */
            goto out;
        }
        if (target != me && !acct_verify(me, req->old_password)) {
            resp->ret = ERR_DENIED; /* admin re-auth for changing others */
            goto out;
        }
    }

    if (target == me && !acct_verify(me, req->old_password)) {
        resp->ret = ERR_DENIED; /* wrong current password */
        goto out;
    }
    if (strlen(req->new_password) < 4) {
        resp->ret = ERR_INVAL; /* too short */
        goto out;
    }

    target->salt    = salt_gen();
    target->pw_hash = pw_hash(req->new_password, target->salt);
    resp->ret       = 0;
    printf("user: password changed for '%s'\n", target->name);
out:
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_useradd(int token, int msg_len, uint64_t caller) {
    user_resp_login_t *resp = (user_resp_login_t *)s_resp;
    memset(resp, 0, sizeof(*resp));
    resp->ret = ERR_INVAL;
    if (msg_len < (int)sizeof(user_req_login_t))
        goto out;
    user_req_login_t *req = (user_req_login_t *)s_req;
    req->name[USER_NAME_MAX - 1]   = '\0';
    req->password[USER_PW_MAX - 1] = '\0';

    user_acct_t *me = acct_of_subject(caller);
    if (!acct_is_admin(me)) {
        resp->ret = ERR_DENIED; /* admin only */
        goto out;
    }
    if (req->name[0] == '\0' || strlen(req->name) > USER_NAME_MAX - 1 ||
        strlen(req->password) < 4) {
        resp->ret = ERR_INVAL;
        goto out;
    }
    if (acct_find(req->name)) {
        resp->ret = VFS_ERR_EXISTS; /* name taken */
        goto out;
    }
    if (req->role >= PERM_ROLE_MAX) {
        resp->ret = ERR_INVAL;
        goto out;
    }
    if (acct_count() >= USER_MAX_ACCOUNTS) {
        resp->ret = ERR_NOMEM;
        goto out;
    }
    for (int i = 0; i < USER_MAX_ACCOUNTS; i++) {
        if (!s_accts[i].in_use) {
            user_acct_t *a   = &s_accts[i];
            a->in_use        = 1;
            strncpy(a->name, req->name, sizeof(a->name) - 1);
            a->name[sizeof(a->name) - 1] = '\0';
            a->salt          = salt_gen();
            a->pw_hash       = pw_hash(req->password, a->salt);
            a->role          = req->role;
            resp->ret        = 0;
            printf("user: account '%s' created (role=%u)\n", a->name, a->role);
            goto out;
        }
    }
    resp->ret = ERR_NOMEM;
out:
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_userdel(int token, int msg_len, uint64_t caller) {
    user_resp_login_t *resp = (user_resp_login_t *)s_resp;
    memset(resp, 0, sizeof(*resp));
    resp->ret = ERR_INVAL;
    if (msg_len < (int)sizeof(user_req_login_t))
        goto out;
    user_req_login_t *req = (user_req_login_t *)s_req;
    req->name[USER_NAME_MAX - 1] = '\0';

    user_acct_t *me = acct_of_subject(caller);
    if (!acct_is_admin(me)) {
        resp->ret = ERR_DENIED;
        goto out;
    }
    user_acct_t *target = acct_find(req->name);
    if (!target) {
        resp->ret = ERR_NOENT;
        goto out;
    }
    if (target == me) {
        resp->ret = ERR_DENIED; /* cannot delete yourself */
        goto out;
    }
    if (acct_is_admin(target) && acct_admin_count() <= 1) {
        resp->ret = ERR_DENIED; /* cannot delete the last admin */
        goto out;
    }
    /* Unbind any subject bound to this account. */
    for (int i = 0; i < USER_MAX_ACCOUNTS; i++) {
        if (s_binds[i].acct == (int)(target - s_accts)) {
            s_binds[i].subject = 0;
            s_binds[i].acct    = -1;
        }
    }
    memset(target, 0, sizeof(*target));
    resp->ret = 0;
    printf("user: account '%s' deleted\n", req->name);
out:
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

/* LOCK/UNLOCK: admin disables or re-enables an account.  Cannot lock
 * yourself or the last admin (same guard as userdel). */
static void do_lock(int token, int msg_len, uint64_t caller, int lock) {
    user_resp_login_t *resp = (user_resp_login_t *)s_resp;
    memset(resp, 0, sizeof(*resp));
    resp->ret = ERR_INVAL;
    if (msg_len < (int)sizeof(user_req_login_t))
        goto out;
    user_req_login_t *req = (user_req_login_t *)s_req;
    req->name[USER_NAME_MAX - 1] = '\0';

    user_acct_t *me = acct_of_subject(caller);
    if (!acct_is_admin(me)) {
        resp->ret = ERR_DENIED;
        goto out;
    }
    user_acct_t *target = acct_find(req->name);
    if (!target) {
        resp->ret = ERR_NOENT;
        goto out;
    }
    if (lock) {
        if (target == me) {
            resp->ret = ERR_DENIED; /* cannot lock yourself */
            goto out;
        }
        if (acct_is_admin(target) && acct_admin_count() <= 1) {
            resp->ret = ERR_DENIED; /* cannot lock the last admin */
            goto out;
        }
        target->disabled = 1;
        /* Unbind any subject currently bound to this account. */
        for (int i = 0; i < USER_MAX_ACCOUNTS; i++) {
            if (s_binds[i].acct == (int)(target - s_accts)) {
                s_binds[i].subject = 0;
                s_binds[i].acct    = -1;
            }
        }
        printf("user: account '%s' locked\n", req->name);
    } else {
        target->disabled   = 0;
        target->fail_count = 0;
        printf("user: account '%s' unlocked\n", req->name);
    }
    resp->ret = 0;
out:
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_users(int token, int msg_len, uint64_t caller) {
    user_resp_login_t *resp = (user_resp_login_t *)s_resp;
    memset(resp, 0, sizeof(*resp));
    (void)msg_len;
    user_acct_t *me = acct_of_subject(caller);
    if (!acct_is_admin(me)) {
        resp->ret = ERR_DENIED;
        goto out;
    }
    /* One account line per call; the shell iterates by index.  Lines
     * are packed into resp->reason as "name role;name role;...". */
    int   pos   = 0;
    char *dst   = resp->reason;
    resp->count = 0;
    for (int i = 0; i < USER_MAX_ACCOUNTS; i++) {
        if (!s_accts[i].in_use)
            continue;
        int n = snprintf(dst + pos,
                         (int)sizeof(resp->reason) - pos,
                         "%s %u%s;",
                         s_accts[i].name,
                         s_accts[i].role,
                         s_accts[i].disabled ? " L" : "");
        if (n < 0 || pos + n >= (int)sizeof(resp->reason))
            break;
        pos += n;
        resp->count++;
    }
    resp->ret = 0;
out:
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

/* ------------------------------------------------------------------ */
/*  Exit guard: stop a user-level process (admin + verified)          */
/* ------------------------------------------------------------------ */

static const char *const s_critical[] = {
    "serial", "term", "keyboard", "vfs", "fs_mem_driver",
    "fs_virtio_blk_driver", "perm", "manager", "user",
};

static int svc_is_critical(const char *name) {
    for (unsigned i = 0; i < sizeof(s_critical) / sizeof(s_critical[0]); i++) {
        if (strcmp(s_critical[i], name) == 0)
            return 1;
    }
    return 0;
}

static void do_stop(int token, int msg_len, uint64_t caller) {
    user_resp_stop_t *resp = (user_resp_stop_t *)s_resp;
    memset(resp, 0, sizeof(*resp));
    resp->ret = ERR_INVAL;
    if (msg_len < (int)sizeof(user_req_stop_t))
        goto out;
    user_req_stop_t *req = (user_req_stop_t *)s_req;
    req->svc[USER_NAME_MAX - 1] = '\0';

    user_acct_t *me = acct_of_subject(caller);
    if (!acct_is_admin(me)) {
        resp->ret = ERR_DENIED;
        snprintf(resp->detail, sizeof(resp->detail), "requires OWNER/ADMIN");
        goto out;
    }
    if (svc_is_critical(req->svc)) {
        resp->ret = ERR_DENIED;
        snprintf(resp->detail, sizeof(resp->detail), "'%s' is a system-critical service",
                 req->svc);
        goto out;
    }

    /* Resolve the process by name and SIGKILL it (this service holds
     * ATOM_SERVICE_MANAGE, so the kernel's kill gate passes). */
    proc_info_t list[64];
    int         n = process_list(list, 64);
    if (n <= 0) {
        resp->ret = ERR_NOENT;
        goto out;
    }
    int found = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(list[i].name, req->svc) == 0 && list[i].state != 3 &&
            list[i].state != 4) {
            int r = kill(list[i].pid, SIGKILL);
            if (r == 0) {
                resp->ret   = 0;
                snprintf(resp->detail, sizeof(resp->detail), "'%s' (PID %d) stopped",
                         req->svc, list[i].pid);
                found = 1;
                printf("user: stop '%s' (PID %d) by %llu\n",
                       req->svc,
                       list[i].pid,
                       (unsigned long long)caller);
                break;
            }
        }
    }
    if (!found) {
        resp->ret = ERR_NOENT;
        snprintf(resp->detail, sizeof(resp->detail), "'%s' not running", req->svc);
    }
out:
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

/* ------------------------------------------------------------------ */
/*  Command-policy proxy (v0.5)                                       */
/*                                                                   */
/*  POLICY_SET/DUMP proxy: the caller (shell) is not management-plane
 *  (no ATOM_SERVICE_MANAGE), so it cannot mutate policy directly.
 *  This service holds the atom AND can resolve the caller's account
 *  role (OWNER/ADMIN), making it the trusted proxy: it re-checks the
 *  caller's role, then forwards to the policy service.              */
/* ------------------------------------------------------------------ */

/* Resolve the caller's bound account; NULL when not logged in. */
static user_acct_t *caller_acct(uint64_t caller) {
    return acct_of_subject(caller);
}

static void do_policy_set(int token, int msg_len, uint64_t caller) {
    user_resp_policy_t *resp = (user_resp_policy_t *)s_resp;
    memset(resp, 0, sizeof(*resp));
    resp->ret = ERR_INVAL;
    if (msg_len < (int)sizeof(user_req_policy_t))
        goto out;
    user_req_policy_t *req = (user_req_policy_t *)s_req;
    req->cmd[sizeof(req->cmd) - 1] = '\0';

    user_acct_t *me = caller_acct(caller);
    if (!acct_is_admin(me)) {
        resp->ret = ERR_DENIED; /* OWNER/ADMIN only */
        goto out;
    }

    /* Forward to the policy service (we hold ATOM_SERVICE_MANAGE). */
    int pp = port_get(POLICY_PORT_NAME);
    if (pp < 0) {
        resp->ret = ERR_NOENT;
        goto out;
    }
    policy_req_set_t pr;
    memset(&pr, 0, sizeof(pr));
    pr.op      = POLICY_OP_SET;
    pr.role    = req->role;
    pr.verdict = (uint8_t)req->verdict;
    strncpy(pr.cmd, req->cmd, sizeof(pr.cmd) - 1);
    pr.cmd[sizeof(pr.cmd) - 1] = '\0';
    policy_resp_set_t prr;
    memset(&prr, 0, sizeof(prr));
    int rlen = (int)sizeof(prr);
    int r    = ipc_call(pp, &pr, (int)sizeof(pr), &prr, &rlen);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }
    resp->ret = prr.ret;
out:
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

static void do_policy_dump(int token, int msg_len, uint64_t caller) {
    user_resp_policy_t *resp = (user_resp_policy_t *)s_resp;
    memset(resp, 0, sizeof(*resp));
    resp->ret = ERR_INVAL;
    (void)msg_len;

    user_acct_t *me = caller_acct(caller);
    if (!acct_is_admin(me)) {
        resp->ret = ERR_DENIED; /* OWNER/ADMIN only */
        goto out;
    }

    int pp = port_get(POLICY_PORT_NAME);
    if (pp < 0) {
        resp->ret = ERR_NOENT;
        goto out;
    }
    policy_req_dump_t pr;
    memset(&pr, 0, sizeof(pr));
    pr.op = POLICY_OP_DUMP;
    policy_resp_dump_t prr;
    memset(&prr, 0, sizeof(prr));
    int rlen = (int)sizeof(prr);
    int r    = ipc_call(pp, &pr, (int)sizeof(pr), &prr, &rlen);
    if (r < 0) {
        resp->ret = r;
        goto out;
    }
    resp->ret = prr.ret;
    if (prr.ret == 0) {
        resp->count = prr.count;
        for (u32 i = 0; i < prr.count && i < 64; i++)
            strncpy(resp->lines[i], prr.lines[i], sizeof(resp->lines[0]) - 1);
    }
out:
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

/* ------------------------------------------------------------------ */
/*  Kill proxy (v0.5)                                                 */
/*                                                                   */
/*  USER_OP_KILL: admin SIGKILLs a process by PID.  The shell cannot
 *  pass the kernel kill gate (no ATOM_SERVICE_MANAGE), so it asks the
 *  user service, which holds the atom and re-checks the caller is
 *  OWNER/ADMIN.  System-critical services are protected exactly like
 *  STOP (a mis-typed PID cannot take down the system).              */
/* ------------------------------------------------------------------ */

/* Same critical list as do_stop. */
static int svc_is_critical_name(const char *name) {
    static const char *const crit[] = {
        "serial", "term", "keyboard", "vfs", "fs_mem_driver",
        "fs_virtio_blk_driver", "perm", "manager", "user", "policy"};
    for (unsigned i = 0; i < sizeof(crit) / sizeof(crit[0]); i++)
        if (strcmp(crit[i], name) == 0)
            return 1;
    return 0;
}

static void do_kill(int token, int msg_len, uint64_t caller) {
    user_resp_kill_t *resp = (user_resp_kill_t *)s_resp;
    memset(resp, 0, sizeof(*resp));
    resp->ret = ERR_INVAL;
    if (msg_len < (int)sizeof(user_req_kill_t))
        goto out;
    user_req_kill_t *req = (user_req_kill_t *)s_req;

    user_acct_t *me = acct_of_subject(caller);
    if (!acct_is_admin(me)) {
        resp->ret = ERR_DENIED; /* OWNER/ADMIN only */
        snprintf(resp->detail, sizeof(resp->detail), "requires OWNER/ADMIN");
        goto out;
    }
    if (req->pid <= 0) {
        resp->ret = ERR_INVAL;
        goto out;
    }

    /* Resolve the process name for the criticality check. */
    proc_info_t list[64];
    int         n = process_list(list, 64);
    if (n <= 0) {
        resp->ret = ERR_NOENT;
        goto out;
    }
    int found = 0;
    for (int i = 0; i < n; i++) {
        if (list[i].pid == req->pid) {
            found = 1;
            if (svc_is_critical_name(list[i].name)) {
                resp->ret = ERR_DENIED;
                snprintf(resp->detail, sizeof(resp->detail),
                         "'%s' is system-critical", list[i].name);
                goto out;
            }
            int r = kill(list[i].pid, SIGKILL);
            if (r == 0) {
                resp->ret = 0;
                snprintf(resp->detail, sizeof(resp->detail), "'%s' (PID %d) killed",
                         list[i].name, list[i].pid);
                printf("user: kill '%s' (PID %d) by %llu\n",
                       list[i].name, list[i].pid, (unsigned long long)caller);
            } else {
                resp->ret = r;
            }
            break;
        }
    }
    if (!found) {
        resp->ret = ERR_NOENT;
        snprintf(resp->detail, sizeof(resp->detail), "PID %d not running", req->pid);
    }
out:
    (void)ipc_reply(token, resp, (int)sizeof(*resp));
}

/* ------------------------------------------------------------------ */
/*  Dispatch                                                          */
/* ------------------------------------------------------------------ */

static void user_handle(int token, u32 op, int msg_len, uint64_t caller) {
    switch (op) {
    case USER_OP_LOGIN:
        do_login(token, msg_len, caller);
        break;
    case USER_OP_LOGOUT:
        do_logout(token, msg_len, caller);
        break;
    case USER_OP_PASSWD:
        do_passwd(token, msg_len, caller);
        break;
    case USER_OP_USERADD:
        do_useradd(token, msg_len, caller);
        break;
    case USER_OP_USERDEL:
        do_userdel(token, msg_len, caller);
        break;
    case USER_OP_USERS:
        do_users(token, msg_len, caller);
        break;
    case USER_OP_WHOAMI:
        do_whoami(token, msg_len, caller);
        break;
    case USER_OP_VERIFY:
        do_verify(token, msg_len);
        break;
    case USER_OP_STOP:
        do_stop(token, msg_len, caller);
        break;
    case USER_OP_LOCK:
        do_lock(token, msg_len, caller, 1);
        break;
    case USER_OP_UNLOCK:
        do_lock(token, msg_len, caller, 0);
        break;
    case USER_OP_POLICY_SET:
        do_policy_set(token, msg_len, caller);
        break;
    case USER_OP_POLICY_DUMP:
        do_policy_dump(token, msg_len, caller);
        break;
    case USER_OP_KILL:
        do_kill(token, msg_len, caller);
        break;
    default: {
        i32 *resp = (i32 *)s_resp;
        *resp     = ERR_INVAL;
        (void)ipc_reply(token, resp, (int)sizeof(i32));
        break;
    }
    }
}

int main(void) {
    printf("user: starting user account service\n");
    memset(s_accts, 0, sizeof(s_accts));
    for (int i = 0; i < USER_MAX_ACCOUNTS; i++)
        s_binds[i].acct = -1;

    /* Bootstrap: seed a default admin account on first boot. */
    if (acct_count() == 0) {
        user_acct_t *a   = &s_accts[0];
        a->in_use        = 1;
        strncpy(a->name, "admin", sizeof(a->name) - 1);
        a->name[sizeof(a->name) - 1] = '\0';
        a->salt          = salt_gen();
        a->pw_hash       = pw_hash("admin", a->salt);
        a->role          = PERM_ROLE_OWNER;
        printf("user: DEFAULT admin/admin created - CHANGE THE PASSWORD (passwd)\n");
    }

    int port = ipc_port_create();
    if (port < 0) {
        printf("user: ipc_port_create failed (%d)\n", port);
        thread_exit(1);
    }
    int ret = port_register(USER_PORT_NAME, port);
    if (ret < 0) {
        printf("user: port_register('%s') failed (%d)\n", USER_PORT_NAME, ret);
        thread_exit(1);
    }
    printf("user: port %d registered as '%s'\n", port, USER_PORT_NAME);

    for (;;) {
        int  msg_len        = (int)sizeof(s_req);
        int  token          = 0;
        u64  caller_subject = 0;
        ret = ipc_recv_from(port, s_req, &msg_len, &token, &caller_subject);
        if (ret < 0) {
            printf("user: ipc_recv failed (%d)\n", ret);
            thread_exit(1);
        }
        if (msg_len < (int)sizeof(u32)) {
            i32 *resp = (i32 *)s_resp;
            *resp     = ERR_INVAL;
            (void)ipc_reply(token, resp, (int)sizeof(i32));
            continue;
        }
        u32 op = *(u32 *)s_req;
        user_handle(token, op, msg_len, caller_subject);
    }
}
