/*
 * shm.h - Shared physical-page pools (zero-copy read path)
 * Copyright (c) 2026 OpSys Project
 *
 * SYS_SHM_CREATE / SYS_SHM_MAP: trusted services allocate physical-
 * page pools; vfs_server maps pool pages read-only into authorized
 * clients.  See kernel/mm/shm.c for the security model.
 */

#ifndef KERNEL_SHM_H
#define KERNEL_SHM_H

#include <kernel/types.h>

/**
 * Release every pool owned by a dying process (called from
 * process_reap in process.c).
 * @param owner  The dying process's subject_id.
 */
void shm_cleanup_process(subject_id_t owner);

#endif /* KERNEL_SHM_H */
