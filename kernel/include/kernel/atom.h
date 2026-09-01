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
 * atom.h - Permission atom enumeration (P0 地基)
 * Copyright (c) 2026 OpSys Project
 *
 * Atoms are the semantic index of the attribute-based permission model.
 * A capability may carry an atom_id (0 = ATOM_NONE = no atom semantics);
 * the perm-engine (P1) is the only entity that signs atom capabilities,
 * gating of the atom syscalls to it lands in P1 (consistent with the
 * currently unrestricted sys_cap_create).
 *
 * Header-only: no atom.c exists (spec constraint).
 */

#ifndef KERNEL_ATOM_H
#define KERNEL_ATOM_H

typedef enum {
    ATOM_NONE = 0,
    /* 系统与硬件 */
    ATOM_SYS_SHUTDOWN,
    ATOM_SYS_SET_TIME,
    ATOM_SYS_SET_TIMEZONE,
    ATOM_HW_CAMERA_CAPTURE,
    ATOM_HW_MIC_RECORD,
    ATOM_HW_GPU_HIGH_PERF,
    ATOM_HW_LOC_COARSE,
    ATOM_HW_LOC_PRECISE,
    /* 数据与文件 */
    ATOM_DATA_DOCS_READ,
    ATOM_DATA_DOCS_WRITE,
    ATOM_DATA_DL_WRITE,
    ATOM_DATA_APP_CONTAINER_READ,
    ATOM_DATA_SYS_LOGS_READ,
    ATOM_BOOKMARK_RESOLVE,
    /* 网络 */
    ATOM_NET_BIND,
    ATOM_NET_CONNECT,
    ATOM_NET_WIFI_SCAN,
    ATOM_NET_WIFI_SET,
    /* 管理 */
    ATOM_PKG_INSTALL,
    ATOM_PKG_UPDATE_SYS,
    ATOM_SERVICE_MANAGE,
    /* 开发者 */
    ATOM_SYS_DEBUG,
    ATOM_CAP_GRANT_SELF,
    ATOM_MAX
} atom_id_t;

#endif /* KERNEL_ATOM_H */
