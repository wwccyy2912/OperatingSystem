/*
 * syscall.c - System call initialization and dispatch
 * Copyright (c) 2026 OpSys Project
 *
 * Single entry point for all syscalls. The assembly stub (syscall_entry.S)
 * saves user registers and calls syscall_dispatch(num, arg1..arg5).
 */

#include <kernel/syscall.h>
#include <kernel/serial.h>
#include <kernel/cap.h>
#include <kernel/ipc.h>
#include <kernel/notify.h>
#include <kernel/irq.h>
#include <kernel/thread.h>
#include <kernel/sched.h>
#include <kernel/process.h>
#include <kernel/syscall_handlers.h>
#include <kernel/vmm.h>
#include <kernel/io.h>
#include <kernel/rtc.h>
#include <kernel/pmm.h>
#include <kernel/mutex.h>
#include <kernel/blob.h>
#include <kernel/framebuffer.h>
#include <kernel/string.h>
#include <kernel/signal.h>
#include <kernel/proc_info.h>
#include <kernel/panic.h>
#include <kernel/pci.h>

/*
 * Validate a user pointer range.  Checks:
 *   - non-zero base, no overflow, entire range below USER_PTR_MAX
 *   - EVERY page in the range is mapped in the current process's
 *     address space (prevents passing kernel addresses or unmapped
 *     addresses that would #PF the kernel)
 *   - when need_write, EVERY page is also writable (the kernel will
 *     write into the range); pass false for read-only user buffers
 */
static bool validate_user_ptr(u64 ptr, u64 size, bool need_write)
{
        process_t *proc = process_current();
        if (!proc || !proc->addr_space)
                return false;
        return vmm_validate_user_range(proc->addr_space, ptr, size, need_write);
}

/* Convenience: cast a validated user pointer */
#define USER_PTR(p)     ((void *)(uintptr_t)(p))

/*
 * Debug log: write a user string to COM1.
 *
 * The user string is copied into a kernel buffer before printing so a
 * non-NUL-terminated or partially-mapped user string can never walk the
 * kernel into a page fault (the original code dereferenced the pointer
 * directly).  The copy is also rate-limited per scheduler tick: serial
 * TX busy-waits on the COM1 LSR, and 0x80 is an interrupt gate (IF=0
 * while inside the syscall), so an unthrottled flood from a hot loop
 * would stall the whole system on the UART.
 */
#define DEBUG_LOG_MAX         512 /* matches userland printf buffer size */
#define DEBUG_LOG_TICK_BUDGET 512 /* bytes of debug output accepted per tick */

static i64 sys_debug_log(u64 arg1)
{
        char buf[DEBUG_LOG_MAX + 1];
        u64 n = 0;
        u64 p = arg1;

        if (arg1 == 0 || arg1 >= USER_PTR_MAX)
                return (i64)ERR_FAULT;

        /* Bounded, page-safe copy: stop at the first NUL; if a page is
     * unmapped or the 512-byte cap is hit, stop and print what we have
     * (never walk unmapped user memory). */
        while (n < DEBUG_LOG_MAX) {
                u64 chunk = PAGE_SIZE - (p & (PAGE_SIZE - 1));
                if (chunk > DEBUG_LOG_MAX - n)
                        chunk = DEBUG_LOG_MAX - n;

                if (!validate_user_ptr(p, chunk, false))
                        break;   /* rest of the string is unmapped — end it here */

                const char *src = (const char *)(uintptr_t)p;
                u64 i = 0;
                while (i < chunk && src[i] != '\0') {
                        buf[n + i] = src[i];
                        i++;
                }
                n += i;
                if (i < chunk)
                        break;   /* NUL found — string ended */
                p += chunk;
        }

        if (n == 0)
                return 0;

        /* Per-tick byte budget: a flooding process must not hold the system
     * inside IF=0 UART busy-waits for more than one tick's worth. */
        static u64 budget_tick = ~0ULL;
        static u64 budget = 0;
        u64 now = sched_get_ticks();
        if (now != budget_tick) {
                budget_tick = now;
                budget = DEBUG_LOG_TICK_BUDGET;
        }
        if (n > budget)
                n = budget;
        if (n == 0)
                return 0;
        budget -= n;

        buf[n] = '\0';
        serial_puts(buf);
        return 0;
}

/*
 * Capability: create a new capability in the caller's table.
 * arg1 = cap_type (0 = CAP_TYPE_KERNEL for backward compat)
 * arg2 = rights bitmask
 * arg3 = obj_id — the object this capability names:
 *          CAP_TYPE_IRQ    → IRQ line number (must equal the line passed
 *                             to bind_irq/unbind_irq)
 *          CAP_TYPE_IO_PORT → port range, encoded as
 *                             (count << 16) | base_port; an access to
 *                             port P is allowed iff base <= P < base+count.
 *          other types     → unused (0)
 */
static i64 sys_cap_create(u64 type, u64 rights, u64 obj_id)
{
        process_t *proc = process_current();
        if (!proc || !proc->cap_table)
                return (i64)ERR_FAULT;

        cap_type_t cap_type = (type == 0) ? CAP_TYPE_KERNEL : (cap_type_t)type;

        /* Validate obj_id for typed caps so a bogus range cannot be stored. */
        if (cap_type == CAP_TYPE_IRQ && obj_id >= 16)
                return (i64)ERR_INVAL;
        if (cap_type == CAP_TYPE_PCI_DEV && obj_id >= (u64)pci_device_count())
                return (i64)ERR_INVAL;   /* obj_id must be a live PCI table index */

        /* Security: CAP_TYPE_DAC_OVERRIDE is a purely privileged capability
     * that bypasses DAC/credential checks (Linux CAP_DAC_OVERRIDE style).
     * Allowing any user process to self-mint it completely breaks the
     * permission model — it must only be kernel-issued (e.g. via
     * cap_create_in_table from kernel code), never created via this
     * syscall.  Reject it with ERR_DENIED.
     *
     * P2 plan (docs/permission_model.md §一): sys_cap_create itself will
     * be retired in favour of sys_cap_grant_to_subject, so the perm-engine
     * becomes the sole signer of all capabilities. */
        if (cap_type == CAP_TYPE_DAC_OVERRIDE)
                return (i64)ERR_DENIED;

        cap_t handle = cap_create_in_table(
                proc->cap_table, cap_type, (rights_t)rights, obj_id, 0);
        if (handle == CAP_NULL)
                return (i64)ERR_NOMEM;

        return (i64)handle;
}

/*
 * Capability: grant a handle from caller to a target process.
 */
static i64 sys_cap_grant(u64 handle, u64 target_pid, u64 rights)
{
        process_t *src_proc = process_current();
        if (!src_proc || !src_proc->cap_table)
                return (i64)ERR_FAULT;

        process_t *dst_proc = process_get((pid_t)target_pid);
        if (!dst_proc || !dst_proc->cap_table)
                return (i64)ERR_NOENT;

        cap_t new_handle = cap_grant(
                src_proc->cap_table, dst_proc->cap_table,
                (cap_t)handle, (rights_t)rights);
        if (new_handle == CAP_NULL)
                return (i64)ERR_DENIED;

        return (i64)new_handle;
}

/*
 * Capability: revoke a handle from the caller's table.
 */
static i64 sys_cap_revoke(u64 handle)
{
        process_t *proc = process_current();
        if (!proc || !proc->cap_table)
                return (i64)ERR_FAULT;

        error_t err = cap_revoke(proc->cap_table, (cap_t)handle);
        return (i64)err;
}

/*
 * P0 地基: create an atom capability in the caller's table with
 * entry.subject = the caller's subject_id (docs/permission_model.md
 * §三/§五).  GATED (docs/ops_format.md §6): the caller must hold
 * ATOM_CAP_GRANT_SELF — an app cannot self-issue atoms it was never
 * granted.
 * arg1 = atom_id, arg2 = rights, arg3 = expiry_ticks (0 = permanent),
 * arg4 = quota (0 = unlimited), arg5 = scope_hash (0 = unrestricted).
 */
