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
 * cmd_fs.c - filesystem inspection & manipulation commands
 * Copyright (c) 2026 OpSys Project
 *
 * The file-oriented half of the v0.9 tool set: df, du, cp, head,
 * hexdump, touch, tree and wc.  Every one of them is a thin libfs
 * client — the shell never talks to a driver, and the vfs_server keeps
 * owning the namespace, the handles and the authorization decisions
 * (a read that the caller may not perform comes back as -105 EACCES,
 * which these commands report verbatim so the Powerbox flow stays
 * visible to the user).
 *
 * ------------------------------------------------------------------
 * Structure (one command per function, shared libfs client):
 *   ShellRegisterFsCommands()
 *     -> ShellRegisterCommand("df"|"du"|"cp"|"head"|"hexdump"|"touch"|"tree"|"wc")
 *   FsResolve()  path -> absolute VFS URL through the shell's cwd
 *   WalkDir()    recursive traversal shared by du / tree / cp
 *   CopyFile()   chunked read -> write used by cp
 * How it works:
 *   Each command resolves its arguments with ShellResolvePath(), then
 *   drives libfs (FsGetItem / FsOpenItem / FsRead / FsWrite /
 *   FsEnumBegin / FsEnumNext / FsStatVolume / FsListVolumes).  Large
 *   traversal state lives in static buffers because a user thread only
 *   has 16 KiB of stack (USER_STACK_PAGES = 4).
 * Purpose:
 *   Turn the raw object model into the tools a user expects from a
 *   filesystem: how much space is left, how big is this tree, copy a
 *   file, peek at the first lines, look at the raw bytes.
 * Caveats:
 *   No glob expansion and no recursive delete here (rm/mv keep their
 *   own commands).  Directory recursion is depth-limited (FS_MAX_DEPTH)
 *   and reports a truncated result instead of looping forever on a
 *   corrupted volume.  Paths are UTF-8 and are passed through to the
 *   server unchanged.
 * ------------------------------------------------------------------
 */

#include "shell.h"

#include <stdarg.h>

#include "../lib/libc/stdio.h"
#include "../lib/libc/stdlib.h"
#include "../lib/libc/string.h"
#include "../lib/libfs/fs.h"
#include "../lib/libos/syscalls.h"

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
 * Shared helpers
 * ==================================================================== */

#define FS_PATH_MAX   256
#define FS_IO_CHUNK   1024
#define FS_MAX_DEPTH  8
#define FS_NAME_MAX   64

/* Resolve a user path against the shell's cwd into an absolute URL.
 * Returns 0 on success, negative on error (message already printed). */
static int FsResolve(const char *path, char *out, size_t outsz) {
    if (ShellResolvePath(path, out, outsz) < 0) {
        PrintfLine("fs: bad path '%s'\n", path);
        return ERR_INVAL;
    }
    return 0;
}

/* Walk the volume list and print capacity, the way df does.  When
 * want is non-NULL only the volume whose URL prefix matches is shown. */
static int FsDfVolume(const char *mount, const char *driver, u32 read_only) {
    char url[FS_NAME_MAX + 16];
    snprintf(url, sizeof(url), "/Volumes/%s", mount);

    u64 total = 0, used = 0;
    u32 ro    = 0;
    int r     = FsStatVolume(url, &total, &used, &ro);
    if (r < 0) {
        PrintfLine("%-10s %-12s %s\n", mount, driver ? driver : "-", "(stat unavailable)");
        return r;
    }
    u64 free_bytes = total > used ? total - used : 0;
    u32 pct        = total ? (u32)((used * 100u) / total) : 0;
    PrintfLine("%-10s %-12s %8d KiB %8d KiB %8d KiB %3d%%%s\n",
                 mount,
                 driver ? driver : "-",
                 (int)(total / 1024u),
                 (int)(used / 1024u),
                 (int)(free_bytes / 1024u),
                 (int)pct,
                 (read_only || ro) ? "  ro" : "");
    return 0;
}

/* ====================================================================
 * df - volume capacity summary
 * ==================================================================== */

