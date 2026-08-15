/*
 * syscalls.h - User-space system call wrappers
 * Copyright (c) 2026 OpSys Project
 *
 * Thin wrappers around INT 0x80 syscalls.
 * Numbers come from kernel/syscall_numbers.h (single source of truth).
 */

#ifndef LIBOS_SYSCALLS_H
#define LIBOS_SYSCALLS_H

#include <stdint.h>

/* Pull in the unified syscall numbers and error codes from the kernel header */
#include <kernel/syscall_numbers.h>

/* PCI enumeration ABI (kernel/user shared struct) */
#include <kernel/pci.h>

/* Block device ABI (kernel/user shared struct) */
#include <kernel/blk.h>

/* Permission-model atom enum (kernel/user shared, docs/permission_model.md §六) */
#include <kernel/atom.h>

/* Error codes - must match kernel/types.h */
#define OK              0
#define ERR_NOMEM       (-1)
#define ERR_INVAL       (-2)
#define ERR_NOCAP       (-3)
#define ERR_NOENT       (-4)
#define ERR_BUSY        (-5)
#define ERR_AGAIN       (-6)
#define ERR_FAULT       (-7)
#define ERR_OVERFLOW    (-8)
#define ERR_DENIED      (-9)
#define ERR_INTERRUPTED (-10)   /* blocking wait aborted by a signal kill */

/* Memory protection flags */
#define PROT_NONE   0x00
#define PROT_READ   0x01
#define PROT_WRITE  0x02
#define PROT_EXEC   0x04

/* Capability types (must match kernel cap.h) */
#define CAP_TYPE_NONE       0
#define CAP_TYPE_THREAD     1
#define CAP_TYPE_PORT       2
#define CAP_TYPE_MEM        3
#define CAP_TYPE_IRQ        4
#define CAP_TYPE_IO_PORT    5
#define CAP_TYPE_PCI_DEV    6
#define CAP_TYPE_SERVICE    7
#define CAP_TYPE_KERNEL     8
#define CAP_TYPE_DAC_OVERRIDE  9

/* Capability rights */
#define RIGHT_READ  (1 << 0)
#define RIGHT_WRITE (1 << 1)
#define RIGHT_EXEC  (1 << 2)
#define RIGHT_GRANT (1 << 3)
#define RIGHT_ALL   (RIGHT_READ | RIGHT_WRITE | RIGHT_EXEC | RIGHT_GRANT)

/* Null handles */
#define CAP_NULL    0UL
#define PORT_NULL   0UL

/**
 * Raw syscall instruction.
 * All syscalls route through this single entry point.
 */
static inline long sys_call(long num, long a1, long a2, long a3,
                                                        long a4, long a5)
{
        long ret;
        /* The kernel's INT 0x80 ABI reads args 1-3 from RDI/RSI/RDX and
     * args 4-5 from R10/R8 (kernel/arch/x86_64/syscall_entry.S:14-15,
     * 171-172).  Plain "r" constraints let GCC pick any register for
     * a4/a5, so 5-arg syscalls like ipc_call() could deliver garbage
     * in arg4/arg5.  Pin the tail args to the exact ABI registers. */
        register long a4_reg __asm__("r10") = a4;
        register long a5_reg __asm__("r8")  = a5;
        __asm__ volatile (
                "int $0x80"
                : "=a"(ret)
                : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(a4_reg), "r"(a5_reg)
                : "memory", "rcx", "r11"
        );
        return ret;
}

/* --- Debug / Serial I/O --- */
int debug_log(const char *str);
int debug_getchar(void);

/* --- Capability management --- */
int cap_create(int type, int rights);
int cap_create_obj(int type, int rights, unsigned long obj_id);
int cap_grant(int handle, int target_pid, int rights);
int cap_revoke(int handle);

/* --- IPC --- */
int ipc_send(int port, const void *msg, int len);
int ipc_recv(int port, void *buf, int *len, int *tok);
int ipc_call(int port, const void *req, int req_len,
             void *resp, int *resp_len);
int ipc_port_create(void);
int ipc_reply(int token, const void *resp, int resp_len);

/* --- Memory --- */
void *map_memory(int cap, int offset, int size, int prot);
int unmap_memory(void *addr, int size);

