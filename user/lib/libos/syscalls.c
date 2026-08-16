/*
 * syscalls.c - User-space system call wrappers
 * Copyright (c) 2026 OpSys Project
 *
 * Each function is a thin wrapper that passes arguments
 * to the raw sys_call() inline assembly via INT 0x80.
 */

#include "syscalls.h"
#include "elf_parse.h"

/* ---- Debug / Serial I/O ---- */

int debug_log(const char *str) {
    return (int)sys_call(SYS_DEBUG_LOG, (long)str, 0, 0, 0, 0);
}

int debug_getchar(void) {
    return (int)sys_call(SYS_DEBUG_GETCHAR, 0, 0, 0, 0, 0);
}

/* ---- Capability management ---- */

int cap_create(int type, int rights) {
    return (int)sys_call(SYS_CAP_CREATE, type, rights, 0, 0, 0);
}

/*
 * cap_create_obj: create a capability naming a specific object.
 * obj_id semantics depend on the type:
 *   CAP_TYPE_IRQ     → IRQ line number (bind_irq requires it to match)
 *   CAP_TYPE_IO_PORT → port range (count << 16) | base_port
 *   other types      → unused (0)
 */
int cap_create_obj(int type, int rights, unsigned long obj_id) {
    return (int)sys_call(SYS_CAP_CREATE, type, rights, (long)obj_id, 0, 0);
}

int cap_grant(int handle, int target_pid, int rights) {
    return (int)sys_call(SYS_CAP_GRANT, handle, target_pid, rights, 0, 0);
}

int cap_revoke(int handle) {
    return (int)sys_call(SYS_CAP_REVOKE, handle, 0, 0, 0, 0);
}

/* ---- IPC ---- */

int ipc_send(int port, const void *msg, int len) {
    return (int)sys_call(SYS_IPC_SEND, port, (long)msg, len, 0, 0);
}

int ipc_recv(int port, void *buf, int *len, int *tok) {
    return (int)sys_call(SYS_IPC_RECV, port, (long)buf, (long)len, (long)tok, 0);
}

int ipc_call(int port, const void *req, int req_len, void *resp, int *resp_len) {
    return (int)sys_call(SYS_IPC_CALL, port, (long)req, req_len, (long)resp, (long)resp_len);
}

int ipc_port_create(void) {
    return (int)sys_call(SYS_IPC_PORT_CREATE, 0, 0, 0, 0, 0);
}

int ipc_reply(int token, const void *resp, int resp_len) {
    return (int)sys_call(SYS_IPC_REPLY, token, (long)resp, resp_len, 0, 0);
}

/* ---- Memory ---- */

void *map_memory(int cap, uint64_t offset, uint64_t size, int prot) {
    return (void *)sys_call(SYS_MAP_MEMORY, cap, (long)offset, (long)size, prot, 0);
}

int unmap_memory(void *addr, uint64_t size) {
    return (int)sys_call(SYS_UNMAP_MEMORY, (long)addr, (long)size, 0, 0, 0);
}

/* ---- Thread management ---- */

int thread_create(void (*entry)(void *), void *arg, int priority) {
    return (int)sys_call(SYS_THREAD_CREATE, (long)entry, (long)arg, priority, 0, 0);
}

void thread_exit(int code) {
    sys_call(SYS_THREAD_EXIT, code, 0, 0, 0, 0);
    /* Should never return */
    __builtin_unreachable();
}

/* --- VSpace (roadmap P1): reserve VA + pre-build page tables --- */
void *vspace_alloc(unsigned long size, unsigned long flags) {
    return (void *)sys_call(SYS_VSPACE_ALLOC, size, flags, 0, 0, 0);
}

/* --- Thread context (roadmap P2): overwrite a thread's saved state --- */
int thread_set_ctx(int tid, const void *ctx, unsigned long ctx_size) {
    return (int)sys_call(SYS_THREAD_SET_CTX, tid, (long)ctx, ctx_size, 0, 0);
}

/* --- PCI enumeration --- */
int pci_get_count(void) {
    return (int)sys_call(SYS_PCI_GET_COUNT, 0, 0, 0, 0, 0);
}

int pci_get_device(int index, pci_device_info_t *out) {
    return (int)sys_call(SYS_PCI_GET_DEVICE, index, (long)out, 0, 0, 0);
}

/* ---- Block device (Phase 1: legacy virtio-blk) ---- */

int64_t sys_blk_read(uint64_t disk, uint64_t lba, uint64_t count, void *buf) {
    return (int64_t)sys_call(SYS_BLK_READ, (long)disk, (long)lba, (long)count, (long)buf, 0);
}

