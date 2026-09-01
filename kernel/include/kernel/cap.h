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
    CAP_TYPE_THREAD,       /* Thread control */
    CAP_TYPE_PORT,         /* IPC port */
    CAP_TYPE_MEM,          /* Memory region */
    CAP_TYPE_IRQ,          /* Interrupt binding */
    CAP_TYPE_IO_PORT,      /* I/O port range */
    CAP_TYPE_PCI_DEV,      /* PCI device */
    CAP_TYPE_SERVICE,      /* System service */
    CAP_TYPE_KERNEL,       /* Kernel object reference */
    CAP_TYPE_DAC_OVERRIDE, /* Bypass DAC/credential checks (Linux CAP_DAC_OVERRIDE style) */
} cap_type_t;

/* Capability object (kernel-side) */
typedef struct {
    cap_t      handle; /* Unique handle value */
    cap_type_t type;
    rights_t   rights;  /* Permission bitmask */
    u64        obj_id;  /* Referenced object ID */
    u64        obj_ptr; /* Pointer to kernel object (or 0) */
    u32        ref_count;

    /* ---- P0 地基: permission-model lifecycle fields ----
     * atom/expiry/quota/scope are the decision-cache encoding: the
     * perm-engine (P1) signs atom caps; the kernel only does local
     * lookups (see docs/permission_model.md §四).  Existing gated
     * paths (MEM/IRQ/IO_PORT) keep atom=0, expiry=0, quota=0,
     * scope=0, so lazy expiry and atom/scope checks pass through. */
    u16          atom_id;      /* atom_id_t 值；0 = ATOM_NONE */
    subject_id_t subject;      /* 持有者主体；0 = System/未指定 */
    u64          expiry_ticks; /* 0 = 永久；绝对 tick，超时后视同不存在 */
    u32          quota;        /* 0 = 无限；>0 = 剩余使用次数 */
    u64          scope_hash;   /* 0 = 不限制；否则匹配 scope */
} cap_entry_t;

/* Per-process capability table.
 *
 * Dynamically allocated via PmmAllocPages() on cap_table_create() and
 * freed on CapTableDestroy().  The gen[] array holds per-slot
 * generation counters (formerly a global s_gen_counters[] array that
 * cost 1 MB of BSS).  sizeof(cap_table_t) ≈ 73 KB → 19 pages. */
typedef struct {
    cap_entry_t entries[MAX_CAPS];
    u32         count;
    pid_t       owner_pid;
    u8          gen[MAX_CAPS]; /* Generation counters (per-slot) */
} cap_table_t;

/**
 * Initialize the capability system.
 */
void CapInit(void);

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
void CapTableDestroy(cap_table_t *table);

/**
 * Create a new capability and add it to a table.
 * @param table  Target capability table.
 * @param type   Capability type.
 * @param rights Initial rights.
 * @param obj_id Object ID.
 * @param obj_ptr Object pointer (kernel-internal).
 * @return Handle, or 0 on failure.
 */
cap_t CapCreateInTable(
    cap_table_t *table, cap_type_t type, rights_t rights, u64 obj_id, u64 obj_ptr);

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
int CapCreateAtom(cap_table_t *table,
                    subject_id_t subject,
                    atom_id_t    atom,
                    rights_t     rights,
                    u64          expiry_ticks,
                    u32          quota,
                    u64          scope_hash,
                    cap_t       *out);

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
int CapConsume(cap_table_t *table, cap_t handle);

/**
 * P1 地基: issue an atom capability to EVERY process holding a subject.
 * Subject-targeted sibling of cap_grant: the perm-engine (the only
 * issuer in P1) signs decision-encoding caps directly into the grantee's
 * table without needing a handle it holds first (docs/permission_model.md
 * §四 — the capability IS the encoded decision).  Resolves the target
 * table via process_get_by_subject() and delegates to cap_create_atom
 * with entry.subject = the target subject.
 * @param subject      Target subject (0 = System → ERR_NOENT).
 * @param atom         Permission atom to sign.
 * @param rights       Initial rights.
 * @param expiry_ticks Absolute tick deadline; 0 = permanent.
 * @param quota        Remaining uses; 0 = unlimited.
 * @param scope_hash   Scope restriction; 0 = unrestricted.
 * @param out          Receives the new handle on success.
 * @return OK, ERR_NOENT (no live process holds the subject),
 *         ERR_INVAL (bad atom/rights), or ERR_NOMEM (table full).
 */
