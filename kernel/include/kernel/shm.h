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
void ShmCleanupProcess(subject_id_t owner);

#endif /* KERNEL_SHM_H */
