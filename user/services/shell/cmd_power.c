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
 * cmd_power.c - power management & service supervision commands
 * Copyright (c) 2026 OpSys Project
 *
 *   power [status|sync|off|reboot|halt]   (+ poweroff / halt / restart aliases)
 *   svc   [list|status|start|stop|restart] [name]
 *
 * Power operations are the *ordered* version of the bare `shutdown` /
 * `reboot` commands: they flush every mounted volume first (VFS_OP_SYNC
 * through libfs) and only then ask the kernel to cut the power, so a
 * driver that buffers metadata cannot lose it.  The kernel gates the
 * actual power transition on ATOM_SYS_SHUTDOWN, which the user service
 * issues at login for OWNER/ADMIN accounts.
 *
 * Service supervision goes through the `user` service admin proxy to
 * the manager's control port (svc_req_t / svc_resp_t in
 * user/services/manager/manager.h): the shell holds no
 * ATOM_SERVICE_MANAGE, the manager re-checks the management plane, and
 * the user service is the component that authenticated the human.
 *
 * ------------------------------------------------------------------
 * Structure (power + supervision):
 *   CmdPower()  -> PowerStatus() | PowerSync() | PowerOff() | PowerReboot() | PowerHalt()
 *   CmdSvc()    -> SvcCall(op, name) -> user proxy -> manager control port
 *   ShellRegisterPowerCommands() registers power/poweroff/halt/restart/svc
 * How it works:
 *   PowerOff/PowerReboot/PowerHalt share the same prologue: confirm
 *   (unless -f), SyncVolumes(), print the transition, then call the
 *   kernel.  A failed sync is reported but does not block the power
 *   transition unless -s (strict) was asked for.
 * Purpose:
 *   Make "turn the machine off" a deliberate, data-safe operation
 *   instead of an abrupt one, and give the administrator a way to see
 *   and steer the service set at runtime.
 * Caveats:
 *   `halt` parks the CPU with interrupts disabled — on QEMU the guest
 *   simply stops, so the terminal looks frozen until QEMU is quit.
 *   Services are not stopped one by one before power-off (only their
 *   buffers are flushed); a full shutdown orchestration is future work.
 * ------------------------------------------------------------------
 */

#include "shell.h"

#include <stdarg.h>

#include "../lib/libc/stdio.h"
#include "../lib/libc/stdlib.h"
#include "../lib/libc/string.h"
#include "../lib/libfs/fs.h"
#include "../lib/libos/syscalls.h"
#include "../user/user.h"

/* ====================================================================
 * Aligned output
 *
 * The shell's own PrintfLine() understands only %d / %x / %s / %c (plus
 * zero-padding) — it has no field widths, so "%-10s" would be emitted
 * literally and the arguments would desynchronize (which is exactly what
 * happened to the first column-aligned tables).  Column layouts here go
 * through libc's full-featured vsnprintf and are then handed to
 * ShellWrite(), which keeps the terminal as the single output path.
 * ==================================================================== */
static void PrintfLine(const char *fmt, ...) {
    char    buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ShellWrite(buf);
}

typedef uint8_t  u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t  i32;

/* Forward declarations: the supervision helpers live next to the svc
 * command at the bottom, but PowerStatus() uses them for its service
 * census. */
static int SvcQuery(u32 user_op, const char *name, svc_resp_t *out);

/* ====================================================================
 * Volume flush
 * ==================================================================== */

/* Flush every mounted volume.  Returns 0 when the vfs_server accepted
 * the request, negative otherwise (the count of flushed volumes is
 * printed either way). */
static int SyncVolumes(const char *why) {
    u32 volumes = 0;
    int r       = FsSync(&volumes);
    if (r < 0) {
        PrintfLine("sync: FAILED (%d)%s\n", r, why ? why : "");
        return r;
    }
    PrintfLine("sync: %d volume(s) flushed%s\n", (int)volumes, why ? why : "");
    return 0;
}

/* ====================================================================
 * power status
 * ==================================================================== */

