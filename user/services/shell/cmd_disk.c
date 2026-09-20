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
 * cmd_disk.c - disk / volume maintenance commands
 * Copyright (c) 2026 OpSys Project
 *
 *   disk list                  volumes + capacity (df-like)
 *   disk info [vol]            driver, geometry, UUID, inode census
 *   disk sync [vol]            flush to the medium (all volumes by default)
 *   disk check [vol]           read-only consistency scan
 *   disk read <lba> [sectors]  raw sector dump (debug)
 *   disk mount|unmount <vol>   (re)register a volume with the vfs_server
 *   disk format <vol>          wipe + re-format (DESTRUCTIVE, asks)
 *   disk fill <vol> [bytes]    write fill.bin until NOSPC or budget
 *
 * The maintenance half (info/sync/check/read/mount/unmount/format/fill)
 * is an **admin proxy** chain: this command talks to the `user` service,
 * which re-checks that the caller is OWNER/ADMIN and then forwards the
 * request to the block-device driver's control plane, where it is gated
 * again on ATOM_SERVICE_MANAGE.  The shell never touches the driver.
 *
 * ------------------------------------------------------------------
 * Structure (client of the user-service disk proxy):
 *   CmdDisk() -> DiskUserCall(USER_OP_DISK_*) -> "user" port
 *                    -> fs_virtio_blk_driver control plane
 *   DiskInfo()/DiskCheck()/DiskRead()/DiskSync() format the replies
 * How it works:
 *   Arguments are parsed into user_req_disk_t (op, volume, size, lba,
 *   length) and one IPC call returns user_resp_disk_t with the payload
 *   union: FILL reports bytes, CHECK reports the scan report, INFO the
 *   volume descriptor, RAW_READ up to USER_DISK_RAW_MAX bytes.
 * Purpose:
 *   Give the administrator the tools to inspect, verify and flush the
 *   only persistence surface in the system without letting the shell
 *   hold device capabilities itself.
 * Caveats:
 *   Only the virtio-blk volume ("Disk") has a control plane; the memory
 *   volume ("System") is read-only and reports an explicit "not
 *   supported" instead of fabricated data.  `disk read` is a raw
 *   sector dump — it can print anything, including non-text bytes.
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

/* ====================================================================
 * Proxy plumbing
 * ==================================================================== */

static int DiskUserCall(const user_req_disk_t *req, user_resp_disk_t *resp) {
    int port = PortGet(USER_PORT_NAME);
    if (port < 0) {
        PrintfLine("disk: 'user' port unavailable (%d)\n", port);
        return port;
    }
    memset(resp, 0, sizeof(*resp));
    int len = (int)sizeof(*resp);
    int r   = IpcCall(port, req, (int)sizeof(*req), resp, &len);
    if (r < 0) {
        PrintfLine("disk: ipc FAILED (%d)\n", r);
        return r;
    }
    if (resp->ret < 0) {
        PrintfLine("disk: FAILED (%d)%s%s\n", resp->ret,
                     resp->detail[0] ? " - " : "", resp->detail);
        return resp->ret;
    }
    return 0;
}

/* Build a request for one named volume.  Returns 0 on success. */
static int DiskReq(user_req_disk_t *req, u32 op, const char *vol) {
    if (!vol || !vol[0]) {
        ShellWrite("disk: a volume name is required (try 'disk list')\n");
        return -1;
    }
    if (strlen(vol) >= sizeof(req->volume)) {
        ShellWrite("disk: volume name too long\n");
        return -1;
    }
    memset(req, 0, sizeof(*req));
    req->op = op;
    strncpy(req->volume, vol, sizeof(req->volume) - 1);
    return 0;
}

/* ====================================================================
 * disk list / info - what is mounted, and what does it look like
 * ==================================================================== */

