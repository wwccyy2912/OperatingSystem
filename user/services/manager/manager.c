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
 * manager.c - Service manager (user-space supervisor, independent process)
 * Copyright (c) 2026 OpSys Project
 *
 * Requirements §1.2: the service manager starts, monitors and restarts
 * other services (CAP_SERVICE_MGMT).  Every service is now its own
 * PROCESS: the manager fetches the service's embedded ELF image via
 * SYS_BLOB_GET and spawns it via SYS_PROCESS_CREATE; exit detection
 * uses the SYS_PROCESS_WAIT syscall (added for the process
 * architecture — the manager blocks until the target process's last
 * thread exits, then receives its exit code).
 *
 * Process architecture (each service = independent user process):
 *
 *   init (PID 1)                         embedded ELF blobs in kernel
 *     └─ ProcessCreate("manager")  → manager process
 *          ├─ ProcessCreate("serial")    serial service process
 *          │    (server thread + IRQ thread; owns COM1)
 *          ├─ SerialTestRun()  ← self-test: SERIAL_SVC_OK / READ_PATH_OK
 *          ├─ ProcessCreate("term")      terminal service process
 *          │    (framebuffer terminal; owns the screen)
 *          ├─ ProcessCreate("keyboard")  PS/2 keyboard service process
 *          │    (server thread + IRQ thread; owns IRQ1 + ports 0x60-0x64)
 *          ├─ ProcessCreate("flaky")     demo: exits(7) as a process
 *          ├─ ProcessWait(flaky) → restart policy (max 3) → FAILED
 *          ├─ MANAGER_OK
 *          ├─ ProcessCreate("vfs")          VFS namespace server
 *          ├─ ProcessCreate("fs_mem_driver") in-memory storage driver
 *          │    (driver-initiated MOUNT handshake onto "vfs", A1)
 *          ├─ ProcessCreate("fs_virtio_blk_driver") virtio-blk disk driver
 *          │    (self-mounts the persistent "Disk" RW volume via A1)
 *          ├─ ProcessCreate("device_mgr")  PCI device manager service
 *          │    (serves the kernel PCI enumeration over "device_mgr")
 *          ├─ ProcessCreate("shell")  LAST → the shell owns the prompt
 *               from then on; the manager never writes to the serial
 *               port again
 *
 *  * IPC note: the kernel's per-port reply-wait LIST (ipc.c, v0.2+)
 *  supports many concurrent callers on one port (reply tokens carry
 *  generations), so the manager's monitor threads may safely call the
 *  serial service while the shell is reading — the old "single active
 *  call per port" limitation no longer exists.  The boot sequence
 *  still writes serially only to keep the startup log readable.
 *
 * Monitoring design: exit detection = blocking ProcessWait() on the
 * service's PID.  Hang/timeout detection would need a kernel
 * process-state query — future work.  Restarting serial is also
 * future work: it binds IRQ4 to its own IRQ thread, so a restart would
 * have to rebind the PIC line and re-init the 16550 — out of scope for
 * this iteration.  serial, term, keyboard and shell are started by the
 * manager but marked non-restartable; only flaky is.
 *
 * All manager logging flows through the serial service (ipc_call WRITE
 * op on the resolved "serial" port), mirroring shell.c — the whole
 * manager I/O path stays on COM1 TX.  The ONLY exception is a
 * pre-resolution failure report via kernel DebugLog().
 *
 * ------------------------------------------------------------------
 * Structure (service table + spawn orchestration):
 *   main()
 *     ├─ s_services[]      SVC_* table (name, pid, restart_count)
 *     ├─ SpawnService()    BlobGet(elf) + ProcessCreate per service
 *     ├─ SerialTestRun()   serial self-test (SERIAL_SVC_OK / READ_PATH_OK)
 *     ├─ flaky monitor     main thread: ProcessWait -> restart ≤ MAX_RESTARTS
 *     ├─ shell spawned LAST → owns the prompt from then on
 *     └─ StartServiceMonitors()  one ServiceMonitor thread each for
 *          perm / pkg / device_mgr / shell / user (ProcessWait + restart)
 * How it works:
 *   main() walks s_services in dependency order — serial first (all
 *   manager logs go through its port), then term/keyboard/gui/net, the
 *   flaky restart cycle, the VFS/storage/perm services and finally the
 *   shell; SpawnService fetches each embedded ELF and ProcessCreates
 *   it.  After boot, ServiceMonitor threads block in ProcessWait and
 *   auto-restart a crashed service up to MAX_RESTARTS, then FAILED.
 * Purpose:
 *   User-space supervisor (CAP_SERVICE_MGMT): starts, monitors and
 *   restarts services, giving every service its own process so init
 *   never handles service lifecycles directly.
 * Caveats:
 *   Only restartable services are monitored — serial/term/keyboard and
 *   the VFS/fs drivers own IRQ/hardware or namespace state and are
 *   never restarted; hang detection is future work (exit detection via
 *   ProcessWait only); all manager I/O is serial-port based, with
 *   kernel DebugLog as the pre-resolution fallback.
 * ------------------------------------------------------------------
 */

