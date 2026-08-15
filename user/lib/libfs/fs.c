/*
 * fs.c - libfs: user-space VFS client library implementation
 * Copyright (c) 2026 OpSys Project
 *
 * Thin client over the "vfs" server port (vfs.h protocol), one
 * ipc_call per operation.  Reads/writes are chunked transparently so
 * callers never see the 4032-byte wire limit.
 *
 * NOTE: single-threaded client library (Phase 0).  The request and
 * response buffers are shared statics — concurrent callers would
 * clobber each other.  The only Phase 0 client (the shell) is
 * single-threaded, so this is fine; a multi-threaded client would need
 * per-thread buffers.
 */

#include "fs.h"
#include "../libos/syscalls.h"
#include "../libc/string.h"

/* Shared message buffers (see header note) */
static u8  s_req[VFS_IPC_MAX];
static u8  s_resp[VFS_IPC_MAX];
static int s_vfs_port = -1;         /* resolved once, cached */

static int fs_port(void)
{
        if (s_vfs_port < 0) {
                s_vfs_port = port_get("vfs");
                if (s_vfs_port < 0)
                        return s_vfs_port;
        }
        return s_vfs_port;
}

int fs_get_item(const char *url, vfs_item_info_t *out_item)
{
        if (!url || !out_item)
                return ERR_INVAL;
        int port = fs_port();
        if (port < 0)
                return port;

        vfs_req_get_item_t *req = (vfs_req_get_item_t *)s_req;
        memset(req, 0, sizeof(*req));
        req->op = VFS_OP_GET_ITEM;
        strncpy(req->path, url, sizeof(req->path) - 1);

        vfs_resp_get_item_t *resp = (vfs_resp_get_item_t *)s_resp;
        int resp_len = (int)sizeof(*resp);
        int r = ipc_call(port, req, (int)sizeof(*req), resp, &resp_len);
        if (r < 0)
                return r;
        if (resp->ret < 0)
                return resp->ret;
        *out_item = resp->item;
        return 0;
}

int fs_create_dir(const char *url)
{
        if (!url)
                return ERR_INVAL;
        int port = fs_port();
        if (port < 0)
                return port;

        vfs_req_create_dir_t *req = (vfs_req_create_dir_t *)s_req;
        memset(req, 0, sizeof(*req));
        req->op = VFS_OP_CREATE_DIR;
        strncpy(req->path, url, sizeof(req->path) - 1);

        vfs_resp_create_dir_t *resp = (vfs_resp_create_dir_t *)s_resp;
        int resp_len = (int)sizeof(*resp);
        int r = ipc_call(port, req, (int)sizeof(*req), resp, &resp_len);
        if (r < 0)
                return r;
        return resp->ret;
}

int fs_delete_item(const char *url, int recursive)
{
        if (!url)
                return ERR_INVAL;
        int port = fs_port();
        if (port < 0)
                return port;

        vfs_req_delete_t *req = (vfs_req_delete_t *)s_req;
        memset(req, 0, sizeof(*req));
        req->op = VFS_OP_DELETE_ITEM;
        strncpy(req->path, url, sizeof(req->path) - 1);
        req->recursive = recursive ? 1 : 0;

        vfs_resp_delete_t *resp = (vfs_resp_delete_t *)s_resp;
        int resp_len = (int)sizeof(*resp);
        int r = ipc_call(port, req, (int)sizeof(*req), resp, &resp_len);
        if (r < 0)
                return r;
        return resp->ret;
}

int fs_open_item(const char *url, u32 flags, u32 access,
                 vfs_handle_t *out_handle)
{
        if (!url || !out_handle)
                return ERR_INVAL;
        int port = fs_port();
        if (port < 0)
                return port;

        vfs_req_open_t *req = (vfs_req_open_t *)s_req;
        memset(req, 0, sizeof(*req));
        req->op = VFS_OP_OPEN_ITEM;
        strncpy(req->path, url, sizeof(req->path) - 1);
        req->flags = flags;
        req->access = access;

        vfs_resp_open_t *resp = (vfs_resp_open_t *)s_resp;
        int resp_len = (int)sizeof(*resp);
        int r = ipc_call(port, req, (int)sizeof(*req), resp, &resp_len);
        if (r < 0)
                return r;
        if (resp->ret < 0)
                return resp->ret;
        *out_handle = resp->handle;
        return 0;
}