static int DiskList(void) {
    static vfs_vol_info_t vols[VFS_MAX_VOLS];
    u32                   count = 0;
    int                   r     = FsListVolumes(vols, &count);
    if (r < 0) {
        PrintfLine("disk: list FAILED (%d)\n", r);
        return -1;
    }
    if (count == 0) {
        ShellWrite("disk: no volumes mounted\n");
        return 0;
    }
    ShellWrite("Volume      Driver        Size      Used      Mount\n");
    ShellWrite("----------  ----------  --------  --------  -----\n");
    for (u32 i = 0; i < count; i++) {
        char url[96];
        snprintf(url, sizeof(url), "/%s", vols[i].mount_name);
        u64 total = 0, used = 0;
        u32 ro   = 0;
        int sr   = FsStatVolume(url, &total, &used, &ro);
        PrintfLine("%-10s  %-10s  %7d K %7d K  %s%s\n",
                     vols[i].mount_name,
                     vols[i].driver_name,
                     sr == 0 ? (int)(total / 1024u) : 0,
                     sr == 0 ? (int)(used / 1024u) : 0,
                     sr == 0 ? "ok" : "(stat n/a)",
                     vols[i].read_only ? " ro" : "");
    }
    PrintfLine("disk: %d volume(s)\n", (int)count);
    return 0;
}

static int DiskInfo(const char *vol) {
    user_req_disk_t  req;
    user_resp_disk_t resp;
    if (DiskReq(&req, USER_OP_DISK_INFO, vol) < 0)
        return -1;

    /* The memory volume has no control plane: report what the vfs knows
     * instead of pretending the driver answered. */
    int r = DiskUserCall(&req, &resp);
    if (r < 0) {
        char url[96];
        snprintf(url, sizeof(url), "/Volumes/%s", vol);
        vfs_item_info_t info;
        u64             total = 0, used = 0;
        u32             ro    = 0;
        PrintfLine("disk: %s has no control plane (memory-backed volume)\n", vol);
        if (FsStatVolume(url, &total, &used, &ro) == 0)
            PrintfLine("  capacity : %d KiB total, %d KiB used\n",
                         (int)(total / 1024u), (int)(used / 1024u));
        if (FsGetItem(url, &info) == 0)
            PrintfLine("  root item: id=%d\n", (int)info.item_id);
        ShellWrite("  note     : detailed geometry is only available for the Disk volume\n");
        return 0;
    }

    PrintfLine("volume      : %s\n", resp.info_mount[0] ? resp.info_mount : vol);
    PrintfLine("driver      : %s\n", resp.info_driver[0] ? resp.info_driver : "?");
    PrintfLine("persistent  : %s\n", resp.info_persistent ? "yes (survives reboot)" : "no");
    PrintfLine("read-only   : %s\n", resp.info_read_only ? "yes" : "no");
    PrintfLine("block size  : %d bytes\n", (int)resp.info_block_size);
    PrintfLine("blocks      : %d total, %d used, %d free\n",
                 (int)resp.info_total_blocks,
                 (int)resp.info_used_blocks,
                 (int)(resp.info_total_blocks - resp.info_used_blocks));
    PrintfLine("capacity    : %d KiB\n",
                 (int)(((u64)resp.info_total_blocks * resp.info_block_size) / 1024u));
    PrintfLine("inodes      : %d total, %d used, %d free\n",
                 (int)resp.info_inode_total,
                 (int)resp.info_inode_used,
                 (int)(resp.info_inode_total - resp.info_inode_used));
    PrintfLine("uuid        : %08x%08x-%08x%08x\n",
                 (unsigned)(resp.info_uuid_hi >> 32),
                 (unsigned)(resp.info_uuid_hi & 0xffffffffu),
                 (unsigned)(resp.info_uuid_lo >> 32),
                 (unsigned)(resp.info_uuid_lo & 0xffffffffu));
    return 0;
}

/* ====================================================================
 * disk sync - flush to the medium
 * ==================================================================== */