/* --- Thread management --- */
int thread_create(void (*entry)(void *), void *arg, int priority);
void thread_exit(int code);
void thread_yield(void);
int thread_set_affinity(int tid, int cpu);
int thread_join(int tid, int *exit_code);

/* --- Time --- */
int get_time(void);
int sleep(int ticks);

/* Wall-clock time (layout must match kernel/include/kernel/rtc.h) */
typedef struct {
        unsigned short year;    /* full year, e.g. 2026 */
        unsigned char  month;   /* 1-12 */
        unsigned char  day;     /* 1-31 */
        unsigned char  hour;    /* 0-23 */
        unsigned char  minute;  /* 0-59 */
        unsigned char  second;  /* 0-59 */
} rtc_time_t;

/* --- RTC wall clock --- */
int os_get_rtc_time(rtc_time_t *out);

/* P2 地基: set the wall clock (SYS_SET_TIME), gated by ATOM_SYS_SET_TIME
 * — an unauthorized caller gets ERR_NOCAP with zero IPC (pure kernel
 * cap-table lookup, docs/permission_model.md §四).  Returns 0 on
 * success, ERR_NOCAP unauthorized, ERR_INVAL bad time range, or
 * ERR_FAULT on a bad pointer. */
int os_set_time(const rtc_time_t *t);

/* --- Port registry --- */
int port_register(const char *name, int port);
int port_get(const char *name);

/* --- Async notification --- */
int notify(int target_tid, unsigned int mask);
int wait_notification(unsigned int mask);

/* --- IRQ binding --- */
int bind_irq(int cap, int irq, unsigned int mask);
int unbind_irq(int cap, int irq);

/* --- I/O port access --- */
int io_read8(unsigned short port);
int io_write8(unsigned short port, unsigned char val);

/* --- System power --- */
int sys_reboot(void);
int sys_panic(void);    /* TEMP test hook: trigger a kernel panic */

/* --- Init protocol --- */
int get_free_pages(void);
int get_pid(void);

/* --- ASLR: per-process randomized heap base (design item ⑭) ---
 * Returns the current process's heap base (0x70000000-based, 64 KB
 * aligned).  malloc.c fetches it lazily on first heap grow so the
 * user heap matches the kernel's randomized layout. */
uint64_t get_heap_base(void);

/* --- Process management --- */
int process_create(const char *name, const void *elf, unsigned long size);
int process_wait(int pid, int *exit_code);

/*
 * Process list entry — layout MUST match kernel proc_info_t
 * (kernel/include/kernel/proc_info.h).  Fixed 84-byte record.
 */
typedef struct {
        int         pid;           /* process ID */
        uint32_t    state;         /* proc_state_t value (0-4) */
        uint32_t    thread_count;  /* live threads */
        int         exit_code;     /* exit code (valid when state == ZOMBIE) */
        uint32_t    main_tid;      /* main thread ID */
        char        name[64];      /* NUL-terminated process name */
} proc_info_t;

/* Fill buf with up to max_entries proc_info_t entries.  Returns
 * the number written (0..max_entries), or a negative error
 * (ERR_FAULT bad buffer). */
int process_list(proc_info_t *buf, int max_entries);

/*
 * Process identity record (Unit 1: kernel-issued app UUID) —
 * layout MUST match kernel proc_ident_t
 * (kernel/include/kernel/proc_info.h).  Fixed layout:
 * pid + name + 128-bit kernel-issued App Subject (uuid).
 */
typedef struct {
        int         pid;        /* process ID */
        char        name[64];   /* NUL-terminated process name */
        uint64_t    uuid_hi;    /* kernel-issued app UUID, high 64 bits */
        uint64_t    uuid_lo;    /* kernel-issued app UUID, low 64 bits */
} proc_ident_t;

/* Resolve a subject_id to its kernel-issued identity
 * (SYS_PROC_INFO_BY_SUBJECT).  App identity moves from the forgeable
 * self-reported u32 app_id_hash to this kernel-issued record
 * (docs/permission_model.md §三).  Returns 0 and fills out on success;
 * ERR_NOENT for subject 0 (kernel), an unknown subject, or a dead
 * process; ERR_FAULT on a bad output pointer. */
int proc_info_by_subject(uint64_t subject, proc_ident_t *out);

/* --- Embedded blob access --- */
int blob_get(const char *name, void *buf, int buf_size);

/* --- Mutex --- */
int mutex_create(void);
int mutex_lock(int handle);
int mutex_unlock(int handle);
int mutex_destroy(int handle);

