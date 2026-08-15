/*
 * pkg_manager.c - pkg-manager service (docs/ops_format.md §5/§7)
 * Copyright (c) 2026 OpSys Project
 *
 * pkg-manager owns application installation and sandbox capability
 * issuance.  Apps declare permissions in a .ops manifest; pkg-manager
 * (kernel-endorsed via ATOM_SERVICE_MANAGE seed) signs atom capabilities
 * into the app's kernel cap table on APP_READY.
 *
 * Ops (user/services/pkg/pkg.h):
 *   INSTALL    1   shell → pkg    install a kernel blob as a .ops app.
 *   LIST       2   shell → pkg    enumerate installed apps.
 *   RUN        3   shell → pkg    spawn an app, return pid.
 *   REMOVE     4   shell → pkg    delete an installed app (recursive).
 *   APP_READY  5   app   → pkg    handshake; pkg signs manifest atoms
 *                                 into the caller's kernel cap table.
 *
 * Security: the app's permission identity is the kernel-issued subject
 * (ipc_recv_from, unforgeable).  APP_READY resolves the real subject via
 * proc_info_by_subject() and cross-checks pid + name against the pending
 * RUN record — a self-claimed name is never trusted.
 */

#include <stdint.h>
#include "../lib/libc/stdio.h"
#include "../lib/libc/string.h"
#include "../lib/libos/syscalls.h"
#include "../lib/libfs/fs.h"
#include "pkg.h"

/* ====================================================================
 * Constants (docs/ops_format.md §2/§3)
 * ==================================================================== */

#define PKG_APPS_DIR      "/Volumes/Users/Apps"   /* installed-app root */
#define OPS_MAGIC         0x3153504Fu             /* "OPS1" LE         */
#define OPS_VERSION       1u
#define PKG_OPS_BUF_MAX   (16 + PKG_MANIFEST_MAX + 128 * 1024)
                                                /* .ops file buffer   */

/* Request/response buffers (all pkg messages < 4096) */
static u8 s_req[VFS_IPC_MAX];
static u8 s_resp[VFS_IPC_MAX];

/* .ops assembly/parse buffer (16-byte header + manifest + ELF payload) */
static u8  s_ops[PKG_OPS_BUF_MAX];

/* URL scratch (any VFS path) */
static char s_url[VFS_PATH_MAX];

/* Enumerator batch buffer (~16.5 KB — must stay static) */
static vfs_enum_batch_t s_enum;

/* ====================================================================
 * Pending RUN records — pid → manifest atoms, consumed on APP_READY
 * ==================================================================== */

typedef struct {
        int        in_use;
        int        pid;                     /* spawned process */
        char       app_id[PKG_NAME_MAX];    /* manifest app_id  */
        atom_id_t  atoms[PKG_MAX_ATOMS];    /* manifest atoms   */
        int        atom_count;
} pkg_pending_t;

static pkg_pending_t s_pending[PKG_PENDING_MAX];

/* ====================================================================
 * Permission atom name table (docs/ops_format.md §4, closed set)
 *
 * The management atoms (service.manage, cap.grant_self, sys.debug) are
 * intentionally NOT in this table: any manifest mentioning them fails
 * pkg_perm_lookup() → ERR_INVAL (install rejected).  Apps can never
 * declare management-plane atoms.
 * ==================================================================== */

typedef struct {
        const char *name;
        atom_id_t   atom;
} pkg_atom_entry_t;

static const pkg_atom_entry_t s_atom_table[] = {
        { "sys.set_time",             ATOM_SYS_SET_TIME },
        { "sys.set_timezone",         ATOM_SYS_SET_TIMEZONE },
        { "sys.shutdown",             ATOM_SYS_SHUTDOWN },
        { "hw.camera.capture",        ATOM_HW_CAMERA_CAPTURE },
        { "hw.mic.record",            ATOM_HW_MIC_RECORD },
        { "hw.gpu.high_perf",         ATOM_HW_GPU_HIGH_PERF },
        { "hw.loc.coarse",            ATOM_HW_LOC_COARSE },
        { "hw.loc.precise",           ATOM_HW_LOC_PRECISE },
        { "data.docs.read",           ATOM_DATA_DOCS_READ },
        { "data.docs.write",          ATOM_DATA_DOCS_WRITE },
        { "data.dl.write",            ATOM_DATA_DL_WRITE },
        { "data.app.container.read",  ATOM_DATA_APP_CONTAINER_READ },
        { "data.sys.logs.read",       ATOM_DATA_SYS_LOGS_READ },
        { "bookmark.resolve",         ATOM_BOOKMARK_RESOLVE },
        { "net.bind",                 ATOM_NET_BIND },
        { "net.connect",              ATOM_NET_CONNECT },
        { "net.wifi.scan",            ATOM_NET_WIFI_SCAN },
        { "net.wifi.set",             ATOM_NET_WIFI_SET },
        { "pkg.install",              ATOM_PKG_INSTALL },
};