static int DiskSync(const char *vol) {
    if (!vol) {
        /* No volume named: flush everything through the VFS, which
         * forwards DRV_OP_SYNC to each mounted driver. */
        u32 volumes = 0;
        int r       = FsSync(&volumes);
        if (r < 0) {
            PrintfLine("disk: sync FAILED (%d)\n", r);
            return -1;
        }
        PrintfLine("disk: %d volume(s) flushed\n", (int)volumes);
        return 0;
    }
    user_req_disk_t  req;
    user_resp_disk_t resp;
    if (DiskReq(&req, USER_OP_DISK_SYNC, vol) < 0)
        return -1;
    if (DiskUserCall(&req, &resp) < 0)
        return -1;
    PrintfLine("disk: %s flushed%s%s\n", vol,
                 resp.detail[0] ? " - " : "", resp.detail);
    return 0;
}

/* ====================================================================
 * disk check - read-only consistency scan
 * ==================================================================== */

static int DiskCheck(const char *vol) {
    user_req_disk_t  req;
    user_resp_disk_t resp;
    if (DiskReq(&req, USER_OP_DISK_CHECK, vol) < 0)
        return -1;
    if (DiskUserCall(&req, &resp) < 0)
        return -1;

    PrintfLine("check %s: %s\n", vol, resp.check_note[0] ? resp.check_note : "(no note)");
    PrintfLine("  signature : %s\n", resp.check_magic_ok ? "valid" : "INVALID");
    PrintfLine("  inodes    : %d total, %d used\n",
                 (int)resp.check_inodes_total, (int)resp.check_inodes_used);
    PrintfLine("  content   : %d file(s), %d dir(s)\n",
                 (int)resp.check_files, (int)resp.check_dirs);
    PrintfLine("  blocks    : %d used, %d free\n",
                 (int)resp.check_used_blocks, (int)resp.check_free_blocks);
    if (resp.check_errors == 0)
        ShellWrite("  result    : clean\n");
    else
        PrintfLine("  result    : %d problem(s), first=0x%x\n",
                     (int)resp.check_errors, (unsigned)resp.check_first_error);
    return resp.check_errors == 0 ? 0 : -1;
}

/* ====================================================================
 * disk read - raw sector dump
 * ==================================================================== */

static int DiskRead(const char *vol, u64 lba, int sectors) {
    user_req_disk_t  req;
    user_resp_disk_t resp;

    if (sectors <= 0)
        sectors = 1;
    if (sectors > 64) {
        ShellWrite("disk: at most 64 sectors per invocation\n");
        sectors = 64;
    }

    PrintfLine("disk read %s: lba %d, %d sector(s)\n", vol, (int)lba, sectors);

    int shown = 0;
    for (int s = 0; s < sectors; s++) {
        if (DiskReq(&req, USER_OP_DISK_RAW_READ, vol) < 0)
            return -1;
        req.lba    = lba + (u64)s;
        req.length = 512;
        if (DiskUserCall(&req, &resp) < 0)
            return shown > 0 ? 0 : -1;
        if (resp.bytes == 0) {
            ShellWrite("disk: driver returned no data\n");
            break;
        }

        for (u32 off = 0; off < (u32)resp.bytes; off += 16) {
            char line[96];
            int  n = snprintf(line, sizeof(line), "%08x  ",
                               (unsigned)((lba + (u64)s) * 512u + off));
            for (u32 i = 0; i < 16; i++) {
                if (off + i < (u32)resp.bytes)
                    n += snprintf(line + n, sizeof(line) - (size_t)n, "%02x ",
                                   resp.raw[off + i]);
                else
                    n += snprintf(line + n, sizeof(line) - (size_t)n, "   ");
                if (i == 7)
                    n += snprintf(line + n, sizeof(line) - (size_t)n, " ");
            }
            n += snprintf(line + n, sizeof(line) - (size_t)n, " |");
            for (u32 i = 0; i < 16 && off + i < (u32)resp.bytes; i++) {
                char c = (char)resp.raw[off + i];
                n += snprintf(line + n, sizeof(line) - (size_t)n, "%c",
                               (c >= 32 && c < 127) ? c : '.');
            }
            n += snprintf(line + n, sizeof(line) - (size_t)n, "|\n");
            ShellWrite(line);
        }
        shown++;
    }
    PrintfLine("disk: %d sector(s) shown\n", shown);
    return 0;
}

/* ====================================================================
 * disk - dispatcher
 * ==================================================================== */