static i64 sys_cap_create_atom(u64 atom, u64 rights, u64 expiry_ticks,
                               u64 quota, u64 scope_hash)
{
        if (atom >= ATOM_MAX)
                return (i64)ERR_INVAL;
        if ((rights & ~(u64)RIGHT_ALL) != 0)
                return (i64)ERR_INVAL;

        process_t *proc = process_current();
        if (!proc || !proc->cap_table)
                return (i64)ERR_FAULT;

        /* GATE FIRST (docs/ops_format.md §6): a live ATOM_CAP_GRANT_SELF
         * cap held by the caller's subject is the entire authorization.
         * Same pattern as sys_set_time (cap_lookup_by_atom gate). */
        if (cap_lookup_by_atom(proc->cap_table, proc->subject_id,
                           ATOM_CAP_GRANT_SELF, 0) == CAP_NULL)
                return (i64)ERR_NOCAP;

        cap_t handle = CAP_NULL;
        int err = cap_create_atom(proc->cap_table, proc->subject_id,
                                                            (atom_id_t)atom, (rights_t)rights,
                                                            expiry_ticks, (u32)quota, scope_hash, &handle);
        if (err != OK)
                return (i64)err;
        return (i64)handle;
}

/*
 * P0 地基: consume one quota unit on the caller's own table entry
 * (SYS_CAP_CONSUME).  Returns 0 on success, or the same errors
 * cap_revoke reports (ERR_NOENT missing/stale/expired, ERR_INVAL bad
 * index).  Gating to perm-engine lands in P1.
 */
static i64 sys_cap_consume(u64 handle)
{
        process_t *proc = process_current();
        if (!proc || !proc->cap_table)
                return (i64)ERR_FAULT;

        return (i64)cap_consume(proc->cap_table, (cap_t)handle);
}

/*
 * P0 地基: revoke every capability held by `subject` matching `atom`
 * across ALL kernel cap tables (SYS_CAP_REVOKE_BY_ATOM).  Returns the
 * number of entries revoked (>= 0), or ERR_INVAL for a bad atom.
 * GATED (docs/ops_format.md §6): the CALLER must hold
 * ATOM_SERVICE_MANAGE — the `subject` argument is the revocation
 * target, not the requester.
 */
static i64 sys_cap_revoke_by_atom(u64 subject, u64 atom, u64 scope_hash)
{
        if (atom >= ATOM_MAX)
                return (i64)ERR_INVAL;

        process_t *proc = process_current();
        if (!proc || !proc->cap_table)
                return (i64)ERR_FAULT;

        /* GATE FIRST (docs/ops_format.md §6): look up the atom in the
         * CALLER's own cap table (same sys_set_time gate pattern). */
        if (cap_lookup_by_atom(proc->cap_table, proc->subject_id,
                           ATOM_SERVICE_MANAGE, 0) == CAP_NULL)
                return (i64)ERR_NOCAP;

        return (i64)cap_revoke_by_atom((subject_id_t)subject,
                                   (atom_id_t)atom, scope_hash);
}

/*
 * P1 地基: issue an atom capability to the process holding `subject`
 * (SYS_CAP_GRANT_TO_SUBJECT).  Resolves the target process by subject
 * and creates the atom cap in ITS table with entry.subject = the target
 * subject — the perm-engine's decision-encoding path (§四: 决策下沉,
 * 授予路径异步).  GATED (docs/ops_format.md §6): the caller must hold
 * ATOM_SERVICE_MANAGE — a service can only sign atoms if it was
 * kernel-endorsed (blob identity) or is the device owner.
 * arg1 = subject, arg2 = atom, arg3 = rights, arg4 = expiry_ticks
 * (0 = permanent), arg5 = quota (0 = unlimited).
 */
static i64 sys_cap_grant_to_subject(u64 subject, u64 atom, u64 rights,
                                                                        u64 expiry_ticks, u64 quota)
{
        if (subject == 0)
                return (i64)ERR_NOENT;
        if (atom >= ATOM_MAX)
                return (i64)ERR_INVAL;
        if ((rights & ~(u64)RIGHT_ALL) != 0)
                return (i64)ERR_INVAL;

        process_t *proc = process_current();
        if (!proc || !proc->cap_table)
                return (i64)ERR_FAULT;

        /* GATE FIRST (docs/ops_format.md §6): a live ATOM_SERVICE_MANAGE
         * cap held by the CALLER's subject is the entire authorization
         * (same sys_set_time gate pattern). */
        if (cap_lookup_by_atom(proc->cap_table, proc->subject_id,
                           ATOM_SERVICE_MANAGE, 0) == CAP_NULL)
                return (i64)ERR_NOCAP;

        cap_t handle = CAP_NULL;
        int err = cap_grant_to_subject((subject_id_t)subject,
                                   (atom_id_t)atom, (rights_t)rights,
                                   expiry_ticks, (u32)quota, 0, &handle);
        if (err != OK)
                return (i64)err;
        return (i64)handle;
}

/*
 * P1 地基: read-only atom-holding query (SYS_CAP_HAS_ATOM).  Returns 1
 * when the process holding `subject` has a live atom cap for `atom` in
 * its kernel table, 0 when it does not.  GATED (docs/ops_format.md §6):
 * the CALLER must hold ATOM_SERVICE_MANAGE — only management-plane
 * processes may inspect another subject's atom holdings.
 */
static i64 sys_cap_has_atom(u64 subject, u64 atom)
{
        if (atom >= ATOM_MAX)
                return (i64)ERR_INVAL;

        process_t *proc = process_current();
        if (!proc || !proc->cap_table)
                return (i64)ERR_FAULT;

        /* GATE FIRST (docs/ops_format.md §6): caller must hold
         * ATOM_SERVICE_MANAGE (same sys_set_time gate pattern). */
        if (cap_lookup_by_atom(proc->cap_table, proc->subject_id,
                           ATOM_SERVICE_MANAGE, 0) == CAP_NULL)
                return (i64)ERR_NOCAP;

        process_t *dst = process_get_by_subject((subject_id_t)subject);
        if (!dst || !dst->cap_table)
                return (i64)ERR_NOENT;

        return (cap_lookup_by_atom(dst->cap_table, (subject_id_t)subject,
                                   (atom_id_t)atom, 0) != CAP_NULL) ? 1 : 0;
}

/*
 * IPC: send a message to a port.
 */
static i64 sys_ipc_send(u64 port, u64 msg, u64 len)
{
        if (!validate_user_ptr(msg, len, false))   /* kernel reads msg */
                return (i64)ERR_FAULT;

        error_t err = ipc_send((port_t)port, USER_PTR(msg), (u32)len);
        return (i64)err;
}

/*
 * IPC: receive a message from a port.
 * arg3 points to an in/out length value in user memory.
 * arg4 (optional) points to a u32 reply-token out value: set to a
 * non-zero token when the received message was a call (must be passed
 * unchanged to SYS_IPC_REPLY), 0 for a plain send.
 */
static i64 sys_ipc_recv(u64 port, u64 buf, u64 len_ptr, u64 tok_ptr)
{
        if (!validate_user_ptr(len_ptr, sizeof(u32), true))  /* kernel writes len */
                return (i64)ERR_FAULT;

        u32 *user_len = (u32 *)USER_PTR(len_ptr);
        u32 len = *user_len;

        if (buf != 0 && !validate_user_ptr(buf, len, true))  /* kernel writes buf */
                return (i64)ERR_FAULT;

        u32 *user_tok = NULL;
        if (tok_ptr != 0) {
                if (!validate_user_ptr(tok_ptr, sizeof(u32), true))  /* kernel writes token */
                        return (i64)ERR_FAULT;
                user_tok = (u32 *)USER_PTR(tok_ptr);
        }

        error_t err = ipc_recv((port_t)port, USER_PTR(buf), &len, user_tok);
        if (err == OK)
                *user_len = len;
        return (i64)err;
}