int CapGrantToSubject(subject_id_t subject,
                         atom_id_t    atom,
                         rights_t     rights,
                         u64          expiry_ticks,
                         u32          quota,
                         u64          scope_hash,
                         cap_t       *out);

/**
 * Revoke every capability held by a subject matching an atom
 * (optionally restricted to a scope) across ALL kernel cap tables
 * (P0 地基).  Same per-entry cleanup as CapRevoke(gen bump,
 * memset, count--).  Holds the cap lock for the whole scan.
 * @param subj       Holding subject whose caps are revoked.
 * @param atom       Atom to match.
 * @param scope_hash 0 = match any scope, else exact scope match.
 * @return Number of entries revoked (>= 0).
 */
int CapRevokeByAtom(subject_id_t subj, atom_id_t atom, u64 scope_hash);

/**
 * Grant a capability to another process's table.
 * @param src_table  Source table (has the cap).
 * @param dst_table  Destination table (receives the cap).
 * @param handle     Capability handle to grant.
 * @param rights     Rights to grant (subset of original).
 * @return New handle in destination, or 0 on failure.
 */
cap_t CapGrant(cap_table_t *src_table, cap_table_t *dst_table, cap_t handle, rights_t rights);

/**
 * Revoke a capability from a table.
 * @param table   Target table.
 * @param handle  Capability handle to revoke.
 * @return OK or error.
 */
error_t CapRevoke(cap_table_t *table, cap_t handle);

/**
 * Look up a capability in a table and validate rights.
 * @param table   Target capability table.
 * @param handle  Capability handle.
 * @param need    Required rights.
 * @return Pointer to cap_entry, or NULL if not found/insufficient.
 */
cap_entry_t *cap_lookup(cap_table_t *table, cap_t handle, rights_t need);

/**
 * P2 地基: look up a LIVE atom capability by (subject, atom, scope)
 * instead of by handle.  This is the sensitive-syscall gate —
 * "capability IS the decision" (docs/permission_model.md §四: 决策下沉,
 * 能力即决策): the cap table IS the decision cache, and the gate is a
 * pure kernel table scan with ZERO IPC to user space.
 *
 * Match rule: an entry qualifies iff type != CAP_TYPE_NONE,
 * entry.subject == subject, entry.atom_id == atom and
 * (scope_hash == 0 || entry.scope_hash == scope_hash).  "Live" mirrors
 * cap_lookup_locked's lazy-expiry rule (an expired matching entry is
 * revoked in place and skipped) and cap_consume's quota rule (a fully
 * consumed quota entry was already revoked to type==CAP_TYPE_NONE, so
 * the type check is the quota liveness test).  When several entries
 * match, the lowest table index wins (deterministic).
 *
 * The scan runs under the cap lock internally and returns a handle
 * (not a pointer), so the caller never races a revocation after the
 * lock is dropped — a stale handle simply fails later handle-based
 * lookups.  No rights check: the gate asks only "does the subject
 * hold the atom", per §四's cap_lookup(subject, atom, scope) shape.
 *
 * @param table      Target capability table.
 * @param subject    Holding subject to match (0 = System).
 * @param atom       Permission atom to match.
 * @param scope_hash 0 = any scope, else exact scope match.
 * @return Handle of the matched entry, or CAP_NULL (0) if none.
 */
cap_t CapLookupByAtom(cap_table_t *table, subject_id_t subject, atom_id_t atom, u64 scope_hash);

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
bool CapHasType(cap_table_t *table, cap_type_t type);

#endif /* KERNEL_CAP_H */
