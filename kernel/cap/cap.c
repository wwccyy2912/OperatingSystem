/*
 * cap.c - Capability system
 * Copyright (c) 2026 OpSys Project
 *
 * All resource access goes through capability handles.
 * Capabilities are per-process, stored in a capability table.
 * Handles encode PID + index + generation for uniqueness.
 *
 * v0.2: Single-CPU, cli/sti spinlock (s_cap_lock) protects the table
 * pool, the per-pid table mapping and the generation counters.
 * Internal helpers assume the lock is held; public functions take it
 * exactly once (cap_grant must NOT nest: it uses the _locked helpers).
 *
 * Caveat: cap_lookup() returns a pointer into the static pool after
 * dropping the lock.  This is safe by construction -- entries are never
 * freed, only memset to CAP_TYPE_NONE by cap_revoke -- so the worst
 * concurrent outcome is a stale pointer seeing a revoked entry and the
 * caller denying the operation.  No use-after-free is possible.
 */

#include <kernel/cap.h>
#include <kernel/serial.h>
#include <kernel/spinlock.h>
#include <kernel/sched.h>    /* sched_get_ticks(): lazy expiry tick source */
#include <kernel/process.h>  /* process_get(): cap_grant dst subject */

/* ---------------------------------------------------------------------------
 * Memory helper (no libc available in kernel)
 * --------------------------------------------------------------------------- */
static void cap_memset(void *dst, u8 val, u64 n)
{
    u8 *d = (u8 *)dst;
    for (u64 i = 0; i < n; i++)
        d[i] = val;
}

/* ---------------------------------------------------------------------------
 * Handle encoding
 *
 *   31         8 7          0
 *   [  INDEX  ] [   GEN   ]
 *
 * INDEX: slot in capability table (up to 24 bits, MAX_CAPS = 1024 fits)
 * GEN:   generation counter (8 bits, wraps 0-255)
 *
 * PID is NOT encoded in the handle. Each process has its own cap_table,
 * so the PID is redundant. gen_counters are indexed by
 * (table->owner_pid * MAX_CAPS + idx), not from the handle.
 *
 * This produces 32-bit handles that fit in user-space int wrappers.
 * --------------------------------------------------------------------------- */
#define HANDLE_INDEX(h)    (((h) >> 8) &  0x00FFFFFFULL)
#define HANDLE_GEN(h)      ((h) & 0xFFULL)

#define MAKE_HANDLE(pid, idx, gen) \
    (((u32)(idx) << 8) | (u32)(gen))

/* ---------------------------------------------------------------------------
 * Static data
 *
 * Memory note (MAX_THREADS = 1024): cap_table_t is 40968 bytes
 * (MAX_CAPS=1024 entries x 40 B + count + owner_pid), so the pool is
 * 1024 x 40968 B = 40 MB of BSS plus 1 MB of generation counters.
 * That is the price of a static, zero-failure-mode pool (no pmm
 * dependency before the first process exists, no per-table malloc).
 * The kernel boots in a 256 MB VM with the first 4 GB identity-mapped,
 * so 41 MB of BSS is comfortably within budget; the cap pool is only
 * ever touched for tables that are actually handed out (one per live
 * process), and most processes use a handful of slots out of 1024.
 * Keeping it static was preferred over lazy pmm allocation so that
 * cap_table_create() can never fail mid-boot.
 * --------------------------------------------------------------------------- */
static spinlock_t    s_cap_lock = SPINLOCK_INIT;
static cap_table_t   s_cap_table_pool[MAX_THREADS];
static cap_table_t  *s_cap_tables[MAX_THREADS];
static u8            s_gen_counters[MAX_THREADS * MAX_CAPS];

/* ---------------------------------------------------------------------------
 * Initialization
 * --------------------------------------------------------------------------- */
void cap_init(void)
{
    cap_memset(s_cap_table_pool, 0, sizeof(s_cap_table_pool));
    cap_memset(s_cap_tables, 0, sizeof(s_cap_tables));
    cap_memset(s_gen_counters, 0, sizeof(s_gen_counters));
}

/* ---------------------------------------------------------------------------
 * Table lifecycle
 * --------------------------------------------------------------------------- */