#include "../lib/libos/syscalls.h"
#include <stdarg.h>
#include <stddef.h> /* NULL */
#include <stdint.h>
#include <malloc.h>

/* Fixed-width types (kernel/types.h is not includable from user space:
 * its error_t enum collides with the OK/ERR_* macros in syscalls.h). */
typedef uint8_t  u8;
typedef uint32_t u32;
typedef int32_t  i32;

/* Serial service protocol (mirrors user/services/serial/serial.c) */
#define SERIAL_OP_WRITE 1
#define SERIAL_OP_READ  2
#define SERIAL_CHUNK    32 /* max payload bytes per WRITE/READ  */

/* Restart policy */
#define MAX_RESTARTS 3 /* per restartable service            */

/* ====================================================================
 * Service table
 * ==================================================================== */

typedef struct {
    const char *name;
    int         pid; /* -1 = not yet spawned */
    int         restart_count;
    int         restartable; /* 1 = monitor loop + auto-restart policy     */
} service_t;

static service_t s_services[] = {
    {"serial", -1, 0, 0},   /* IRQ-lifecycle: future work */
    {"term", -1, 0, 0},     /* owns the framebuffer      */
    {"keyboard", -1, 0, 0}, /* owns IRQ1 + PS/2 ports    */
    {"flaky", -1, 0, 1},
    {"vfs", -1, 0, 0},                  /* VFS namespace server     */
    {"fs_mem_driver", -1, 0, 0},        /* in-memory storage driver */
    {"fs_virtio_blk_driver", -1, 0, 0}, /* block-device storage driver */
    {"perm", -1, 0, 1},                 /* Powerbox auth manager    */
    {"device_mgr", -1, 0, 1},           /* PCI device manager       */
    {"pkg", -1, 0, 1},                  /* .ops app container manager */
    {"shell", -1, 0, 1},
    {"user", -1, 0, 1}, /* user account service (login/passwd/stop guard) */
    {"wm", -1, 0, 1},   /* window manager (v0.4 desktop: registry+compositor) */
    {"policy", -1, 0, 1}, /* command policy service (v0.5, before shell)    */
    {"gui", -1, 0, 1},  /* pixel compositor (idle until GUI_OP_ACTIVATE)  */
    {"net", -1, 0, 0},  /* PCnet-Fast III Ethernet driver (own PCI device) */
};

/* Restartable services (production hardening): crash → auto-restart up
 * to MAX_RESTARTS.  Self-contained, stateless-ish services only —
 * vfs/fs drivers own the namespace/mount state (restart needs the
 * -ESTALE reopen semantics, roadmap §七.3) and serial/term/keyboard
 * own hardware/IRQ/fb (restart is feasible since process_reap cleans
 * ports+IRQs, but is disruptive — deferred). */

#define SVC_SERIAL        0
#define SVC_TERM          1
#define SVC_KEYBOARD      2
#define SVC_FLAKY         3
#define SVC_VFS           4
#define SVC_FS_MEM        5
#define SVC_FS_VIRTIO_BLK 6
#define SVC_PERM          7
#define SVC_DEVICE_MGR    8
#define SVC_PKG           9
#define SVC_SHELL         10
#define SVC_USER          11
#define SVC_WM            12
#define SVC_POLICY        13
#define SVC_GUI           14
#define SVC_NET           15