static int CmdDf(int argc, char *argv[]) {
    static vfs_vol_info_t vols[VFS_MAX_VOLS];
    u32                   count = 0;
    int                   r     = FsListVolumes(vols, &count);
    if (r < 0) {
        PrintfLine("df: volume list FAILED (%d)\n", r);
        return -1;
    }

    ShellWrite("Filesystem   Driver          Size      Used      Avail  Use%\n");
    ShellWrite("----------   ------------  --------  --------  --------  ----\n");

    int shown = 0;
    for (u32 i = 0; i < count; i++) {
        /* df [vol|url]: filter to one volume when an argument is given. */
        if (argc >= 2) {
            const char *want = argv[1];
            if (want[0] == '/') {
                /* URL form: compare the "/Volumes/<name>" prefix. */
                char prefix[FS_NAME_MAX + 16];
                snprintf(prefix, sizeof(prefix), "/Volumes/%s", vols[i].mount_name);
                if (strncmp(want, prefix, strlen(prefix)) != 0)
                    continue;
            } else if (strcmp(want, vols[i].mount_name) != 0)
                continue;
        }
        (void)FsDfVolume(vols[i].mount_name, vols[i].driver_name, vols[i].read_only);
        shown++;
    }
    if (argc >= 2 && shown == 0) {
        PrintfLine("df: no such volume '%s'\n", argv[1]);
        return -1;
    }
    PrintfLine("df: %d volume(s) mounted\n", (int)count);
    return 0;
}

/* ====================================================================
 * du - recursive size of a subtree
 * ==================================================================== */

typedef struct {
    u64 bytes;
    u64 files;
    u64 dirs;
    int errors;
    int truncated;
} du_totals_t;

static void DuWalk(const char *url, int depth, du_totals_t *tot, u8 *batch_raw) {
    if (depth > FS_MAX_DEPTH) {
        tot->truncated = 1;
        return;
    }
    vfs_handle_t e = 0;
    if (FsEnumBegin(url, &e) < 0) {
        tot->errors++;
        return;
    }
    /* The batch is ~16.5 KB: the caller owns the storage (static). */
    vfs_enum_batch_t *batch = (vfs_enum_batch_t *)batch_raw;
    for (;;) {
        int r = FsEnumNext(e, batch);
        if (r < 0) {
            tot->errors++;
            break;
        }
        if (batch->batch_count == 0)
            break;

        for (u32 i = 0; i < batch->batch_count; i++) {
            char child[FS_PATH_MAX];
            const char *sep = (url[strlen(url) - 1] == '/') ? "" : "/";
            if (snprintf(child, sizeof(child), "%s%s%s", url, sep, batch->batch[i]) >=
                (int)sizeof(child))
                continue;

            vfs_item_info_t info;
            if (FsGetItem(child, &info) < 0) {
                tot->errors++;
                continue;
            }
            if (info.type == VFS_ITEM_DIR) {
                tot->dirs++;
                DuWalk(child, depth + 1, tot, batch_raw);
            } else {
                tot->files++;
                tot->bytes += info.size;
            }
        }
        if (batch->batch_count < 8) /* short batch = end of directory */
            break;
    }
    (void)FsEnumEnd(e);
}

static int CmdDu(int argc, char *argv[]) {
    char target[FS_PATH_MAX];
    const char *arg = (argc >= 2) ? argv[1] : NULL;
    if (arg) {
        if (FsResolve(arg, target, sizeof(target)) < 0)
            return -1;
    } else {
        strncpy(target, ShellCwd(), sizeof(target) - 1);
        target[sizeof(target) - 1] = '\0';
    }

    static u8 scratch[sizeof(vfs_enum_batch_t)];
    du_totals_t tot;
    memset(&tot, 0, sizeof(tot));

    /* du prints the subtree total, not per-entry (like du -s). */
    if (strcmp(target, "/") == 0 || strcmp(target, "/Volumes") == 0) {
        static vfs_vol_info_t vols[VFS_MAX_VOLS];
        u32                   count = 0;
        if (FsListVolumes(vols, &count) < 0) {
            ShellWrite("du: volume list FAILED\n");
            return -1;
        }
        for (u32 i = 0; i < count; i++) {
            char vurl[FS_NAME_MAX + 16];
            snprintf(vurl, sizeof(vurl), "/Volumes/%s", vols[i].mount_name);
            DuWalk(vurl, 0, &tot, scratch);
        }
    } else {
        vfs_item_info_t info;
        if (FsGetItem(target, &info) < 0) {
            PrintfLine("du: %s FAILED (not found or not permitted)\n", target);
            return -1;
        }
        if (info.type == VFS_ITEM_DIR)
            DuWalk(target, 0, &tot, scratch);
        else {
            tot.files = 1;
            tot.bytes = info.size;
        }
    }

    PrintfLine("%d KiB\t%s\n", (int)((tot.bytes + 1023u) / 1024u), target);
    PrintfLine("du: %d file(s), %d dir(s), %d byte(s)%s%s\n",
                 (int)tot.files,
                 (int)tot.dirs,
                 (int)tot.bytes,
                 tot.errors ? ", errors=" : "",
                 tot.errors ? "yes" : "");
    if (tot.truncated)
        ShellWrite("du: depth limit reached, result truncated\n");
    return 0;
}