int fs_read(vfs_handle_t handle, u64 offset, void *buf, u32 len, u32 *got)
{
        if (!buf && len > 0)
                return ERR_INVAL;
        int port = fs_port();
        if (port < 0)
                return port;

        u32 total = 0;
        u8 *p = (u8 *)buf;
        while (total < len) {
                u32 chunk = len - total;
                if (chunk > VFS_MAX_READ)
                        chunk = VFS_MAX_READ;

                vfs_req_read_t *req = (vfs_req_read_t *)s_req;
                memset(req, 0, sizeof(*req));
                req->op = VFS_OP_READ;
                req->handle = handle;
                req->offset = offset + total;
                req->len = chunk;

                vfs_resp_read_t *resp = (vfs_resp_read_t *)s_resp;
                int resp_len = (int)sizeof(*resp);
                int r = ipc_call(port, req, (int)sizeof(*req), resp, &resp_len);
                if (r < 0)
                        return r;
                if (resp->ret < 0)
                        return resp->ret;

                u32 n = (u32)resp->ret;
                if (n > chunk)
                        n = chunk;              /* defensive: server clamp */
                memcpy(p + total, resp->data, n);
                total += n;
                if (n < chunk)
                        break;                  /* EOF */
        }
        if (got)
                *got = total;
        return 0;
}

int fs_write(vfs_handle_t handle, u64 offset, const void *buf, u32 len)
{
        if (!buf && len > 0)
                return ERR_INVAL;
        int port = fs_port();
        if (port < 0)
                return port;

        u32 total = 0;
        const u8 *p = (const u8 *)buf;
        while (total < len) {
                u32 chunk = len - total;
                if (chunk > VFS_MAX_WRITE)
                        chunk = VFS_MAX_WRITE;

                vfs_req_write_t *req = (vfs_req_write_t *)s_req;
                memset(req, 0, sizeof(*req));
                req->op = VFS_OP_WRITE;
                req->handle = handle;
                req->offset = offset + total;
                req->len = chunk;
                memcpy(req->data, p + total, chunk);

                vfs_resp_write_t *resp = (vfs_resp_write_t *)s_resp;
                int resp_len = (int)sizeof(*resp);
                /* Header = { u32 op; u32 handle; u64 offset; u32 len } */
                int req_len = (int)(sizeof(u32) * 3 + sizeof(u64)) + (int)chunk;
                int r = ipc_call(port, req, req_len, resp, &resp_len);
                if (r < 0)
                        return r;
                if (resp->ret < 0)
                        return resp->ret;
                total += (u32)resp->ret;
                if ((u32)resp->ret < chunk)
                        return VFS_ERR_NOSPC;    /* short write: volume full etc. */
        }
        return 0;
}

int fs_close(vfs_handle_t handle)
{
        int port = fs_port();
        if (port < 0)
                return port;

        vfs_req_close_t *req = (vfs_req_close_t *)s_req;
        memset(req, 0, sizeof(*req));
        req->op = VFS_OP_CLOSE;
        req->handle = handle;

        vfs_resp_close_t *resp = (vfs_resp_close_t *)s_resp;
        int resp_len = (int)sizeof(*resp);
        int r = ipc_call(port, req, (int)sizeof(*req), resp, &resp_len);
        if (r < 0)
                return r;
        return resp->ret;
}

int fs_enum_begin(const char *url, vfs_handle_t *out_handle)
{
        if (!url || !out_handle)
                return ERR_INVAL;
        int port = fs_port();
        if (port < 0)
                return port;

        vfs_req_enum_begin_t *req = (vfs_req_enum_begin_t *)s_req;
        memset(req, 0, sizeof(*req));
        req->op = VFS_OP_ENUM_BEGIN;
        strncpy(req->path, url, sizeof(req->path) - 1);

        vfs_resp_enum_begin_t *resp = (vfs_resp_enum_begin_t *)s_resp;
        int resp_len = (int)sizeof(*resp);
        int r = ipc_call(port, req, (int)sizeof(*req), resp, &resp_len);
        if (r < 0)
                return r;
        if (resp->ret < 0)
                return resp->ret;
        *out_handle = resp->handle;
        return 0;
}