cap_table_t *cap_table_create(pid_t pid)
{
    if (pid < 0 || pid >= MAX_THREADS)
        return NULL;

    spin_lock(&s_cap_lock);
    cap_table_t *ret = NULL;

    /* Find a free slot in the static pool */
    for (u32 i = 0; i < MAX_THREADS; i++) {
        if (s_cap_table_pool[i].owner_pid == 0 &&
            s_cap_table_pool[i].count == 0) {

            cap_table_t *t = &s_cap_table_pool[i];
            t->owner_pid = pid;
            t->count     = 0;
            cap_memset(t->entries, 0, sizeof(t->entries));
            s_cap_tables[pid] = t;
            ret = t;
            break;
        }
    }
    spin_unlock(&s_cap_lock);
    return ret;
}

void cap_table_destroy(cap_table_t *table)
{
    if (!table)
        return;

    spin_lock(&s_cap_lock);
    pid_t pid = table->owner_pid;

    /* Clear all entries */
    cap_memset(table->entries, 0, sizeof(table->entries));
    table->count = 0;
    table->owner_pid = 0;

    /* Unregister */
    if (pid >= 0 && pid < MAX_THREADS)
        s_cap_tables[pid] = NULL;
    spin_unlock(&s_cap_lock);
}

/* ---------------------------------------------------------------------------
 * Capability creation
 * --------------------------------------------------------------------------- */
static cap_t cap_create_atom_locked(cap_table_t *table, subject_id_t subject,
                                    u16 atom, cap_type_t type, rights_t rights,
                                    u64 obj_id, u64 obj_ptr,
                                    u64 expiry_ticks, u32 quota,
                                    u64 scope_hash)
{
    if (!table)
        return CAP_NULL;

    pid_t pid = table->owner_pid;

    /* Find free slot */
    for (u32 i = 0; i < MAX_CAPS; i++) {
        if (table->entries[i].type == CAP_TYPE_NONE) {
            /* Compute generation counter index */
            u32 gen_idx = (u32)pid * MAX_CAPS + i;
            u8 gen = s_gen_counters[gen_idx];
            gen = (gen + 1) & 0xFF;
            if (gen == 0)
                gen = 1; /* Skip zero generation (means unused) */
            s_gen_counters[gen_idx] = gen;

            cap_t handle = MAKE_HANDLE(pid, i, gen);

            cap_entry_t *e = &table->entries[i];
            e->handle       = handle;
            e->type         = type;
            e->rights       = rights;
            e->obj_id       = obj_id;
            e->obj_ptr      = obj_ptr;
            e->ref_count    = 1;
            e->atom_id      = atom;
            e->subject      = subject;
            e->expiry_ticks = expiry_ticks;
            e->quota        = quota;
            e->scope_hash   = scope_hash;

            table->count++;
            return handle;
        }
    }
    return CAP_NULL;
}

static cap_t cap_create_in_table_locked(cap_table_t *table, cap_type_t type,
                                        rights_t rights, u64 obj_id,
                                        u64 obj_ptr)
{
    /* Existing paths (MEM/IRQ/IO_PORT/THREAD...) get the P0 lifecycle
     * defaults: subject = System (0), no atom, permanent, unlimited. */
    return cap_create_atom_locked(table, 0, ATOM_NONE, type, rights,
                                  obj_id, obj_ptr, 0, 0, 0);
}

cap_t cap_create_in_table(cap_table_t *table, cap_type_t type,
                          rights_t rights, u64 obj_id, u64 obj_ptr)
{
    spin_lock(&s_cap_lock);
    cap_t handle = cap_create_in_table_locked(table, type, rights,
                                              obj_id, obj_ptr);
    spin_unlock(&s_cap_lock);
    return handle;
}

int cap_create_atom(cap_table_t *table, subject_id_t subject,
                    atom_id_t atom, rights_t rights, u64 expiry_ticks,
                    u32 quota, u64 scope_hash, cap_t *out)
{
    if (!table || !out)
        return ERR_INVAL;

    spin_lock(&s_cap_lock);
    cap_t handle = cap_create_atom_locked(table, subject, (u16)atom,
                                          CAP_TYPE_KERNEL, rights, 0, 0,
                                          expiry_ticks, quota, scope_hash);
    spin_unlock(&s_cap_lock);

    if (handle == CAP_NULL)
        return ERR_NOMEM;
    *out = handle;
    return OK;
}