/* ====================================================================
 * cp - copy a file (or a directory tree)
 * ==================================================================== */

/* Copy one file's contents.  Returns 0 on success. */
static int CopyFile(const char *src, const char *dst) {
    vfs_item_info_t info;
    int             r = FsGetItem(src, &info);
    if (r < 0) {
        PrintfLine("cp: cannot stat %s (%d)\n", src, r);
        return r;
    }
    if (info.type == VFS_ITEM_DIR) {
        PrintfLine("cp: %s is a directory\n", src);
        return ERR_INVAL;
    }

    vfs_handle_t in = 0;
    r = FsOpenItem(src, VFS_OPEN_READONLY, VFS_ACCESS_READ, &in);
    if (r < 0) {
        PrintfLine("cp: open %s FAILED (%d)\n", src, r);
        return r;
    }

    vfs_handle_t out = 0;
    r = FsOpenItem(dst, VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE, VFS_ACCESS_WRITE, &out);
    if (r < 0) {
        PrintfLine("cp: create %s FAILED (%d)\n", dst, r);
        FsClose(in);
        return r;
    }

    static u8 buf[FS_IO_CHUNK];
    u64        off = 0;
    for (;;) {
        u32 got = 0;
        r       = FsRead(in, off, buf, sizeof(buf), &got);
        if (r < 0) {
            PrintfLine("cp: read FAILED (%d)\n", r);
            FsClose(in);
            FsClose(out);
            return r;
        }
        if (got == 0)
            break;
        r = FsWrite(out, off, buf, got);
        if (r < 0) {
            PrintfLine("cp: write FAILED (%d)\n", r);
            FsClose(in);
            FsClose(out);
            return r;
        }
        off += got;
    }

    FsClose(in);
    FsClose(out);
    PrintfLine("cp: %s -> %s (%d bytes)\n", src, dst, (int)off);
    return 0;
}

/* Recursive directory copy.  dst must already exist. */
static int CopyTree(const char *src, const char *dst, int depth) {
    if (depth > FS_MAX_DEPTH) {
        ShellWrite("cp: depth limit reached\n");
        return ERR_INVAL;
    }
    static u8 scratch[sizeof(vfs_enum_batch_t)];
    vfs_enum_batch_t *batch = (vfs_enum_batch_t *)scratch;

    vfs_handle_t e = 0;
    int          r = FsEnumBegin(src, &e);
    if (r < 0) {
        PrintfLine("cp: enumerate %s FAILED (%d)\n", src, r);
        return r;
    }

    int copied = 0;
    for (;;) {
        r = FsEnumNext(e, batch);
        if (r < 0 || batch->batch_count == 0)
            break;

        for (u32 i = 0; i < batch->batch_count; i++) {
            char s[FS_PATH_MAX];
            char d[FS_PATH_MAX];
            snprintf(s, sizeof(s), "%s/%s", src, batch->batch[i]);
            snprintf(d, sizeof(d), "%s/%s", dst, batch->batch[i]);

            vfs_item_info_t info;
            if (FsGetItem(s, &info) < 0)
                continue;
            if (info.type == VFS_ITEM_DIR) {
                if (FsCreateDir(d) < 0 && FsGetItem(d, &info) < 0) {
                    PrintfLine("cp: mkdir %s FAILED\n", d);
                    continue;
                }
                (void)CopyTree(s, d, depth + 1);
            } else {
                if (CopyFile(s, d) == 0)
                    copied++;
            }
        }
        if (batch->batch_count < 8)
            break;
    }
    (void)FsEnumEnd(e);
    return copied >= 0 ? 0 : -1;
}

