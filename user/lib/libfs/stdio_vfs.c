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
 * stdio_vfs.c - VFS backend for libc's FILE I/O
 * Copyright (c) 2026 OpSys Project
 *
 * libc keeps FILE opaque and delegates every real operation to a
 * backend vtable (user/lib/libc/stdio.h).  This file is that backend for
 * OpSys files: it maps stdio onto libfs, which talks to the vfs_server
 * over IPC.
 *
 * Registration runs from an .init_array constructor, so any program that
 * links the shared user objects (i.e. every service) can call
 * fopen("/Volumes/Disk/notes.txt", "r") with no extra setup.
 *
 * ------------------------------------------------------------------
 * Structure (backend vtable over libfs):
 *   stdio_backend_t { open, read, write, seek, close, size, unlink, rename }
 *        open   -> FsOpenItem(url, VFS_OPEN_*, VFS_ACCESS_*)
 *        read   -> FsRead(handle, pos, buf, len, &got)
 *        write  -> FsWrite(handle, pos, buf, len)
 *        seek   -> position bookkeeping (SEEK_END from the size cache)
 *        size   -> RefreshSize() + the cached size
 *        close  -> FsClose(handle) + free(context)
 *   __attribute__((constructor)) StdioVfsRegister() -> StdioSetBackend()
 * How it works:
 *   Each FILE owns a context {handle, path, pos, size}.  A VFS handle is
 *   a stateless (offset, length) window, so the backend tracks the
 *   position itself and caches the file size to answer SEEK_END / fseek.
 *   RefreshSize() asks the SERVER for that size through the handle
 *   (FsStatHandle, VFS_OP_STAT_HANDLE) and falls back to the path
 *   (FsGetItem) and finally to the cached value, so a read/seek never
 *   fails just because a size query did.  Append mode opens the file and
 *   starts the position at its end.
 * Purpose:
 *   Give programs the ordinary stdio API on top of the object-handle
 *   VFS, without introducing kernel file descriptors.
 * Caveats:
 *   Paths are VFS URLs ("/Volumes/Disk/x"), not POSIX paths: libc has no
 *   current directory.  Two appenders on one file race (the size is
 *   cached per FILE).  Backend calls return negative VFS/kernel codes;
 *   stdio_file.c turns them into errno values.
 * ------------------------------------------------------------------
 */

#include "fs.h"                  /* libfs — VFS client (pulls in vfs.h) */
#include "../libos/syscalls.h"     /* OK / ERR_* codes                    */
#include <stdio.h>                 /* stdio_backend_t, StdioSetBackend    */
#include <malloc.h>
#include <stddef.h>
#include <string.h>

/* ====================================================================
 * Per-FILE context
 * ==================================================================== */

typedef struct {
    vfs_handle_t  handle;
    char          path[256]; /* URL, kept for SEEK_END / size queries */
    unsigned long pos;       /* logical byte offset inside the file   */
    unsigned long size;      /* cached file size in bytes             */
    int           append;    /* 1 = keep the position pinned to the end */
} vfs_stdio_ctx_t;

/* Parse an fopen() mode string into VFS open flags + access mask.
 * A pure read handle must not carry the write right: the vfs_server
 * masks the requested access against the caller's grants, so asking
 * for WRITE would fail the open on a read-only volume. */
static void ModeToVfs(const char *mode, u32 *flags, u32 *access, int *append) {
    u32 fl = 0, ac = 0;
    int app = 0, plus = 0, wr = 0;

    for (const char *p = mode; p && *p; p++) {
        switch (*p) {
        case 'w':
            fl |= VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE;
            wr = 1;
            break;
        case 'a':
            fl |= VFS_OPEN_CREATE | VFS_OPEN_APPEND;
            wr = 1;
            app = 1;
            break;
        case '+': plus = 1; break;
        default: break; /* 'r', 'b', 't' need no flag */
        }
    }

    ac = VFS_ACCESS_READ;
    if (wr || plus)
        ac |= VFS_ACCESS_WRITE;
    /* "r+" on an existing file: readable and writable, not truncated. */
    if (plus && !wr)
        ac = VFS_ACCESS_READ | VFS_ACCESS_WRITE;

    *flags  = fl;
    *access = ac;
    *append = app;
}

/* Refresh the cached file size, preferring the HANDLE over the URL.
 *
 * VFS_OP_STAT_HANDLE exists precisely for this (v1.0 object-model
 * completion): a FILE is a handle, and the handle is authoritative even
 * when the item was renamed after the open — the cached URL would then
 * describe nothing.  It is still a fallback chain rather than a single
 * call:
 *   1. FsStatHandle(handle)      — authoritative, rename-proof;
 *   2. FsGetItem(path)           — for a vfs_server without op 21;
 *   3. keep the cached size      — a size query must never fail an I/O.
 * A stale handle therefore degrades to the last known size instead of
 * turning a working read into an error. */
static void RefreshSize(vfs_stdio_ctx_t *ctx) {
    vfs_item_info_t info;
    if (FsStatHandle(ctx->handle, &info) == 0) {
        ctx->size = (unsigned long)info.size;
        return;
    }
    if (ctx->path[0] && FsGetItem(ctx->path, &info) == 0)
        ctx->size = (unsigned long)info.size;
}