/* --- Signals ---
 * Constants mirror kernel/include/kernel/signal.h (POSIX subset).
 * signal() registers a handler; kill() sends a signal to a process;
 * the kernel's SYS_SIGRETURN is invoked by the __restore_rt trampoline
 * in user/runtime/sigrestore.S (never call it directly).
 *
 * SIG_DFL/SIG_IGN are encoded as the integer values 0/1 (the kernel
 * compares handler values numerically) but typed as sighandler_t so
 * they can be passed to/returned from signal() directly. */
typedef void (*sighandler_t)(int signum);

#define SIGKILL     9    /* uncatchable, unignorable terminate */
#define SIGUSR1     10
#define SIGSEGV     11
#define SIGUSR2     12
#define SIGPIPE     13
#define SIGALRM     14
#define SIGTERM     15
#define SIGSTOP     19   /* reserved: cannot be caught/ignored */
#define SIG_DFL     ((sighandler_t)0)   /* default action (terminate or ignore) */
#define SIG_IGN     ((sighandler_t)1)   /* ignore */
#define NSIG        64

/* Register a handler for signum.  Returns the previous handler
 * (SIG_DFL if never set), or a negative error code. */
sighandler_t signal(int signum, sighandler_t handler);

/* Send a signal to a process.  Returns 0 on success, or a negative
 * error code (ERR_INVAL bad args, ERR_NOENT unknown process). */
int kill(int pid, int signum);

/*
 * --- VSpace (roadmap P1) ---
 * vspace_alloc(size, flags): reserve a contiguous virtual range in the
 * calling process and pre-build its page-table hierarchy.  Returns the
 * base address, or a negative error.  Physical pages are mapped into it
 * afterwards with map_memory(). */
void *vspace_alloc(unsigned long size, unsigned long flags);

/*
 * --- Thread context (roadmap P2) ---
 * thread_set_ctx(tid, ctx, ctx_size): overwrite a thread's saved
 * register context.  Used by user-space signal trampolines to resume
 * with modified state.  Returns 0 or a negative error. */
int thread_set_ctx(int tid, const void *ctx, unsigned long ctx_size);

/*
 * --- PCI enumeration ---
 * pci_get_count(): number of PCI devices found at boot.
 * pci_get_device(index, out): fill pci_device_info_t for device `index`.
 * Both return non-negative values or negative errors. */
int pci_get_count(void);
int pci_get_device(int index, pci_device_info_t *out);

/*
 * --- Block device (Phase 1: legacy virtio-blk) ---
 * disk = PCI table index of the virtio-blk adapter, obtained from
 * pci_get_device() (vendor 0x1AF4, device 0x1001).  The calling
 * process must hold a CAP_TYPE_PCI_DEV cap for that index with
 * RIGHT_READ|RIGHT_WRITE (cap_create_obj(CAP_TYPE_PCI_DEV, ...)).
 *
 * Return values follow the repo convention: 0 on success, negative
 * error code on failure (ERR_NOCAP unauthorized, ERR_INVAL bad
 * disk/range, ERR_FAULT bad buffer or device error, ERR_NOMEM,
 * ERR_AGAIN DMA timeout, ERR_BUSY another op in flight).
 *
 * NOTE: libos cannot typedef u64/i64 (vfs.h already does for u64 in
 * shared TUs), so these use the equivalent stdint spellings:
 *   sys_blk_read (disk, lba, count, buf) — read `count` sectors at
 *     lba into buf (count*512 bytes).
 *   sys_blk_write (disk, lba, count, buf) — write from buf.
 *   sys_blk_info (disk, out) — fill blk_info_t {sectors, sector_size}. */
int64_t sys_blk_read(uint64_t disk, uint64_t lba, uint64_t count, void *buf);
int64_t sys_blk_write(uint64_t disk, uint64_t lba, uint64_t count,
                                            const void *buf);
int64_t sys_blk_info(uint64_t disk, blk_info_t *out);

/*
 * --- Framebuffer ---
 * Mirror of kernel fb_user_info_t (kernel/include/kernel/framebuffer.h).
 * SYS_FB_GET_INFO fills this; SYS_FB_MAP maps the framebuffer's
 * physical pages into the calling process's address space at a
 * user-chosen virtual address (page-aligned, size page-aligned,
 * size clamped to the real framebuffer size).
 */
