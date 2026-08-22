/*
 * crashpeer.c - IPC peer-death test service (independent process)
 * Copyright (c) 2026 OpSys Project
 *
 * Exists ONLY to prove kernel crash recovery: it owns a port, receives
 * ONE synchronous call, then exits WITHOUT replying — a simulated
 * crash mid-call.  The init regression suite blocks in ipc_call() on
 * this port and asserts it wakes with ERR_NOENT (process_reap →
 * ipc_cleanup_process destroys the port, waking the caller) instead of
 * hanging forever.
 *
 * Deliberately given no caps and no IRQ bindings.
 */

#include <libos/syscalls.h>

#define CRASHPEER_PORT_NAME "crashpeer"

int main(void) {
    int port = ipc_port_create();
    if (port < 0)
        return 1;

    int ret = port_register(CRASHPEER_PORT_NAME, port);
    if (ret < 0)
        return 1;

    /* Serve exactly one call, then die without replying: the caller's
     * pending reply slot is orphaned exactly like a real service crash. */
    char buf[64];
    int  len = (int)sizeof(buf);
    int  tok = 0;
    ret      = ipc_recv(port, buf, &len, &tok);
    if (ret < 0)
        return 1;

    return 0; /* no ipc_reply — simulated crash */
}