/* ====================================================================
 * Backend operations
 * ==================================================================== */

static int VfsOpen(const char *path, const char *mode, void **out_ctx) {
    u32 flags = 0, access = 0;
    int append = 0;

    if (!path || !path[0])
        return ERR_INVAL;
    ModeToVfs(mode, &flags, &access, &append);

    vfs_handle_t h = 0;
    int          r = FsOpenItem(path, flags, access, &h);
    if (r < 0)
        return r;

    vfs_stdio_ctx_t *ctx = (vfs_stdio_ctx_t *)malloc(sizeof(*ctx));
    if (!ctx) {
        (void)FsClose(h);
        return ERR_NOMEM;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->handle = h;
    ctx->append = append;
    strncpy(ctx->path, path, sizeof(ctx->path) - 1);
    RefreshSize(ctx);
    if (append)
        ctx->pos = ctx->size;

    *out_ctx = ctx;
    return 0;
}

static int VfsRead(void *vctx, void *buf, unsigned long len, unsigned long *got) {
    vfs_stdio_ctx_t *ctx = (vfs_stdio_ctx_t *)vctx;
    u32              n   = 0;

    if (got)
        *got = 0;
    if (len == 0)
        return 0;
    int r = FsRead(ctx->handle, (u64)ctx->pos, buf, (u32)len, &n);
    if (r < 0)
        return r;
    ctx->pos += n;
    if (got)
        *got = n;
    return 0;
}

static int VfsWrite(void *vctx, const void *buf, unsigned long len) {
    vfs_stdio_ctx_t *ctx = (vfs_stdio_ctx_t *)vctx;

    if (len == 0)
        return 0;
    if (ctx->append) {
        RefreshSize(ctx);
        ctx->pos = ctx->size;
    }
    int r = FsWrite(ctx->handle, (u64)ctx->pos, buf, (u32)len);
    if (r < 0)
        return r;
    ctx->pos += len;
    if (ctx->pos > ctx->size)
        ctx->size = ctx->pos;
    return 0;
}

static int VfsSeek(void *vctx, long offset, int whence, unsigned long *pos) {
    vfs_stdio_ctx_t *ctx = (vfs_stdio_ctx_t *)vctx;
    long             base;

    if (whence == SEEK_SET)
        base = 0;
    else if (whence == SEEK_CUR)
        base = (long)ctx->pos;
    else if (whence == SEEK_END) {
        /* RefreshSize() re-stats through the handle (fstat), so
         * fseek(f, 0, SEEK_END) sees a size another writer just
         * changed — not the value cached at open. */
        RefreshSize(ctx);
        base = (long)ctx->size;
    } else
        return ERR_INVAL;

    long target = base + offset;
    if (target < 0)
        return ERR_INVAL;
    ctx->pos = (unsigned long)target;
    if (pos)
        *pos = ctx->pos;
    return 0;
}

static int VfsClose(void *vctx) {
    vfs_stdio_ctx_t *ctx = (vfs_stdio_ctx_t *)vctx;
    int              r   = FsClose(ctx->handle);
    free(ctx);
    return r;
}

/* fstat-by-handle: the backend has the handle, so the size query goes
 * through VFS_OP_STAT_HANDLE (FsStatHandle) rather than re-resolving the
 * path.  When the handle is gone (stale) the last size we know is
 * returned — ftell/fseek stay usable, and the next read reports the
 * authoritative VFS_ERR_STALE. */
static unsigned long VfsSize(void *vctx) {
    vfs_stdio_ctx_t *ctx = (vfs_stdio_ctx_t *)vctx;
    RefreshSize(ctx);
    return ctx->size;
}

static int VfsUnlink(const char *path) {
    /* recursive=1: remove() on a directory should take its contents
     * with it, matching what `rm` does on the shell. */
    return FsDeleteItem(path, 1);
}

/* rename(old, new): libfs moves by (src, destination directory,
 * optional new name), so split the destination path here. */
static int VfsRename(const char *oldpath, const char *newpath) {
    char        dst[VFS_PATH_MAX];
    char        dir[VFS_PATH_MAX];
    const char *base = NULL;

    if (!oldpath || !newpath)
        return ERR_INVAL;
    if (strlen(newpath) >= sizeof(dst))
        return ERR_INVAL;
    strcpy(dst, newpath);

    for (const char *p = dst; *p; p++)
        if (*p == '/')
            base = p;
    if (!base)
        return ERR_INVAL; /* relative destination: needs an explicit dir */

    unsigned long dlen = (unsigned long)(base - dst);
    if (dlen == 0)
        dlen = 1; /* "/name" -> directory "/" */
    memcpy(dir, dst, dlen);
    dir[dlen] = '\0';

    return FsMoveItem(oldpath, dir, base + 1, NULL);
}

static const stdio_backend_t s_vfs_backend = {
    VfsOpen, VfsRead, VfsWrite, VfsSeek, VfsClose, VfsSize, VfsUnlink, VfsRename,
};

/* ====================================================================
 * Registration — runs before main()
 * ==================================================================== */

__attribute__((constructor)) static void StdioVfsRegister(void) {
    (void)StdioSetBackend(&s_vfs_backend);
}