static int CmdCp(int argc, char *argv[]) {
    if (argc < 3) {
        ShellWrite("Usage: cp <src-url> <dst-url>\n");
        return -1;
    }
    char src[FS_PATH_MAX], dst[FS_PATH_MAX];
    if (FsResolve(argv[1], src, sizeof(src)) < 0)
        return -1;
    if (FsResolve(argv[2], dst, sizeof(dst)) < 0)
        return -1;

    vfs_item_info_t sinfo;
    if (FsGetItem(src, &sinfo) < 0) {
        PrintfLine("cp: %s FAILED (not found or not permitted)\n", src);
        return -1;
    }

    /* A trailing '/' or an existing directory means "copy into". */
    vfs_item_info_t dinfo;
    int             dst_is_dir = (FsGetItem(dst, &dinfo) == 0 && dinfo.type == VFS_ITEM_DIR);
    if (dst[strlen(dst) - 1] == '/')
        dst_is_dir = 1;

    char final[FS_PATH_MAX];
    const char *target = dst;
    if (dst_is_dir) {
        const char *base = strrchr(src, '/');
        base             = base ? base + 1 : src;
        snprintf(final, sizeof(final), "%s%s%s",
                  dst, (dst[strlen(dst) - 1] == '/') ? "" : "/", base);
        target = final;
    }

    if (sinfo.type == VFS_ITEM_DIR) {
        if (!dst_is_dir) {
            PrintfLine("cp: %s is a directory (give an existing target dir)\n", src);
            return -1;
        }
        if (FsCreateDir(target) < 0) {
            vfs_item_info_t t;
            if (FsGetItem(target, &t) < 0) {
                PrintfLine("cp: cannot create %s\n", target);
                return -1;
            }
        }
        return CopyTree(src, target, 0);
    }
    return CopyFile(src, target);
}

/* ====================================================================
 * touch - create an empty file (or leave an existing one alone)
 * ==================================================================== */

static int CmdTouch(int argc, char *argv[]) {
    if (argc < 2) {
        ShellWrite("Usage: touch <file-url>\n");
        return -1;
    }
    char url[FS_PATH_MAX];
    if (FsResolve(argv[1], url, sizeof(url)) < 0)
        return -1;

    vfs_item_info_t info;
    if (FsGetItem(url, &info) == 0) {
        PrintfLine("touch: %s exists (%d bytes)\n", url, (int)info.size);
        return 0;
    }

    vfs_handle_t h = 0;
    int r = FsOpenItem(url, VFS_OPEN_CREATE, VFS_ACCESS_WRITE, &h);
    if (r < 0) {
        PrintfLine("touch: create %s FAILED (%d)%s\n", url, r,
                     (r == VFS_ERR_ACCESS) ? " - permission required (Powerbox)" : "");
        return -1;
    }
    FsClose(h);
    PrintfLine("touch: created %s\n", url);
    return 0;
}

/* ====================================================================
 * head - first N lines of a text file
 * ==================================================================== */

static int CmdHead(int argc, char *argv[]) {
    if (argc < 2) {
        ShellWrite("Usage: head <file-url> [lines]\n");
        return -1;
    }
    int want = 10;
    if (argc >= 3) {
        want = atoi(argv[2]);
        if (want <= 0)
            want = 10;
    }
    char url[FS_PATH_MAX];
    if (FsResolve(argv[1], url, sizeof(url)) < 0)
        return -1;

    vfs_handle_t h = 0;
    int r = FsOpenItem(url, VFS_OPEN_READONLY, VFS_ACCESS_READ, &h);
    if (r < 0) {
        PrintfLine("head: open %s FAILED (%d)\n", url, r);
        return -1;
    }

    static u8 buf[FS_IO_CHUNK];
    u64 off      = 0;
    int lines    = 0; /* complete lines emitted          */
    int done     = 0;
    int printed  = 0; /* any byte emitted at all         */
    char last_ch = '\n';
    while (!done && lines < want) {
        u32 got = 0;
        r       = FsRead(h, off, buf, sizeof(buf), &got);
        if (r < 0 || got == 0)
            break;
        for (u32 i = 0; i < got; i++) {
            ShellPutc((char)buf[i]);
            printed++;
            last_ch = (char)buf[i];
            if (buf[i] == '\n') {
                lines++;
                if (lines >= want) {
                    done = 1;
                    break;
                }
            }
        }
        off += got;
    }
    /* A file that does not end in a newline still has one more line. */
    if (printed && last_ch != '\n') {
        lines++;
        ShellWrite("\n");
    }
    FsClose(h);
    PrintfLine("head: %d line(s) shown of %s\n", lines, url);
    return 0;
}

