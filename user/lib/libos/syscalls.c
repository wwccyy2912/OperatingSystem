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
 * syscalls.c - User-space system call wrappers
 * Copyright (c) 2026 OpSys Project
 *
 * Each function is a thin wrapper that passes arguments
 * to the raw sys_call() inline assembly via INT 0x80.
 *
 * ------------------------------------------------------------------
 * Structure (flat wrapper set, grouped by subsystem):
 *   user app / lib (shell, libfs, libipc, libpkg ...)
 *        |
 *   syscalls.c: one thin wrapper per SYS_* operation
 *        |  pack args -> sys_call(SYS_X, a1..a5)
 *        v
 *   INT 0x80 -> kernel syscall handler
 *
 * How it works:
 *   Each wrapper maps its C arguments onto the fixed six-slot
 *   sys_call(number, a1..a5) inline asm; the kernel dispatches on the
 *   SYS_* number and returns a raw errno-style int.
 *
 * Purpose:
 *   The single user-space ABI surface for talking to the kernel;
 *   everything else (IPC, VFS, pkg, process, caps) builds on it.
 *
 * Caveats:
 *   Wrappers do no validation, buffering or locking; callers must pass
 *   sane pointers and check the raw error return.  A few calls never
 *   return on success (ThreadExit, sys_reboot/shutdown/panic).
 * ------------------------------------------------------------------
 */

#include "syscalls.h"
#include "elf_parse.h"

/* ---- Debug / Serial I/O ---- */

int DebugLog(const char *str) {
    return (int)sys_call(SYS_DEBUG_LOG, (long)str, 0, 0, 0, 0);
}

int DebugGetchar(void) {
    return (int)sys_call(SYS_DEBUG_GETCHAR, 0, 0, 0, 0, 0);
}

/* ---- Capability management ---- */

int CapCreate(int type, int rights) {
    return (int)sys_call(SYS_CAP_CREATE, type, rights, 0, 0, 0);
}

/*
 * cap_create_obj: create a capability naming a specific object.
 * obj_id semantics depend on the type:
 *   CAP_TYPE_IRQ     → IRQ line number (bind_irq requires it to match)
 *   CAP_TYPE_IO_PORT → port range (count << 16) | base_port
 *   other types      → unused (0)
 */
int CapCreateObj(int type, int rights, unsigned long obj_id) {
    return (int)sys_call(SYS_CAP_CREATE, type, rights, (long)obj_id, 0, 0);
}

int CapGrant(int handle, int target_pid, int rights) {
    return (int)sys_call(SYS_CAP_GRANT, handle, target_pid, rights, 0, 0);
}

int CapRevoke(int handle) {
    return (int)sys_call(SYS_CAP_REVOKE, handle, 0, 0, 0, 0);
}

/* ---- IPC ---- */

int IpcSend(int port, const void *msg, int len) {
    return (int)sys_call(SYS_IPC_SEND, port, (long)msg, len, 0, 0);
}

int IpcRecv(int port, void *buf, int *len, int *tok) {
    return (int)sys_call(SYS_IPC_RECV, port, (long)buf, (long)len, (long)tok, 0);
}

int IpcCall(int port, const void *req, int req_len, void *resp, int *resp_len) {
    return (int)sys_call(SYS_IPC_CALL, port, (long)req, req_len, (long)resp, (long)resp_len);
}

int IpcPortCreate(void) {
    return (int)sys_call(SYS_IPC_PORT_CREATE, 0, 0, 0, 0, 0);
}

int IpcReply(int token, const void *resp, int resp_len) {
    return (int)sys_call(SYS_IPC_REPLY, token, (long)resp, resp_len, 0, 0);
}

/* ---- Memory ---- */

void *map_memory(int cap, uint64_t offset, uint64_t size, int prot) {
    return (void *)sys_call(SYS_MAP_MEMORY, cap, (long)offset, (long)size, prot, 0);
}

