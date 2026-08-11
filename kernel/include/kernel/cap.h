/*
 * cap.h - Capability system
 * Copyright (c) 2026 OpSys Project
 *
 * All resource access goes through capability handles.
 * Capabilities are per-process, stored in a capability table.
 */

#ifndef KERNEL_CAP_H
#define KERNEL_CAP_H

#include <kernel/types.h>
#include <kernel/atom.h>

/* Capability object types */
typedef enum {
    CAP_TYPE_NONE = 0,
    CAP_TYPE_THREAD,        /* Thread control */
    CAP_TYPE_PORT,          /* IPC port */
    CAP_TYPE_MEM,           /* Memory region */
    CAP_TYPE_IRQ,           /* Interrupt binding */
    CAP_TYPE_IO_PORT,       /* I/O port range */
    CAP_TYPE_PCI_DEV,       /* PCI device */
    CAP_TYPE_SERVICE,       /* System service */
    CAP_TYPE_KERNEL,        /* Kernel object reference */
    CAP_TYPE_DAC_OVERRIDE,  /* Bypass DAC/credential checks (Linux CAP_DAC_OVERRIDE style) */
} cap_type_t;

/* Capability object (kernel-side) */
typedef struct {
    cap_t       handle;     /* Unique handle value */
    cap_type_t  type;
    rights_t    rights;     /* Permission bitmask */
    u64         obj_id;     /* Referenced object ID */
    u64         obj_ptr;    /* Pointer to kernel object (or 0) */
    u32         ref_count;

    /* ---- P0 地基: permission-model lifecycle fields ----
     * atom/expiry/quota/scope are the decision-cache encoding: the
     * perm-engine (P1) signs atom caps; the kernel only does local
     * lookups (see docs/permission_model.md §四).  Existing gated
     * paths (MEM/IRQ/IO_PORT) keep atom=0, expiry=0, quota=0,
     * scope=0, so lazy expiry and atom/scope checks pass through. */
    u16         atom_id;       /* atom_id_t 值；0 = ATOM_NONE */
    subject_id_t subject;      /* 持有者主体；0 = System/未指定 */
    u64         expiry_ticks;  /* 0 = 永久；绝对 tick，超时后视同不存在 */
    u32         quota;         /* 0 = 无限；>0 = 剩余使用次数 */
    u64         scope_hash;    /* 0 = 不限制；否则匹配 scope */
} cap_entry_t;

/* Per-process capability table */
typedef struct {
    cap_entry_t entries[MAX_CAPS];
    u32         count;
    pid_t       owner_pid;
} cap_table_t;

/**
 * Initialize the capability system.
 */
void cap_init(void);

/**
 * Create a new empty capability table for a process.
 * @param pid  Process ID.
 * @return Pointer to capability table, or NULL.
 */
cap_table_t *cap_table_create(pid_t pid);

/**
 * Destroy a capability table.
 * @param table  Table to destroy.
 */
void cap_table_destroy(cap_table_t *table);

/**
 * Create a new capability and add it to a table.
 * @param table  Target capability table.
 * @param type   Capability type.
 * @param rights Initial rights.
 * @param obj_id Object ID.
 * @param obj_ptr Object pointer (kernel-internal).
 * @return Handle, or 0 on failure.
 */
cap_t cap_create_in_table(cap_table_t *table, cap_type_t type,
                          rights_t rights, u64 obj_id, u64 obj_ptr);

/**
 * Create an atom capability (P0 地基): like cap_create_in_table but
 * with the permission-model lifecycle fields.  entry.subject is the
 * subject that HOLDS the capability (the syscall wrapper passes the
 * caller's subject_id; kernel-internal callers pass 0 = System).
 * @param table        Target capability table.
 * @param subject      Holding subject (0 = System/未指定).
 * @param atom         Permission atom (ATOM_NONE = no atom semantics).
 * @param rights       Initial rights.
 * @param expiry_ticks Absolute tick deadline; 0 = permanent.  Expired
 *                     entries are lazily revoked by cap_lookup.
 * @param quota        Remaining uses; 0 = unlimited.
 * @param scope_hash   Scope restriction; 0 = unrestricted.
 * @param out          Receives the new handle on success.
 * @return OK, or ERR_INVAL (bad table/out) / ERR_NOMEM (table full).
 */
int cap_create_atom(cap_table_t *table, subject_id_t subject,
                    atom_id_t atom, rights_t rights, u64 expiry_ticks,
                    u32 quota, u64 scope_hash, cap_t *out);

/**
 * Consume one quota unit of a capability (P0 地基).
 * Lookup includes lazy expiry: an expired entry is revoked in place
 * and treated as not found.  quota==0 (unlimited) is a no-op that
 * returns OK; when a positive quota drops to 0 the entry is revoked
 * (same cleanup as cap_revoke).
 * @param table   Target table.
 * @param handle  Capability handle to consume.
 * @return OK, or the same errors cap_revoke returns (ERR_INVAL for a
 *         bad table/index, ERR_NOENT for missing/stale/expired).
 */
int cap_consume(cap_table_t *table, cap_t handle);

/**
 * Revoke every capability held by a subject matching an atom
 * (optionally restricted to a scope) across ALL kernel cap tables
 * (P0 地基).  Same per-entry cleanup as cap_revoke (gen bump,
 * memset, count--).  Holds the cap lock for the whole scan.
 * @param subj       Holding subject whose caps are revoked.
 * @param atom       Atom to match.
 * @param scope_hash 0 = match any scope, else exact scope match.
 * @return Number of entries revoked (>= 0).
 */
int cap_revoke_by_atom(subject_id_t subj, atom_id_t atom, u64 scope_hash);

/**
 * Grant a capability to another process's table.
 * @param src_table  Source table (has the cap).
 * @param dst_table  Destination table (receives the cap).
 * @param handle     Capability handle to grant.
 * @param rights     Rights to grant (subset of original).
 * @return New handle in destination, or 0 on failure.
 */
cap_t cap_grant(cap_table_t *src_table, cap_table_t *dst_table,
                cap_t handle, rights_t rights);

/**
 * Revoke a capability from a table.
 * @param table   Target table.
 * @param handle  Capability handle to revoke.
 * @return OK or error.
 */
error_t cap_revoke(cap_table_t *table, cap_t handle);

/**
 * Look up a capability in a table and validate rights.
 * @param table   Target table.
 * @param handle  Capability handle.
 * @param need    Required rights.
 * @return Pointer to cap_entry, or NULL if not found/insufficient.
 */
cap_entry_t *cap_lookup(cap_table_t *table, cap_t handle, rights_t need);

/**
 * Get the capability table for a given PID.
 * @param pid  Process ID.
 * @return Pointer to capability table, or NULL.
 */
cap_table_t *cap_get_table(pid_t pid);

/**
 * Check whether a capability table contains ANY entry of a given type.
 * Useful for privilege checks (e.g. CAP_TYPE_DAC_OVERRIDE).
 * @param table  Target capability table.
 * @param type   Capability type to look for.
 * @return true if at least one entry of the type exists.
 */
bool cap_has_type(cap_table_t *table, cap_type_t type);

#endif /* KERNEL_CAP_H */