static int PowerStatus(void) {
    int ticks = GetTime();
    int pages = GetFreePages();

    PrintfLine("uptime   : %d ticks (%d.%02d s at 100 Hz)\n",
                 ticks, ticks / 100, ticks % 100);
    PrintfLine("memory   : %d free pages (%d MB)\n", pages, pages / 256);

    static proc_info_t procs[24];
    int                n = ProcessList(procs, 24);
    PrintfLine("processes: %d\n", n > 0 ? n : 0);

    static vfs_vol_info_t vols[VFS_MAX_VOLS];
    u32                   count = 0;
    if (FsListVolumes(vols, &count) == 0) {
        ShellWrite("volumes  :");
        for (u32 i = 0; i < count; i++)
            PrintfLine(" %s%s", vols[i].mount_name, vols[i].read_only ? "(ro)" : "");
        ShellWrite("\n");
    }

    /* Service census through the admin proxy: the shell holds no
     * ATOM_SERVICE_MANAGE, so a direct call to the manager's port would
     * be refused.  A missing/unreachable manager is reported, not fatal. */
    static svc_resp_t resp;
    if (SvcQuery(USER_OP_SVC_LIST, NULL, &resp) < 0 || resp.ret < 0) {
        PrintfLine("services : unavailable (%d)%s%s\n", resp.ret,
                     resp.detail[0] ? " - " : "", resp.detail);
        return 0;
    }
    int alive = 0;
    for (u32 i = 0; i < resp.count; i++)
        if (resp.entries[i].alive)
            alive++;
    PrintfLine("services : %d/%d running\n", alive, (int)resp.count);
    return 0;
}

/* ====================================================================
 * power off / reboot / halt
 * ==================================================================== */

/* Ask for confirmation unless force is set.  Returns 1 to proceed. */
static int PowerConfirm(const char *what, int force) {
    if (force)
        return 1;
    PrintfLine("power: this will %s the machine.\n", what);
    ShellWrite("Type YES to continue: ");
    char line[16];
    int  n = ShellReadLine(line, sizeof(line));
    if (n < 0)
        return 0;
    if (strcmp(line, "YES") == 0 || strcmp(line, "yes") == 0 || strcmp(line, "y") == 0)
        return 1;
    ShellWrite("power: cancelled\n");
    return 0;
}

/* Shared prologue: confirm, flush the volumes, report. */
static int PowerPrologue(const char *what, int force, int strict, int skip_sync) {
    if (!PowerConfirm(what, force))
        return 0;
    if (!skip_sync) {
        int r = SyncVolumes("");
        if (r < 0 && strict) {
            ShellWrite("power: aborted (sync failed and -s was given)\n");
            return 0;
        }
    }
    return 1;
}

static int PowerOff(int force, int strict, int skip_sync) {
    if (!PowerPrologue("power off", force, strict, skip_sync))
        return 0;
    ShellWrite("power: powering off (ACPI S5)...\n");
    int r = sys_shutdown();
    PrintfLine("power: shutdown FAILED (%d)%s\n", r,
                 (r == ERR_NOCAP) ? " - log in as OWNER/ADMIN (ATOM_SYS_SHUTDOWN)" : "");
    return r < 0 ? -1 : 0;
}

static int PowerReboot(int force, int strict, int skip_sync) {
    if (!PowerPrologue("reboot", force, strict, skip_sync))
        return 0;
    ShellWrite("power: restarting...\n");
    int r = sys_reboot();
    PrintfLine("power: reboot FAILED (%d)%s\n", r,
                 (r == ERR_NOCAP) ? " - log in as OWNER/ADMIN (ATOM_SYS_SHUTDOWN)" : "");
    return r < 0 ? -1 : 0;
}

static int PowerHalt(int force, int strict, int skip_sync) {
    if (!PowerPrologue("halt", force, strict, skip_sync))
        return 0;
    ShellWrite("power: halted (CPU parked; quit QEMU to return)\n");
    int r = sys_halt();
    PrintfLine("power: halt FAILED (%d)%s\n", r,
                 (r == ERR_NOCAP) ? " - log in as OWNER/ADMIN (ATOM_SYS_SHUTDOWN)" : "");
    return r < 0 ? -1 : 0;
}

