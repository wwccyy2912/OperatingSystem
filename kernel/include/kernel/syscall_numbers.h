/*
 * syscall_numbers.h - Single source of truth for syscall numbers
 * Copyright (c) 2026 OpSys Project
 *
 * Included by both kernel (kernel/syscall.h) and user-space
 * (user/lib/libos/syscalls.h).  Uses #define so it works in
 * both C enum initializers and preprocessor comparisons.
 */

#ifndef KERNEL_SYSCALL_NUMBERS_H
#define KERNEL_SYSCALL_NUMBERS_H

/* ---- Debug ---- */
#define SYS_DEBUG_LOG 0

/* ---- Capability management ---- */
#define SYS_CAP_CREATE 1
#define SYS_CAP_GRANT  2
#define SYS_CAP_REVOKE 3

/* ---- IPC ---- */
#define SYS_IPC_SEND        4
#define SYS_IPC_RECV        5
#define SYS_IPC_CALL        6
#define SYS_IPC_PORT_CREATE 7

/* ---- Memory ---- */
#define SYS_MAP_MEMORY   8
#define SYS_UNMAP_MEMORY 9

/* ---- Thread management ---- */
#define SYS_THREAD_CREATE       10
#define SYS_THREAD_EXIT         11
#define SYS_THREAD_YIELD        12
#define SYS_THREAD_SET_AFFINITY 13
#define SYS_THREAD_JOIN         14

/* ---- Time ---- */
#define SYS_GET_TIME 15
#define SYS_SLEEP    16

/* ---- Port registry ---- */
#define SYS_PORT_REGISTER 17
#define SYS_PORT_GET      18

/* ---- Serial I/O ---- */
#define SYS_DEBUG_GETCHAR 29

/* ---- Async notification (seL4-style signal/wait) ---- */
#define SYS_NOTIFY            30
#define SYS_WAIT_NOTIFICATION 31

/* ---- IRQ binding (interrupt → notification forwarding) ---- */
#define SYS_BIND_IRQ   32
#define SYS_UNBIND_IRQ 33

/* ---- I/O port access (used by device drivers) ---- */
#define SYS_IPC_REPLY 34
#define SYS_IO_READ8  35
#define SYS_IO_WRITE8 36
/* NOTE: 16-bit I/O must NOT use 48/49 — those were already taken by
 * SYS_SIGNAL / SYS_KILL (below), which would shadow them in the
 * dispatch table.  They live at the end instead. */
#define SYS_IO_READ16  69
#define SYS_IO_WRITE16 70

/* ---- System power ---- */
#define SYS_REBOOT 37
#define SYS_SHUTDOWN 39

/* ---- Temporary test hook: user-triggered kernel panic (shell 'panic') ---- */
#define SYS_PANIC 38

/* ---- Init protocol ---- */
#define SYS_GET_FREE_PAGES 19
#define SYS_GET_PID        20

/* ---- Process management ---- */
#define SYS_PROCESS_CREATE 21
#define SYS_PROCESS_WAIT   41 /* wait for a process to exit */

/* ---- Kernel mutexes ---- */
#define SYS_MUTEX_CREATE  22
#define SYS_MUTEX_LOCK    23
#define SYS_MUTEX_UNLOCK  24
#define SYS_MUTEX_DESTROY 25

/* ---- Embedded blob access ---- */
#define SYS_BLOB_GET 40

/* ---- Wall clock (RTC/CMOS) ---- */
#define SYS_GET_RTC_TIME 42

/* ---- Framebuffer ---- */
#define SYS_FB_GET_INFO 44
#define SYS_FB_MAP      45

/* ---- PCI enumeration ---- */
#define SYS_PCI_GET_COUNT  46
#define SYS_PCI_GET_DEVICE 47

/* ---- POSIX-style signals ---- */
#define SYS_SIGNAL    48
#define SYS_KILL      49
#define SYS_SIGRETURN 50

/* ---- ASLR: per-process randomized heap base (design item ⑭) ---- */
#define SYS_GET_HEAP_BASE 51

/* ---- Process enumeration ---- */
#define SYS_PROCESS_LIST 52

/* ---- VSpace (roadmap P1): kernel allocates VA + builds page tables ---- */
#define SYS_VSPACE_ALLOC 53