int64_t sys_blk_write(uint64_t disk, uint64_t lba, uint64_t count, const void *buf) {
    return (int64_t)sys_call(SYS_BLK_WRITE, (long)disk, (long)lba, (long)count, (long)buf, 0);
}

int64_t sys_blk_info(uint64_t disk, blk_info_t *out) {
    return (int64_t)sys_call(SYS_BLK_INFO, (long)disk, (long)out, 0, 0, 0);
}

void thread_yield(void) {
    sys_call(SYS_THREAD_YIELD, 0, 0, 0, 0, 0);
}

int thread_set_affinity(int tid, int cpu) {
    return (int)sys_call(SYS_THREAD_SET_AFFINITY, tid, cpu, 0, 0, 0);
}

int thread_join(int tid, int *exit_code) {
    return (int)sys_call(SYS_THREAD_JOIN, tid, (long)exit_code, 0, 0, 0);
}

/* ---- Time ---- */

int get_time(void) {
    return (int)sys_call(SYS_GET_TIME, 0, 0, 0, 0, 0);
}

int sleep(int ticks) {
    return (int)sys_call(SYS_SLEEP, ticks, 0, 0, 0, 0);
}

int os_get_rtc_time(rtc_time_t *out) {
    return (int)sys_call(SYS_GET_RTC_TIME, (long)out, 0, 0, 0, 0);
}

int os_set_time(const rtc_time_t *t) {
    return (int)sys_call(SYS_SET_TIME, (long)t, 0, 0, 0, 0);
}

/* ---- Port registry ---- */

int port_register(const char *name, int port) {
    return (int)sys_call(SYS_PORT_REGISTER, (long)name, port, 0, 0, 0);
}

int port_get(const char *name) {
    return (int)sys_call(SYS_PORT_GET, (long)name, 0, 0, 0, 0);
}

/* ---- Async notification ---- */

int notify(int target_tid, unsigned int mask) {
    return (int)sys_call(SYS_NOTIFY, target_tid, (long)mask, 0, 0, 0);
}

int wait_notification(unsigned int mask) {
    return (int)sys_call(SYS_WAIT_NOTIFICATION, (long)mask, 0, 0, 0, 0);
}

/* ---- IRQ binding ---- */

int bind_irq(int cap, int irq, unsigned int mask) {
    return (int)sys_call(SYS_BIND_IRQ, cap, irq, (long)mask, 0, 0);
}

int unbind_irq(int cap, int irq) {
    return (int)sys_call(SYS_UNBIND_IRQ, cap, irq, 0, 0, 0);
}

/* ---- I/O port access ---- */

int io_read8(unsigned short port) {
    return (int)sys_call(SYS_IO_READ8, (long)port, 0, 0, 0, 0);
}

int io_write8(unsigned short port, unsigned char val) {
    return (int)sys_call(SYS_IO_WRITE8, (long)port, (long)val, 0, 0, 0);
}

/* ---- System power ---- */

int sys_reboot(void) {
    return (int)sys_call(SYS_REBOOT, 0, 0, 0, 0, 0);
}

/* TEMP test hook: triggers a kernel panic; never returns on success. */
int sys_panic(void) {
    return (int)sys_call(SYS_PANIC, 0, 0, 0, 0, 0);
}

/* ---- Init protocol ---- */

int get_free_pages(void) {
    return (int)sys_call(SYS_GET_FREE_PAGES, 0, 0, 0, 0, 0);
}

int get_pid(void) {
    return (int)sys_call(SYS_GET_PID, 0, 0, 0, 0, 0);
}

uint64_t get_heap_base(void) {
    return (uint64_t)sys_call(SYS_GET_HEAP_BASE, 0, 0, 0, 0, 0);
}

/* ---- Process management ---- */

/*
 * Roadmap P1: ELF parsing now happens HERE in user space.  process_create()
 * parses the ELF blob with elf_parse() into a proc_image_desc_t + segment
 * table, then asks the kernel to build the address space, copy the opaque
 * segment bytes from the blob, zero BSS and start the process.  The kernel
 * never parses the blob's file format (see kernel/syscall/process_desc.c).
 *
 * The descriptor and its segment table must be CONTIGUOUS: the kernel ABI
 * reads the proc_seg_desc_t array right after the proc_image_desc_t.
 */