/* ====================================================================
 * hexdump - raw bytes as hex + ASCII
 * ==================================================================== */

static int CmdHexdump(int argc, char *argv[]) {
    if (argc < 2) {
        ShellWrite("Usage: hexdump <file-url> [bytes]\n");
        return -1;
    }
    long limit = 256;
    if (argc >= 3) {
        limit = atol(argv[2]);
        if (limit <= 0)
            limit = 256;
    }
    char url[FS_PATH_MAX];
    if (FsResolve(argv[1], url, sizeof(url)) < 0)
        return -1;

    vfs_item_info_t info;
    if (FsGetItem(url, &info) < 0) {
        PrintfLine("hexdump: %s FAILED (not found or not permitted)\n", url);
        return -1;
    }
    PrintfLine("== %s (%d bytes) ==\n", url, (int)info.size);

    vfs_handle_t h = 0;
    int r = FsOpenItem(url, VFS_OPEN_READONLY, VFS_ACCESS_READ, &h);
    if (r < 0) {
        PrintfLine("hexdump: open FAILED (%d)\n", r);
        return -1;
    }

    u8  row[16];
    u64 off     = 0;
    long shown  = 0;
    while (shown < limit) {
        u32 want = sizeof(row);
        if ((long)want > limit - shown)
            want = (u32)(limit - shown);
        u32 got = 0;
        r       = FsRead(h, off, row, want, &got);
        if (r < 0) {
            PrintfLine("hexdump: read FAILED (%d)\n", r);
            break;
        }
        if (got == 0)
            break;

        char line[128];
        int  n = snprintf(line, sizeof(line), "%08x  ", (unsigned)off);
        for (u32 i = 0; i < 16; i++) {
            if (i < got)
                n += snprintf(line + n, sizeof(line) - (size_t)n, "%02x ", row[i]);
            else
                n += snprintf(line + n, sizeof(line) - (size_t)n, "   ");
            if (i == 7)
                n += snprintf(line + n, sizeof(line) - (size_t)n, " ");
        }
        n += snprintf(line + n, sizeof(line) - (size_t)n, " |");
        for (u32 i = 0; i < got; i++) {
            char c = (char)row[i];
            n += snprintf(line + n, sizeof(line) - (size_t)n, "%c",
                           (c >= 32 && c < 127) ? c : '.');
        }
        n += snprintf(line + n, sizeof(line) - (size_t)n, "|\n");
        ShellWrite(line);

        off += got;
        shown += (long)got;
    }
    FsClose(h);
    PrintfLine("hexdump: %d byte(s) shown%s\n", (int)shown,
                 (info.size > (u64)shown) ? " (truncated; pass a byte count)" : "");
    return 0;
}

/* ====================================================================
 * tree - directory hierarchy
 * ==================================================================== */

