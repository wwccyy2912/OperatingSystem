/*
 * syscall_handlers.h - External syscall handler declarations
 * Copyright (c) 2026 OpSys Project
 *
 * Handlers owned by their subsystems (vspace, sched, pci, process).
 * syscall.c's dispatch table references these; the SYSCALLn() static
 * wrappers cover only the handlers that live inside syscall.c itself.
 */

#ifndef KERNEL_SYSCALL_HANDLERS_H
#define KERNEL_SYSCALL_HANDLERS_H

#include <kernel/types.h>

/* kernel/mm/vspace.c — SYS_VSPACE_ALLOC (arg1=size, arg2=flags) */
i64 sc_sys_vspace_alloc(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5);

/* kernel/sched/thread_ctx.c — SYS_THREAD_SET_CTX (arg1=tid, arg2=ctx, arg3=size) */
i64 sc_sys_thread_set_ctx(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5);

/* kernel/syscall/pci.c — SYS_PCI_GET_COUNT / SYS_PCI_GET_DEVICE */
i64 sc_sys_pci_get_count(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5);
i64 sc_sys_pci_get_device(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5);

/* kernel/syscall/process_desc.c — SYS_PROCESS_CREATE (descriptor-based image) */
i64 sc_sys_process_create(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5);

/* kernel/arch/x86_64/virtio_blk.c — SYS_BLK_READ/WRITE/INFO (legacy
 * virtio-blk; arg1 = disk = PCI table index of the adapter, gated on
 * CAP_TYPE_PCI_DEV with RIGHT_READ|RIGHT_WRITE for that obj_id) */
i64 sc_sys_blk_read(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5);
i64 sc_sys_blk_write(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5);
i64 sc_sys_blk_info(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5);

/* kernel/mm/shm.c — SYS_SHM_CREATE/MAP (zero-copy read path pools) */
i64 sc_sys_shm_create(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5);
i64 sc_sys_shm_map(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5);

#endif /* KERNEL_SYSCALL_HANDLERS_H */