int UnmapMemory(void *addr, uint64_t size) {
    return (int)sys_call(SYS_UNMAP_MEMORY, (long)addr, (long)size, 0, 0, 0);
}

/* ---- Thread management ---- */

int ThreadCreate(void (*entry)(void *), void *arg, int priority) {
    return (int)sys_call(SYS_THREAD_CREATE, (long)entry, (long)arg, priority, 0, 0);
}

void ThreadExit(int code) {
    sys_call(SYS_THREAD_EXIT, code, 0, 0, 0, 0);
    /* Should never return */
    __builtin_unreachable();
}

/* --- VSpace (roadmap P1): reserve VA + pre-build page tables --- */
void *vspace_alloc(unsigned long size, unsigned long flags) {
    return (void *)sys_call(SYS_VSPACE_ALLOC, size, flags, 0, 0, 0);
}

/* --- Thread context (roadmap P2): overwrite a thread's saved state --- */
int ThreadSetCtx(int tid, const void *ctx, unsigned long ctx_size) {
    return (int)sys_call(SYS_THREAD_SET_CTX, tid, (long)ctx, ctx_size, 0, 0);
}

/* --- PCI enumeration --- */
int PciGetCount(void) {
    return (int)sys_call(SYS_PCI_GET_COUNT, 0, 0, 0, 0, 0);
}

int PciGetDevice(int index, pci_device_info_t *out) {
    return (int)sys_call(SYS_PCI_GET_DEVICE, index, (long)out, 0, 0, 0);
}

int PciCfgRead32(int index, unsigned offset) {
    return (int)sys_call(SYS_PCI_CFG_READ, index, (long)offset, 0, 0, 0);
}