typedef struct {
        uint64_t    phys_addr;  /* Physical address of the framebuffer */
        uint32_t    width;      /* Width in pixels (logical px in VGA text mode) */
        uint32_t    height;     /* Height in pixels (logical px in VGA text mode) */
        uint32_t    pitch;      /* Bytes per scanline (linear mode only) */
        uint8_t     bpp;        /* Bits per pixel (linear mode only) */
        uint8_t     vga_text;   /* 1 = VGA text mode (0xB8000), 0 = linear RGB */
} fb_user_info_t;

/* Fetch the user-facing framebuffer descriptor.  Returns 0 on success,
 * or a negative error (ERR_NOENT if no framebuffer, ERR_FAULT on a bad
 * output pointer). */
int fb_get_info(fb_user_info_t *out);

/* Map the framebuffer at a user-chosen page-aligned virtual address.
 * Returns the mapped virtual address on success, or a negative error. */
void *fb_map(void *virt, unsigned long size);

/*
 * --- P0 地基: permission model (docs/permission_model.md) ---
 * Kernel-side implementation complete; user wrappers below.  Gating of
 * these syscalls to the perm-engine lands in P1 (consistent with the
 * currently unrestricted cap_create).
 */

/* Return the caller's kernel-issued subject ID (SYS_GET_SUBJECT).
 * Unforgeable: it comes from the PCB, never from a user-supplied value.
 * subject 0 = System (kernel); every user process gets a unique ID. */
uint64_t get_subject(void);

/* Create an atom capability with the permission-model lifecycle fields
 * (SYS_CAP_CREATE_ATOM): atom, rights, expiry (absolute tick deadline,
 * 0 = permanent), quota (remaining uses, 0 = unlimited), scope_hash
 * (0 = unrestricted).  The holding subject is the caller.  Returns a
 * handle > 0, or a negative error (ERR_NOMEM table full). */
int cap_create_atom(atom_id_t atom, int rights, uint64_t expiry_ticks,
                                        uint32_t quota, uint64_t scope_hash);

/* Consume one quota unit of a caller-owned capability (SYS_CAP_CONSUME).
 * Includes lazy expiry: an expired entry is revoked in place and
 * reported as ERR_NOENT.  Returns 0, or ERR_NOENT (missing/stale/
 * expired) / ERR_INVAL (bad handle). */
int cap_consume(int handle);

/* Revoke every capability held by `subject` matching `atom`, optionally
 * restricted to a scope_hash (0 = any scope), across ALL kernel cap
 * tables (SYS_CAP_REVOKE_BY_ATOM).  Returns the number of entries
 * revoked (>= 0), or ERR_INVAL for a bad atom. */
int cap_revoke_by_atom(uint64_t subject, atom_id_t atom, uint64_t scope_hash);

/* P1 地基: issue an atom capability to the process holding `subject`
 * (SYS_CAP_GRANT_TO_SUBJECT).  The perm-engine's decision-encoding
 * path: resolves the target process by subject and creates the atom
 * cap in ITS table (entry.subject = target).  Returns a handle > 0,
 * ERR_NOENT (no live process holds the subject), ERR_INVAL (bad
 * atom/rights), or ERR_NOMEM (target table full). */
int cap_grant_to_subject(uint64_t subject, atom_id_t atom, int rights,
                         uint64_t expiry_ticks, uint32_t quota);

/* P1 地基: read-only atom-holding query (SYS_CAP_HAS_ATOM).  Returns 1
 * when the process holding `subject` has a live atom cap for `atom`,
 * 0 when it does not.  Gated on the CALLER holding ATOM_SERVICE_MANAGE
 * (docs/ops_format.md §6) — only management-plane processes may inspect
 * another subject's atom holdings.  ERR_NOENT (no live process holds
 * the subject), ERR_INVAL (bad atom), ERR_NOCAP (caller not
 * management-plane). */
int cap_has_atom(uint64_t subject, atom_id_t atom);

/* Receive with sender identity (SYS_IPC_RECV_FROM): identical to
 * ipc_recv() plus, when sender_subject is non-NULL, the kernel-filled
 * unforgeable sender subject is written to it. */
int ipc_recv_from(int port, void *buf, int *len, int *tok,
                                    uint64_t *sender_subject);

#endif /* LIBOS_SYSCALLS_H */
