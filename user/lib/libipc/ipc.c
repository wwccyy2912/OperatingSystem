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
 * ipc.c - Higher-level IPC client library
 * Copyright (c) 2026 OpSys Project
 *
 * Simplifies service discovery and request/response patterns
 * on top of the raw syscall wrappers.
 *
 * ------------------------------------------------------------------
 * Structure (two thin helpers):
 *   client app
 *     |
 *   IpcConnect(name) -> PortGet(name) -> port (registry lookup)
 *     |
 *   IpcRequest(port, req, resp) -> IpcCall() -> server reply
 *     |
 *     v
 *   raw syscall wrappers (syscalls.c: IpcCall / PortGet)
 *
 * How it works:
 *   IpcConnect resolves a well-known service name through the port
 *   registry; IpcRequest performs one synchronous request/response
 *   exchange via IpcCall, propagating both layers' errors.
 *
 * Purpose:
 *   A tiny convenience layer so clients write name-based connect +
 *   call instead of poking the port registry and IpcCall directly.
 *
 * Caveats:
 *   Blocking calls with no timeout or retry; registry and transport
 *   errors are returned as-is, and the response length the server
 *   saw is not reflected back to the caller.
 * ------------------------------------------------------------------
 */

#include "ipc_client.h"
#include "../libos/syscalls.h"

int IpcConnect(const char *service_name) {
    /* Look up the port by well-known name */
    int port = PortGet(service_name);
    if (port < 0) {
        return port; /* propagate error from registry */
    }
    return port;
}

int IpcRequest(int port, const void *req, int req_len, void *resp, int resp_len) {
    int actual_len = resp_len;
    int ret        = IpcCall(port, req, req_len, resp, &actual_len);
    return ret;
}