int fs_enum_next(vfs_handle_t enum_handle, vfs_enum_batch_t *batch)
{
        if (!batch)
                return ERR_INVAL;
        int port = fs_port();
        if (port < 0)
                return port;

        batch->handle = enum_handle;
        batch->batch_count = 0;

        /* Pull ≤8-entry wire batches from the server until the client
     * batch (64 names) is full or the directory is exhausted. */
        for (;;) {
                vfs_req_enum_next_t *req = (vfs_req_enum_next_t *)s_req;
                memset(req, 0, sizeof(*req));
                req->op = VFS_OP_ENUM_NEXT;
                req->handle = enum_handle;

                vfs_resp_enum_next_t *resp = (vfs_resp_enum_next_t *)s_resp;
                int resp_len = (int)sizeof(*resp);
                int r = ipc_call(port, req, (int)sizeof(*req), resp, &resp_len);
                if (r < 0)
                        return r;
                if (resp->ret < 0)
                        return resp->ret;

                u32 n = resp->count;
                for (u32 i = 0; i < n && batch->batch_count < 64; i++) {
                        strncpy(batch->batch[batch->batch_count],
                                        resp->items[i].name,
                                        sizeof(batch->batch[0]) - 1);
                        batch->batch[batch->batch_count][sizeof(batch->batch[0]) - 1] = '\0';
                        batch->batch_ids[batch->batch_count] = resp->items[i].id;
                        batch->batch_count++;
                }
                if (n == 0 || batch->batch_count >= 64)
                        break;
        }
        return (int)batch->batch_count;
}

int fs_enum_end(vfs_handle_t enum_handle)
{
        return fs_close(enum_handle);
}

int fs_stat_volume(const char *url, u64 *total_bytes, u64 *used_bytes,
                   u32 *read_only)
{
        if (!url)
                return ERR_INVAL;
        int port = fs_port();
        if (port < 0)
                return port;

        vfs_req_stat_volume_t *req = (vfs_req_stat_volume_t *)s_req;
        memset(req, 0, sizeof(*req));
        req->op = VFS_OP_STAT_VOLUME;
        strncpy(req->path, url, sizeof(req->path) - 1);

        vfs_resp_stat_volume_t *resp = (vfs_resp_stat_volume_t *)s_resp;
        int resp_len = (int)sizeof(*resp);
        int r = ipc_call(port, req, (int)sizeof(*req), resp, &resp_len);
        if (r < 0)
                return r;
        if (resp->ret < 0)
                return resp->ret;
        if (total_bytes)
                *total_bytes = resp->total_bytes;
        if (used_bytes)
                *used_bytes = resp->used_bytes;
        if (read_only)
                *read_only = resp->read_only;
        return 0;
}

int fs_whoami(u64 *out_subject)
{
        if (!out_subject)
                return ERR_INVAL;
        int port = fs_port();
        if (port < 0)
                return port;

        vfs_req_whoami_t *req = (vfs_req_whoami_t *)s_req;
        memset(req, 0, sizeof(*req));
        req->op = VFS_OP_WHOAMI;

        vfs_resp_whoami_t *resp = (vfs_resp_whoami_t *)s_resp;
        int resp_len = (int)sizeof(*resp);
        int r = ipc_call(port, req, (int)sizeof(*req), resp, &resp_len);
        if (r < 0)
                return r;
        if (resp->ret < 0)
                return resp->ret;
        *out_subject = resp->subject_id;
        return 0;
}

/* ====================================================================
 * Phase 2: security-scoped bookmarks + move (design §5, §8)
 * ==================================================================== */