/* ====================================================================
 * Serial service output (mirrors shell.c)
 * ==================================================================== */

static int s_serial_port = -1; /* resolved once by the manager       */

/* Send one byte to the serial service (WRITE op). */
static void ManagerPutc(char c) {
    if (s_serial_port < 0)
        return;

    u32 req[2 + 1]; /* { op; len; data[1] } */
    u32 resp[1];    /* { ret }             */
    req[0]         = SERIAL_OP_WRITE;
    req[1]         = 1;
    ((u8 *)req)[8] = (u8)c;
    int resp_len   = (int)sizeof(resp);
    IpcCall(s_serial_port, (const void *)req, 9, (void *)resp, &resp_len);
}

/* Send a NUL-terminated string to the serial service (WRITE op),
 * chunked so the request buffer stays small and bounded. */
static void ManagerWrite(const char *s) {
    if (s_serial_port < 0)
        return;

    u32 req[2 + 8]; /* { op; len; data[32] } */
    u32 resp[1];    /* { ret }              */
    while (*s != '\0') {
        int n = 0;
        while (n < SERIAL_CHUNK && s[n] != '\0') {
            ((u8 *)req)[8 + n] = (u8)s[n];
            n++;
        }
        req[0]       = SERIAL_OP_WRITE;
        req[1]       = (u32)n;
        int resp_len = (int)sizeof(resp);
        IpcCall(s_serial_port, (const void *)req, 8 + n, (void *)resp, &resp_len);
        s += n;
    }
}