/*
 * IPC (P0 地基): receive with sender identity (SYS_IPC_RECV_FROM).
 * Identical semantics to sys_ipc_recv PLUS: when arg5 (sender_subject_ptr)
 * is non-NULL, the kernel-filled, unforgeable sender subject is written
 * to it (docs/permission_model.md §三).  NULL is tolerated (skips the
 * write).  The user-pointer check is the same one sys_ipc_recv uses.
 */
static i64 sys_ipc_recv_from(u64 port, u64 buf, u64 len_ptr, u64 tok_ptr,
                             u64 subj_ptr)
{
        if (!validate_user_ptr(len_ptr, sizeof(u32), true))  /* kernel writes len */
                return (i64)ERR_FAULT;

        u32 *user_len = (u32 *)USER_PTR(len_ptr);
        u32 len = *user_len;

        if (buf != 0 && !validate_user_ptr(buf, len, true))  /* kernel writes buf */
                return (i64)ERR_FAULT;

        u32 *user_tok = NULL;
        if (tok_ptr != 0) {
                if (!validate_user_ptr(tok_ptr, sizeof(u32), true))  /* kernel writes token */
                        return (i64)ERR_FAULT;
                user_tok = (u32 *)USER_PTR(tok_ptr);
        }

        u64 *user_subj = NULL;
        if (subj_ptr != 0) {
                if (!validate_user_ptr(subj_ptr, sizeof(u64), true))  /* kernel writes subject */
                        return (i64)ERR_FAULT;
                user_subj = (u64 *)USER_PTR(subj_ptr);
        }

        subject_id_t subj = 0;
        error_t err = ipc_recv_from((port_t)port, USER_PTR(buf), &len,
                                                                user_tok, &subj);
        if (err == OK) {
                *user_len = len;
                if (user_subj)
                        *user_subj = subj;
        }
        return (i64)err;
}

/*
 * IPC: synchronous call (send request, wait for response).
 * arg5 points to an in/out response length value.
 */
static i64 sys_ipc_call(u64 port, u64 req, u64 req_len,
                         u64 resp, u64 resp_len_ptr)
{
        if (!validate_user_ptr(req, req_len, false))  /* kernel reads request */
                return (i64)ERR_FAULT;

        if (!validate_user_ptr(resp_len_ptr, sizeof(u32), true))  /* kernel writes resp_len */
                return (i64)ERR_FAULT;

        u32 *user_resp_len = (u32 *)USER_PTR(resp_len_ptr);
        u32 resp_len = *user_resp_len;

        if (resp != 0 && !validate_user_ptr(resp, resp_len, true))  /* kernel writes resp */
                return (i64)ERR_FAULT;

        error_t err = ipc_call(
                (port_t)port, USER_PTR(req), (u32)req_len,
                USER_PTR(resp), &resp_len);
        *user_resp_len = resp_len;
        return (i64)err;
}

/*
 * IPC: create a new port owned by the current thread.
 */
static i64 sys_ipc_port_create(void)
{
        port_t port = ipc_port_create();
        return (i64)port;
}

/*
 * IPC: reply to a pending call (send response to the caller).
 * arg1 = reply token (opaque handle returned by SYS_IPC_RECV for a
 * call message), arg2 = response buffer, arg3 = response length.
 */
static i64 sys_ipc_reply(u64 token, u64 buf, u64 len)
{
        if (!validate_user_ptr(buf, len, false))  /* kernel reads reply buffer */
                return (i64)ERR_FAULT;

        error_t err = ipc_reply((u32)token, USER_PTR(buf), (u32)len);
        return (i64)err;
}

/*
 * Memory: map physical pages into the caller's address space.
 * Simplified v0.1: allocates physical pages and maps them.
 */
static i64 sys_map_memory(u64 cap_handle, u64 virt, u64 size, u64 prot)
{
        process_t *proc = process_current();
        if (!proc || !proc->cap_table)
                return 0;

        /* Validate capability: must hold a MEM cap with WRITE right */
        cap_entry_t *cap = cap_lookup(proc->cap_table, (cap_t)cap_handle, RIGHT_WRITE);
        if (!cap)
                return 0;
        if (cap->type != CAP_TYPE_MEM)
                return 0;

        /* Address range: must be in user space */
        if (virt == 0 || virt >= USER_PTR_MAX)
                return 0;
        /* Overflow-safe form: virt < USER_PTR_MAX is guaranteed above, so
     * USER_PTR_MAX - virt cannot underflow.  A plain virt + size could
     * wrap past USER_PTR_MAX and bypass the user-space range check,
     * letting a caller reach the shared kernel-half page tables. */
        if (size > USER_PTR_MAX - virt)
                return 0;

        if (!proc->addr_space)
                return 0;

        if (size == 0 || (virt % PAGE_SIZE) != 0)
                return 0;

        u64 page_count = size / PAGE_SIZE;
        if (page_count * PAGE_SIZE != size)
                return 0;

        /* Heap guard pages: the per-process heap (ASLR, design item ⑭)
     * occupies [heap_base, heap_base + HEAP_USER_SIZE); malloc.c grows
     * upward from heap_base.  Refuse mappings that touch the guard page
     * below the base or the guard page at the max, so a heap
     * overflow/underflow always faults on an unmapped page instead of
     * silently corrupting adjacent user memory. */
        u64 hbase = proc->heap_base;
        u64 hmax  = proc->heap_base + HEAP_USER_SIZE;
        if (virt < hbase && virt + size > hbase - PAGE_SIZE)
                return 0;   /* overlaps low guard page */
        if (virt < hmax + PAGE_SIZE && virt + size > hmax)
                return 0;   /* overlaps high guard page */

        /* Build PTE flags from protection bits */
        u64 flags = PTE_PRESENT | PTE_USER;
        if (prot & PROT_WRITE)
                flags |= PTE_WRITABLE;
        if (!(prot & PROT_EXEC))
                flags |= PTE_NO_EXECUTE;

        error_t err = OK;
        for (u64 i = 0; i < page_count; i++) {
                u64 v = virt + i * PAGE_SIZE;
                err = vmm_alloc_and_map(proc->addr_space, v, flags);
                if (err != OK)
                        return 0;
        }

        return (i64)virt;
}

/*
 * Memory: unmap pages in the caller's address space and free physical pages.
 */
static i64 sys_unmap_memory(u64 virt, u64 size)
{
        /* Validate user address range */
        if (virt == 0 || virt >= USER_PTR_MAX)
                return (i64)ERR_INVAL;
        /* Overflow-safe form: see sys_map_memory. */
        if (size > USER_PTR_MAX - virt)
                return (i64)ERR_INVAL;

        process_t *proc = process_current();
        if (!proc || !proc->addr_space)
                return (i64)ERR_FAULT;

        if (size == 0 || (virt % PAGE_SIZE) != 0)
                return (i64)ERR_INVAL;

        /* Heap guard pages: refuse to unmap the guard pages around the
     * per-process heap region (see sys_map_memory).  They can never be
     * mapped, so an attempt to unmap them is always an error — keep the
     * boundary intact. */
        u64 hbase = proc->heap_base;
        u64 hmax  = proc->heap_base + HEAP_USER_SIZE;
        if (virt < hbase && virt + size > hbase - PAGE_SIZE)
                return (i64)ERR_INVAL;
        if (virt < hmax + PAGE_SIZE && virt + size > hmax)
                return (i64)ERR_INVAL;

        u64 page_count = size / PAGE_SIZE;

        error_t err = vmm_unmap_range(proc->addr_space, virt, page_count);
        return (i64)err;
}

/*
 * Framebuffer: return the user-facing fb descriptor.
 * arg1 = user pointer to a fb_user_info_t (kernel writes it).
 * Returns 0 on success, or a negative error.
 */
static i64 sys_fb_get_info(u64 buf_ptr)
{
        fb_user_info_t info;
        if (fb_get_user_info(&info) < 0)
                return (i64)ERR_NOENT;

        if (!validate_user_ptr(buf_ptr, sizeof(fb_user_info_t), true))
                return (i64)ERR_FAULT;
        memcpy(USER_PTR(buf_ptr), &info, sizeof(fb_user_info_t));
        return 0;
}

