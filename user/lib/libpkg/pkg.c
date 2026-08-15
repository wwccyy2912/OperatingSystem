/*
 * pkg.c - libpkg: user-space pkg-manager client library
 * Copyright (c) 2026 OpSys Project
 *
 * Thin client over the "pkg" server port (user/services/pkg/pkg.h
 * protocol, docs/ops_format.md §7).  One ipc_call per operation, with
 * every request/response struct on the stack (all < 4096 bytes, so no
 * malloc and no shared buffers).
 */

#include "pkg.h"
#include "../libos/syscalls.h"
#include "../libc/string.h"

/* Service port handle, resolved once and cached (same as libfs). */
static int s_pkg_port = -1;

static int pkg_port(void) {
    if (s_pkg_port < 0) {
        s_pkg_port = port_get(PKG_PORT_NAME);
        if (s_pkg_port < 0)
            return s_pkg_port;
    }
    return s_pkg_port;
}

int pkg_install(const char *name, const char *perms) {
    if (!name || !perms)
        return ERR_INVAL;
    int port = pkg_port();
    if (port < 0)
        return port;

    pkg_req_install_t req;
    memset(&req, 0, sizeof(req));
    req.op = PKG_OP_INSTALL;
    strncpy(req.name, name, sizeof(req.name) - 1);
    strncpy(req.perms, perms, sizeof(req.perms) - 1);

    pkg_resp_install_t resp;
    int                resp_len = (int)sizeof(resp);
    int                r        = ipc_call(port, &req, (int)sizeof(req), &resp, &resp_len);
    if (r < 0)
        return r;
    return resp.ret;
}

int pkg_list(char apps[PKG_MAX_APPS][PKG_NAME_MAX], uint32_t *count) {
    if (!apps || !count)
        return ERR_INVAL;
    int port = pkg_port();
    if (port < 0)
        return port;

    pkg_req_list_t req;
    memset(&req, 0, sizeof(req));
    req.op = PKG_OP_LIST;

    pkg_resp_list_t resp;
    int             resp_len = (int)sizeof(resp);
    int             r        = ipc_call(port, &req, (int)sizeof(req), &resp, &resp_len);
    if (r < 0)
        return r;
    if (resp.ret < 0)
        return resp.ret;
    *count = resp.count;
    for (uint32_t i = 0; i < resp.count && i < PKG_MAX_APPS; i++)
        strncpy(apps[i], resp.apps[i], PKG_NAME_MAX);
    return 0;
}

int pkg_run(const char *app_id, int32_t *pid) {
    if (!app_id || !pid)
        return ERR_INVAL;
    int port = pkg_port();
    if (port < 0)
        return port;

    pkg_req_run_t req;
    memset(&req, 0, sizeof(req));
    req.op = PKG_OP_RUN;
    strncpy(req.app_id, app_id, sizeof(req.app_id) - 1);

    pkg_resp_run_t resp;
    int            resp_len = (int)sizeof(resp);
    int            r        = ipc_call(port, &req, (int)sizeof(req), &resp, &resp_len);
    if (r < 0)
        return r;
    if (resp.ret < 0)
        return resp.ret;
    *pid = resp.pid;
    return 0;
}

int pkg_remove(const char *app_id) {
    if (!app_id)
        return ERR_INVAL;
    int port = pkg_port();
    if (port < 0)
        return port;

    pkg_req_remove_t req;
    memset(&req, 0, sizeof(req));
    req.op = PKG_OP_REMOVE;
    strncpy(req.app_id, app_id, sizeof(req.app_id) - 1);

    pkg_resp_remove_t resp;
    int               resp_len = (int)sizeof(resp);
    int               r        = ipc_call(port, &req, (int)sizeof(req), &resp, &resp_len);
    if (r < 0)
        return r;
    return resp.ret;
}

int pkg_ready(void) {
    /* Derive our own app_id from the kernel-issued identity: the
     * pkg-manager cross-checks the real subject (ipc_recv_from) with
     * proc_info_by_subject, so the name we send must match. */
    uint64_t     subject = get_subject();
    proc_ident_t ident;
    int          r = proc_info_by_subject(subject, &ident);
    if (r < 0)
        return r;

    int port = pkg_port();
    if (port < 0)
        return port;

    pkg_req_app_ready_t req;
    memset(&req, 0, sizeof(req));
    req.op = PKG_OP_APP_READY;
    strncpy(req.app_id, ident.name, sizeof(req.app_id) - 1);

    pkg_resp_app_ready_t resp;
    int                  resp_len = (int)sizeof(resp);
    r                             = ipc_call(port, &req, (int)sizeof(req), &resp, &resp_len);
    if (r < 0)
        return r;
    return resp.ret;
}