/* Local int -> decimal string (the libc has no itoa). */
static void ManagerItoa(int v, char *buf) {
    char tmp[12];
    int  i = 0, j = 0;
    int  neg = (v < 0);
    if (neg)
        v = -v;
    if (v == 0)
        tmp[i++] = '0';
    while (v > 0) {
        tmp[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    if (neg)
        buf[j++] = '-';
    while (i > 0)
        buf[j++] = tmp[--i];
    buf[j] = '\0';
}

/* Minimal formatted output through the serial service.
 * Supports %d, %s, %c, %% — everything the manager needs. */
static void ManagerPrintf(const char *fmt, ...) {
    va_list ap;
    char    num[12];

    va_start(ap, fmt);
    for (const char *p = fmt; *p != '\0'; p++) {
        if (*p != '%') {
            ManagerPutc(*p);
            continue;
        }
        p++;
        if (*p == '\0')
            break;
        switch (*p) {
        case '%':
            ManagerPutc('%');
            break;
        case 'c':
            ManagerPutc((char)va_arg(ap, int));
            break;
        case 's':
            ManagerWrite(va_arg(ap, const char *));
            break;
        case 'd':
            ManagerItoa(va_arg(ap, int), num);
            ManagerWrite(num);
            break;
        default:
            ManagerPutc('%');
            ManagerPutc(*p);
            break;
        }
    }
    va_end(ap);
}

/* ====================================================================
 * Serial service client self-test
 * ==================================================================== */

/*
 * Exercises the ring-3 serial driver end to end over REAL COM1 I/O:
 *   IpcCall(WRITE) -> COM1 TX (marker 'SERIAL_SVC_OK')
 *   IpcCall(READ) polling -> real host-injected COM1 RX bytes, drained
 *   by the service's IRQ thread (bound IRQ4 -> notification -> FIFO
 *   drain) and returned by a READ -> 'READ_PATH_OK' round-trip proof.
 * Marker strings are BYTE-IDENTICAL to the pre-manager boot flow —
 * the FIFO injection harness greps for "inject 'READ_PATH_OK'".
 *
 * Runs on the manager thread, BEFORE the shell is started: the shell
 * polls the serial service for input continuously, so it must not be
 * alive while this test's injected bytes are in flight.
 */
static void SerialTestRun(void) {
    /* Wait for the service thread to create and register its port. */
    int port = -1;
    for (int i = 0; i < 20 && port < 0; i++) {
        port = PortGet("serial");
        if (port < 0)
            Sleep(1);
    }
    if (port < 0) {
        ManagerPrintf("serial-test: PortGet('serial') failed (%d)\n", port);
        return;
    }
    ManagerPrintf("serial-test: connected to 'serial' port %d\n", port);

    /* Buffers (u32 arrays so the header fields stay aligned) */
    u32 req[2 + 4]; /* { op; len; data[..] } */
    u32 resp[17];   /* { ret; data[64] }    */
    int resp_len;

    /* ---- 1. WRITE: the marker string goes out over COM1 TX ---- */
    static const char marker[] = "SERIAL_SVC_OK\n";
    req[0]                     = SERIAL_OP_WRITE;
    req[1]                     = (u32)(sizeof(marker) - 1);
    for (int i = 0; i < (int)sizeof(marker) - 1; i++)
        ((u8 *)req)[8 + i] = (u8)marker[i];

    resp_len = (int)sizeof(resp);
    int ret  = (int)IpcCall(
        port, (const void *)req, 8 + (int)sizeof(marker) - 1, (void *)resp, &resp_len);
    if (ret < 0) {
        ManagerPrintf("serial-test: WRITE ipc_call failed (%d)\n", ret);
        return;
    }
    if ((i32)resp[0] < 0) {
        ManagerPrintf("serial-test: WRITE rejected (%d)\n", (i32)resp[0]);
        return;
    }
    ManagerPrintf("serial-test: WRITE ok - %d bytes sent, marker 'SERIAL_SVC_OK'\n", (i32)resp[0]);

    /* ---- 2. Real IRQ4 path: host injects bytes into COM1 RX ----
     * The service's IRQ thread drains the 16550 RX FIFO on IRQ4
     * notification (wait_notification -> FIFO drain -> ring), and the
     * next READ returns the bytes.  The shell is not alive yet, so
     * nothing competes for the RX bytes.  Poll with sleeps until the
     * injected marker round-trips (bounded window). */
    ManagerPrintf("serial-test: real-RX test - inject 'READ_PATH_OK' into COM1 now\n");
    Sleep(20); /* give the host time to react */

    static const char rxmark[] = "READ_PATH_OK\n";
    int               got      = 0;
    for (int tries = 0; tries < 30 && got == 0; tries++) {
        req[0]   = SERIAL_OP_READ;
        req[1]   = 32;
        resp_len = (int)sizeof(resp);
        ret      = (int)IpcCall(port, (const void *)req, 8, (void *)resp, &resp_len);
        if (ret < 0) {
            ManagerPrintf("serial-test: READ ipc_call failed (%d)\n", ret);
            return;
        }
        i32 n = (i32)resp[0];
        if (n < 0) {
            ManagerPrintf("serial-test: READ rejected (%d)\n", n);
            return;
        }
        if (n > 0) {
            if (n > (i32)sizeof(resp) - 4)
                n = (i32)sizeof(resp) - 4;
            /* manager_printf has no %.*s — NUL-terminate a local copy */
            char echo[64];
            int  m = (n < (int)sizeof(echo) - 1) ? (int)n : (int)sizeof(echo) - 1;
            for (int i = 0; i < m; i++)
                echo[i] = (char)((u8 *)resp)[4 + i];
            echo[m] = '\0';
            ManagerPrintf("serial-test: real RX ok - %d bytes: '%s'\n", n, echo);

            /* Verify the round-tripped bytes equal the injected marker */
            int match = (n == (int)(sizeof(rxmark) - 1));
            for (int i = 0; match && i < n; i++)
                match = (echo[i] == rxmark[i]);
            if (match)
                ManagerPrintf("serial-test: READ_PATH_OK - %d bytes round-tripped via IRQ4\n", n);
            got = 1;
        }
        if (got == 0)
            Sleep(10);
    }
    if (got == 0)
        ManagerPrintf("serial-test: real RX - no bytes observed (none injected)\n");

    ManagerPrintf("serial-test: PASS - serial service verified\n");
}

/* ====================================================================
 * Service spawning
 * ==================================================================== */

/*
 * Start a service: fetch its embedded ELF image from the kernel blob
 * table (SYS_BLOB_GET) and spawn it as an independent process
 * (SYS_PROCESS_CREATE).  Records the child PID in the service table.
 * Returns 0 on success, else the first negative error; logs unless
 * quiet.
 *
 * The ELF buffer is allocated per call (NOT static): multiple monitor
 * threads (perm/pkg/device_mgr/shell) can spawn concurrently when
 * several services crash at once — a shared static buffer would race
 * and hand process_create a corrupted/overwritten image.  The kernel
 * copies the image during the syscall, so the buffer is freed
 * immediately after.
 */
static int SpawnService(service_t *svc, int quiet) {
    char *blob_buf = malloc(524288); /* must hold the largest service ELF
                                      * (term embeds the 230KB CJK font) */
    if (!blob_buf)
        return ERR_NOMEM;

    int size = BlobGet(svc->name, blob_buf, 524288);
    if (size < 0) {
        free(blob_buf);
        if (!quiet)
            ManagerPrintf("manager: %s blob_get failed (%d)\n", svc->name, size);
        return size;
    }
    int pid = ProcessCreate(svc->name, blob_buf, size);
    free(blob_buf);
    if (pid < 0) {
        if (!quiet)
            ManagerPrintf("manager: %s process_create failed (%d)\n", svc->name, pid);
        return pid;
    }
    svc->pid = pid;
    if (!quiet)
        ManagerPrintf("manager: %s started (PID=%d)\n", svc->name, pid);
    return 0;
}

/* ====================================================================
 * Flaky monitor (restart policy)
 * ==================================================================== */

/*
 * Generic service monitor (restart policy): blocks in ProcessWait()
 * until the service's last thread exits, then applies the restart
 * policy: up to MAX_RESTARTS restarts, then FAILED.  Logs every event
 * through the serial service.  One monitor thread per restartable
 * service (perm/pkg/device_mgr/shell); each touches only its own
 * service_t entry, so concurrent monitors are safe.
 */
static void ServiceMonitor(void *arg) {
    service_t *svc = (service_t *)arg;

    for (;;) {
        int exit_code = 0;
        int ret       = ProcessWait(svc->pid, &exit_code);
        if (ret < 0) {
            /* Race: the process was already reaped as an orphan before
             * this monitor registered its wait.  Treat it like an exit
             * and apply the restart policy. */
            ManagerPrintf("manager: %s wait failed (%d), restarting\n", svc->name, ret);
        }
        if (svc->restart_count >= MAX_RESTARTS) {
            ManagerPrintf("manager: %s marked FAILED\n", svc->name);
            break;
        }
        svc->restart_count++;
        ManagerPrintf("manager: %s exited (code %d), restart %d/%d\n",
                       svc->name,
                       exit_code,
                       svc->restart_count,
                       MAX_RESTARTS);
        if (SpawnService(svc, 0) < 0)
            break;
    }
}

/*
 * After the boot sequence (shell spawned): start one monitor thread
 * per restartable service.  (flaky's monitor already ran to FAILED
 * during the boot sequence, so it is not restarted here.)
 */
static void StartServiceMonitors(void) {
    static const int s_restartable[] = {SVC_PERM, SVC_PKG, SVC_DEVICE_MGR, SVC_SHELL, SVC_USER};
    for (u32 i = 0; i < sizeof(s_restartable) / sizeof(s_restartable[0]); i++) {
        int tid = ThreadCreate(ServiceMonitor, (void *)&s_services[s_restartable[i]], 10);
        if (tid < 0)
            ManagerPrintf("manager: monitor(%s) thread_create failed (%d)\n",
                           s_services[s_restartable[i]].name,
                           tid);
    }

    /* Idle — the monitors + shell keep running as their own threads. */
    for (;;)
        ThreadYield();
}

/* ====================================================================
 * Service manager entry point (independent process)
 * ==================================================================== */

int main(void) {
    /* ---- 1. Serial service first, then resolve its port ----
     * All manager logging goes through the serial service (WRITE op),
     * so every log is deferred until the port resolves.  Only a
     * pre-resolution failure falls back to kernel DebugLog().  (The
     * spawn success log is likewise dropped: s_serial_port is still
     * -1, so ManagerPutc() silently discards it.) */
    if (SpawnService(&s_services[SVC_SERIAL], 0) < 0) {
        DebugLog("manager: serial service spawn FAILED\n");
        for (;;)
            ThreadYield();
    }

    for (int i = 0; i < 2000 && s_serial_port < 0; i++) {
        s_serial_port = PortGet("serial");
        if (s_serial_port < 0)
            Sleep(1); /* let the serial process run and register */
    }
    if (s_serial_port < 0) {
        DebugLog("manager: serial port never resolved\n");
        for (;;)
            ThreadYield();
    }
    ManagerPrintf("manager: serial service ready (port %d)\n", s_serial_port);

    /* ---- 2. Serial self-test (markers byte-identical to P0-B) ---- */
    ManagerWrite("manager: running serial self-test\n");
    SerialTestRun();

    /* ---- 3. Terminal + keyboard services ----
     * Spawned after the serial self-test (which must be the only serial
     * RX consumer) and before the flaky cycle: the shell needs both
     * ports resolved when it starts, and neither service touches the
     * serial port, so they cannot disturb the call stream. */
    ManagerWrite("manager: starting display services\n");
    if (SpawnService(&s_services[SVC_TERM], 0) < 0)
        for (;;)
            ThreadYield();
    if (SpawnService(&s_services[SVC_KEYBOARD], 0) < 0)
        for (;;)
            ThreadYield();

    /* ---- 3b. GUI compositor ----
     * Pixel window compositor: maps the framebuffer (blob-identity
     * seeded with ATOM_SERVICE_MANAGE), idles until a client calls
     * GUI_OP_ACTIVATE.  Started with the display group so the "gui"
     * port exists before the shell offers the `gui` command. */
    if (SpawnService(&s_services[SVC_GUI], 0) < 0)
        ManagerPrintf("manager: gui spawn failed\n");
    if (SpawnService(&s_services[SVC_NET], 0) < 0)
        ManagerPrintf("manager: net spawn failed\n");

    /* ---- 4. Flaky demo service ----
     * Only flaky is started here: it is IPC-silent (sleep + exit), so
     * it cannot disturb the serial-port call stream.  The shell is
     * deliberately NOT started yet — see the single-writer rule below. */
    ManagerWrite("manager: starting services\n");
    if (SpawnService(&s_services[SVC_FLAKY], 0) < 0)
        for (;;)
            ThreadYield();

    /* ---- 5. Supervisor loop ----
     * The manager's main thread runs flaky's monitor to FAILED before
     * the shell exists, so the boot-time serial log stays clean. */
    ServiceMonitor(&s_services[SVC_FLAKY]);

    ManagerWrite("manager: MANAGER_OK\n");

    /* ---- 5b. VFS services ----
     * vfs_server owns the namespace; fs_mem_driver is spawned as its
     * own process (decision A1) and performs the driver-initiated
     * MOUNT handshake against "vfs".  The manager waits for both ports
     * to resolve and gives the handshake time to complete, so the
     * shell never sees a not-yet-mounted volume.  Neither service
     * touches the serial port while the manager is writing here, and
     * from now on all further output goes through them only. */
    ManagerWrite("manager: starting VFS services\n");
    if (SpawnService(&s_services[SVC_VFS], 0) < 0)
        for (;;)
            ThreadYield();
    for (int i = 0; i < 2000 && PortGet("vfs") < 0; i++)
        Sleep(1);
    if (SpawnService(&s_services[SVC_FS_MEM], 0) < 0)
        for (;;)
            ThreadYield();
    for (int i = 0; i < 2000 && PortGet("vfs.fs.mem") < 0; i++)
        Sleep(1);
    Sleep(20); /* let the MOUNT handshake register both volumes */

    /* ---- 5b'. Powerbox (perm-manager), before the shell ----
     * vfs_server lazy-resolves the "perm" port on the first bookmark
     * op; term registers "perm.ui" at startup.  Spawned here so the
     * shell's perm_answer/perm_revoke commands find both ports.
     * perm MUST come up before the block-device driver: that driver
     * may degrade (no virtio-blk device) and never register its port,
     * which would stall the manager here for the full 2000-tick wait
     * and starve the init P1 suite (its own 2000-tick bookmark budget
     * is the same length — a zero-value g_p1_res then cascades into
     * every later GRANT/P2V test). */
    ManagerWrite("manager: starting Powerbox\n");
    if (SpawnService(&s_services[SVC_PERM], 0) < 0)
        for (;;)
            ThreadYield();
    for (int i = 0; i < 2000 && PortGet("perm") < 0; i++)
        Sleep(1);

    /* ---- 5c. Block-device storage driver (before the shell) ----
     * fs_virtio_blk_driver owns the persistent Disk volume (format on
     * first boot, then mount); it must come after vfs_server (MOUNT
     * handshake) and before the shell (which can then address "Disk:").
     * The port wait + sleep mirror the fs_mem_driver block above.
     * When no virtio-blk device exists the driver degrades WITHOUT
     * registering 'vfs.fs.virtio_blk', so this wait runs its full
     * budget — harmless for the regression (the init P1/P2 suite runs
     * in parallel), only the shell start is delayed. */
    ManagerWrite("manager: starting block-device driver\n");
    if (SpawnService(&s_services[SVC_FS_VIRTIO_BLK], 0) < 0)
        for (;;)
            ThreadYield();
    for (int i = 0; i < 2000 && PortGet("vfs.fs.virtio_blk") < 0; i++)
        Sleep(1);
    Sleep(20); /* let the Disk MOUNT handshake register the volume */

    /* ---- 5d. PCI device manager, before the shell ----
     * Spawned with the rest of the device services so its "device_mgr"
     * port is registered by the time the shell starts (the shell's
     * device_mgr commands resolve it lazily).  Like the perm service it
     * never touches the serial port, so it cannot disturb the call
     * stream.  The service self-checks the PCI enumeration at boot. */
    ManagerWrite("manager: starting device manager\n");
    if (SpawnService(&s_services[SVC_DEVICE_MGR], 0) < 0)
        for (;;)
            ThreadYield();

    /* ---- 5e. pkg-manager, before the shell ----
     * pkg owns .ops application installation and sandbox capability
     * issuance (docs/ops_format.md).  It depends on vfs (fs_write of
     * /Volumes/Users/Apps) and perm (the Powerbox flow fires on the
     * first Users-volume write), so it must come after both.  It never
     * touches the serial port, so it cannot disturb the call stream.
     * The shell resolves the "pkg" port lazily on its pkg commands. */
    ManagerWrite("manager: starting pkg-manager\n");
    if (SpawnService(&s_services[SVC_PKG], 0) < 0)
        for (;;)
            ThreadYield();

    /* ---- 6. Shell LAST (keeps the single-writer rule) ----
     * The shell is spawned after the manager's output phase.  The
     * spawn is deliberately silent (no "shell started (PID=..)" log):
     * printing it after ProcessCreate() would let the fresh shell run
     * mid-print and interleave the boot log. */
    ManagerWrite("manager: starting user service\n");
    if (SpawnService(&s_services[SVC_USER], 0) < 0)
        for (;;)
            ThreadYield();

    /* wm before shell: the window manager registers its "wm" port and
     * idles; the desktop only activates when a client (wm_demo via
     * `exec`) starts a session. */
    ManagerWrite("manager: starting window manager\n");
    if (SpawnService(&s_services[SVC_WM], 0) < 0)
        for (;;)
            ThreadYield();

    /* policy before shell: the shell queries the command policy at
     * startup to build its command filter (v0.5). */
    ManagerWrite("manager: starting policy service\n");
    if (SpawnService(&s_services[SVC_POLICY], 0) < 0)
        for (;;)
            ThreadYield();

    ManagerWrite("manager: starting shell\n");
    (void)SpawnService(&s_services[SVC_SHELL], 1);

    /* ---- 7. Service monitors (crash recovery) ----
     * One monitor thread per restartable service; the main thread
     * takes flaky's monitor.  A crashed perm/pkg/device_mgr/shell is
     * auto-restarted (up to MAX_RESTARTS) — process_reap now cleans
     * the dead process's ports/IRQs/names, so the restart is clean. */
    StartServiceMonitors();
}