static void TreeWalk(const char *url, int depth, u32 *dirs, u32 *files, u8 *scratch) {
    if (depth > FS_MAX_DEPTH) {
        ShellWrite("    ... (depth limit)\n");
        return;
    }
    vfs_enum_batch_t *batch = (vfs_enum_batch_t *)scratch;
    vfs_handle_t      e     = 0;
    if (FsEnumBegin(url, &e) < 0)
        return;

    for (;;) {
        int r = FsEnumNext(e, batch);
        if (r < 0 || batch->batch_count == 0)
            break;
        for (u32 i = 0; i < batch->batch_count; i++) {
            char child[FS_PATH_MAX];
            snprintf(child, sizeof(child), "%s/%s", url, batch->batch[i]);

            vfs_item_info_t info;
            if (FsGetItem(child, &info) < 0) {
                PrintfLine("%*s%s (unreadable)\n", depth * 2 + 2, "", batch->batch[i]);
                continue;
            }
            for (int k = 0; k < depth * 2 + 2; k++)
                ShellPutc(' ');
            if (info.type == VFS_ITEM_DIR) {
                PrintfLine("%s/\n", batch->batch[i]);
                (*dirs)++;
                TreeWalk(child, depth + 1, dirs, files, scratch);
            } else {
                PrintfLine("%s (%d bytes)\n", batch->batch[i], (int)info.size);
                (*files)++;
            }
        }
        if (batch->batch_count < 8)
            break;
    }
    (void)FsEnumEnd(e);
}

static int CmdTree(int argc, char *argv[]) {
    char target[FS_PATH_MAX];
    const char *arg = (argc >= 2) ? argv[1] : NULL;
    if (arg) {
        if (FsResolve(arg, target, sizeof(target)) < 0)
            return -1;
    } else {
        strncpy(target, ShellCwd(), sizeof(target) - 1);
        target[sizeof(target) - 1] = '\0';
    }

    vfs_item_info_t info;
    if (FsGetItem(target, &info) < 0) {
        PrintfLine("tree: %s FAILED (not found or not permitted)\n", target);
        return -1;
    }
    PrintfLine("%s\n", target);
    if (info.type != VFS_ITEM_DIR) {
        PrintfLine("tree: not a directory (%d bytes)\n", (int)info.size);
        return 0;
    }

    static u8 scratch[sizeof(vfs_enum_batch_t)];
    u32       dirs = 0, files = 0;
    TreeWalk(target, 0, &dirs, &files, scratch);
    PrintfLine("tree: %d dir(s), %d file(s)\n", (int)dirs, (int)files);
    return 0;
}

/* ====================================================================
 * wc - line / word / byte count
 * ==================================================================== */

static int CmdWc(int argc, char *argv[]) {
    if (argc < 2) {
        ShellWrite("Usage: wc <file-url>\n");
        return -1;
    }
    char url[FS_PATH_MAX];
    if (FsResolve(argv[1], url, sizeof(url)) < 0)
        return -1;

    vfs_handle_t h = 0;
    int r = FsOpenItem(url, VFS_OPEN_READONLY, VFS_ACCESS_READ, &h);
    if (r < 0) {
        PrintfLine("wc: open %s FAILED (%d)\n", url, r);
        return -1;
    }

    static u8 buf[FS_IO_CHUNK];
    u64 off = 0, bytes = 0, lines = 0, words = 0;
    int in_word = 0;
    for (;;) {
        u32 got = 0;
        r       = FsRead(h, off, buf, sizeof(buf), &got);
        if (r < 0 || got == 0)
            break;
        for (u32 i = 0; i < got; i++) {
            char c = (char)buf[i];
            if (c == '\n')
                lines++;
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                words++;
            }
        }
        off += got;
        bytes += got;
    }
    FsClose(h);
    PrintfLine("%6d %6d %6d  %s\n", (int)lines, (int)words, (int)bytes, url);
    return 0;
}

/* ====================================================================
 * Registration
 * ==================================================================== */

void ShellRegisterFsCommands(void) {
    ShellRegisterCommand("df", "Volume usage: df [vol|url]", CmdDf);
    ShellRegisterCommand("du", "Recursive size: du [url] (default: cwd)", CmdDu);
    ShellRegisterCommand("cp", "Copy a file or tree: cp <src> <dst>", CmdCp);
    ShellRegisterCommand("touch", "Create an empty file: touch <url>", CmdTouch);
    ShellRegisterCommand("head", "First lines of a file: head <url> [n=10]", CmdHead);
    ShellRegisterCommand("hexdump", "Raw bytes as hex+ASCII: hexdump <url> [bytes=256]", CmdHexdump);
    ShellRegisterCommand("tree", "Directory hierarchy: tree [url]", CmdTree);
    ShellRegisterCommand("wc", "Line/word/byte count: wc <url>", CmdWc);
}
