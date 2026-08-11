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
#define SYS_DEBUG_LOG              0

/* ---- Capability management ---- */
#define SYS_CAP_CREATE             1
#define SYS_CAP_GRANT              2
#define SYS_CAP_REVOKE             3

/* ---- IPC ---- */
#define SYS_IPC_SEND               4
#define SYS_IPC_RECV               5
#define SYS_IPC_CALL               6
#define SYS_IPC_PORT_CREATE        7

/* ---- Memory ---- */
#define SYS_MAP_MEMORY             8
#define SYS_UNMAP_MEMORY           9

/* ---- Thread management ---- */
#define SYS_THREAD_CREATE          10
#define SYS_THREAD_EXIT            11
#define SYS_THREAD_YIELD           12
#define SYS_THREAD_SET_AFFINITY    13
#define SYS_THREAD_JOIN            14

/* ---- Time ---- */
#define SYS_GET_TIME               15
#define SYS_SLEEP                  16

/* ---- Port registry ---- */
#define SYS_PORT_REGISTER          17
#define SYS_PORT_GET               18

/* ---- Serial I/O ---- */
#define SYS_DEBUG_GETCHAR          29

/* ---- Async notification (seL4-style signal/wait) ---- */
#define SYS_NOTIFY                 30
#define SYS_WAIT_NOTIFICATION      31

/* ---- IRQ binding (interrupt → notification forwarding) ---- */
#define SYS_BIND_IRQ               32
#define SYS_UNBIND_IRQ             33

/* ---- Serial service (P0-A): IPC reply + I/O port access ---- */
#define SYS_IPC_REPLY              34
#define SYS_IO_READ8               35
#define SYS_IO_WRITE8              36

/* ---- System power ---- */
#define SYS_REBOOT                 37

/* ---- Temporary test hook: user-triggered kernel panic (shell 'panic') ---- */
#define SYS_PANIC                  38

/* ---- Init protocol ---- */
#define SYS_GET_FREE_PAGES         19
#define SYS_GET_PID                20

/* ---- Process management ---- */
#define SYS_PROCESS_CREATE         21
#define SYS_PROCESS_WAIT           41   /* wait for a process to exit */

/* ---- Kernel mutexes ---- */
#define SYS_MUTEX_CREATE           22
#define SYS_MUTEX_LOCK             23
#define SYS_MUTEX_UNLOCK           24
#define SYS_MUTEX_DESTROY          25

/* ---- Embedded blob access ---- */
#define SYS_BLOB_GET               40

/* ---- Wall clock (RTC/CMOS) ---- */
#define SYS_GET_RTC_TIME           42

/* ---- Process kill ---- */
#define SYS_PROCESS_KILL           43

/* ---- Framebuffer ---- */
#define SYS_FB_GET_INFO            44
#define SYS_FB_MAP                 45

/* ---- PCI enumeration ---- */
#define SYS_PCI_GET_COUNT          46
#define SYS_PCI_GET_DEVICE         47

/* ---- POSIX-style signals ---- */
#define SYS_SIGNAL                 48
#define SYS_KILL                   49
#define SYS_SIGRETURN              50

/* ---- ASLR: per-process randomized heap base (design item ⑭) ---- */
#define SYS_GET_HEAP_BASE          51

/* ---- Process enumeration ---- */
#define SYS_PROCESS_LIST           52

/* ---- VSpace (roadmap P1): kernel allocates VA + builds page tables ---- */
#define SYS_VSPACE_ALLOC           53

/* ---- Thread context (roadmap P2): overwrite a thread's saved state ---- */
#define SYS_THREAD_SET_CTX         54

/* ---- P0 地基: permission model (docs/permission_model.md) ----
 * Gating of these to the perm-engine lands in P1, consistent with the
 * currently unrestricted sys_cap_create. */
#define SYS_GET_SUBJECT            55   /* () -> caller's subject_id */
#define SYS_IPC_RECV_FROM          56   /* (port, buf, len, tok, subj) */
#define SYS_CAP_CREATE_ATOM        57   /* (atom, rights, expiry, quota, scope) */
#define SYS_CAP_CONSUME            58   /* (handle) */
#define SYS_CAP_REVOKE_BY_ATOM     59   /* (subject, atom, scope_hash) */

#endif /* KERNEL_SYSCALL_NUMBERS_H */