int process_create(const char *name, const void *elf, unsigned long size) {
    struct {
        proc_image_desc_t desc;
        proc_seg_desc_t   segs[ELF_MAX_LOAD_SEGS];
    } img;

    int err = elf_parse(elf, size, &img.desc, img.segs, ELF_MAX_LOAD_SEGS);
    if (err < 0)
        return err;

    return (int)sys_call(SYS_PROCESS_CREATE, (long)name, (long)&img.desc, (long)elf, (long)size, 0);
}

int process_wait(int pid, int *exit_code) {
    return (int)sys_call(SYS_PROCESS_WAIT, (long)pid, (long)exit_code, 0, 0, 0);
}

int process_list(proc_info_t *buf, int max_entries) {
    return (int)sys_call(SYS_PROCESS_LIST, (long)buf, max_entries, 0, 0, 0);
}

int proc_info_by_subject(uint64_t subject, proc_ident_t *out) {
    return (int)sys_call(SYS_PROC_INFO_BY_SUBJECT, (long)subject, (long)out, 0, 0, 0);
}

/* ---- Embedded blob access ---- */

int blob_get(const char *name, void *buf, int buf_size) {
    return (int)sys_call(SYS_BLOB_GET, (long)name, (long)buf, buf_size, 0, 0);
}

/* ---- Mutex ---- */

int mutex_create(void) {
    return (int)sys_call(SYS_MUTEX_CREATE, 0, 0, 0, 0, 0);
}

int mutex_lock(int handle) {
    return (int)sys_call(SYS_MUTEX_LOCK, handle, 0, 0, 0, 0);
}

int mutex_unlock(int handle) {
    return (int)sys_call(SYS_MUTEX_UNLOCK, handle, 0, 0, 0, 0);
}

int mutex_destroy(int handle) {
    return (int)sys_call(SYS_MUTEX_DESTROY, handle, 0, 0, 0, 0);
}

/* ---- Signals ---- */

/*
 * Register a signal handler.  The kernel needs the __restore_rt
 * trampoline address (user/runtime/sigrestore.S) so it can re-enter
 * the kernel after the handler returns; supplying it is the wrapper's
 * job — the kernel stores it per-process at registration time.
 */
sighandler_t signal(int signum, sighandler_t handler) {
    extern void __restore_rt(void);
    long ret = sys_call(
        SYS_SIGNAL, signum, (long)handler, (long)(uintptr_t)__restore_rt, 0, 0);
    return (ret < 0) ? SIG_ERR : (sighandler_t)ret;
}

int kill(int pid, int signum) {
    return (int)sys_call(SYS_KILL, pid, signum, 0, 0, 0);
}

/* ---- Framebuffer ---- */

int fb_get_info(fb_user_info_t *out) {
    return (int)sys_call(SYS_FB_GET_INFO, (long)out, 0, 0, 0, 0);
}

void *fb_map(void *virt, unsigned long size) {
    return (void *)sys_call(SYS_FB_MAP, (long)virt, (long)size, 0, 0, 0);
}

/* ---- P0 地基: permission model (docs/permission_model.md) ---- */

uint64_t get_subject(void) {
    return (uint64_t)sys_call(SYS_GET_SUBJECT, 0, 0, 0, 0, 0);
}

int cap_create_atom(
    atom_id_t atom, int rights, uint64_t expiry_ticks, uint32_t quota, uint64_t scope_hash) {
    return (int)sys_call(SYS_CAP_CREATE_ATOM,
                         (long)atom,
                         (long)rights,
                         (long)expiry_ticks,
                         (long)quota,
                         (long)scope_hash);
}

int cap_consume(int handle) {
    return (int)sys_call(SYS_CAP_CONSUME, (long)handle, 0, 0, 0, 0);
}

int cap_revoke_by_atom(uint64_t subject, atom_id_t atom, uint64_t scope_hash) {
    return (int)sys_call(SYS_CAP_REVOKE_BY_ATOM, (long)subject, (long)atom, (long)scope_hash, 0, 0);
}

int cap_grant_to_subject(
    uint64_t subject, atom_id_t atom, int rights, uint64_t expiry_ticks, uint32_t quota) {
    return (int)sys_call(SYS_CAP_GRANT_TO_SUBJECT,
                         (long)subject,
                         (long)atom,
                         (long)rights,
                         (long)expiry_ticks,
                         (long)quota);
}

int cap_has_atom(uint64_t subject, atom_id_t atom) {
    return (int)sys_call(SYS_CAP_HAS_ATOM, (long)subject, (long)atom, 0, 0, 0);
}

int ipc_recv_from(int port, void *buf, int *len, int *tok, uint64_t *sender_subject) {
    return (int)sys_call(
        SYS_IPC_RECV_FROM, port, (long)buf, (long)len, (long)tok, (long)sender_subject);
}