/*
 * Framebuffer: map the framebuffer's physical pages into the caller's
 * address space at a user-chosen virtual address.
 * arg1 = page-aligned user virtual address, arg2 = size in bytes
 * (page-aligned).  The size is clamped to the framebuffer's real size,
 * so a caller cannot map past the physical framebuffer region.
 * Returns the virtual address on success, or a negative error.
 */
static i64 sys_fb_map(u64 virt, u64 size)
{
        fb_user_info_t info;
        if (fb_get_user_info(&info) < 0)
                return (i64)ERR_NOENT;

        process_t *proc = process_current();
        if (!proc || !proc->addr_space)
                return (i64)ERR_FAULT;

        /* Address range: must be in user space, page-aligned size */
        if (virt == 0 || virt >= USER_PTR_MAX)
                return (i64)ERR_INVAL;
        /* Overflow-safe form: see sys_map_memory. */
        if (size > USER_PTR_MAX - virt)
                return (i64)ERR_INVAL;
        if (size == 0 || (virt % PAGE_SIZE) != 0 || (size % PAGE_SIZE) != 0)
                return (i64)ERR_INVAL;

        /* Heap guard pages: mirror sys_map_memory's refusal to map the
     * per-process heap guard range. */
        u64 hbase = proc->heap_base;
        u64 hmax  = proc->heap_base + HEAP_USER_SIZE;
        if (virt < hbase && virt + size > hbase - PAGE_SIZE)
                return (i64)ERR_INVAL;   /* overlaps low guard page */
        if (virt < hmax + PAGE_SIZE && virt + size > hmax)
                return (i64)ERR_INVAL;   /* overlaps high guard page */

        /* Real framebuffer size: VGA text mode is one page at 0xB8000;
     * linear mode is pitch * height.  Clamp the mapping to it. */
        u64 fb_size;
        if (info.vga_text) {
                fb_size = PAGE_SIZE;             /* 0xB8000..0xB8FFF */
        } else {
                fb_size = (u64)info.pitch * info.height;
                fb_size = (fb_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        }
        if (size > fb_size)
                return (i64)ERR_INVAL;

        /* Map the fb physical pages read/write, non-executable */
        u64 flags = PTE_PRESENT | PTE_USER | PTE_WRITABLE | PTE_NO_EXECUTE;
        u64 page_count = size / PAGE_SIZE;
        for (u64 i = 0; i < page_count; i++) {
                error_t err = vmm_map(proc->addr_space,
                                                            virt + i * PAGE_SIZE,
                                                            info.phys_addr + i * PAGE_SIZE,
                                                            flags);
                if (err != OK) {
                        /* Partial map on failure: unmap what we mapped, then bail */
                        vmm_unmap_range(proc->addr_space, virt, i);
                        return (i64)err;
                }
        }
        return (i64)virt;
}

/*
 * Thread: create a new user thread in the current process.
 */
static i64 sys_thread_create(u64 entry, u64 arg, u64 priority)
{
        /* Entry must be a user-mode address */
        if (entry == 0 || entry >= USER_PTR_MAX)
                return (i64)ERR_INVAL;

        process_t *proc = process_current();
        if (!proc || !proc->addr_space)
                return (i64)ERR_FAULT;

        tid_t tid = thread_create_user(
                entry, arg, proc->addr_space, (int)priority);
        if (tid < 0)
                return (i64)tid;

        /* Back-link the thread to this process (mirrors process.c).
     * Without this every spawned thread has pid=0, process_current()
     * resolves to the kernel process (cap_table == NULL) and all
     * capability-gated syscalls fail from it. */
        thread_t *t = thread_get(tid);
        if (t) {
                t->pid = proc->pid;
                /* Count the spawned thread.  Every user thread exits through
         * thread_exit() -> process_thread_exited(), which decrements
         * thread_count; without this increment the count would hit
         * zero as soon as the first spawned thread exits (premature
         * PROC_STATE_ZOMBIE) and keep drifting negative. */
                proc->thread_count++;
        }

        return (i64)tid;
}

/*
 * Thread: exit the current thread.
 * Should never return.
 */
static i64 sys_thread_exit(u64 code)
{
        thread_exit((int)code);
        return 0; /* unreachable, satisfies compiler */
}

/*
 * Thread: yield the CPU to the scheduler.
 */
static i64 sys_thread_yield(void)
{
        thread_yield();
        return 0;
}

/*
 * Thread: set CPU affinity.
 */
static i64 sys_thread_set_affinity(u64 tid, u64 cpu)
{
        error_t err = thread_set_affinity((tid_t)tid, (i32)cpu);
        return (i64)err;
}

/*
 * Thread: wait for another thread to finish.
 */
static i64 sys_thread_join(u64 tid, u64 exit_code_ptr)
{
        thread_t *target = thread_get((tid_t)tid);
        if (!target)
                return (i64)ERR_NOENT;

        /* Can't join yourself */
        thread_t *cur = thread_current();
        if (cur && cur->tid == (tid_t)tid)
                return (i64)ERR_INVAL;

        /* Target already finished — collect immediately */
        if (target->state == THREAD_STATE_FINISHED) {
                if (exit_code_ptr != 0) {
                        if (!validate_user_ptr(exit_code_ptr, sizeof(int), true))  /* kernel writes */
                                return (i64)ERR_FAULT;
                        *(int *)USER_PTR(exit_code_ptr) = target->exit_code;
                }
                thread_release(target);
                return 0;
        }

        /* Someone else already joining this thread */
        if (target->joiner_tid >= 0)
                return (i64)ERR_BUSY;

        /* Block until target finishes */
        target->joiner_tid = cur->tid;
        cur->state = THREAD_STATE_BLOCKED;
        sched_dequeue(cur);
        sched_reschedule();  /* Pure switch — returns when we're unblocked */

        /* We're awake now — target finished */
        if (exit_code_ptr != 0) {
                if (!validate_user_ptr(exit_code_ptr, sizeof(int), true))  /* kernel writes */
                        return (i64)ERR_FAULT;
                *(int *)USER_PTR(exit_code_ptr) = target->exit_code;
        }
        thread_release(target);
        return 0;
}

/*
 * Time: get the current tick count.
 */
static i64 sys_get_time(void)
{
        return (i64)sched_get_ticks();
}

/*
 * Time: sleep for the given number of ticks.
 *
 * The argument arrives as u64, but the user-side API is int (signed).
 * Negative values (e.g. sleep(-10)) are sign-extended to 0xFFFF...FFF6
 * and, read as u64, would park the thread until s_ticks wraps around —
 * i.e. effectively forever.  Reject anything outside the positive int
 * range so a bad argument can never wedge a thread in the sleep queue.
 */
static i64 sys_sleep(u64 ticks)
{
        if (ticks == 0 || ticks > (u64)0x7FFFFFFF)
                return (i64)ERR_INVAL;
        sched_sleep((u64)ticks);
        return 0;
}

/*
 * Time: read the wall clock (RTC/CMOS) into a user rtc_time_t.
 * arg1 = user pointer to rtc_time_t {u16 year; u8 month; u8 day;
 *                                    u8 hour; u8 minute; u8 second}.
 */
static i64 sys_rtc_time(u64 out_ptr)
{
        if (out_ptr == 0)
                return (i64)ERR_FAULT;
        if (!validate_user_ptr(out_ptr, sizeof(rtc_time_t), true))  /* kernel writes */
                return (i64)ERR_FAULT;

        rtc_read((rtc_time_t *)USER_PTR(out_ptr));
        return 0;
}

/*
 * P2 地基: set the wall clock (CMOS RTC), gated by ATOM_SYS_SET_TIME
 * (docs/permission_model.md §四: 决策下沉, 能力即决策).  The gate is a
 * pure kernel cap-table lookup — ZERO IPC to user space — and runs
 * BEFORE any user memory is touched or any CMOS access happens.
 * arg1 = user pointer to rtc_time_t {u16 year; u8 month; u8 day;
 *                                    u8 hour; u8 minute; u8 second}.
 * Unauthorized callers get ERR_NOCAP.
 */
static i64 sys_set_time(u64 user_t_ptr)
{
        process_t *proc = process_current();
        if (!proc || !proc->cap_table)
                return (i64)ERR_FAULT;

        /* GATE FIRST: a live ATOM_SYS_SET_TIME cap held by the caller's
     * subject is the entire authorization.  The cap table IS the
     * decision cache; no IPC, no user-space policy query. */
        if (cap_lookup_by_atom(proc->cap_table, proc->subject_id,
                           ATOM_SYS_SET_TIME, 0) == CAP_NULL)
                return (i64)ERR_NOCAP;

        if (user_t_ptr == 0)
                return (i64)ERR_FAULT;
        if (!validate_user_ptr(user_t_ptr, sizeof(rtc_time_t), false))
                return (i64)ERR_FAULT;   /* kernel reads time struct */

        rtc_time_t t;
        memcpy(&t, USER_PTR(user_t_ptr), sizeof(rtc_time_t));

        /* Range validation before any CMOS access. */
        if (t.year < 1970 || t.month < 1 || t.month > 12 ||
                t.day < 1 || t.day > 31 || t.hour > 23 ||
                t.minute > 59 || t.second > 59)
                return (i64)ERR_INVAL;

        rtc_write(&t);
        return 0;
}

/*
 * Port: register a well-known port by name.
 */
static i64 sys_port_register(u64 name, u64 port)
{
        if (!validate_user_ptr(name, 1, false))  /* kernel reads name string */
                return (i64)ERR_FAULT;

        error_t err = ipc_register_port((const char *)USER_PTR(name),
                                                                        (port_t)port);
        return (i64)err;
}

/*
 * Port: look up a well-known port by name.
 */
static i64 sys_port_get(u64 name)
{
        if (!validate_user_ptr(name, 1, false))  /* kernel reads name string */
                return (i64)ERR_FAULT;

        port_t port = ipc_get_port((const char *)USER_PTR(name));
        return (i64)port;
}

/*
 * Init protocol: return number of free physical pages.
 */
static i64 sys_get_free_pages(void)
{
        return (i64)(pmm_get_free_memory() / PAGE_SIZE);
}

/*
 * Init protocol: return current process PID.
 */
static i64 sys_get_pid(void)
{
        process_t *proc = process_current();
        if (!proc)
                return (i64)ERR_FAULT;
        return (i64)proc->pid;
}

/*
 * P0 地基: return the caller's kernel-issued subject ID
 * (SYS_GET_SUBJECT, docs/permission_model.md §三).  Unforgeable:
 * it comes from the PCB, not from any user-supplied value.
 */
static i64 sys_get_subject(void)
{
        process_t *proc = process_current();
        if (!proc)
                return (i64)ERR_FAULT;
        return (i64)proc->subject_id;
}

/*
 * ASLR: return the current process's randomized heap base (design
 * item ⑭).  User-space malloc.c calls this once on first heap grow so
 * its HEAP_BASE matches the kernel's per-process choice.  Returns 0
 * (never a valid base) on failure.
 */
static i64 sys_get_heap_base(void)
{
        process_t *proc = process_current();
        if (!proc)
                return (i64)ERR_FAULT;
        return (i64)proc->heap_base;
}

/*
 * Process: wait for a process to exit.
 * arg1 = PID to wait on, arg2 = pointer to an int exit code (may be 0).
 * Blocks until the target process's last thread exits, then returns the
 * PID and stores the exit code, and REAPS the process (its address
 * space, capability table and table slot are freed).  Returns a
 * negative error otherwise:
 *   ERR_NOENT — no such process (or already reaped)
 *   ERR_INVAL — waiting on the caller's own process (would deadlock)
 *   ERR_BUSY  — another thread is already waiting on this process
 *   ERR_FAULT — bad exit-code pointer
 */
static i64 sys_process_wait(u64 pid, u64 exit_code_ptr)
{
        process_t *proc = process_get((pid_t)pid);
        if (!proc)
                return (i64)ERR_NOENT;

        /* Can't wait on your own process — its threads never all exit
     * while the caller is blocked in here. */
        process_t *cur_proc = process_current();
        if (cur_proc && proc->pid == cur_proc->pid)
                return (i64)ERR_INVAL;

        /* Process already finished — claim it and collect immediately.
     * Validate the pointer before claiming so a bad buffer does not
     * leave the zombie unclaimed. */
        if (proc->state == PROC_STATE_ZOMBIE) {
                if (exit_code_ptr != 0 &&
                        !validate_user_ptr(exit_code_ptr, sizeof(int), true))
                        return (i64)ERR_FAULT;   /* kernel writes */
                if (proc->waiting_tid >= 0)
                        return (i64)ERR_BUSY;   /* a woken waiter owns the reap */

                if (exit_code_ptr != 0)
                        *(int *)USER_PTR(exit_code_ptr) = proc->exit_code;
                pid_t out_pid = proc->pid;
                process_reap(proc);
                return (i64)out_pid;
        }

        /* Another thread is already waiting on this process. */
        if (proc->waiting_tid >= 0)
                return (i64)ERR_BUSY;

        /* Block until the process's last thread exits.  The waker is
     * process_thread_exited(), called from thread_exit(). */
        thread_t *cur = thread_current();
        proc->waiting_tid = cur->tid;
        cur->state = THREAD_STATE_BLOCKED;
        sched_dequeue(cur);
        sched_reschedule();  /* Pure switch — returns when we're woken */

        /* Awake: the process finished while we were blocked.  Only the
     * thread that owns waiting_tid may collect and reap.  (If the
     * waiter vanished while blocked, process_thread_exited() reaped
     * the process as an orphan; that path is unreachable from here.) */
        if (proc->waiting_tid != cur->tid)
                return (i64)ERR_NOENT;
        proc->waiting_tid = -1;

        if (exit_code_ptr != 0) {
                if (!validate_user_ptr(exit_code_ptr, sizeof(int), true)) {
                        /* We claimed the process (waiting_tid cleared above) and
             * nothing else will reap it — free it even though the exit
             * code cannot be delivered. */
                        process_reap(proc);
                        return (i64)ERR_FAULT;   /* kernel writes */
                }
                *(int *)USER_PTR(exit_code_ptr) = proc->exit_code;
        }
        pid_t out_pid = proc->pid;
        process_reap(proc);
        return (i64)out_pid;
}

/*
 * Process: enumerate the user process table (SYS_PROCESS_LIST).
 * arg1 = user buffer of proc_info_t entries, arg2 = max entries.
 * Returns the number of processes written (0..max_entries), or a
 * negative error (ERR_FAULT bad buffer).
 */
static i64 sys_process_list(u64 buf_ptr, u64 max_entries)
{
        if (max_entries == 0)
                return 0;
        if (max_entries > MAX_THREADS)
                max_entries = MAX_THREADS;
        if (!validate_user_ptr(buf_ptr, max_entries * sizeof(proc_info_t), true))
                return (i64)ERR_FAULT;
        i64 n = process_list_fill((proc_info_t *)USER_PTR(buf_ptr),
                                                            (u32)max_entries);
        serial_printf("proc: SYS_LIST n=%d max=%d\n", (int)n, (int)max_entries);
        return n;
}

/*
 * Process: resolve a subject_id to its kernel-issued identity
 * (SYS_PROC_INFO_BY_SUBJECT).  arg1 = subject_id, arg2 = user
 * proc_ident_t pointer.  Unit 1 (TUI 权限查询): app identity moves
 * from a forgeable self-reported u32 app_id_hash to the kernel-issued
 * App Subject (uuid), allocated at app instantiation
 * (docs/permission_model.md §三).  Returns 0 and fills the record for
 * a live user process; ERR_NOENT for subject 0 (kernel), an unknown
 * subject, or a dead process; ERR_FAULT on a bad output pointer.
 */
static i64 sys_proc_info_by_subject(u64 subject, u64 out_ptr)
{
        if (out_ptr == 0 || !validate_user_ptr(out_ptr, sizeof(proc_ident_t), true))
                return (i64)ERR_FAULT;   /* kernel writes */

        process_t *proc = process_get_by_subject((subject_id_t)subject);
        if (!proc || proc->state == PROC_STATE_ZOMBIE)
                return (i64)ERR_NOENT;   /* kernel/unknown/dead */

        proc_ident_t ident;
        memset(&ident, 0, sizeof(ident));   /* zeroed name tail: NUL-terminated */
        ident.pid     = (i32)proc->pid;
        ident.uuid_hi = proc->app_uuid_hi;
        ident.uuid_lo = proc->app_uuid_lo;
        for (size_t i = 0; i < sizeof(ident.name) - 1 && proc->name[i]; i++)
                ident.name[i] = proc->name[i];

        memcpy(USER_PTR(out_ptr), &ident, sizeof(ident));
        serial_printf("proc: SYS_PROC_INFO subj=%d pid=%d uuid=%016x%016x\n",
                                    (int)subject, (int)proc->pid, ident.uuid_hi, ident.uuid_lo);
        return 0;
}

/*
 * Blob: fetch an embedded ELF image by name into caller memory.
 * arg1 = blob name (user string), arg2 = destination buffer,
 * arg3 = buffer size.  Returns the blob size in bytes, or a
 * negative error (ERR_NOENT unknown blob, ERR_OVERFLOW buffer
 * too small, ERR_FAULT bad pointers).
 */
static i64 sys_blob_get(u64 name_ptr, u64 buf_ptr, u64 buf_size)
{
        /* Validate the blob name: bounded 32-byte read below. */
        if (name_ptr == 0 || !validate_user_ptr(name_ptr, BLOB_NAME_MAX, false))  /* kernel reads */
                return (i64)ERR_FAULT;

        /* Validate the destination buffer range (every page mapped). */
        if (buf_ptr == 0 || buf_size == 0 || !validate_user_ptr(buf_ptr, buf_size, true))  /* kernel writes */
                return (i64)ERR_FAULT;

        /* Copy the name (bounded; all 32 bytes validated mapped above). */
        char name[BLOB_NAME_MAX];
        const char *u = (const char *)USER_PTR(name_ptr);
        u64 i;
        for (i = 0; i < sizeof(name) - 1 && u[i]; i++)
                name[i] = u[i];
        name[i] = '\0';

        /* Look up the blob and copy it out. */
        const void *data;
        u64 size;
        int err = blob_get(name, &data, &size);
        if (err != OK)
                return (i64)err;

        if (size > buf_size)
                return (i64)ERR_OVERFLOW;

        memcpy(USER_PTR(buf_ptr), data, (size_t)size);
        return (i64)size;
}

/* ---- Serial I/O ---- */

static i64 sys_debug_getchar(void)
{
        char c = serial_getchar();
        return (i64)(unsigned char)c;
}

/*
 * Notification: OR a bitmask into a target thread's pending signals.
 */
static i64 sys_notify(u64 target_tid, u64 mask)
{
        return (i64)notify((tid_t)target_tid, (u32)mask);
}

/*
 * Notification: wait for (or poll) a set of signal bits.
 */
static i64 sys_wait_notification(u64 mask)
{
        return (i64)wait_notification((u32)mask);
}

/*
 * IRQ: bind the calling thread to an IRQ line.
 * arg1 = IRQ capability handle, arg2 = IRQ number, arg3 = notification mask.
 */
static i64 sys_bind_irq(u64 cap_handle, u64 irq, u64 mask)
{
        process_t *proc = process_current();
        if (!proc || !proc->cap_table)
                return (i64)ERR_FAULT;

        cap_entry_t *cap = cap_lookup(proc->cap_table, (cap_t)cap_handle, RIGHT_READ);
        if (!cap || cap->type != CAP_TYPE_IRQ)
                return (i64)ERR_NOCAP;

        /* I-3: an IRQ capability names a specific line (obj_id == irq). */
        if (cap->obj_id != irq)
                return (i64)ERR_NOCAP;

        return (i64)irq_bind((u8)irq, (u32)mask);
}

/*
 * IRQ: clear the binding on an IRQ line.
 * arg1 = IRQ capability handle, arg2 = IRQ number.
 */
static i64 sys_unbind_irq(u64 cap_handle, u64 irq)
{
        process_t *proc = process_current();
        if (!proc || !proc->cap_table)
                return (i64)ERR_FAULT;

        cap_entry_t *cap = cap_lookup(proc->cap_table, (cap_t)cap_handle, RIGHT_READ);
        if (!cap || cap->type != CAP_TYPE_IRQ)
                return (i64)ERR_NOCAP;

        /* I-3: mirror the bind check — cap must name this exact line. */
        if (cap->obj_id != irq)
                return (i64)ERR_NOCAP;

        return (i64)irq_unbind((u8)irq);
}

/*
 * Check whether the current process holds an I/O port capability that
 * covers the given port with the required right.
 *
 * obj_id encodes a port range: (count << 16) | base_port.
 * A cap with count == 0 names exactly the single port base_port.
 */
static bool proc_has_io_port_cap(rights_t need, u16 port)
{
        process_t *proc = process_current();
        if (!proc || !proc->cap_table)
                return false;

        for (u32 i = 0; i < MAX_CAPS; i++) {
                cap_entry_t *e = &proc->cap_table->entries[i];
                if (e->type == CAP_TYPE_IO_PORT && (e->rights & need) == need) {
                        u16 base  = (u16)(e->obj_id & 0xFFFF);
                        u16 count = (u16)((e->obj_id >> 16) & 0xFFFF);
                        if (port >= base && (u32)port < (u32)base + (u32)count)
                                return true;
                }
        }
        return false;
}

/*
 * I/O: read a byte from an I/O port.
 * Requires the calling process to hold a CAP_TYPE_IO_PORT capability
 * covering `port` with RIGHT_READ.  arg1 = port number.
 */
static i64 sys_io_read8(u64 port)
{
        if (!proc_has_io_port_cap(RIGHT_READ, (u16)port))
                return (i64)ERR_NOCAP;

        return (i64)io_inb((u16)port);
}

/*
 * I/O: write a byte to an I/O port.
 * Requires the calling process to hold a CAP_TYPE_IO_PORT capability
 * covering `port` with RIGHT_WRITE.  arg1 = port number, arg2 = byte value.
 */
static i64 sys_io_write8(u64 port, u64 val)
{
        if (!proc_has_io_port_cap(RIGHT_WRITE, (u16)port))
                return (i64)ERR_NOCAP;

        io_outb((u16)port, (u8)val);
        return 0;
}

/*
 * Mutex: create a new mutex owned by no thread.
 */
static i64 sys_mutex_create(void)
{
        u32 handle = mutex_create();
        /* mutex_create returns 0 (invalid handle sentinel) when full */
        if (handle == 0)
                return (i64)ERR_NOMEM;
        return (i64)handle;
}

/*
 * Mutex: acquire, blocking until free.
 */
static i64 sys_mutex_lock(u64 handle)
{
        return (i64)mutex_lock((u32)handle);
}

/*
 * Mutex: release, handing off to the next FIFO waiter.
 */
static i64 sys_mutex_unlock(u64 handle)
{
        return (i64)mutex_unlock((u32)handle);
}

/*
 * Mutex: destroy, waking every waiter with ERR_NOENT.
 */
static i64 sys_mutex_destroy(u64 handle)
{
        mutex_destroy((u32)handle);
        return 0;
}

/*
 * Reboot the system via the 8042 keyboard controller reset line.
 * Writing 0xFE to port 0x64 asserts the CPU reset signal, which
 * QEMU (and real hardware) honors by restarting from the reset
 * vector.  Used by the shell's 'reboot' command.
 *
 * Note: there is no ACPI/PM support in v0.1, so this is the only
 * power-control path.  Does not return on success (CPU is reset).
 */
static i64 sys_reboot(void)
{
        process_t *proc = process_current();
        if (!proc || !proc->cap_table)
                return (i64)ERR_FAULT;

        /* P2 地基: gate on ATOM_SYS_SHUTDOWN before ANY side effect
     * (docs/permission_model.md §四).  Pure kernel cap-table lookup —
     * zero IPC.  An unauthorized caller gets ERR_NOCAP and the system
     * keeps running (the reset line is never touched). */
        if (cap_lookup_by_atom(proc->cap_table, proc->subject_id,
                           ATOM_SYS_SHUTDOWN, 0) == CAP_NULL)
                return (i64)ERR_NOCAP;

        serial_puts("OpSys: reboot requested, asserting 8042 reset line\n");

        /* Disable interrupts before the reset so a stray IRQ cannot
     * re-enter the kernel during the reset sequence. */
        __asm__ volatile("cli");

        /* Standard PC reboot sequence: pulse the 8042 reset line. */
        io_outb(0x64, 0xFE);

        /* If the reset line is ignored (non-standard hardware), fall back
     * to a triple fault: load IDT with base 0 and raise #BP.
     * QEMU treats a triple fault as a reboot request as well. */
        io_delay();
        io_outb(0x64, 0xFE);

        /* We should never get here.  Busy-loop to keep the CPU parked. */
        for (;;)
                __asm__ volatile("hlt");
        __builtin_unreachable();
}

/*
 * Temporary test hook (SYS_PANIC): panic the kernel on demand from user
 * space.  Lets the shell 'panic' command exercise the unified panic
 * path (kernel/panic.c) end-to-end.  noreturn: the kernel halts here.
 */
static i64 sc_sys_panic(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5)
{
        (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
        panic("User-triggered panic (SYS_PANIC from shell)");
        __builtin_unreachable();
}

/*
 * POSIX-style signal registration (SYS_SIGNAL).
 * a1 = signum (1..NSIG-1; SIGKILL/SIGSTOP are uncatchable/unignorable),
 * a2 = handler: SIG_DFL (0), SIG_IGN (1), or a user handler address,
 * a3 = restorer: __restore_rt trampoline address (required when a2 is
 *      a real handler; the kernel jumps to it after the handler rets).
 * @return Previous handler for signum (SIG_DFL if never set), or a
 *         negative error code.
 */
static i64 sys_signal(u64 signum, u64 handler, u64 restorer)
{
        if (signum == 0 || signum >= NSIG)
                return (i64)ERR_INVAL;
        if (signum == SIGKILL || signum == SIGSTOP)
                return (i64)ERR_INVAL;

        if (handler != SIG_DFL && handler != SIG_IGN) {
                if (handler < 0x1000 || handler >= USER_PTR_MAX)
                        return (i64)ERR_INVAL;
                if (restorer < 0x1000 || restorer >= USER_PTR_MAX)
                        return (i64)ERR_INVAL;
        }

        process_t *proc = process_current();
        if (!proc)
                return (i64)ERR_NOMEM;

        u64 old = proc->sig_handlers[signum];
        proc->sig_handlers[signum] = handler;
        if (handler != SIG_DFL && handler != SIG_IGN)
                proc->sig_restorer = restorer;
        return (i64)old;
}

/*
 * Send a signal to a process (SYS_KILL).
 * a1 = pid, a2 = signum.
 * @return OK, or a negative error code.
 */
static i64 sys_kill(u64 pid, u64 signum)
{
        if (pid == 0 || signum == 0 || signum >= NSIG)
                return (i64)ERR_INVAL;

        process_t *proc = process_get((pid_t)pid);
        if (!proc)
                return (i64)ERR_NOENT;
        if (proc->state == PROC_STATE_ZOMBIE || proc->state == PROC_STATE_FINISHED)
                return (i64)ERR_NOENT;

        if (signum == SIGKILL) {
                /* Force-kill: every thread gets force_exit and blocked ones
         * are woken; each dies at its next checkpoint. */
                signal_kill_process(proc, 128 + SIGKILL);
                return OK;
        }

        if (signum == SIGSTOP) {
                /* Reserved: stop/resume is out of scope, but SIGSTOP itself is
         * a valid delivery and is a documented no-op (process never
         * resumes, so there is nothing to observe). */
                return OK;
        }

        u64 handler = proc->sig_handlers[signum];
        if (handler == SIG_IGN)
                return OK;

        if (handler == SIG_DFL) {
                if (signal_default_terminates((int)signum))
                        signal_kill_process(proc, 128 + (int)signum);
                return OK;   /* otherwise default action is ignore */
        }

        /* Registered handler: latch the pending bit; the delivery core
     * pops it lazily at the next checkpoint on some thread. */
        proc->sig_pending |= (1ULL << signum);
        return OK;
}

/*
 * Return from a signal handler (SYS_SIGRETURN).
 * a1 = sigframe base address, passed in RDI by __restore_rt.
 * @return The restored user RAX (or a negative error code).
 */
static i64 sys_sigreturn(u64 frame_ptr)
{
        return signal_restore(frame_ptr);
}

/*
 * Initialize the syscall subsystem.
 * The IDT entry for vector 0x80 is set up by idt.c.
 */
void syscall_init(void)
{
}

/*
 * Re-enable PIT timer and PIC IRQs after the init process has started.
 * Called once on the first successful syscall. The PIT was disabled and
 * all PIC IRQs were masked before the IRETQ transition to ring 3 to
 * prevent timer interrupts during the CR3/IRETQ critical section.
 */
static void enable_scheduler_once(void)
{
        /* PIT: re-program Channel 0, lobyte/hibyte, rate generator (mode 2) */
        io_outb(0x43, 0x34);  /* channel 0, lobyte/hibyte, mode 2 */
        u32 divisor = 1193182 / 100;  /* 100 Hz */
        io_outb(0x40, (u8)(divisor & 0xFF));
        io_outb(0x40, (u8)((divisor >> 8) & 0xFF));

        /* Unmask IRQ0 (timer) and IRQ2 (cascade) on master PIC */
        u8 mask = io_inb(0x21);
        mask &= ~((1 << 0) | (1 << 2));  /* unmask IRQ0 and IRQ2 */
        io_outb(0x21, mask);

        serial_puts("  PIT re-enabled at 100 Hz, IRQs unmasked\n");
}

/* ------------------------------------------------------------------ */
/*  Dispatch table                                                     */
/* ------------------------------------------------------------------ */

/*
 * Every syscall is dispatched through a function-pointer table rather
 * than a monolithic switch: O(1) lookup, sparse numbers via designated
 * initializers, and an explicit bounds check.  Handlers keep their
 * natural arity; the SYSCALLn() macros below generate uniform 5-arg
 * adapter functions so the whole table shares one function-pointer
 * type.  Unimplemented numbers (reserved FB/PCI/signal slots, gaps)
 * stay NULL and are rejected with ERR_INVAL.
 */

typedef i64 (*syscall_fn_t)(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5);

#define SYSCALL0(fn) \
        static i64 sc_##fn(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) \
        { (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return fn(); }
#define SYSCALL1(fn) \
        static i64 sc_##fn(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) \
        { (void)a2; (void)a3; (void)a4; (void)a5; return fn(a1); }
#define SYSCALL2(fn) \
        static i64 sc_##fn(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) \
        { (void)a3; (void)a4; (void)a5; return fn(a1, a2); }
#define SYSCALL3(fn) \
        static i64 sc_##fn(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) \
        { (void)a4; (void)a5; return fn(a1, a2, a3); }
#define SYSCALL4(fn) \
        static i64 sc_##fn(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) \
        { (void)a5; return fn(a1, a2, a3, a4); }
#define SYSCALL5(fn) \
        static i64 sc_##fn(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) \
        { return fn(a1, a2, a3, a4, a5); }

SYSCALL1(sys_debug_log)
SYSCALL3(sys_cap_create)
SYSCALL3(sys_cap_grant)
SYSCALL1(sys_cap_revoke)
SYSCALL5(sys_cap_create_atom)
SYSCALL1(sys_cap_consume)
SYSCALL3(sys_cap_revoke_by_atom)
SYSCALL5(sys_cap_grant_to_subject)
SYSCALL2(sys_cap_has_atom)
SYSCALL3(sys_ipc_send)
SYSCALL4(sys_ipc_recv)
SYSCALL5(sys_ipc_recv_from)
SYSCALL5(sys_ipc_call)
SYSCALL0(sys_ipc_port_create)
SYSCALL3(sys_ipc_reply)
SYSCALL4(sys_map_memory)
SYSCALL2(sys_unmap_memory)
SYSCALL3(sys_thread_create)
SYSCALL1(sys_thread_exit)
SYSCALL0(sys_thread_yield)
SYSCALL2(sys_thread_set_affinity)
SYSCALL2(sys_thread_join)
SYSCALL0(sys_get_time)
SYSCALL1(sys_sleep)
SYSCALL1(sys_rtc_time)
SYSCALL2(sys_port_register)
SYSCALL1(sys_port_get)
SYSCALL0(sys_get_free_pages)
SYSCALL0(sys_get_pid)
SYSCALL0(sys_get_subject)
SYSCALL0(sys_get_heap_base)
SYSCALL2(sys_process_wait)
SYSCALL2(sys_process_list)
SYSCALL2(sys_proc_info_by_subject)
SYSCALL3(sys_blob_get)
SYSCALL0(sys_debug_getchar)
SYSCALL2(sys_notify)
SYSCALL1(sys_wait_notification)
SYSCALL3(sys_bind_irq)
SYSCALL2(sys_unbind_irq)
SYSCALL1(sys_io_read8)
SYSCALL2(sys_io_write8)
SYSCALL0(sys_mutex_create)
SYSCALL1(sys_mutex_lock)
SYSCALL1(sys_mutex_unlock)
SYSCALL1(sys_mutex_destroy)
SYSCALL0(sys_reboot)
SYSCALL1(sys_set_time)
SYSCALL1(sys_fb_get_info)
SYSCALL2(sys_fb_map)
SYSCALL3(sys_signal)
SYSCALL2(sys_kill)
SYSCALL1(sys_sigreturn)

static const syscall_fn_t s_syscall_table[SYS_COUNT] = {
        [SYS_DEBUG_LOG]         = sc_sys_debug_log,
        [SYS_CAP_CREATE]        = sc_sys_cap_create,
        [SYS_CAP_GRANT]         = sc_sys_cap_grant,
        [SYS_CAP_REVOKE]        = sc_sys_cap_revoke,
        [SYS_CAP_CREATE_ATOM]   = sc_sys_cap_create_atom,
        [SYS_CAP_CONSUME]       = sc_sys_cap_consume,
        [SYS_CAP_REVOKE_BY_ATOM] = sc_sys_cap_revoke_by_atom,
        [SYS_CAP_GRANT_TO_SUBJECT] = sc_sys_cap_grant_to_subject,
        [SYS_CAP_HAS_ATOM]      = sc_sys_cap_has_atom,
        [SYS_IPC_SEND]          = sc_sys_ipc_send,
        [SYS_IPC_RECV]          = sc_sys_ipc_recv,
        [SYS_IPC_RECV_FROM]     = sc_sys_ipc_recv_from,
        [SYS_IPC_CALL]          = sc_sys_ipc_call,
        [SYS_IPC_PORT_CREATE]   = sc_sys_ipc_port_create,
        [SYS_MAP_MEMORY]        = sc_sys_map_memory,
        [SYS_UNMAP_MEMORY]      = sc_sys_unmap_memory,
        [SYS_THREAD_CREATE]     = sc_sys_thread_create,
        [SYS_THREAD_EXIT]       = sc_sys_thread_exit,
        [SYS_THREAD_YIELD]      = sc_sys_thread_yield,
        [SYS_THREAD_SET_AFFINITY] = sc_sys_thread_set_affinity,
        [SYS_THREAD_JOIN]       = sc_sys_thread_join,
        [SYS_GET_TIME]          = sc_sys_get_time,
        [SYS_SLEEP]             = sc_sys_sleep,
        [SYS_GET_RTC_TIME]      = sc_sys_rtc_time,
        [SYS_PORT_REGISTER]     = sc_sys_port_register,
        [SYS_PORT_GET]          = sc_sys_port_get,
        [SYS_DEBUG_GETCHAR]     = sc_sys_debug_getchar,
        [SYS_NOTIFY]            = sc_sys_notify,
        [SYS_WAIT_NOTIFICATION] = sc_sys_wait_notification,
        [SYS_BIND_IRQ]          = sc_sys_bind_irq,
        [SYS_UNBIND_IRQ]        = sc_sys_unbind_irq,
        [SYS_IPC_REPLY]         = sc_sys_ipc_reply,
        [SYS_IO_READ8]          = sc_sys_io_read8,
        [SYS_IO_WRITE8]         = sc_sys_io_write8,
        [SYS_REBOOT]            = sc_sys_reboot,
        [SYS_SET_TIME]          = sc_sys_set_time,
        [SYS_PANIC]             = sc_sys_panic,
        [SYS_GET_FREE_PAGES]    = sc_sys_get_free_pages,
        [SYS_GET_PID]           = sc_sys_get_pid,
        [SYS_GET_SUBJECT]       = sc_sys_get_subject,
        [SYS_GET_HEAP_BASE]     = sc_sys_get_heap_base,
        [SYS_PROCESS_CREATE]    = sc_sys_process_create,
        [SYS_PROCESS_WAIT]      = sc_sys_process_wait,
        [SYS_BLOB_GET]          = sc_sys_blob_get,
        [SYS_MUTEX_CREATE]      = sc_sys_mutex_create,
        [SYS_MUTEX_LOCK]        = sc_sys_mutex_lock,
        [SYS_MUTEX_UNLOCK]      = sc_sys_mutex_unlock,
        [SYS_MUTEX_DESTROY]     = sc_sys_mutex_destroy,
        [SYS_FB_GET_INFO]       = sc_sys_fb_get_info,
        [SYS_FB_MAP]            = sc_sys_fb_map,
        [SYS_PCI_GET_COUNT]     = sc_sys_pci_get_count,
        [SYS_PCI_GET_DEVICE]    = sc_sys_pci_get_device,
        [SYS_SIGNAL]            = sc_sys_signal,
        [SYS_KILL]              = sc_sys_kill,
        [SYS_SIGRETURN]         = sc_sys_sigreturn,
        [SYS_PROCESS_LIST]      = sc_sys_process_list,
        [SYS_PROC_INFO_BY_SUBJECT] = sc_sys_proc_info_by_subject,
        [SYS_VSPACE_ALLOC]      = sc_sys_vspace_alloc,
        [SYS_THREAD_SET_CTX]    = sc_sys_thread_set_ctx,
        [SYS_BLK_READ]          = sc_sys_blk_read,
        [SYS_BLK_WRITE]         = sc_sys_blk_write,
        [SYS_BLK_INFO]          = sc_sys_blk_info,
};

/**
 * Main syscall dispatch. Called from the assembly stub with user
 * register values already saved.
 *
 * @param num   System call number.
 * @param arg1-arg5  Arguments from user registers (RDI, RSI, RDX, R10, R8).
 * @return      Result value (positive on success, negative error on failure).
 */
i64 syscall_dispatch(u64 num, u64 arg1, u64 arg2, u64 arg3,
                     u64 arg4, u64 arg5)
{
        static bool scheduler_started = false;
        if (!scheduler_started) {
                scheduler_started = true;
                enable_scheduler_once();
        }

        /* Bounds check, then table lookup (NULL entry = unimplemented) */
        if (num >= SYS_COUNT)
                return (i64)ERR_INVAL;
        syscall_fn_t fn = s_syscall_table[num];
        if (!fn)
                return (i64)ERR_INVAL;

        return fn(arg1, arg2, arg3, arg4, arg5);
}
