/*
 * ipc.c - Higher-level IPC client library
 * Copyright (c) 2026 OpSys Project
 *
 * Simplifies service discovery and request/response patterns
 * on top of the raw syscall wrappers.
 */

#include "ipc_client.h"
#include "../libos/syscalls.h"

int ipc_connect(const char *service_name) {
    /* Look up the port by well-known name */
    int port = port_get(service_name);
    if (port < 0) {
        return port; /* propagate error from registry */
    }
    return port;
}

int ipc_request(int port, const void *req, int req_len, void *resp, int resp_len) {
    int actual_len = resp_len;
    int ret        = ipc_call(port, req, req_len, resp, &actual_len);
    return ret;
}