/* ====================================================================
 * .ops v1 little-endian helpers (docs/ops_format.md §2)
 * ==================================================================== */

static u32 pkg_rd32(const u8 *p)
{
        return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16)
               | ((u32)p[3] << 24);
}

static void pkg_wr32(u8 *p, u32 v)
{
        p[0] = (u8)(v & 0xFFu);
        p[1] = (u8)((v >> 8) & 0xFFu);
        p[2] = (u8)((v >> 16) & 0xFFu);
        p[3] = (u8)((v >> 24) & 0xFFu);
}

/* ====================================================================
 * app_id validation — [a-zA-Z0-9_]{1,63} (docs/ops_format.md §3).
 * Doubles as path-safety: only this charset may reach a VFS URL, so no
 * "/../" traversal can be smuggled through app_id.
 * ==================================================================== */

static int pkg_app_id_valid(const char *id)
{
        size_t n;

        if (id == NULL || id[0] == '\0')
                return 0;
        for (n = 0; n < PKG_NAME_MAX - 1 && id[n] != '\0'; n++) {
                char c = id[n];
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                      || (c >= '0' && c <= '9') || c == '_'))
                        return 0;
        }
        return id[n] == '\0';   /* fully consumed (rejects >63 chars) */
}

/* ====================================================================
 * Manifest text parsing (docs/ops_format.md §3)
 *
 * Lines are "key=value"; '#' starts a comment; blank lines are ignored.
 * Unknown keys are rejected (strict).  app_id is mandatory.
 * ==================================================================== */

typedef struct {
        char       app_id[PKG_NAME_MAX];
        atom_id_t  atoms[PKG_MAX_ATOMS];
        int        atom_count;
} pkg_manifest_t;

/* Resolve one atom name against the closed table. */
static int pkg_perm_lookup(const char *name, atom_id_t *out_atom)
{
        for (size_t i = 0; i < sizeof(s_atom_table) / sizeof(s_atom_table[0]);
             i++) {
                if (strcmp(name, s_atom_table[i].name) == 0) {
                        *out_atom = s_atom_table[i].atom;
                        return 0;
                }
        }
        return ERR_INVAL;       /* unknown or forbidden atom */
}

/* Parse a comma-separated permission list; "" = no permissions. */
static int pkg_perms_parse(const char *perms, pkg_manifest_t *m)
{
        const char *p = perms;
        int count = 0;

        while (*p != '\0') {
                char name[PKG_NAME_MAX];
                const char *start;
                size_t nlen;

                while (*p == ',' || *p == ' ' || *p == '\t')
                        p++;
                if (*p == '\0')
                        break;
                start = p;
                while (*p != '\0' && *p != ',')
                        p++;
                nlen = (size_t)(p - start);
                if (nlen == 0 || nlen >= sizeof(name))
                        return ERR_INVAL;
                memcpy(name, start, nlen);
                name[nlen] = '\0';
                if (count >= PKG_MAX_ATOMS)
                        return ERR_INVAL;       /* too many atoms */
                if (pkg_perm_lookup(name, &m->atoms[count]) < 0)
                        return ERR_INVAL;       /* unknown/forbidden */
                count++;
        }
        m->atom_count = count;
        return 0;
}