static int CmdPower(int argc, char *argv[]) {
    /* Flags may appear before OR after the subcommand — "power halt -f"
     * is the natural spelling and must not fall through to the
     * confirmation prompt.  The first non-flag argument is the
     * subcommand; anything else is rejected. */
    int         force = 0, strict = 0, skip_sync = 0;
    const char *sub = NULL;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--force") == 0)
                force = 1;
            else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--strict") == 0)
                strict = 1;
            else if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--no-sync") == 0)
                skip_sync = 1;
            else {
                PrintfLine("power: unknown option '%s'\n", argv[i]);
                return -1;
            }
            continue;
        }
        if (sub) {
            PrintfLine("power: unexpected argument '%s'\n", argv[i]);
            return -1;
        }
        sub = argv[i];
    }
    if (!sub)
        sub = "status";

    if (strcmp(sub, "status") == 0)
        return PowerStatus();
    if (strcmp(sub, "sync") == 0)
        return SyncVolumes("") < 0 ? -1 : 0;
    if (strcmp(sub, "off") == 0 || strcmp(sub, "poweroff") == 0)
        return PowerOff(force, strict, skip_sync);
    if (strcmp(sub, "reboot") == 0 || strcmp(sub, "restart") == 0)
        return PowerReboot(force, strict, skip_sync);
    if (strcmp(sub, "halt") == 0)
        return PowerHalt(force, strict, skip_sync);

    ShellWrite("Usage: power [status|sync|off|reboot|halt] [-f] [-s] [-n]\n");
    ShellWrite("  -f force (no confirmation)   -s abort if sync fails\n");
    ShellWrite("  -n skip the volume flush\n");
    return -1;
}

/* ---- aliases ---- */

static int CmdPoweroff(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    return PowerOff(0, 0, 0);
}

static int CmdHalt(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    return PowerHalt(0, 0, 0);
}

static int CmdRestart(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    return PowerReboot(0, 0, 0);
}

/* ====================================================================
 * svc - service supervision through the user service admin proxy
 * ==================================================================== */

/* Send one supervision request to the "user" service admin proxy.
 *
 * The opcode passed here is a USER_OP_SVC_* value — the proxy maps it
 * onto the manager's own SVC_OP_* space (the two enums overlap
 * numerically, so they must never be mixed up: sending SVC_OP_LIST(1)
 * would arrive as USER_OP_LOGIN).  The reply is a svc_resp_t verbatim.
 */
static int SvcQuery(u32 user_op, const char *name, svc_resp_t *out) {
    memset(out, 0, sizeof(*out));

    int port = PortGet(USER_PORT_NAME);
    if (port < 0) {
        out->ret = port;
        snprintf(out->detail, sizeof(out->detail), "'user' port unavailable");
        return port;
    }

    static user_req_svc_t req;
    memset(&req, 0, sizeof(req));
    req.op = user_op;
    if (name)
        strncpy(req.name, name, SVC_NAME_MAX - 1);

    int len = (int)sizeof(*out);
    int r   = IpcCall(port, &req, (int)sizeof(req), out, &len);
    if (r < 0) {
        out->ret = r;
        snprintf(out->detail, sizeof(out->detail), "ipc failed (%d)", r);
        return r;
    }
    return 0;
}

/* Query + report a denial; returns 0 when the request was answered. */
static int SvcCall(u32 user_op, const char *name, svc_resp_t *out) {
    int r = SvcQuery(user_op, name, out);
    if (r < 0) {
        PrintfLine("svc: request FAILED (%d)\n", r);
        return r;
    }
    if (out->ret == ERR_NOCAP || out->ret == ERR_DENIED) {
        PrintfLine("svc: denied - %s\n",
                     out->detail[0] ? out->detail : "administrator account required");
        return out->ret;
    }
    return 0;
}