/* ---- Thread context (roadmap P2): overwrite a thread's saved state ---- */
#define SYS_THREAD_SET_CTX 54

/* ---- P0 地基: permission model (docs/permission_model.md) ----
 * Gating of these to the perm-engine lands in P1, consistent with the
 * currently unrestricted sys_cap_create. */
#define SYS_GET_SUBJECT        55 /* () -> caller's subject_id */
#define SYS_IPC_RECV_FROM      56 /* (port, buf, len, tok, subj) */
#define SYS_CAP_CREATE_ATOM    57 /* (atom, rights, expiry, quota, scope) */
#define SYS_CAP_CONSUME        58 /* (handle) */
#define SYS_CAP_REVOKE_BY_ATOM 59 /* (subject, atom, scope_hash) */

/* ---- P1 地基: perm-engine signing path (docs/permission_model.md §四) ----
 * Subject-targeted atom issuance: resolves the target process by
 * subject_id and creates the atom cap in ITS table (entry.subject =
 * target).  This is how the perm-engine encodes an authorization
 * decision into a capability without holding a handle first. */
#define SYS_CAP_GRANT_TO_SUBJECT 60 /* (subject, atom, rights, expiry, quota) */

/* ---- P2 地基: sensitive syscall gating (docs/permission_model.md §四) ----
 * Sensitive syscalls are gated by atom caps: the handler does a pure
 * kernel cap-table lookup (决策下沉 — ZERO IPC to user space) and
 * returns ERR_NOCAP when the caller holds no live atom cap. */
#define SYS_SET_TIME 61 /* (rtc_time_t *user_t) */

/* ---- Block device (Phase 1: legacy virtio-blk, kernel/arch/x86_64/
 * virtio_blk.c).  Gated on CAP_TYPE_PCI_DEV (obj_id = PCI table index
 * of the adapter, rights RIGHT_READ|RIGHT_WRITE). */
#define SYS_BLK_READ  62 /* (disk, lba, count, buf) */
#define SYS_BLK_WRITE 63 /* (disk, lba, count, buf) */
#define SYS_BLK_INFO  64 /* (disk, out blk_info_t) */

/* ---- P0 地基: app identity query (docs/permission_model.md §三) ----
 * Unit 1 (TUI 权限查询): the app identity moves from a forgeable
 * self-reported u32 app_id_hash to the kernel-issued App Subject
 * (uuid), allocated at app instantiation.  Resolves a subject_id to
 * the target process's kernel-issued identity record. */
#define SYS_PROC_INFO_BY_SUBJECT 65 /* (subject, out proc_ident_t *) */

/* ---- P1 地基: management-plane atom inspection (docs/ops_format.md §6) ----
 * Read-only query: does `subject` hold a live atom cap for `atom`?
 * Returns 1/0.  Used by the perm-engine's do_grant gate (capability-
 * based: a caller holding ATOM_SERVICE_MANAGE may grant even when its
 * ROLE is not management — grants beat role defaults, §四).  GATED on
 * ATOM_SERVICE_MANAGE of the CALLER: only management-plane processes
 * may inspect others' atom holdings. */
#define SYS_CAP_HAS_ATOM 66 /* (subject, atom) -> 1/0 */

/* ---- Phase 3: zero-copy read path (shared physical-page pools) ----
 * SYS_SHM_CREATE: allocate a contiguous physical-page pool and map it
 *   into the caller (gated on ATOM_SERVICE_MANAGE).
 * SYS_SHM_MAP: map an existing pool READ-ONLY into a client's address
 *   space (gated on ATOM_SERVICE_MANAGE + pool-table verification). */
#define SYS_SHM_CREATE 67 /* (count, virt) -> phys_base */
#define SYS_SHM_MAP    68 /* (phys_base, count, subject, virt) -> OK */

/* ---- PCI config-space access (device drivers: enable bus master,
 * read/write BARs, IRQ line, etc.).  Gated on CAP_TYPE_PCI_DEV with
 * obj_id = cached PCI table index (same gate as SYS_BLK_*). ---- */
#define SYS_PCI_CFG_READ  71 /* (idx, offset) -> u32 dword */
#define SYS_PCI_CFG_WRITE 72 /* (idx, offset, val) -> OK */

#endif /* KERNEL_SYSCALL_NUMBERS_H */