static int pkg_manifest_parse(const char *text, u32 len, pkg_manifest_t *m)
{
        char buf[PKG_MANIFEST_MAX + 1];
        char *line;
        int have_app_id = 0;

        memset(m, 0, sizeof(*m));
        if (len == 0 || len > PKG_MANIFEST_MAX)
                return ERR_INVAL;
        memcpy(buf, text, len);
        buf[len] = '\0';

        line = buf;
        while (line != NULL) {
                char *nl = strchr(line, '\n');
                char *p = line;

                if (nl != NULL)
                        *nl = '\0';
                while (*p == ' ' || *p == '\t')
                        p++;
                if (*p != '\0' && *p != '#') {
                        char *eq = strchr(p, '=');
                        char *key;
                        char *val;

                        if (eq == NULL)
                                return ERR_INVAL;   /* malformed line */
                        *eq = '\0';
                        key = p;
                        val = eq + 1;
                        while (*val == ' ' || *val == '\t')
                                val++;

                        if (strcmp(key, "app_id") == 0) {
                                if (!pkg_app_id_valid(val))
                                        return ERR_INVAL;
                                strncpy(m->app_id, val, sizeof(m->app_id) - 1);
                                m->app_id[sizeof(m->app_id) - 1] = '\0';
                                have_app_id = 1;
                        } else if (strcmp(key, "permissions") == 0) {
                                if (pkg_perms_parse(val, m) < 0)
                                        return ERR_INVAL;
                        } else if (strcmp(key, "app_name") == 0
                                   || strcmp(key, "version") == 0
                                   || strcmp(key, "entry") == 0) {
                                /* informational; ignored */
                        } else {
                                return ERR_INVAL;   /* unknown key */
                        }
                }
                line = (nl != NULL) ? nl + 1 : NULL;
        }
        if (!have_app_id)
                return ERR_INVAL;
        return 0;
}

/* ====================================================================
 * .ops binary parsing (docs/ops_format.md §2)
 * ==================================================================== */

static int pkg_ops_parse(const u8 *ops, u32 ops_len, pkg_manifest_t *m,
                         const u8 **payload, u32 *payload_len)
{
        u32 magic, ver, mlen, plen;

        if (ops_len < 16)
                return ERR_INVAL;
        magic = pkg_rd32(ops + 0);
        ver   = pkg_rd32(ops + 4);
        mlen  = pkg_rd32(ops + 8);
        plen  = pkg_rd32(ops + 12);
        if (magic != OPS_MAGIC || ver != OPS_VERSION)
                return ERR_INVAL;
        if (mlen == 0 || mlen > PKG_MANIFEST_MAX)
                return ERR_INVAL;
        if (plen == 0)
                return ERR_INVAL;
        if (16u + mlen + plen != ops_len)
                return ERR_INVAL;       /* exact size match required */

        if (pkg_manifest_parse((const char *)(ops + 16), mlen, m) < 0)
                return ERR_INVAL;
        *payload = ops + 16 + mlen;
        *payload_len = plen;
        return 0;
}

/* Read an entire open file into buf (chunked; fs_read handles 4032 cap). */
static int pkg_read_all(vfs_handle_t h, u8 *buf, u32 cap, u32 *out_len)
{
        u32 total = 0;

        for (;;) {
                u32 want, got = 0;
                int r;

                if (total >= cap)
                        return ERR_OVERFLOW;    /* file exceeds buffer */
                want = cap - total;
                r = fs_read(h, total, buf + total, want, &got);
                if (r < 0)
                        return r;
                total += got;
                if (got < want)
                        break;                  /* EOF */
        }
        *out_len = total;
        return 0;
}

/* ====================================================================
 * INSTALL — fetch a kernel blob, pack .ops, persist it.
 * ==================================================================== */