static void SvcPrintEntry(const svc_entry_t *e) {
    PrintfLine("  %-22s pid %-4d %-10s %s%s\n",
                 e->name,
                 (int)e->pid,
                 e->alive ? "running" : "stopped",
                 e->monitored ? "monitored" : "not-monitored",
                 e->restarts ? " restarts=?" : "");
}

static int CmdSvc(int argc, char *argv[]) {
    static svc_resp_t resp;
    const char *sub = (argc >= 2) ? argv[1] : "list";

    if (strcmp(sub, "list") == 0) {
        if (SvcCall(USER_OP_SVC_LIST, NULL, &resp) < 0)
            return -1;
        if (resp.ret < 0) {
            PrintfLine("svc: list FAILED (%d)%s%s\n", resp.ret,
                        resp.detail[0] ? " - " : "", resp.detail);
            return -1;
        }
        ShellWrite("Service                PID   State      Supervision\n");
        ShellWrite("---------------------  ----  ---------  --------------------\n");
        int alive = 0;
        for (u32 i = 0; i < resp.count; i++) {
            const svc_entry_t *e = &resp.entries[i];
            PrintfLine("  %-20s %5d  %-9s  %s%s\n",
                         e->name,
                         (int)e->pid,
                         e->alive ? "running" : "stopped",
                         e->monitored ? "auto-restart" : "-",
                         (e->restarts && e->alive) ? " (restarted)" : "");
            if (e->alive)
                alive++;
        }
        PrintfLine("svc: %d/%d running\n", alive, (int)resp.count);
        return 0;
    }

    if (argc < 3) {
        PrintfLine("Usage: svc %s <name>\n", sub);
        return -1;
    }
    const char *name = argv[2];
    u32         op   = 0;

    /* USER_OP_SVC_*: the proxy owns the translation to SVC_OP_*. */
    if (strcmp(sub, "status") == 0)
        op = USER_OP_SVC_STATUS;
    else if (strcmp(sub, "start") == 0)
        op = USER_OP_SVC_START;
    else if (strcmp(sub, "stop") == 0)
        op = USER_OP_SVC_STOP;
    else if (strcmp(sub, "restart") == 0)
        op = USER_OP_SVC_RESTART;
    else {
        PrintfLine("svc: unknown subcommand '%s'\n", sub);
        ShellWrite("Usage: svc <list|status|start|stop|restart> [name]\n");
        return -1;
    }

    /* Stopping the shell (or the user service that authorises this
     * request) would cut the ground from under our feet: refuse here
     * with a clear message instead of failing halfway. */
    if ((op == USER_OP_SVC_STOP || op == USER_OP_SVC_RESTART) &&
        (strcmp(name, "shell") == 0 || strcmp(name, "user") == 0)) {
        PrintfLine("svc: refusing to %s '%s' from its own session\n", sub, name);
        return -1;
    }

    if (SvcCall(op, name, &resp) < 0)
        return -1;
    if (resp.ret < 0) {
        PrintfLine("svc: %s %s FAILED (%d)%s%s\n", sub, name, resp.ret,
                     resp.detail[0] ? " - " : "", resp.detail);
        return -1;
    }
    if (op == USER_OP_SVC_STATUS && resp.count > 0)
        SvcPrintEntry(&resp.entries[0]);
    else
        PrintfLine("svc: %s\n", resp.detail);
    return 0;
}

/* ====================================================================
 * Registration
 * ==================================================================== */

void ShellRegisterPowerCommands(void) {
    ShellRegisterCommand("power",
                           "Power: power [status|sync|off|reboot|halt] [-f] [-s] [-n]",
                           CmdPower);
    ShellRegisterCommand("poweroff", "Flush the volumes and power off", CmdPoweroff);
    ShellRegisterCommand("halt", "Flush the volumes and park the CPU", CmdHalt);
    ShellRegisterCommand("restart", "Flush the volumes and reboot", CmdRestart);
    ShellRegisterCommand("svc",
                           "Services: svc <list|status|start|stop|restart> [name]",
                           CmdSvc);
}
