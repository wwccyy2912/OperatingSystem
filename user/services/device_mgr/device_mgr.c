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
 * device_mgr.c - PCI device manager service (ring-3, independent process)
 * Copyright (c) 2026 OpSys Project
 *
 * User-space front-end for the kernel's PCI enumeration syscalls
 * (SYS_PCI_GET_COUNT / SYS_PCI_GET_DEVICE, kernel/syscall/pci.c).
 * The kernel scans PCI config space over the 0xCF8/0xCFC I/O ports
 * once at boot; this service queries that snapshot through the libos
 * wrappers (pci_get_count / pci_get_device) and serves it to clients
 * over a registered IPC port — so a client needs no I/O-port or PCI
 * capability to learn about the bus.
 *
 * Architecture (mirrors serial.c / perm-manager.c):
 *
 *   manager process
 *     └─ ProcessCreate("device_mgr")      this process
 *          └─ main() = register "device_mgr" port, then serve
 *               └─ loop: ipc_recv -> op dispatch -> ipc_reply
 *
 * IPC protocol (flat structs, raw copy, native little-endian):
 *   Request:  { u32 op; i32 index; }
 *     op 0 = GET_COUNT   (index unused)
 *     op 1 = GET_DEVICE  (index = 0-based device index)
 *   Response: { i32 ret; pci_device_info_t dev; }
 *     GET_COUNT : ret = number of PCI devices found (>= 0)
 *     GET_DEVICE: ret = 0 and dev filled on success; else a negative
 *                 error code (ERR_INVAL bad index / ERR_FAULT bad
 *                 pointer) and dev is zeroed.
 *
 * pci_device_info_t is the kernel/user shared ABI struct
 * (kernel/include/kernel/pci.h): bus/dev/func, vendor_id, device_id,
 * class_code, prog_if, revision_id, bar[6], irq_line.
 *
 * ------------------------------------------------------------------
 * Structure (main):
 *   main() -> register "device_mgr" port, then serve loop
 *     ipc_recv -> op dispatch (GET_COUNT / GET_DEVICE) -> ipc_reply
 *   data source: kernel PCI snapshot via pci_get_count/get_device
 *   pci_device_info_t: bus/dev/func, ids, class, bars, irq_line
 * How it works:
 *   The kernel scans PCI config space (0xCF8/0xCFC) once at boot; this
 *   service queries the snapshot through libos wrappers and serves it
 *   over IPC, so clients need no I/O-port or PCI capability.
 * Purpose:
 *   User-space PCI device manager: exposes the kernel's PCI enumeration
 *   to unprivileged clients through a registered IPC port.
 * Caveats:
 *   Serves a boot-time snapshot — no live bus rescan.  Bad indices or
 *   bad client pointers return negative errors (ERR_INVAL/ERR_FAULT).
 * ------------------------------------------------------------------
 */

#include "../lib/libc/stdio.h"
#include "../lib/libc/string.h"
#include "../lib/libos/syscalls.h"
#include <stdint.h>

/* Fixed-width types (kernel/types.h is not includable from user space:
 * its error_t enum collides with the OK/ERR_* macros in syscalls.h). */
typedef uint8_t  u8;
typedef uint32_t u32;
typedef int32_t  i32;

/* ====================================================================
 * Constants
 * ==================================================================== */

#define DMGR_PORT_NAME "device_mgr" /* well-known service port */

/* Op codes (req->op) */
#define DMGR_OP_GET_COUNT  0 /* reply = device count                 */
#define DMGR_OP_GET_DEVICE 1 /* request index, reply = device info   */

/* ====================================================================
 * Protocol structures (flat, raw copy — see header comment)
 * ==================================================================== */

typedef struct {
    u32 op;    /* DMGR_OP_* */
    i32 index; /* GET_DEVICE: 0-based device index */
} dmgr_req_t;

typedef struct {
    i32               ret; /* GET_COUNT: count; GET_DEVICE: 0 or error */
    pci_device_info_t dev; /* GET_DEVICE: device snapshot (zeroed on error) */
} dmgr_resp_t;

/* ====================================================================
 * Service state
 * ==================================================================== */

static u8 s_req[sizeof(dmgr_req_t)];
static u8 s_resp[sizeof(dmgr_resp_t)];

/*
 * Reply to the client whose call token we are answering.  ret carries
 * the GET_COUNT value directly (never negative for a healthy bus) or
 * a negative error code; on error dev is zeroed so a confused client
 * never reads stale device bytes.
 */