static int do_install(const pkg_req_install_t *req)
{
        char name[PKG_NAME_MAX];
        char perms[PKG_PERMS_MAX];
        char manifest[PKG_MANIFEST_MAX];
        pkg_manifest_t m;
        vfs_handle_t h = 0;
        u32 mlen, payload_len, ops_len;
        int r, n;

        memcpy(name, req->name, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        memcpy(perms, req->perms, sizeof(perms) - 1);
        perms[sizeof(perms) - 1] = '\0';

        if (!pkg_app_id_valid(name))
                return ERR_INVAL;

        /* Validate permission atoms BEFORE any side effect. */
        if (pkg_perms_parse(perms, &m) < 0)
                return ERR_INVAL;

        /* Build the manifest text (app_id mandatory; permissions only
         * when non-empty — empty/missing means no permissions). */
        n = snprintf(manifest, sizeof(manifest), "app_id=%s\n", name);
        if (n < 0 || n >= (int)sizeof(manifest))
                return ERR_INVAL;
        mlen = (u32)n;
        if (perms[0] != '\0') {
                n = snprintf(manifest + mlen, sizeof(manifest) - mlen,
                             "permissions=%s\n", perms);
                if (n < 0 || (u32)n >= sizeof(manifest) - mlen)
                        return ERR_INVAL;
                mlen += (u32)n;
        }
        if (mlen > PKG_MANIFEST_MAX)
                return ERR_INVAL;

        /* Pack the .ops header + manifest, then fetch the blob payload. */
        pkg_wr32(s_ops + 0, OPS_MAGIC);
        pkg_wr32(s_ops + 4, OPS_VERSION);
        pkg_wr32(s_ops + 8, mlen);
        memcpy(s_ops + 16, manifest, mlen);
        r = blob_get(name, s_ops + 16 + mlen,
                     (int)(sizeof(s_ops) - 16 - mlen));
        if (r <= 0)
                return (r < 0) ? r : ERR_INVAL;
        payload_len = (u32)r;
        pkg_wr32(s_ops + 12, payload_len);
        ops_len = 16 + mlen + payload_len;

        /* Persist: /Volumes/Users/Apps/<name>/app.ops (Users volume RW).
         * The first write may trigger the Powerbox prompt — VFS_ERR_ACCESS
         * from a user "no" is expected and propagated, never bypassed. */
        r = fs_create_dir(PKG_APPS_DIR);
        if (r < 0 && r != VFS_ERR_EXISTS)
                return r;
        n = snprintf(s_url, sizeof(s_url), "%s/%s", PKG_APPS_DIR, name);
        if (n < 0 || n >= (int)sizeof(s_url))
                return ERR_INVAL;
        r = fs_create_dir(s_url);
        if (r < 0 && r != VFS_ERR_EXISTS)
                return r;
        n = snprintf(s_url, sizeof(s_url), "%s/%s/app.ops",
                     PKG_APPS_DIR, name);
        if (n < 0 || n >= (int)sizeof(s_url))
                return ERR_INVAL;
        r = fs_open_item(s_url, VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE,
                         VFS_ACCESS_WRITE, &h);
        if (r < 0)
                return r;
        r = fs_write(h, 0, s_ops, ops_len);
        (void)fs_close(h);
        if (r < 0)
                return r;
        printf("pkg: installed '%s' (%d atom(s), %u bytes)\n",
               name, m.atom_count, ops_len);
        return 0;
}

/* ====================================================================
 * LIST — enumerate installed apps under /Volumes/Users/Apps/.
 * ==================================================================== */

static int do_list(pkg_resp_list_t *resp)
{
        vfs_handle_t eh = 0;
        int r;

        resp->count = 0;
        r = fs_enum_begin(PKG_APPS_DIR, &eh);
        if (r < 0)
                return (r == ERR_NOENT) ? 0 : r;   /* no apps yet = empty */

        for (;;) {
                r = fs_enum_next(eh, &s_enum);
                if (r <= 0)
                        break;
                for (u32 i = 0; i < s_enum.batch_count
                     && resp->count < PKG_MAX_APPS; i++) {
                        strncpy(resp->apps[resp->count], s_enum.batch[i],
                                PKG_NAME_MAX - 1);
                        resp->apps[resp->count][PKG_NAME_MAX - 1] = '\0';
                        resp->count++;
                }
        }
        (void)fs_enum_end(eh);
        return (r < 0) ? r : 0;
}

/* ====================================================================
 * RUN — read the .ops back, parse manifest, record pending, spawn.
 * ==================================================================== */

static int do_run(const pkg_req_run_t *req, pkg_resp_run_t *resp)
{
        char app_id[PKG_NAME_MAX];
        pkg_manifest_t m;
        const u8 *payload = NULL;
        u32 payload_len = 0, ops_len = 0;
        vfs_handle_t h = 0;
        pkg_pending_t *slot = NULL;
        int r, n;

        memcpy(app_id, req->app_id, sizeof(app_id) - 1);
        app_id[sizeof(app_id) - 1] = '\0';
        if (!pkg_app_id_valid(app_id))
                return ERR_INVAL;

        n = snprintf(s_url, sizeof(s_url), "%s/%s/app.ops",
                     PKG_APPS_DIR, app_id);
        if (n < 0 || n >= (int)sizeof(s_url))
                return ERR_INVAL;
        r = fs_open_item(s_url, VFS_OPEN_READONLY, VFS_ACCESS_READ, &h);
        if (r < 0)
                return r;
        r = pkg_read_all(h, s_ops, (u32)sizeof(s_ops), &ops_len);
        (void)fs_close(h);
        if (r < 0)
                return r;

        r = pkg_ops_parse(s_ops, ops_len, &m, &payload, &payload_len);
        if (r < 0)
                return r;
        /* The manifest app_id must match the requested app dir. */
        if (strcmp(m.app_id, app_id) != 0)
                return ERR_INVAL;

        /* Reserve a pending slot BEFORE spawning (cleared on failure). */
        for (int i = 0; i < PKG_PENDING_MAX; i++) {
                if (!s_pending[i].in_use) {
                        slot = &s_pending[i];
                        break;
                }
        }
        if (slot == NULL)
                return ERR_BUSY;        /* handshake table full */
        slot->in_use = 1;
        slot->pid = 0;
        strncpy(slot->app_id, m.app_id, sizeof(slot->app_id) - 1);
        slot->app_id[sizeof(slot->app_id) - 1] = '\0';
        slot->atom_count = m.atom_count;
        for (int i = 0; i < m.atom_count; i++)
                slot->atoms[i] = m.atoms[i];

        r = process_create(m.app_id, payload, (unsigned long)payload_len);
        if (r < 0) {
                slot->in_use = 0;
                return r;
        }
        slot->pid = r;
        resp->ret = 0;
        resp->pid = r;
        printf("pkg: run '%s' pid %d (%d atom(s) pending)\n",
               m.app_id, r, m.atom_count);
        return 0;
}

/* ====================================================================
 * REMOVE — recursive delete of an installed app directory.
 * ==================================================================== */

static int do_remove(const pkg_req_remove_t *req)
{
        char app_id[PKG_NAME_MAX];
        int n;

        memcpy(app_id, req->app_id, sizeof(app_id) - 1);
        app_id[sizeof(app_id) - 1] = '\0';
        if (!pkg_app_id_valid(app_id))
                return ERR_INVAL;
        n = snprintf(s_url, sizeof(s_url), "%s/%s", PKG_APPS_DIR, app_id);
        if (n < 0 || n >= (int)sizeof(s_url))
                return ERR_INVAL;
        return fs_delete_item(s_url, 1);
}

/* ====================================================================
 * APP_READY — sandbox handshake.  The caller's real subject comes from
 * ipc_recv_from (kernel-filled, unforgeable); pid + name come from
 * proc_info_by_subject().  Only on a match with the pending RUN record
 * are the manifest atoms signed into the caller's cap table.
 * ==================================================================== */

static int do_app_ready(const pkg_req_app_ready_t *req, u64 subject)
{
        proc_ident_t ident;
        pkg_pending_t *slot = NULL;
        char app_id[PKG_NAME_MAX];
        int r;

        memcpy(app_id, req->app_id, sizeof(app_id) - 1);
        app_id[sizeof(app_id) - 1] = '\0';

        r = proc_info_by_subject(subject, &ident);
        if (r < 0)
                return ERR_NOENT;
        /* Self-claimed name must equal the kernel-issued name. */
        if (strcmp(app_id, ident.name) != 0)
                return ERR_NOENT;

        for (int i = 0; i < PKG_PENDING_MAX; i++) {
                if (s_pending[i].in_use && s_pending[i].pid == ident.pid
                    && strcmp(s_pending[i].app_id, app_id) == 0) {
                        slot = &s_pending[i];
                        break;
                }
        }
        if (slot == NULL)
                return ERR_NOENT;       /* no pending RUN record */

        /* Sign each manifest atom into the caller's kernel cap table. */
        for (int i = 0; i < slot->atom_count; i++) {
                r = cap_grant_to_subject(subject, slot->atoms[i],
                                         RIGHT_ALL, 0, 0);
                if (r < 0) {
                        slot->in_use = 0;
                        return r;
                }
        }
        printf("pkg: signed '%s' pid %d with %d atom(s)\n",
               ident.name, ident.pid, slot->atom_count);
        slot->in_use = 0;
        return 0;
}

/* ====================================================================
 * Service entry
 * ==================================================================== */

int main(void)
{
        int port, ret;

        printf("pkg: starting\n");
        port = ipc_port_create();
        if (port < 0) {
                printf("pkg: ipc_port_create failed (%d)\n", port);
                thread_exit(1);
        }
        ret = port_register(PKG_PORT_NAME, port);
        if (ret < 0) {
                printf("pkg: port_register('%s') failed (%d)\n",
                       PKG_PORT_NAME, port);
                thread_exit(1);
        }
        printf("pkg: port %d registered as '%s'\n", port, PKG_PORT_NAME);

        for (;;) {
                int msg_len = (int)sizeof(s_req);
                int token = 0;
                u64 caller_subject = 0;
                u32 op;

                ret = ipc_recv_from(port, s_req, &msg_len, &token,
                                    &caller_subject);
                if (ret < 0) {
                        printf("pkg: ipc_recv failed (%d)\n", ret);
                        continue;
                }
                if (msg_len < (int)sizeof(u32)) {   /* no op code */
                        i32 *resp = (i32 *)s_resp;
                        *resp = ERR_INVAL;
                        (void)ipc_reply(token, resp, (int)sizeof(i32));
                        continue;
                }
                op = ((pkg_req_install_t *)s_req)->op;

                switch (op) {
                case PKG_OP_INSTALL:
                        if (msg_len >= (int)sizeof(pkg_req_install_t)) {
                                pkg_resp_install_t *resp =
                                        (pkg_resp_install_t *)s_resp;
                                resp->ret = do_install(
                                        (const pkg_req_install_t *)s_req);
                                (void)ipc_reply(token, resp, (int)sizeof(*resp));
                        } else {
                                i32 *resp = (i32 *)s_resp;
                                *resp = ERR_INVAL;
                                (void)ipc_reply(token, resp, (int)sizeof(i32));
                        }
                        break;
                case PKG_OP_LIST:
                        {
                                pkg_resp_list_t *resp =
                                        (pkg_resp_list_t *)s_resp;
                                memset(resp, 0, sizeof(*resp));
                                resp->ret = do_list(resp);
                                (void)ipc_reply(token, resp, (int)sizeof(*resp));
                        }
                        break;
                case PKG_OP_RUN:
                        if (msg_len >= (int)sizeof(pkg_req_run_t)) {
                                pkg_resp_run_t *resp =
                                        (pkg_resp_run_t *)s_resp;
                                memset(resp, 0, sizeof(*resp));
                                resp->ret = do_run(
                                        (const pkg_req_run_t *)s_req, resp);
                                (void)ipc_reply(token, resp, (int)sizeof(*resp));
                        } else {
                                i32 *resp = (i32 *)s_resp;
                                *resp = ERR_INVAL;
                                (void)ipc_reply(token, resp, (int)sizeof(i32));
                        }
                        break;
                case PKG_OP_REMOVE:
                        if (msg_len >= (int)sizeof(pkg_req_remove_t)) {
                                pkg_resp_remove_t *resp =
                                        (pkg_resp_remove_t *)s_resp;
                                resp->ret = do_remove(
                                        (const pkg_req_remove_t *)s_req);
                                (void)ipc_reply(token, resp, (int)sizeof(*resp));
                        } else {
                                i32 *resp = (i32 *)s_resp;
                                *resp = ERR_INVAL;
                                (void)ipc_reply(token, resp, (int)sizeof(i32));
                        }
                        break;
                case PKG_OP_APP_READY:
                        if (msg_len >= (int)sizeof(pkg_req_app_ready_t)) {
                                pkg_resp_app_ready_t *resp =
                                        (pkg_resp_app_ready_t *)s_resp;
                                resp->ret = do_app_ready(
                                        (const pkg_req_app_ready_t *)s_req,
                                        caller_subject);
                                (void)ipc_reply(token, resp, (int)sizeof(*resp));
                        } else {
                                i32 *resp = (i32 *)s_resp;
                                *resp = ERR_INVAL;
                                (void)ipc_reply(token, resp, (int)sizeof(i32));
                        }
                        break;
                default:
                        {
                                i32 *resp = (i32 *)s_resp;
                                *resp = ERR_INVAL;
                                (void)ipc_reply(token, resp, (int)sizeof(i32));
                        }
                        break;
                }
        }
}
