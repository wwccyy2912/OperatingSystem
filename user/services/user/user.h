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
 *   - The caller identity always comes from IpcRecvFrom(kernel-
 *     filled subject), never from request bytes.
 *
 * Port name: "user"
 */

#ifndef USER_H
#define USER_H

#include <stdint.h>

/* Service-supervision protocol (svc_req_t / svc_resp_t) — shared with
 * the manager's control port so the proxy can forward it unchanged. */
#include "../manager/manager.h"

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
    USER_OP_DISK_MOUNT = 15, /* admin proxy: mount the Disk volume   */
    USER_OP_DISK_UNMOUNT = 16, /* admin proxy: unmount the Disk volume */
    USER_OP_DISK_FORMAT = 17, /* admin proxy: wipe + re-format Disk   */
    USER_OP_DISK_FILL  = 18, /* admin proxy: fill Disk until NOSPC/budget */
    USER_OP_DISK_SYNC     = 19, /* admin proxy: flush the volume to the medium */
    USER_OP_DISK_CHECK    = 20, /* admin proxy: read-only consistency scan     */
    USER_OP_DISK_INFO     = 21, /* admin proxy: volume detail                  */
    USER_OP_DISK_RAW_READ = 22, /* admin proxy: raw sectors (debug)            */
    /* v0.9: admin proxy to the service manager's control port.  The
     * shell holds no ATOM_SERVICE_MANAGE, so it cannot command the
     * manager directly; it asks this service, which re-checks that the
     * human behind the request is OWNER/ADMIN and then forwards the
     * request verbatim (request svc_req_t, reply svc_resp_t). */
    USER_OP_SVC_LIST    = 23,
    USER_OP_SVC_STATUS  = 24,
    USER_OP_SVC_START   = 25,
    USER_OP_SVC_STOP    = 26,
    USER_OP_SVC_RESTART = 27,
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

/* DISK_*: admin proxy for the block-device filesystem driver
 * (fs_virtio_blk_driver).  The shell must not talk to the driver
 * directly — the driver gates its management control plane on
 * ATOM_SERVICE_MANAGE, which the user service holds; the user service
 * re-checks the caller is OWNER/ADMIN, exactly like KILL/POLICY_SET. */
typedef struct {
    uint32_t op;         /* USER_OP_DISK_* */
    char     volume[64]; /* mount name("Disk") */
    uint32_t size;       /* FILL: byte budget (0 = fill until NOSPC) */
    uint64_t lba;        /* RAW_READ: first sector */
    uint32_t length;     /* RAW_READ: bytes to read (<= USER_DISK_RAW_MAX) */
} user_req_disk_t;

/* Largest raw read the proxy will forward in one reply. */
#define USER_DISK_RAW_MAX 1024

typedef struct {
    int32_t  ret;
    uint64_t bytes;   /* FILL: bytes written to fill.bin;
                       * RAW_READ: bytes actually returned in raw[] */
    char     detail[64];
    /* DISK_CHECK (mirrors drv_check_report_t) */
    uint32_t check_magic_ok;
    uint32_t check_inodes_total;
    uint32_t check_inodes_used;
    uint32_t check_files;
    uint32_t check_dirs;
    uint32_t check_used_blocks;
    uint32_t check_free_blocks;
    uint32_t check_errors;
    uint32_t check_first_error;
    char     check_note[64];
    /* DISK_INFO (mirrors drv_info_t) */
    char     info_driver[64];
    char     info_mount[64];
    uint32_t info_read_only;
    uint32_t info_block_size;
    uint32_t info_total_blocks;
    uint32_t info_used_blocks;
    uint32_t info_inode_total;
    uint32_t info_inode_used;
    uint32_t info_persistent;
    uint64_t info_uuid_hi;
    uint64_t info_uuid_lo;
    /* DISK_RAW_READ payload */
    uint8_t  raw[USER_DISK_RAW_MAX];
} user_resp_disk_t;

/* SVC_*: admin proxy to the manager's control port.  The request
 * carries the manager's own SVC_OP_* opcode plus an optional service
 * name; the reply is a svc_resp_t verbatim (user/services/manager/
 * manager.h), so the proxy adds no protocol of its own. */
typedef struct {
    uint32_t op; /* SVC_OP_* */
    char     name[SVC_NAME_MAX];
} user_req_svc_t;

/* Compile-time guard: every message fits the 4096-byte IPC limit. */
#define USER_IPC_MAX 4096
_Static_assert(sizeof(svc_resp_t) <= USER_IPC_MAX, "svc_resp_t exceeds the IPC limit");
_Static_assert(sizeof(user_req_svc_t) <= USER_IPC_MAX, "user_req_svc_t exceeds the IPC limit");
_Static_assert(sizeof(user_resp_disk_t) <= USER_IPC_MAX, "user_resp_disk_t exceeds the IPC limit");

#endif /* USER_H */
