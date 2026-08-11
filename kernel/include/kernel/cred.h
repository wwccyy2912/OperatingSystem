/*
 * cred.h - Process credentials (user/group identity)
 * Copyright (c) 2026 OpSys Project
 *
 * Each process carries a set of credentials that determine its
 * ownership and permissions.  Design follows POSIX conventions:
 *
 *   uid    — Real user ID (identifies the user who owns the process)
 *   euid   — Effective user ID (used for permission checks)
 *   suid   — Saved set-user-ID (allows euid toggling after setuid exec)
 *   gid    — Real group ID
 *   egid   — Effective group ID (used for group permission checks)
 *
 * UID 0 (root) bypasses all permission checks.  This file provides
 * the data structure and allocation helpers; the actual permission
 * check logic lives in the relevant subsystems (VFS, IPC, etc.).
 *
 * Future extension points:
 *   - Supplementary group list (struct cred *groups[])
 *   - Filesystem UID/GID (FS credentials for NFS etc.)
 *   - Capability sets (POSIX 1003.1e dropped, but Linux has bounding set)
 *   - SELinux / LSM security blobs
 */

#ifndef KERNEL_CRED_H
#define KERNEL_CRED_H

#include <kernel/types.h>

/** Default root credentials (uid=0, gid=0). */
#define CRED_ROOT_VAL(_uid) \
    { .uid = (_uid), .euid = (_uid), .suid = (_uid), \
      .gid = 0, .egid = 0 }

/** Per-process credentials. */
typedef struct cred {
    uid_t   uid;        /* Real user ID        (POSIX: getuid)  */
    uid_t   euid;       /* Effective user ID   (POSIX: geteuid) */
    uid_t   suid;       /* Saved set-user-ID                     */
    gid_t   gid;        /* Real group ID       (POSIX: getgid)  */
    gid_t   egid;       /* Effective group ID  (POSIX: getegid) */
} cred_t;

/**
 * Allocate and initialise a new cred structure.
 * Returns NULL on allocation failure.
 */
cred_t *cred_create(uid_t uid, gid_t gid);

/**
 * Duplicate an existing cred structure (deep copy).
 * Returns NULL on allocation failure.
 */
cred_t *cred_clone(const cred_t *src);

/**
 * Free a cred structure allocated by cred_create / cred_clone.
 */
void cred_destroy(cred_t *cred);

/**
 * Check whether a given EUID is privileged (root).
 * In POSIX, uid 0 bypasses all permission checks.
 */
static inline bool cred_is_root(uid_t euid)
{
    return euid == UID_ROOT;
}

#endif /* KERNEL_CRED_H */
