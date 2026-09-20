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
 * fs.h - libfs: user-space VFS client library (docs/vfs_design.md §4.3)
 * Copyright (c) 2026 OpSys Project
 *
 * Thin client over the "vfs" server port (vfs.h protocol).  All
 * functions return 0 on success or a negative error code.  URLs are
 * "Volume/a/b/c" or "/Volumes/Volume/a/b/c".
 */

#ifndef LIBFS_FS_H
#define LIBFS_FS_H

#include <stdint.h>
#include "../../services/vfs/vfs.h"

/* Metadata for a single item (no open required). */
int FsGetItem(const char *url, vfs_item_info_t *out_item);

/* Create an empty directory.  Fails with VFS_ERR_EXISTS if it exists. */
int FsCreateDir(const char *url);

/* Delete an item; recursive=1 removes a non-empty directory tree. */
int FsDeleteItem(const char *url, int recursive);

/* Open (optionally create/truncate) a file.  flags = VFS_OPEN_*,
 * access = VFS_ACCESS_* bitmask.  The vfs_server derives the caller's
 * identity from its kernel-issued subject (ipc_recv_from) for the
 * open-time authz gate.  Fills *out_handle on success. */
int FsOpenItem(const char *url, u32 flags, u32 access, vfs_handle_t *out_handle);

/* Read up to len bytes at offset into buf; *got receives the actual
 * count (0 = EOF).  buf may be NULL only when len == 0. */
int FsRead(vfs_handle_t handle, u64 offset, void *buf, u32 len, u32 *got);

/* Write len bytes at offset.  Returns 0 on success (all bytes written),
 * or a negative error. */
int FsWrite(vfs_handle_t handle, u64 offset, const void *buf, u32 len);

/* Release a file handle or enumerator. */
int FsClose(vfs_handle_t handle);

/* Start iterating a directory.  Fills *out_handle (enumerator). */
int FsEnumBegin(const char *url, vfs_handle_t *out_handle);

/* Fill batch with up to 64 child names (returned count).  Call until
 * it returns 0.  A batch buffer is ~16.5 KB — keep it static/heap. */
int FsEnumNext(vfs_handle_t enum_handle, vfs_enum_batch_t *batch);

/* End iteration (same as fs_close). */
int FsEnumEnd(vfs_handle_t enum_handle);

/* Volume capacity/usage.  Any out pointer may be NULL. */
int FsStatVolume(const char *url, u64 *total_bytes, u64 *used_bytes, u32 *read_only);

/* Return the caller's kernel-issued subject ID as seen by the vfs
 * server (VFS_OP_WHOAMI proxies SYS_GET_SUBJECT through the trusted
 * server, so the value is unforgeable).  Fills *out_subject. */
int FsWhoami(u64 *out_subject);

/* Enumerate currently-mounted volumes (the "/" root view).  Fills
 * out_vols (up to VFS_MAX_VOLS entries) and sets *out_count.  There
 * is no root item to enum "/" on — URLs start at a volume name, so
 * the root view is served from the mount table. */
int FsListVolumes(vfs_vol_info_t *out_vols, u32 *out_count);

/* ====================================================================
 * Phase 2: security-scoped bookmarks + move (design §5, §8)
 *
 * A bookmark is an opaque capability blob.  The vfs_server holds the
 * authoritative record; the blob is only a carrier.  Creating one goes
 * through the Powerbox (perm-manager); un-authorized requests fail
 * with VFS_ERR_ACCESS (-105) and a UI prompt is pushed to "perm.ui".
 * ==================================================================== */

/* Request a Powerbox-gated bookmark for url.  Fills a blob
 * (VFS_BOOKMARK_MAX bytes max).  Fails with VFS_ERR_ACCESS if the
 * access was denied.  The bookmark is bound to the caller's kernel
 * subject (the vfs_server derives it from ipc_recv_from). */
int FsCreateBookmark(
    const char *url, u32 access, u64 expiry_ticks, u8 *out_blob, u32 *out_blob_len);

/* Blob → temporary FileHandle.  The handle is only valid while the
 * bookmark record exists server-side; revoke closes it remotely.
 * Fills handle/item/granted access on success. */
int FsResolveBookmark(const u8        *blob,
                        u32              bk_len,
                        vfs_handle_t    *out_handle,
                        vfs_item_info_t *out_item,
                        u32             *out_access);

/* Drop the bookmark server-side (idempotent).  After this, resolve
 * fails with VFS_ERR_ACCESS. */
