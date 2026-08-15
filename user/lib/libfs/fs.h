/*
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
int fs_get_item(const char *url, vfs_item_info_t *out_item);

/* Create an empty directory.  Fails with VFS_ERR_EXISTS if it exists. */
int fs_create_dir(const char *url);

/* Delete an item; recursive=1 removes a non-empty directory tree. */
int fs_delete_item(const char *url, int recursive);

/* Open (optionally create/truncate) a file.  flags = VFS_OPEN_*,
 * access = VFS_ACCESS_* bitmask.  The vfs_server derives the caller's
 * identity from its kernel-issued subject (ipc_recv_from) for the
 * open-time authz gate.  Fills *out_handle on success. */
int fs_open_item(const char *url, u32 flags, u32 access,
                 vfs_handle_t *out_handle);

/* Read up to len bytes at offset into buf; *got receives the actual
 * count (0 = EOF).  buf may be NULL only when len == 0. */
int fs_read(vfs_handle_t handle, u64 offset, void *buf, u32 len, u32 *got);

/* Write len bytes at offset.  Returns 0 on success (all bytes written),
 * or a negative error. */
int fs_write(vfs_handle_t handle, u64 offset, const void *buf, u32 len);

/* Release a file handle or enumerator. */
int fs_close(vfs_handle_t handle);

/* Start iterating a directory.  Fills *out_handle (enumerator). */
int fs_enum_begin(const char *url, vfs_handle_t *out_handle);

/* Fill batch with up to 64 child names (returned count).  Call until
 * it returns 0.  A batch buffer is ~16.5 KB — keep it static/heap. */
int fs_enum_next(vfs_handle_t enum_handle, vfs_enum_batch_t *batch);

/* End iteration (same as fs_close). */
int fs_enum_end(vfs_handle_t enum_handle);

/* Volume capacity/usage.  Any out pointer may be NULL. */
int fs_stat_volume(const char *url, u64 *total_bytes, u64 *used_bytes,
                   u32 *read_only);

/* Return the caller's kernel-issued subject ID as seen by the vfs
 * server (VFS_OP_WHOAMI proxies SYS_GET_SUBJECT through the trusted
 * server, so the value is unforgeable).  Fills *out_subject. */
int fs_whoami(u64 *out_subject);

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
int fs_create_bookmark(const char *url, u32 access, u64 expiry_ticks,
                       u8 *out_blob, u32 *out_blob_len);

/* Blob → temporary FileHandle.  The handle is only valid while the
 * bookmark record exists server-side; revoke closes it remotely.
 * Fills handle/item/granted access on success. */
int fs_resolve_bookmark(const u8 *blob, u32 bk_len, vfs_handle_t *out_handle,
                                                vfs_item_info_t *out_item, u32 *out_access);

/* Drop the bookmark server-side (idempotent).  After this, resolve
 * fails with VFS_ERR_ACCESS. */
int fs_revoke_bookmark(const u8 *blob, u32 bk_len);

/* Move/rename an item.  The itemID stays stable, so bookmarks that
 * reference it remain valid across the move.  new_name = "" or NULL
 * keeps the current name.  Fills *out_item on success. */
int fs_move_item(const char *src, const char *dst_dir, const char *new_name,
                 vfs_item_info_t *out_item);

#endif /* LIBFS_FS_H */