static int CmdDisk(int argc, char *argv[]) {
    static const char *usage =
        "Usage: disk <list|info|sync|check|read|mount|unmount|format|fill> [args]\n"
        "  disk list                    volumes + capacity\n"
        "  disk info [vol]              driver/geometry/UUID/inode census\n"
        "  disk sync [vol]              flush volume(s) to the medium\n"
        "  disk check [vol]             read-only consistency scan\n"
        "  disk read <lba> [sectors]    raw sector dump (default vol: Disk)\n"
        "  disk mount|unmount <vol>     (re)register a volume\n"
        "  disk format <vol>            wipe + re-format (DESTRUCTIVE)\n"
        "  disk fill <vol> [bytes]      write fill.bin until NOSPC/budget\n";

    if (argc < 2) {
        ShellWrite(usage);
        return -1;
    }

    if (strcmp(argv[1], "list") == 0)
        return DiskList();
    if (strcmp(argv[1], "info") == 0)
        return DiskInfo(argc >= 3 ? argv[2] : "Disk");
    if (strcmp(argv[1], "sync") == 0)
        return DiskSync(argc >= 3 ? argv[2] : NULL);
    if (strcmp(argv[1], "check") == 0)
        return DiskCheck(argc >= 3 ? argv[2] : "Disk");
    if (strcmp(argv[1], "read") == 0) {
        if (argc < 3) {
            ShellWrite("Usage: disk read <lba> [sectors] [vol]\n");
            return -1;
        }
        u64 lba     = (u64)strtoul(argv[2], NULL, 0);
        int sectors = (argc >= 4) ? atoi(argv[3]) : 1;
        const char *vol = (argc >= 5) ? argv[4] : "Disk";
        return DiskRead(vol, lba, sectors);
    }

    if (argc < 3) {
        ShellWrite(usage);
        return -1;
    }
    const char *vol = argv[2];
    user_req_disk_t req;

    if (strcmp(argv[1], "mount") == 0) {
        if (DiskReq(&req, USER_OP_DISK_MOUNT, vol) < 0)
            return -1;
    } else if (strcmp(argv[1], "unmount") == 0) {
        if (DiskReq(&req, USER_OP_DISK_UNMOUNT, vol) < 0)
            return -1;
    } else if (strcmp(argv[1], "format") == 0) {
        if (DiskReq(&req, USER_OP_DISK_FORMAT, vol) < 0)
            return -1;
        /* Format is destructive: require an explicit confirmation word. */
        PrintfLine("disk: formatting '%s' destroys ALL data on it.\n", vol);
        ShellWrite("Type YES to continue: ");
        char conf[8];
        if (ShellReadLine(conf, sizeof(conf)) < 0)
            return -1;
        if (strcmp(conf, "YES") != 0) {
            ShellWrite("disk: format cancelled\n");
            return 0;
        }
    } else if (strcmp(argv[1], "fill") == 0) {
        if (DiskReq(&req, USER_OP_DISK_FILL, vol) < 0)
            return -1;
        if (argc >= 4)
            req.size = (u32)strtoul(argv[3], NULL, 0); /* 0 = until NOSPC */
    } else {
        PrintfLine("disk: unknown subcommand '%s'\n", argv[1]);
        ShellWrite(usage);
        return -1;
    }

    user_resp_disk_t resp;
    if (DiskUserCall(&req, &resp) < 0)
        return -1;

    if (req.op == USER_OP_DISK_FILL)
        PrintfLine("disk: %d KiB written to %s/fill.bin\n",
                     (int)(resp.bytes / 1024u), vol);
    else
        PrintfLine("disk: %s '%s' ok%s%s\n", argv[1], vol,
                     resp.detail[0] ? " - " : "", resp.detail);
    return 0;
}

/* ====================================================================
 * Registration
 * ==================================================================== */

void ShellRegisterDiskCommands(void) {
    ShellRegisterCommand("disk",
                           "Disk: disk <list|info|sync|check|read|mount|unmount|format|fill>",
                           CmdDisk);
}