int FsRevokeBookmark(const u8 *blob, u32 bk_len);

/* Move/rename an item.  The itemID stays stable, so bookmarks that
 * reference it remain valid across the move.  new_name = "" or NULL
 * keeps the current name.  Fills *out_item on success. */
int FsMoveItem(const char      *src,
                 const char      *dst_dir,
                 const char      *new_name,
                 vfs_item_info_t *out_item);

/* Phase 3 zero-copy read: map the file's backing pool pages READ-ONLY
 * into the caller at `map_virt` (a vspace_alloc()'d range).  Returns
 * the mapped size via *mapped_size and 0 on success; ERR_NOENT means
 * the file is not pool-backed (fall back to FsRead()). */
int FsReadMap(vfs_handle_t handle, void *map_virt, u32 *mapped_size);

/* Flush every mounted volume to its backing store (VFS_OP_SYNC).
 * Called before power-off so buffered driver metadata reaches the
 * medium.  When out_volumes is non-NULL it receives the number of
 * volumes that acknowledged the flush.  Returns 0, or a negative
 * error (the vfs_server reports the first hard failure). */
int FsSync(u32 *out_volumes);

/* ====================================================================
 * v1.0: object-model completion (design §3, vfs.h ops 21-23)
 *
 * Everything above asks a question about a URL.  These three ask it
 * about an OPEN HANDLE instead — which is all a stdio FILE, a resolved
 * bookmark or a zero-copy reader still holds.  Like every other handle
 * operation they re-run the caller's authorization server-side, so a
 * revoked grant surfaces as an error instead of silently succeeding.
 * ==================================================================== */

/**
 * Metadata for an open handle (fstat by handle, VFS_OP_STAT_HANDLE).
 *
 * No URL and no re-resolve needed, so a program that only ever saw a
 * handle can still answer "how big is this, and what is it called?".
 *
 * Returns 0 and fills *out_item on success.
 * Errors: ERR_INVAL for a NULL out_item; VFS_ERR_STALE (-102) when the
 * handle is no longer valid (item deleted, handle closed, access
 * revoked); any negative port/IPC error is passed through unchanged.
 */
int FsStatHandle(vfs_handle_t handle, vfs_item_info_t *out_item);

/**
 * Set a file's length through its handle (VFS_OP_TRUNCATE).
 *
 * Shrinking releases the tail; growing fills the new range with zeros.
 * The server re-checks the write side of the caller's authorization,
 * exactly like FsWrite().
 *
 * Returns 0 on success; *out_size (optional) receives the resulting
 * length (== size).
 * Errors: VFS_ERR_STALE (-102) for a dead handle; VFS_ERR_PERM (-103)
 * or VFS_ERR_READONLY (-100) when the caller may not write; VFS_ERR_NOSPC
 * (-101) when the volume cannot grow the file; ERR_INVAL for a bad call.
 */
int FsTruncate(vfs_handle_t handle, u64 size, u64 *out_size);

/**
 * Extend a bookmark's lifetime (VFS_OP_REFRESH_BOOKMARK).
 *
 * An expired bookmark stops resolving (VFS_ERR_STALE); its holder can
 * renew it here as long as the grant behind it is still live.  The
 * server rewrites expiry_ticks inside the blob and hands the updated
 * blob back, so the caller must replace its cached copy.
 * extend_ticks: 0 = make it permanent, > 0 = now + extend_ticks.
 *
 * out_blob MUST provide VFS_BOOKMARK_MAX (256) bytes.  On success
 * *out_bk_len holds the updated blob's real length and *out_expiry the
 * new absolute deadline (0 = permanent); both are left untouched on
 * error.
 *
 * Returns 0 on success.
 * Errors: ERR_INVAL for a NULL blob/out_blob/out_bk_len or a bk_len
 * outside [1, VFS_BOOKMARK_MAX]; VFS_ERR_STALE (-102) when the bookmark
 * is unknown or was revoked; VFS_ERR_ACCESS (-105) when the grant
 * behind it was dropped; ERR_FAULT if the server reports a blob larger
 * than VFS_BOOKMARK_MAX.
 */
int FsRefreshBookmark(const u8 *blob,
                      u32       bk_len,
                      u64       extend_ticks,
                      u8       *out_blob,
                      u32      *out_bk_len,
                      u64      *out_expiry);

#endif /* LIBFS_FS_H */