/* ---------------------------------------------------------------------------
 * Entry teardown (shared by cap_revoke, lazy expiry, quota exhaustion and
 * cap_revoke_by_atom): bump the generation so stale handles are rejected,
 * clear the slot and decrement the table count.  Caller holds s_cap_lock.
 * --------------------------------------------------------------------------- */
static void cap_revoke_entry_locked(cap_table_t *table, u32 idx)
{
    u32 gen_idx = (u32)table->owner_pid * MAX_CAPS + idx;
    s_gen_counters[gen_idx] = (s_gen_counters[gen_idx] + 1) & 0xFF;

    cap_memset(&table->entries[idx], 0, sizeof(table->entries[idx]));
    table->count--;
}

/* ---------------------------------------------------------------------------
 * Capability grant (delegation with rights intersection)
 * --------------------------------------------------------------------------- */
static cap_entry_t *cap_lookup_locked(cap_table_t *table, cap_t handle,
                                      rights_t need);

cap_t cap_grant(cap_table_t *src_table, cap_table_t *dst_table,
                cap_t handle, rights_t rights)
{
    if (!src_table || !dst_table)
        return CAP_NULL;

    spin_lock(&s_cap_lock);

    /* Look up source capability with GRANT permission */
    cap_entry_t *src = cap_lookup_locked(src_table, handle, RIGHT_GRANT);
    if (!src) {
        spin_unlock(&s_cap_lock);
        return CAP_NULL;
    }

    /* Create new entry in destination table with intersected rights */
    rights_t granted = src->rights & rights;

    cap_t new_handle = cap_create_in_table_locked(
        dst_table, src->type, granted, src->obj_id, src->obj_ptr);

    if (new_handle != CAP_NULL) {
        /* P0 地基: the copy is HELD by the destination process's
         * subject (never the source's).  The other lifecycle fields
         * default to 0/NONE on the granted copy (existing API
         * semantics unchanged). */
        subject_id_t dst_subj = 0;
        process_t *dst_proc = process_get(dst_table->owner_pid);
        if (dst_proc)
            dst_subj = dst_proc->subject_id;
        dst_table->entries[HANDLE_INDEX(new_handle)].subject = dst_subj;
        src->ref_count++;
        /* Ref the new entry too (already set to 1 by create) */
    }
    spin_unlock(&s_cap_lock);
    return new_handle;
}

/* ---------------------------------------------------------------------------
 * Capability revocation
 * --------------------------------------------------------------------------- */
error_t cap_revoke(cap_table_t *table, cap_t handle)
{
    if (!table)
        return ERR_INVAL;

    spin_lock(&s_cap_lock);

    u32 idx = (u32)HANDLE_INDEX(handle);
    if (idx >= MAX_CAPS) {
        spin_unlock(&s_cap_lock);
        return ERR_INVAL;
    }

    cap_entry_t *e = &table->entries[idx];

    /* Validate handle matches (generation check) */
    if (e->type == CAP_TYPE_NONE) {
        spin_unlock(&s_cap_lock);
        return ERR_NOENT;
    }
    if (e->handle != handle) {
        spin_unlock(&s_cap_lock);
        return ERR_NOENT;
    }

    e->ref_count--;
    if (e->ref_count == 0) {
        /* Bump generation so stale handles are rejected */
        cap_revoke_entry_locked(table, idx);
    }
    spin_unlock(&s_cap_lock);
    return OK;
}

/* ---------------------------------------------------------------------------
 * Capability lookup with rights validation
 * --------------------------------------------------------------------------- */
static cap_entry_t *cap_lookup_locked(cap_table_t *table, cap_t handle,
                                      rights_t need)
{
    if (!table || handle == CAP_NULL)
        return NULL;

    u32 idx = (u32)HANDLE_INDEX(handle);
    if (idx >= MAX_CAPS)
        return NULL;

    cap_entry_t *e = &table->entries[idx];

    /* Check slot is in use */
    if (e->type == CAP_TYPE_NONE)
        return NULL;

    /* Validate full handle (including generation) to detect stale caps */
    if (e->handle != handle)
        return NULL;

    /* P0 地基 — lazy expiry: an entry past its absolute tick deadline
     * is revoked in place (same cleanup as cap_revoke) and treated as
     * not found.  No timer, no periodic scan: the check runs only on
     * this lookup path.  0 = permanent, so the existing MEM/IRQ/IO_PORT
     * caps (expiry_ticks == 0) pass through unchanged. */
    if (e->expiry_ticks != 0 && sched_get_ticks() >= e->expiry_ticks) {
        cap_revoke_entry_locked(table, idx);
        return NULL;
    }

    /* Check rights */
    if ((e->rights & need) != need)
        return NULL;

    return e;
}

