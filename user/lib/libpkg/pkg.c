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
 * pkg.c - libpkg: user-space pkg-manager client library
 * Copyright (c) 2026 OpSys Project
 *
 * Thin client over the "pkg" server port (user/services/pkg/pkg.h
 * protocol, docs/ops_format.md §7).  One ipc_call per operation, with
 * every request/response struct on the stack (all < 4096 bytes, so no
 * malloc and no shared buffers).
 *
 * ------------------------------------------------------------------
 * Structure (pkg-manager client):
 *   app -> PkgInstall/PkgList/PkgRun/PkgRemove/PkgReady
 *        -> build pkg_req_* on the stack -> IpcCall(s_pkg_port)
 *        -> pkg_resp_* on the stack -> return resp.ret / data
 *   PkgPort(): lazy PortGet(PKG_PORT_NAME), cached in s_pkg_port
 *
 * How it works:
 *   Each op encodes a PKG_OP_* request, sends it with one IpcCall and
 *   decodes the reply; PkgReady derives its own app_id from the kernel
 *   subject (GetSubject + ProcInfoBySubject) so the server's identity
 *   cross-check matches.
 *
 * Purpose:
 *   User-space package-manager client: install, list, run, remove
 *   apps and signal app readiness (docs/ops_format.md sec. 7).
 *
 * Caveats:
 *   Every request/response struct must stay under 4096 bytes (all on
 *   the stack, no shared buffer); names are bounded by strncpy, and
 *   both the transport error and the server's resp.ret propagate.
 * ------------------------------------------------------------------
 */

#include "pkg.h"
#include "../libos/syscalls.h"
#include "../libc/string.h"

/* Service port handle, resolved once and cached (same as libfs). */
static int s_pkg_port = -1;

static int PkgPort(void) {
    if (s_pkg_port < 0) {
        s_pkg_port = PortGet(PKG_PORT_NAME);
        if (s_pkg_port < 0)
            return s_pkg_port;
    }
    return s_pkg_port;
}

int PkgInstall(const char *name, const char *perms) {
    if (!name || !perms)
        return ERR_INVAL;
    int port = PkgPort();
    if (port < 0)
        return port;

    pkg_req_install_t req;
    memset(&req, 0, sizeof(req));
    req.op = PKG_OP_INSTALL;
    strncpy(req.name, name, sizeof(req.name) - 1);
    strncpy(req.perms, perms, sizeof(req.perms) - 1);

    pkg_resp_install_t resp;
    int                resp_len = (int)sizeof(resp);
    int                r        = IpcCall(port, &req, (int)sizeof(req), &resp, &resp_len);
    if (r < 0)
        return r;
    return resp.ret;
}

int PkgList(char apps[PKG_MAX_APPS][PKG_NAME_MAX], uint32_t *count) {
    if (!apps || !count)
        return ERR_INVAL;
    int port = PkgPort();
    if (port < 0)
        return port;

    pkg_req_list_t req;
    memset(&req, 0, sizeof(req));
    req.op = PKG_OP_LIST;

    pkg_resp_list_t resp;
    int             resp_len = (int)sizeof(resp);
    int             r        = IpcCall(port, &req, (int)sizeof(req), &resp, &resp_len);
    if (r < 0)
        return r;
    if (resp.ret < 0)
        return resp.ret;
    *count = resp.count;
    for (uint32_t i = 0; i < resp.count && i < PKG_MAX_APPS; i++)
        strncpy(apps[i], resp.apps[i], PKG_NAME_MAX);
    return 0;
}

int PkgRun(const char *app_id, int32_t *pid) {
    if (!app_id || !pid)
        return ERR_INVAL;
    int port = PkgPort();
    if (port < 0)
        return port;

    pkg_req_run_t req;
    memset(&req, 0, sizeof(req));
    req.op = PKG_OP_RUN;
    strncpy(req.app_id, app_id, sizeof(req.app_id) - 1);

    pkg_resp_run_t resp;
    int            resp_len = (int)sizeof(resp);
    int            r        = IpcCall(port, &req, (int)sizeof(req), &resp, &resp_len);
    if (r < 0)
        return r;
    if (resp.ret < 0)
        return resp.ret;
    *pid = resp.pid;
    return 0;
}

int PkgRemove(const char *app_id) {
    if (!app_id)
        return ERR_INVAL;
    int port = PkgPort();
    if (port < 0)
        return port;

    pkg_req_remove_t req;
    memset(&req, 0, sizeof(req));
    req.op = PKG_OP_REMOVE;
    strncpy(req.app_id, app_id, sizeof(req.app_id) - 1);

    pkg_resp_remove_t resp;
    int               resp_len = (int)sizeof(resp);
    int               r        = IpcCall(port, &req, (int)sizeof(req), &resp, &resp_len);
    if (r < 0)
        return r;
    return resp.ret;
}

int PkgReady(void) {
    /* Derive our own app_id from the kernel-issued identity: the
     * pkg-manager cross-checks the real subject (ipc_recv_from) with
     * proc_info_by_subject, so the name we send must match. */
    uint64_t     subject = GetSubject();
    proc_ident_t ident;
    int          r = ProcInfoBySubject(subject, &ident);
    if (r < 0)
        return r;

    int port = PkgPort();
    if (port < 0)
        return port;

    pkg_req_app_ready_t req;
    memset(&req, 0, sizeof(req));
    req.op = PKG_OP_APP_READY;
    strncpy(req.app_id, ident.name, sizeof(req.app_id) - 1);

    pkg_resp_app_ready_t resp;
    int                  resp_len = (int)sizeof(resp);
    r                             = IpcCall(port, &req, (int)sizeof(req), &resp, &resp_len);
    if (r < 0)
        return r;
    return resp.ret;
}
