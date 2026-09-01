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
 * pkg.h - libpkg: user-space pkg-manager client library
 * Copyright (c) 2026 OpSys Project
 *
 * Thin client over the "pkg" server port (user/services/pkg/pkg.h
 * protocol, docs/ops_format.md §7).  All functions return 0 on success
 * or a negative error code.
 *
 * Contract status: FROZEN for Phase A (see docs/ops_format.md header).
 * Do not modify prototypes in this file without orchestrator approval.
 */

#ifndef USER_LIB_LIBPKG_PKG_H
#define USER_LIB_LIBPKG_PKG_H

#include <stdint.h>
#include "../../services/pkg/pkg.h"

/* Install kernel blob `name` as a .ops app.  `perms` is a
 * comma-separated atom-name list (may be "" = no permissions). */
int PkgInstall(const char *name, const char *perms);

/* Enumerate installed apps (max PKG_MAX_APPS).  Fills *count. */
int PkgList(char apps[PKG_MAX_APPS][PKG_NAME_MAX], uint32_t *count);

/* Spawn an installed app; fills *pid on success. */
int PkgRun(const char *app_id, int32_t *pid);

/* Delete an installed app (recursive). */
int PkgRemove(const char *app_id);

/* Sandbox handshake: the app derives its own name via
 * GetSubject() + ProcInfoBySubject(), sends PKG_OP_APP_READY, and
 * returns the reply.  On 0, manifest atoms are in the caller's kernel
 * cap table.  Non-zero means no rights were granted. */
int PkgReady(void);

#endif /* USER_LIB_LIBPKG_PKG_H */