cap_entry_t *cap_lookup(cap_table_t *table, cap_t handle, rights_t need)
{
    spin_lock(&s_cap_lock);
    cap_entry_t *e = cap_lookup_locked(table, handle, need);
    spin_unlock(&s_cap_lock);
    return e;
}

/* ---------------------------------------------------------------------------
 * Type presence check (for privilege capabilities like DAC_OVERRIDE)
 * --------------------------------------------------------------------------- */
bool cap_has_type(cap_table_t *table, cap_type_t type)
{
    if (!table)
        return false;

    spin_lock(&s_cap_lock);
    bool found = false;
    for (u32 i = 0; i < MAX_CAPS; i++) {
        if (table->entries[i].type == type) {
            found = true;
            break;
        }
    }
    spin_unlock(&s_cap_lock);
    return found;
}

/* ---------------------------------------------------------------------------
 * Table lookup by PID
 * --------------------------------------------------------------------------- */
cap_table_t *cap_get_table(pid_t pid)
{
    if (pid < 0 || pid >= MAX_THREADS)
        return NULL;

    spin_lock(&s_cap_lock);
    cap_table_t *t = s_cap_tables[pid];
    spin_unlock(&s_cap_lock);
    return t;
}

/* ---------------------------------------------------------------------------
 * P0 地基: quota consumption
 * --------------------------------------------------------------------------- */
int cap_consume(cap_table_t *table, cap_t handle)
{
    if (!table)
        return ERR_INVAL;

    spin_lock(&s_cap_lock);

    u32 idx = (u32)HANDLE_INDEX(handle);
    if (idx >= MAX_CAPS) {
        spin_unlock(&s_cap_lock);
        return ERR_INVAL;
    }

    cap_entry_t *e = &table->entries[idx];

    /* Same validation as cap_revoke: missing/stale -> ERR_NOENT */
    if (e->type == CAP_TYPE_NONE) {
        spin_unlock(&s_cap_lock);
        return ERR_NOENT;
    }
    if (e->handle != handle) {
        spin_unlock(&s_cap_lock);
        return ERR_NOENT;
    }

    /* Lazy expiry applies to consumption too: an expired entry is
     * revoked in place and reported as not found. */
    if (e->expiry_ticks != 0 && sched_get_ticks() >= e->expiry_ticks) {
        cap_revoke_entry_locked(table, idx);
        spin_unlock(&s_cap_lock);
        return ERR_NOENT;
    }

    /* quota == 0 is unlimited: nothing to consume */
    if (e->quota == 0) {
        spin_unlock(&s_cap_lock);
        return OK;
    }

    e->quota--;
    if (e->quota == 0) {
        /* Last use consumed: revoke the entry (same cleanup as
         * cap_revoke) so the handle goes stale. */
        cap_revoke_entry_locked(table, idx);
    }
    spin_unlock(&s_cap_lock);
    return OK;
}

/* ---------------------------------------------------------------------------
 * P0 地基: revoke by atom across all kernel cap tables
 * --------------------------------------------------------------------------- */
int cap_revoke_by_atom(subject_id_t subj, atom_id_t atom, u64 scope_hash)
{
    int revoked = 0;

    spin_lock(&s_cap_lock);

    for (u32 p = 0; p < MAX_THREADS; p++) {
        cap_table_t *t = s_cap_tables[p];
        if (!t)
            continue;

        for (u32 i = 0; i < MAX_CAPS; i++) {
            cap_entry_t *e = &t->entries[i];
            if (e->type == CAP_TYPE_NONE)
                continue;
            if (e->subject != subj)
                continue;
            if ((u16)atom != e->atom_id)
                continue;
            if (scope_hash != 0 && e->scope_hash != scope_hash)
                continue;

            cap_revoke_entry_locked(t, i);
            revoked++;
        }
    }

    spin_unlock(&s_cap_lock);
    return revoked;
}