static void DmgrReply(int token, i32 ret, const pci_device_info_t *dev) {
    dmgr_resp_t *resp = (dmgr_resp_t *)s_resp;
    resp->ret         = ret;
    if (dev) {
        resp->dev = *dev;
    } else {
        memset(&resp->dev, 0, sizeof(resp->dev));
    }
    int r = IpcReply(token, s_resp, (int)sizeof(*resp));
    if (r < 0)
        printf("device_mgr: ipc_reply failed (%d)\n", r);
}

/*
 * Interpret one client request and reply.  Never crashes on malformed
 * input: the opcode is validated and every buffer access is bounded
 * by the length ipc_recv actually reported.
 */
static void DmgrHandleRequest(int token, u32 op, int msg_len) {
    switch (op) {
    case DMGR_OP_GET_COUNT: {
        int n = PciGetCount();
        DmgrReply(token, (i32)n, NULL);
        break;
    }
    case DMGR_OP_GET_DEVICE: {
        if (msg_len < (int)sizeof(dmgr_req_t)) {
            DmgrReply(token, ERR_INVAL, NULL);
            break;
        }
        dmgr_req_t       *req = (dmgr_req_t *)s_req;
        pci_device_info_t dev;
        int               r = PciGetDevice(req->index, &dev);
        if (r < 0) {
            DmgrReply(token, (i32)r, NULL);
            break;
        }
        DmgrReply(token, 0, &dev);
        break;
    }
    default:
        DmgrReply(token, ERR_INVAL, NULL);
        break;
    }
}

/* ====================================================================
 * Server loop
 * ==================================================================== */

/*
 * Main service loop: receive a request, serve it, reply, repeat.
 * Runs on the single service thread (the port owner).
 */
static void DmgrServerLoop(int port) {
    for (;;) {
        int msg_len = (int)sizeof(s_req);
        int token   = 0;
        int ret     = IpcRecv(port, s_req, &msg_len, &token);
        if (ret < 0) {
            printf("device_mgr: ipc_recv failed (%d)\n", ret);
            ThreadExit(1);
        }
        if (msg_len < (int)sizeof(u32)) { /* no op code */
            DmgrReply(token, ERR_INVAL, NULL);
            continue;
        }
        u32 op = *(u32 *)s_req;
        DmgrHandleRequest(token, op, msg_len);
    }
}

/* ====================================================================
 * Entry point (device_mgr process main)
 * ==================================================================== */

/*
 * Boot self-check: exercise the kernel PCI syscalls before serving.
 * The count is expected to be >= 2 on QEMU (i440FX host bridge + VGA);
 * a clean 0 is still valid (no devices) and the service serves the
 * empty table fine.
 */
static void DmgrSelfCheck(void) {
    int count = PciGetCount();
    if (count < 0) {
        printf("device_mgr: pci_get_count failed (%d)\n", count);
        ThreadExit(1);
    }
    printf("device_mgr: %d PCI devices\n", count);

    if (count > 0) {
        pci_device_info_t dev;
        int               r = PciGetDevice(0, &dev);
        if (r < 0) {
            printf("device_mgr: PciGetDevice(0) failed (%d)\n", r);
        } else {
            printf("device_mgr: device 0 bus=%u dev=%u func=%u "
                   "vendor=0x%x device=0x%x class=0x%x irq=%u\n",
                   (u32)dev.bus,
                   (u32)dev.dev,
                   (u32)dev.func,
                   (u32)dev.vendor_id,
                   (u32)dev.device_id,
                   (u32)dev.class_code,
                   (u32)dev.irq_line);
        }
    }
}

int main(void) {
    printf("device_mgr: starting PCI device manager\n");

    DmgrSelfCheck();

    /* IPC port, registered under the well-known name "device_mgr". */
    int port = IpcPortCreate();
    if (port < 0) {
        printf("device_mgr: ipc_port_create failed (%d)\n", port);
        ThreadExit(1);
    }
    int ret = PortRegister(DMGR_PORT_NAME, port);
    if (ret < 0) {
        printf("device_mgr: PortRegister('%s') failed (%d)\n", DMGR_PORT_NAME, ret);
        ThreadExit(1);
    }
    printf("device_mgr: port %d registered as '%s'\n", port, DMGR_PORT_NAME);

    /* Serve clients. */
    printf("device_mgr: serving on port %d\n", port);
    DmgrServerLoop(port);
    return 0; /* unreachable */
}