int fs_create_bookmark(const char *url, u32 access, u64 expiry_ticks,
                       u8 *out_blob, u32 *out_blob_len)
{
        if (!url || !out_blob || !out_blob_len)
                return ERR_INVAL;
        int port = fs_port();
        if (port < 0)
                return port;

        vfs_req_create_bookmark_t *req = (vfs_req_create_bookmark_t *)s_req;
        memset(req, 0, sizeof(*req));
        req->op = VFS_OP_CREATE_BOOKMARK;
        strncpy(req->path, url, sizeof(req->path) - 1);
        req->access = access;
        req->expiry_ticks = expiry_ticks;

        vfs_resp_create_bookmark_t *resp = (vfs_resp_create_bookmark_t *)s_resp;
        int resp_len = (int)sizeof(*resp);
        int r = ipc_call(port, req, (int)sizeof(*req), resp, &resp_len);
        if (r < 0)
                return r;
        if (resp->ret < 0)
                return resp->ret;
        if (resp->bk_len > VFS_BOOKMARK_MAX)
                return ERR_FAULT;
        memcpy(out_blob, resp->data, resp->bk_len);
        *out_blob_len = resp->bk_len;
        return 0;
}

int fs_resolve_bookmark(const u8 *blob, u32 bk_len, vfs_handle_t *out_handle,
                                                vfs_item_info_t *out_item, u32 *out_access)
{
        if (!blob || !out_handle || bk_len == 0 || bk_len > VFS_BOOKMARK_MAX)
                return ERR_INVAL;
        int port = fs_port();
        if (port < 0)
                return port;

        vfs_req_resolve_bookmark_t *req = (vfs_req_resolve_bookmark_t *)s_req;
        memset(req, 0, sizeof(*req));
        req->op = VFS_OP_RESOLVE_BOOKMARK;
        req->bk_len = bk_len;
        memcpy(req->data, blob, bk_len);

        vfs_resp_resolve_bookmark_t *resp = (vfs_resp_resolve_bookmark_t *)s_resp;
        int resp_len = (int)sizeof(*resp);
        int r = ipc_call(port, req, (int)sizeof(*req), resp, &resp_len);
        if (r < 0)
                return r;
        if (resp->ret < 0)
                return resp->ret;
        *out_handle = resp->handle;
        if (out_item)
                *out_item = resp->item;
        if (out_access)
                *out_access = resp->access;
        return 0;
}

int fs_revoke_bookmark(const u8 *blob, u32 bk_len)
{
        if (!blob || bk_len == 0 || bk_len > VFS_BOOKMARK_MAX)
                return ERR_INVAL;
        int port = fs_port();
        if (port < 0)
                return port;

        vfs_req_revoke_bookmark_t *req = (vfs_req_revoke_bookmark_t *)s_req;
        memset(req, 0, sizeof(*req));
        req->op = VFS_OP_REVOKE_BOOKMARK;
        req->bk_len = bk_len;
        memcpy(req->data, blob, bk_len);

        vfs_resp_revoke_bookmark_t *resp = (vfs_resp_revoke_bookmark_t *)s_resp;
        int resp_len = (int)sizeof(*resp);
        int r = ipc_call(port, req, (int)sizeof(*req), resp, &resp_len);
        if (r < 0)
                return r;
        return resp->ret;
}

int fs_move_item(const char *src, const char *dst_dir, const char *new_name,
                 vfs_item_info_t *out_item)
{
        if (!src || !dst_dir)
                return ERR_INVAL;
        int port = fs_port();
        if (port < 0)
                return port;

        vfs_req_move_t *req = (vfs_req_move_t *)s_req;
        memset(req, 0, sizeof(*req));
        req->op = VFS_OP_MOVE;
        strncpy(req->src, src, sizeof(req->src) - 1);
        strncpy(req->dst_dir, dst_dir, sizeof(req->dst_dir) - 1);
        if (new_name)
                strncpy(req->new_name, new_name, sizeof(req->new_name) - 1);

        vfs_resp_move_t *resp = (vfs_resp_move_t *)s_resp;
        int resp_len = (int)sizeof(*resp);
        int r = ipc_call(port, req, (int)sizeof(*req), resp, &resp_len);
        if (r < 0)
                return r;
        if (resp->ret < 0)
                return resp->ret;
        if (out_item)
                *out_item = resp->item;
        return 0;
}