int PciCfgWrite32(int index, unsigned offset, unsigned val) {
    return (int)sys_call(SYS_PCI_CFG_WRITE, index, (long)offset, (long)val, 0, 0);
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

void ThreadYield(void) {
    sys_call(SYS_THREAD_YIELD, 0, 0, 0, 0, 0);
}

int ThreadSetAffinity(int tid, int cpu) {
    return (int)sys_call(SYS_THREAD_SET_AFFINITY, tid, cpu, 0, 0, 0);
}

int ThreadJoin(int tid, int *exit_code) {
    return (int)sys_call(SYS_THREAD_JOIN, tid, (long)exit_code, 0, 0, 0);
}

/* ---- Time ---- */

int GetTime(void) {
    return (int)sys_call(SYS_GET_TIME, 0, 0, 0, 0, 0);
}

int Sleep(int ticks) {
    return (int)sys_call(SYS_SLEEP, ticks, 0, 0, 0, 0);
}

int OsGetRtcTime(rtc_time_t *out) {
    return (int)sys_call(SYS_GET_RTC_TIME, (long)out, 0, 0, 0, 0);
}

int OsSetTime(const rtc_time_t *t) {
    return (int)sys_call(SYS_SET_TIME, (long)t, 0, 0, 0, 0);
}

/* ---- Port registry ---- */

int PortRegister(const char *name, int port) {
    return (int)sys_call(SYS_PORT_REGISTER, (long)name, port, 0, 0, 0);
}

int PortGet(const char *name) {
    return (int)sys_call(SYS_PORT_GET, (long)name, 0, 0, 0, 0);
}

/* ---- Async notification ---- */

int Notify(int target_tid, unsigned int mask) {
    return (int)sys_call(SYS_NOTIFY, target_tid, (long)mask, 0, 0, 0);
}

int WaitNotification(unsigned int mask) {
    return (int)sys_call(SYS_WAIT_NOTIFICATION, (long)mask, 0, 0, 0, 0);
}

/* ---- IRQ binding ---- */

int BindIrq(int cap, int irq, unsigned int mask) {
    return (int)sys_call(SYS_BIND_IRQ, cap, irq, (long)mask, 0, 0);
}

int UnbindIrq(int cap, int irq) {
    return (int)sys_call(SYS_UNBIND_IRQ, cap, irq, 0, 0, 0);
}

/* ---- I/O port access ---- */

int IoRead8(unsigned short port) {
    return (int)sys_call(SYS_IO_READ8, (long)port, 0, 0, 0, 0);
}

int IoWrite8(unsigned short port, unsigned char val) {
    return (int)sys_call(SYS_IO_WRITE8, (long)port, (long)val, 0, 0, 0);
}

int IoRead16(unsigned short port) {
    return (int)sys_call(SYS_IO_READ16, (long)port, 0, 0, 0, 0);
}

int IoWrite16(unsigned short port, unsigned short val) {
    return (int)sys_call(SYS_IO_WRITE16, (long)port, (long)val, 0, 0, 0);
}

/* ---- System power ---- */

int sys_reboot(void) {
    return (int)sys_call(SYS_REBOOT, 0, 0, 0, 0, 0);
}

int sys_shutdown(void) {
    return (int)sys_call(SYS_SHUTDOWN, 0, 0, 0, 0, 0);
}

/* TEMP test hook: triggers a kernel panic; never returns on success. */
int sys_panic(void) {
    return (int)sys_call(SYS_PANIC, 0, 0, 0, 0, 0);
}

/* ---- Init protocol ---- */

int GetFreePages(void) {
    return (int)sys_call(SYS_GET_FREE_PAGES, 0, 0, 0, 0, 0);
}

int GetPid(void) {
    return (int)sys_call(SYS_GET_PID, 0, 0, 0, 0, 0);
}

uint64_t GetHeapBase(void) {
    return (uint64_t)sys_call(SYS_GET_HEAP_BASE, 0, 0, 0, 0, 0);
}

/* ---- Process management ---- */

/*
 * Roadmap P1: ELF parsing now happens HERE in user space.  ProcessCreate()
 * parses the ELF blob with ElfParse() into a proc_image_desc_t + segment
 * table, then asks the kernel to build the address space, copy the opaque
 * segment bytes from the blob, zero BSS and start the process.  The kernel
 * never parses the blob's file format (see kernel/syscall/process_desc.c).
 *
 * The descriptor and its segment table must be CONTIGUOUS: the kernel ABI
 * reads the proc_seg_desc_t array right after the proc_image_desc_t.
 */
int ProcessCreate(const char *name, const void *elf, unsigned long size) {
    struct {
        proc_image_desc_t desc;
        proc_seg_desc_t   segs[ELF_MAX_LOAD_SEGS];
    } img;

    int err = ElfParse(elf, size, &img.desc, img.segs, ELF_MAX_LOAD_SEGS);
    if (err < 0)
        return err;

    return (int)sys_call(SYS_PROCESS_CREATE, (long)name, (long)&img.desc, (long)elf, (long)size, 0);
}

int ProcessWait(int pid, int *exit_code) {
    return (int)sys_call(SYS_PROCESS_WAIT, (long)pid, (long)exit_code, 0, 0, 0);
}

int ProcessList(proc_info_t *buf, int max_entries) {
    return (int)sys_call(SYS_PROCESS_LIST, (long)buf, max_entries, 0, 0, 0);
}

int ProcInfoBySubject(uint64_t subject, proc_ident_t *out) {
    return (int)sys_call(SYS_PROC_INFO_BY_SUBJECT, (long)subject, (long)out, 0, 0, 0);
}

/* ---- Embedded blob access ---- */

int BlobGet(const char *name, void *buf, int buf_size) {
    return (int)sys_call(SYS_BLOB_GET, (long)name, (long)buf, buf_size, 0, 0);
}

/* ---- Mutex ---- */

int MutexCreate(void) {
    return (int)sys_call(SYS_MUTEX_CREATE, 0, 0, 0, 0, 0);
}

int MutexLock(int handle) {
    return (int)sys_call(SYS_MUTEX_LOCK, handle, 0, 0, 0, 0);
}

int MutexUnlock(int handle) {
    return (int)sys_call(SYS_MUTEX_UNLOCK, handle, 0, 0, 0, 0);
}

int MutexDestroy(int handle) {
    return (int)sys_call(SYS_MUTEX_DESTROY, handle, 0, 0, 0, 0);
}

/* ---- Signals ---- */

/*
 * Signal() lives in the C runtime (user/runtime/signal_user.c): with
 * semantics moved to Ring 3 (kernel_roadmap.md D4/P2), registering a
 * handler is a plain user-memory table swap -- no syscall involved.
 * Kill() remains a syscall: the kernel latches the pending bit and
 * the Ring 3 dispatcher decides ignore/default/handler at delivery.
 */
int Kill(int pid, int signum) {
    return (int)sys_call(SYS_KILL, pid, signum, 0, 0, 0);
}

/* ---- Framebuffer ---- */

int FbGetInfo(fb_user_info_t *out) {
    return (int)sys_call(SYS_FB_GET_INFO, (long)out, 0, 0, 0, 0);
}

void *fb_map(void *virt, unsigned long size) {
    return (void *)sys_call(SYS_FB_MAP, (long)virt, (long)size, 0, 0, 0);
}

/* ---- P0 地基: permission model (docs/permission_model.md) ---- */

uint64_t GetSubject(void) {
    return (uint64_t)sys_call(SYS_GET_SUBJECT, 0, 0, 0, 0, 0);
}

int CapCreateAtom(
    atom_id_t atom, int rights, uint64_t expiry_ticks, uint32_t quota, uint64_t scope_hash) {
    return (int)sys_call(SYS_CAP_CREATE_ATOM,
                         (long)atom,
                         (long)rights,
                         (long)expiry_ticks,
                         (long)quota,
                         (long)scope_hash);
}

int CapConsume(int handle) {
    return (int)sys_call(SYS_CAP_CONSUME, (long)handle, 0, 0, 0, 0);
}

int CapRevokeByAtom(uint64_t subject, atom_id_t atom, uint64_t scope_hash) {
    return (int)sys_call(SYS_CAP_REVOKE_BY_ATOM, (long)subject, (long)atom, (long)scope_hash, 0, 0);
}

int CapGrantToSubject(
    uint64_t subject, atom_id_t atom, int rights, uint64_t expiry_ticks, uint32_t quota) {
    return (int)sys_call(SYS_CAP_GRANT_TO_SUBJECT,
                         (long)subject,
                         (long)atom,
                         (long)rights,
                         (long)expiry_ticks,
                         (long)quota);
}

int CapHasAtom(uint64_t subject, atom_id_t atom) {
    return (int)sys_call(SYS_CAP_HAS_ATOM, (long)subject, (long)atom, 0, 0, 0);
}

int IpcRecvFrom(int port, void *buf, int *len, int *tok, uint64_t *sender_subject) {
    return (int)sys_call(
        SYS_IPC_RECV_FROM, port, (long)buf, (long)len, (long)tok, (long)sender_subject);
}

/* ---- Phase 3: zero-copy read path (shared physical-page pools) ---- */

/* SYS_SHM_CREATE: allocate `count` contiguous physical pages and map
 * them at `virt` (a vspace_alloc()'d range) in the caller's address
 * space.  Gated on ATOM_SERVICE_MANAGE.  Returns the pool's physical
 * base (the handle for shm_map), or a negative error. */
uint64_t ShmCreate(uint64_t count, void *virt) {
    return (uint64_t)sys_call(SYS_SHM_CREATE, (long)count, (long)virt, 0, 0, 0);
}

/* SYS_SHM_MAP: map `count` pages of the pool at `phys_base` READ-ONLY
 * into the process holding `subject` at `virt` (vspace_alloc'ed by the
 * client).  Gated on ATOM_SERVICE_MANAGE + pool-table verification.
 * Returns 0, or a negative error. */
int ShmMap(uint64_t phys_base, uint64_t count, uint64_t subject, void *virt) {
    return (int)sys_call(
        SYS_SHM_MAP, (long)phys_base, (long)count, (long)subject, (long)virt, 0);
}
