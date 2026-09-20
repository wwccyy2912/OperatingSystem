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
 * cmd_perm.c - permission-model inspection & administration commands
 * Copyright (c) 2026 OpSys Project
 *
 * The management face of the perm engine (user/services/perm/perm.h):
 *
 *   perm audit [n] [allow|deny] [subject=<id>] [since=<tick>]
 *   perm audit save [url]                 export the audit ring as text
 *   perm ctx [list]                       foreground/background table
 *   perm ctx <subject|pid> fg|bg          bind a subject's context
 *   perm freq [subject|all]               counters + quarantine state
 *   perm freq release <subject|all>       lift a quarantine
 *   perm save [url] [all]                 write a policy snapshot
 *   perm load [url]                       restore a policy snapshot
 *   permguard [list|<subject|pid> [release]]   quarantine entry point
 *
 * The shell holds ATOM_SERVICE_MANAGE and IS the management plane, so it
 * talks to the "perm" port directly.  A *state-changing* verb is still
 * gated on the human at the console: the "user" service is asked
 * USER_OP_WHOAMI and only an OWNER/ADMIN account may proceed — the same
 * rule the disk / svc admin proxies follow.  Read-only verbs (audit,
 * ctx list, freq query) run for any caller.
 *
 * ------------------------------------------------------------------
 * Structure (one entry point per verb over a shared perm client):
 *   PermCmdAudit/PermCmdCtx/PermCmdFreq/PermCmdSave/PermCmdLoad
 *        -> PermDo() -> IpcCall("perm")
 *   CmdPermGuard()      the registered "permguard" quarantine entry
 *   PermRequireAdmin()  -> IpcCall("user", USER_OP_WHOAMI)
 *   snapshots / audit dumps -> libfs (FsOpenItem / FsWrite / FsRead)
 *   ShellRegisterPermCommands() registers permguard + the perm_* aliases
 * How it works:
 *   Each verb fills one flat perm_req_* from the frozen perm.h ABI, does
 *   one IpcCall("perm") and formats the matching perm_resp_*.  The admin
 *   gate runs before the request leaves the shell, so a refused verb
 *   never reaches the engine.  "perm audit save" and "perm save" stream
 *   their payload to a VFS URL through libfs, which keeps the shell out
 *   of every driver.
 * Purpose:
 *   Make the permission engine observable and recoverable from the
 *   console: see why a decision came out the way it did, who is
 *   quarantined, release them, and carry policy between machines as an
 *   ordinary file.
 * Caveats:
 *   Subject ids and PIDs are unrelated namespaces and the kernel maps
 *   subject -> process only (ProcInfoBySubject), so a PID argument is
 *   resolved through the perm context table and refused with an
 *   explanation when that fails — never guessed.  The verbs accept both
 *   the "perm <verb> ..." shape (argv[0] = verb, the way the shell's
 *   dispatcher calls them) and the untouched line (argv[0] = "perm").
 * ------------------------------------------------------------------
 */

#include "shell.h"

#include <stdarg.h>

#include "../lib/libc/stdio.h"
#include "../lib/libc/stdlib.h"
#include "../lib/libc/string.h"
#include "../lib/libfs/fs.h"
#include "../lib/libos/syscalls.h"
#include "../perm/perm.h"
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
 * Defaults
 * ==================================================================== */

/* Where "perm save" / "perm load" keep the policy snapshot, and where
 * "perm audit save" keeps the exported ring.  Both are plain VFS files:
 * a snapshot is something a user can copy, mail or diff, not hidden
 * kernel state (docs/vfs_design.md §8). */
#define PERM_POLICY_URL_DEFAULT "/Volumes/Disk/perm.policy"
#define PERM_AUDIT_URL_DEFAULT  "/Volumes/Disk/perm.audit"

/* Smallest possible policy snapshot: the perm.h header (magic, version,
 * grant_count, role_count) is 16 bytes, so anything shorter cannot even
 * carry a version stamp. */
#define PERM_POLICY_HDR 16

/* ProcessList() depth for the pid -> process step of subject resolution
 * (static: a user thread only has 16 KiB of stack). */
#define PERM_PROC_SCAN 32

/* ====================================================================
 * perm / user plumbing
 * ==================================================================== */

/* The "perm" port, resolved once and cached.  A negative result is
 * retried on the next call, so a verb typed before perm-manager is up
 * fails once instead of breaking the command forever. */
static int PermPort(void) {
    static int s_port = -1;
    if (s_port < 0)
        s_port = PortGet(PERM_PORT_NAME);
    return s_port;
}

/* One request/reply against the perm engine.  Returns 0 when a reply
 * arrived (the caller still inspects resp->ret: an engine-level refusal
 * is a legitimate answer), or the negative port/IPC error. */
static int PermDo(const void *req, int req_len, void *resp, int resp_len) {
    int port = PermPort();
    if (port < 0)
        return port;
    int rl = resp_len;
    return IpcCall(port, req, req_len, resp, &rl);
}

static int UserPort(void) {
    static int s_port = -1;
    if (s_port < 0)
        s_port = PortGet(USER_PORT_NAME);
    return s_port;
}

static const char *PermRoleName(u32 role) {
    switch (role) {
    case PERM_ROLE_OWNER: return "OWNER";
    case PERM_ROLE_ADMIN: return "ADMIN";
    case PERM_ROLE_STANDARD: return "STANDARD";
    case PERM_ROLE_CHILD: return "CHILD";
    case PERM_ROLE_GUEST: return "GUEST";
    case PERM_ROLE_AUDITOR: return "AUDITOR";
    default: return "?";
    }
}

/* Role gate for the state-changing verbs.
 *
 * perm-manager trusts the management plane, so the interesting question
 * is not "who sent this?" but "may the human at this console do it?".
 * The user service is the only component that knows: it authenticated
 * the login and bound the shell's kernel subject to an account
 * (USER_OP_WHOAMI — never a self-reported name).  Returns 0 when the
 * bound account is OWNER or ADMIN, -1 (after printing why) otherwise. */
static int PermRequireAdmin(const char *verb) {
    int port = UserPort();
    if (port < 0) {
        PrintfLine("%s: cannot verify the caller - 'user' unavailable (%d)\n", verb, port);
        return -1;
    }

    user_req_login_t req;
    memset(&req, 0, sizeof(req));
    req.op = USER_OP_WHOAMI;

    user_resp_login_t resp;
    memset(&resp, 0, sizeof(resp));
    int rl = (int)sizeof(resp);
    if (IpcCall(port, &req, (int)sizeof(req), &resp, &rl) < 0) {
        PrintfLine("%s: cannot verify the caller - user IPC FAILED\n", verb);
        return -1;
    }
    if (resp.ret < 0) {
        PrintfLine("%s: permission denied - log in as OWNER/ADMIN first\n", verb);
        return -1;
    }
    if (resp.role != PERM_ROLE_OWNER && resp.role != PERM_ROLE_ADMIN) {
        PrintfLine("%s: permission denied - '%s' is %s, OWNER/ADMIN required\n",
                   verb,
                   resp.name,
                   PermRoleName(resp.role));
        return -1;
    }
    return 0;
}

/* Parse a strict decimal u64: "0x10" and "-1" are rejected so that a
 * typo can never silently become subject 0 (which means "every subject"
 * to several ops). */
static int PermParseU64(const char *s, u64 *out) {
    if (!s || !*s)
        return -1;
    for (const char *p = s; *p; p++)
        if (*p < '0' || *p > '9')
            return -1;
    *out = (u64)strtoull(s, NULL, 10);
    return 0;
}

/* Reverse-map a PID to its kernel subject.
 *
 * The kernel hands out two unrelated numbers — the subject_id (identity,
 * what every permission decision uses) and the PID (the scheduler's
 * handle) — and only exposes subject -> process (ProcInfoBySubject).
 * The reverse lookup therefore starts from a set of KNOWN subjects,
 * which is exactly what the perm context table holds, and confirms each
 * candidate through the kernel identity record.  Returns 0 on a hit. */
static int PermSubjectOfPid(u64 pid, u64 *out_subject) {
    perm_req_ctx_query_t req;
    memset(&req, 0, sizeof(req));
    req.op = PERM_OP_CTX_QUERY;

    static perm_resp_context_t resp;
    memset(&resp, 0, sizeof(resp));
    int r = PermDo(&req, (int)sizeof(req), &resp, (int)sizeof(resp));
    if (r < 0 || resp.ret < 0)
        return -1;

    u32 n = resp.count;
    if (n > PERM_CTX_LIST_MAX)
        n = PERM_CTX_LIST_MAX;
    for (u32 i = 0; i < n; i++) {
        proc_ident_t ident;
        if (resp.entries[i].subject_id == 0)
            continue;
        if (ProcInfoBySubject(resp.entries[i].subject_id, &ident) == 0 && (u64)ident.pid == pid) {
            *out_subject = resp.entries[i].subject_id;
            return 0;
        }
    }
    return -1;
}

/* Resolve a "<subject|pid>" argument to a subject id.
 *
 *   1. a number that names a LIVE subject wins — the unambiguous,
 *      documented form;
 *   2. otherwise, if a process with that PID is alive, name it through
 *      ProcessList() and try the reverse lookup above;
 *   3. otherwise refuse with an explanation.  Guessing is not on the
 *      table: a wrong subject silently rewrites somebody else's policy.
 * Subject 0 (System/kernel) never resolves — callers that accept "all
 * subjects" test for it themselves. */
static int PermResolveSubject(const char *arg, u64 *out_subject) {
    u64 n = 0;
    if (PermParseU64(arg, &n) < 0) {
        PrintfLine("perm: '%s' is not a decimal subject id\n", arg);
        return -1;
    }

    proc_ident_t ident;
    if (n != 0 && ProcInfoBySubject(n, &ident) == 0) {
        *out_subject = n;
        return 0;
    }

    static proc_info_t procs[PERM_PROC_SCAN];
    int                count = ProcessList(procs, PERM_PROC_SCAN);
    const char        *pname = NULL;
    for (int i = 0; i < count; i++)
        if ((u64)procs[i].pid == n)
            pname = procs[i].name;

    if (!pname) {
        PrintfLine("perm: %llu is neither a live subject nor a live PID\n", (unsigned long long)n);
        return -1;
    }

    u64 subject = 0;
    if (PermSubjectOfPid(n, &subject) == 0) {
        PrintfLine("perm: PID %llu is '%s' -> subject %llu\n",
                   (unsigned long long)n,
                   pname,
                   (unsigned long long)subject);
        *out_subject = subject;
        return 0;
    }

    PrintfLine("perm: %llu is PID '%s', not a subject id; the kernel exposes\n",
               (unsigned long long)n,
               pname);
    ShellWrite("      no pid->subject lookup - run 'perm ctx list' and pass the id.\n");
    return -1;
}

/* Audit event names (perm.h PERM_EV_*). */
static const char *PermEventName(u32 event) {
    switch (event) {
    case PERM_EV_CHECK_ALLOW: return "CHECK_ALLOW";
    case PERM_EV_CHECK_DENY: return "CHECK_DENY";
    case PERM_EV_POWERBOX: return "POWERBOX";
    case PERM_EV_ANSWER: return "ANSWER";
    case PERM_EV_GRANT: return "GRANT";
    case PERM_EV_REVOKE: return "REVOKE";
    case PERM_EV_ROLE_SET: return "ROLE_SET";
    case PERM_EV_CONTEXT: return "CONTEXT";
    case PERM_EV_QUARANTINE: return "QUARANTINE";
    case PERM_EV_POLICY_LOAD: return "POLICY_LOAD";
    case PERM_EV_POLICY_SAVE: return "POLICY_SAVE";
    case PERM_EV_EXPIRE: return "EXPIRE";
    default: return "?";
    }
}

/* perm.h: verdict 0 = granted, 1 = denied. */
static const char *PermVerdictName(u32 verdict) {
    return verdict == 0 ? "granted" : "denied";
}

/* Render a vfs_resource_t as "uuid/item" for table output.  The volume
 * UUID is printed in full because an itemID alone repeats across
 * volumes — the pair is what makes a resource global (vfs_design §3.1). */
static void PermResourceStr(const vfs_resource_t *res, char *out, size_t cap) {
    if (res->vol.hi == 0 && res->vol.lo == 0 && res->id == 0) {
        snprintf(out, cap, "-");
        return;
    }
    snprintf(out,
             cap,
             "%08x%08x%08x%08x/%llu",
             (unsigned)(res->vol.hi >> 32),
             (unsigned)(res->vol.hi & 0xFFFFFFFFu),
             (unsigned)(res->vol.lo >> 32),
             (unsigned)(res->vol.lo & 0xFFFFFFFFu),
             (unsigned long long)res->id);
}

/* The shell's "perm" dispatcher hands each verb an argv whose argv[0] is
 * the verb itself (CmdPermAnswer(argc - 1, argv + 1) style), while the
 * perm_* aliases registered below arrive with argv[0] = "perm_audit".
 * Both mean "the real arguments start at argv[1]"; a caller that passes
 * the untouched line ("perm audit ...") is accepted too by skipping a
 * leading token that repeats the verb. */
static int PermArgsFrom(int argc, char *argv[], const char *verb) {
    if (argc > 1 && strcmp(argv[1], verb) == 0)
        return 2;
    return 1;
}

/* ====================================================================
 * Shared by the verbs: PERM_OP_FREQ round trip
 * ==================================================================== */

/* Read (clear=0) or clear (clear=1) the counters of one subject;
 * subject 0 means "every subject" (perm.h).  Returns 0 with *out filled,
 * or the negative error, so the caller can print it, skip a row or
 * abort. */
static int PermFreqQuery(u64 subject, u32 clear, perm_resp_freq_t *out) {
    perm_req_freq_t req;
    memset(&req, 0, sizeof(req));
    req.op               = PERM_OP_FREQ;
    req.subject_id       = subject;
    req.clear_quarantine = clear;

    memset(out, 0, sizeof(*out));
    int r = PermDo(&req, (int)sizeof(req), out, (int)sizeof(*out));
    if (r < 0)
        return r;
    return out->ret;
}

/* ====================================================================
 * perm audit — the decision trail
 *
 * perm-manager appends one record per decision and per state change to a
 * ring (PERM_AUDIT_MAX).  That ring is the answer to "why was this
 * refused?", and the only trace of the events that change nothing
 * visible (a quarantine being entered, a policy being loaded).
 * ==================================================================== */

static void PermAuditUsage(void) {
    ShellWrite("Usage: perm audit [n] [allow|deny] [subject=<id>] [since=<tick>]\n");
    ShellWrite("       perm audit save [url]   (default " PERM_AUDIT_URL_DEFAULT ")\n");
}

/* Fetch the ring with the filter the caller built.  Returns 0, or the
 * negative error (IPC/port failure, or the engine's own ret). */
static int PermAuditFetch(const perm_req_audit_t *req, perm_resp_audit_t *out) {
    memset(out, 0, sizeof(*out));
    int r = PermDo(req, (int)sizeof(*req), out, (int)sizeof(*out));
    if (r < 0)
        return r;
    return out->ret;
}

static void PermAuditHeader(void) {
    ShellWrite("tick       subject  event         atom   verdict  resource\n");
    ShellWrite("---------  -------  ------------  -----  -------  --------\n");
}

/* Print entries [first, first + count) of an audit response. */
static void PermAuditTable(const perm_resp_audit_t *resp, u32 first, u32 count) {
    for (u32 i = 0; i < count; i++) {
        const perm_audit_ent_t *e = &resp->entries[first + i];
        char                    res[64];
        PermResourceStr(&e->resource, res, sizeof(res));
        PrintfLine("%-9llu  %-7llu  %-12s  %-5u  %-7s  %s\n",
                   (unsigned long long)e->tick,
                   (unsigned long long)e->subject_id,
                   PermEventName(e->event),
                   (unsigned)e->atom,
                   PermVerdictName(e->verdict),
                   res);
    }
}

/* perm audit save [url] — export the ring to a file as text.
 *
 * Read-only as far as the engine goes (it exports the log, it changes
 * nothing), so no role gate here; the file itself is still subject to
 * the ordinary VFS authorization on the target volume. */
static int PermAuditSave(const char *path) {
    char url[VFS_PATH_MAX];
    if (ShellResolvePath(path, url, sizeof(url)) < 0) {
        PrintfLine("perm audit save: bad path '%s'\n", path);
        return -1;
    }

    perm_req_audit_t req;
    memset(&req, 0, sizeof(req)); /* subject 0 / verdict 0 / since 0 = all */
    req.op = PERM_OP_AUDIT;

    static perm_resp_audit_t resp;
    int                      r = PermAuditFetch(&req, &resp);
    if (r < 0) {
        PrintfLine("perm audit save: export FAILED (%d)\n", r);
        return -1;
    }

    u32 count = resp.count;
    if (count > PERM_AUDIT_MAX)
        count = PERM_AUDIT_MAX;

    vfs_handle_t h = 0;
    r = FsOpenItem(url, VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE, VFS_ACCESS_WRITE, &h);
    if (r < 0) {
        PrintfLine("perm audit save: open '%s' FAILED (%d)\n", url, r);
        return -1;
    }

    /* One FsWrite per line: the text is small, and streaming it keeps
     * the whole dump off the stack (a user thread has 16 KiB). */
    char line[192];
    u64  off = 0;
    snprintf(line,
             sizeof(line),
             "# perm audit: %u entries (ring capacity %u)\n"
             "# tick subject event atom verdict resource\n",
             (unsigned)count,
             (unsigned)PERM_AUDIT_MAX);
    r = FsWrite(h, off, line, (u32)strlen(line));
    if (r == 0) {
        off += (u64)strlen(line);
        for (u32 i = 0; i < count; i++) {
            const perm_audit_ent_t *e = &resp.entries[i];
            char                    res[64];
            PermResourceStr(&e->resource, res, sizeof(res));
            snprintf(line,
                     sizeof(line),
                     "%llu %llu %s %u %s %s\n",
                     (unsigned long long)e->tick,
                     (unsigned long long)e->subject_id,
                     PermEventName(e->event),
                     (unsigned)e->atom,
                     PermVerdictName(e->verdict),
                     res);
            r = FsWrite(h, off, line, (u32)strlen(line));
            if (r < 0)
                break;
            off += (u64)strlen(line);
        }
    }
    (void)FsClose(h);
    if (r < 0) {
        PrintfLine("perm audit save: write FAILED (%d)\n", r);
        return -1;
    }

    PrintfLine("perm audit save: %u entries (%llu bytes) -> %s\n",
               (unsigned)count,
               (unsigned long long)off,
               url);
    return 0;
}

int PermCmdAudit(int argc, char *argv[]) {
    int base = PermArgsFrom(argc, argv, "audit");

    if (base < argc && strcmp(argv[base], "save") == 0) {
        const char *url = (base + 1 < argc) ? argv[base + 1] : PERM_AUDIT_URL_DEFAULT;
        return PermAuditSave(url);
    }

    static perm_req_audit_t req;
    memset(&req, 0, sizeof(req));
    req.op = PERM_OP_AUDIT;
    req.max_entries = 0; /* 0 = PERM_AUDIT_MAX (perm.h) */

    u32 limit = 0;
    for (int i = base; i < argc; i++) {
        const char *a = argv[i];
        u64         v = 0;

        if (strcmp(a, "allow") == 0) {
            req.verdict_filter = 1;
            continue;
        }
        if (strcmp(a, "deny") == 0) {
            req.verdict_filter = 2;
            continue;
        }
        if (strncmp(a, "subject=", 8) == 0 && PermParseU64(a + 8, &v) == 0) {
            req.subject_id = v;
            continue;
        }
        if (strncmp(a, "since=", 6) == 0 && PermParseU64(a + 6, &v) == 0) {
            req.since_tick = v;
            continue;
        }
        if (PermParseU64(a, &v) == 0) {
            limit           = (u32)v;
            req.max_entries = limit;
            continue;
        }
        PrintfLine("perm audit: unexpected argument '%s'\n", a);
        PermAuditUsage();
        return -1;
    }

    static perm_resp_audit_t resp;
    int                      r = PermAuditFetch(&req, &resp);
    if (r < 0) {
        PrintfLine("perm audit: export FAILED (%d)\n", r);
        return -1;
    }

    u32 count = resp.count;
    if (count > PERM_AUDIT_MAX)
        count = PERM_AUDIT_MAX;

    /* max_entries is a request hint; clamp again here so "perm audit 5"
     * can never print 64 lines.  The tail of the ring is the newest, so
     * that is the part kept. */
    u32 first = 0;
    if (limit != 0 && count > limit) {
        first = count - limit;
        count = limit;
    }

    PermAuditHeader();
    if (count == 0)
        ShellWrite("(no audit record matches the filter)\n");
    PermAuditTable(&resp, first, count);

    PrintfLine("audit: %u entr%s shown, ring capacity %u\n",
               (unsigned)count,
               count == 1 ? "y" : "ies",
               (unsigned)PERM_AUDIT_MAX);
    return 0;
}

/* ====================================================================
 * perm ctx — the foreground/background table
 *
 * A subject that is not in the foreground is refused WITHOUT a Powerbox
 * prompt (PERM_DEC_BACKGROUND): no application may raise a panel at the
 * user while it sits in the background.  This verb is how the session
 * layer tells the engine which app is being looked at, and how anyone
 * can see the current bindings.
 * ==================================================================== */

static void PermCtxUsage(void) {
    ShellWrite("Usage: perm ctx [list]\n");
    ShellWrite("       perm ctx <subject-id|pid> fg|bg\n");
    ShellWrite("         subject-id is decimal; a live PID is resolved to its subject\n");
    ShellWrite("         (OWNER/ADMIN required for the fg|bg form)\n");
}

static int PermCtxList(void) {
    perm_req_ctx_query_t req;
    memset(&req, 0, sizeof(req));
    req.op = PERM_OP_CTX_QUERY;

    static perm_resp_context_t resp;
    memset(&resp, 0, sizeof(resp));
    int r = PermDo(&req, (int)sizeof(req), &resp, (int)sizeof(resp));
    if (r < 0) {
        PrintfLine("perm ctx list: IPC FAILED (%d)\n", r);
        return -1;
    }
    if (resp.ret < 0) {
        PrintfLine("perm ctx list: engine returned %d\n", resp.ret);
        return -1;
    }

    u32 n = resp.count;
    if (n > PERM_CTX_LIST_MAX)
        n = PERM_CTX_LIST_MAX;

    ShellWrite("subject  pid   name              ctx  state\n");
    ShellWrite("-------  ----  ----------------  ---  -------------\n");
    if (n == 0)
        ShellWrite("(no subject has a foreground/background binding yet)\n");

    for (u32 i = 0; i < n; i++) {
        u64          subject = resp.entries[i].subject_id;
        proc_ident_t ident;
        char         pidbuf[16];
        const char  *pname = "(not live)";

        /* The kernel maps subject -> process, so pid/name are decoration
         * on this table; a subject with no live process is normal (the
         * app exited but its context record is still there). */
        if (subject != 0 && ProcInfoBySubject(subject, &ident) == 0) {
            snprintf(pidbuf, sizeof(pidbuf), "%d", ident.pid);
            pname = ident.name;
        } else {
            snprintf(pidbuf, sizeof(pidbuf), "%s", "-");
        }

        PrintfLine("%-7llu  %-4s  %-16s  %-3s  %s\n",
                   (unsigned long long)subject,
                   pidbuf,
                   pname,
                   resp.entries[i].foreground ? "fg" : "bg",
                   resp.entries[i].quarantined ? "QUARANTINED" : "ok");
    }

    PrintfLine("ctx: %u binding(s) - background subjects are refused without a prompt\n",
               (unsigned)n);
    return 0;
}

int PermCmdCtx(int argc, char *argv[]) {
    int base = PermArgsFrom(argc, argv, "ctx");

    if (base >= argc || strcmp(argv[base], "list") == 0) {
        if (base + 1 < argc) {
            PrintfLine("perm ctx: unexpected argument '%s'\n", argv[base + 1]);
            PermCtxUsage();
            return -1;
        }
        return PermCtxList();
    }

    if (base + 1 >= argc) {
        PermCtxUsage();
        return -1;
    }

    u32 fg = 0;
    if (strcmp(argv[base + 1], "fg") == 0 || strcmp(argv[base + 1], "foreground") == 0)
        fg = 1;
    else if (strcmp(argv[base + 1], "bg") == 0 || strcmp(argv[base + 1], "background") == 0)
        fg = 0;
    else {
        PrintfLine("perm ctx: '%s' is not fg|bg\n", argv[base + 1]);
        PermCtxUsage();
        return -1;
    }

    if (PermRequireAdmin("perm ctx") < 0)
        return -1;

    u64 subject = 0;
    if (PermResolveSubject(argv[base], &subject) < 0)
        return -1;
    if (subject == 0) {
        ShellWrite("perm ctx: subject 0 is the kernel - it has no context to set\n");
        return -1;
    }

    perm_req_context_t req;
    memset(&req, 0, sizeof(req));
    req.op         = PERM_OP_CONTEXT;
    req.subject_id = subject;
    req.foreground = fg;
    req.list       = 0; /* 0 = this is a state change, not a query */

    static perm_resp_context_t resp;
    memset(&resp, 0, sizeof(resp));
    int r = PermDo(&req, (int)sizeof(req), &resp, (int)sizeof(resp));
    if (r < 0) {
        PrintfLine("perm ctx: IPC FAILED (%d)\n", r);
        return -1;
    }
    if (resp.ret < 0) {
        PrintfLine("perm ctx: engine returned %d\n", resp.ret);
        return -1;
    }

    PrintfLine("perm ctx: subject %llu is now %s\n",
               (unsigned long long)subject,
               fg ? "foreground" : "background");
    return 0;
}

/* ====================================================================
 * perm freq — counters and quarantine
 *
 * DoCheck counts every decision per subject; PERM_DENY_THRESHOLD denials
 * inside PERM_DENY_WINDOW_TICKS put the subject in quarantine for
 * PERM_QUARANTINE_TICKS, during which its requests are refused without a
 * panel (a rogue or background app cannot spam the user).  The counters
 * are also the evidence an administrator looks at before releasing one.
 * ==================================================================== */

static void PermFreqUsage(void) {
    ShellWrite("Usage: perm freq [subject-id|all]        hits/denies/quarantine\n");
    ShellWrite("       perm freq release <subject-id|all>  lift quarantines (OWNER/ADMIN)\n");
}

/* Show one subject's counters (subject 0 = the table-wide aggregate). */
static int PermFreqShow(const char *verb, u64 subject) {
    static perm_resp_freq_t resp;
    int                     r = PermFreqQuery(subject, 0, &resp);
    if (r < 0) {
        PrintfLine("%s: query FAILED (%d)\n", verb, r);
        return -1;
    }

    if (subject == 0) {
        PrintfLine("%s: every subject\n", verb);
    } else {
        proc_ident_t ident;
        if (ProcInfoBySubject(subject, &ident) == 0)
            PrintfLine("%s: subject %llu - %s (PID %d)\n",
                       verb,
                       (unsigned long long)subject,
                       ident.name,
                       ident.pid);
        else
            PrintfLine("%s: subject %llu - no live process\n",
                       verb,
                       (unsigned long long)subject);
    }

    ShellWrite("hits  denies  quarantined  left  slots\n");
    ShellWrite("----  ------  -----------  ----  -----\n");
    PrintfLine("%-4u  %-6u  %-11s  %-4u  %u\n",
               (unsigned)resp.count,
               (unsigned)resp.denies,
               resp.quarantined ? "yes" : "no",
               (unsigned)resp.quarantine_ticks,
               (unsigned)resp.slots);
    PrintfLine("policy: %u denies inside %u ticks quarantine a subject for %u ticks\n",
               (unsigned)PERM_DENY_THRESHOLD,
               (unsigned)PERM_DENY_WINDOW_TICKS,
               (unsigned)PERM_QUARANTINE_TICKS);
    return 0;
}

/* Release one subject (or every subject, via "all") from quarantine.
 * Only this call clears the state: the window expires on its own, but an
 * administrator who has understood the cause should not have to wait.
 * verb is the caller's own name ("perm freq release" or "permguard"), so
 * the messages point at the command the user actually typed. */
static int PermFreqRelease(const char *verb, const char *arg) {
    if (PermRequireAdmin(verb) < 0)
        return -1;

    u64 subject = 0; /* perm.h: subject_id 0 = every subject */
    if (strcmp(arg, "all") != 0 && PermResolveSubject(arg, &subject) < 0)
        return -1;

    perm_resp_freq_t resp;
    int              r = PermFreqQuery(subject, 1, &resp);
    if (r < 0) {
        PrintfLine("%s: FAILED (%d)\n", verb, r);
        return -1;
    }

    if (subject == 0)
        PrintfLine("%s: quarantine lifted for every subject\n", verb);
    else
        PrintfLine("%s: subject %llu released (quarantined=%s, left=%u)\n",
                   verb,
                   (unsigned long long)subject,
                   resp.quarantined ? "yes" : "no",
                   (unsigned)resp.quarantine_ticks);
    PrintfLine("%s: counters now hits %u, denies %u\n",
               verb,
               (unsigned)resp.count,
               (unsigned)resp.denies);
    return 0;
}

int PermCmdFreq(int argc, char *argv[]) {
    int base = PermArgsFrom(argc, argv, "freq");

    if (base < argc && strcmp(argv[base], "release") == 0) {
        if (base + 1 >= argc) {
            PermFreqUsage();
            return -1;
        }
        if (base + 2 < argc) {
            PrintfLine("perm freq release: unexpected argument '%s'\n", argv[base + 2]);
            PermFreqUsage();
            return -1;
        }
        return PermFreqRelease("perm freq release", argv[base + 1]);
    }

    u64 subject = 0; /* 0 = every subject (perm.h) */
    if (base < argc) {
        u64 n = 0;
        if (strcmp(argv[base], "all") == 0)
            subject = 0;
        else if (PermParseU64(argv[base], &n) == 0 && n == 0)
            subject = 0; /* an explicit "0" is the aggregate as well */
        else if (PermResolveSubject(argv[base], &subject) < 0)
            return -1;

        if (base + 1 < argc) {
            PrintfLine("perm freq: unexpected argument '%s'\n", argv[base + 1]);
            PermFreqUsage();
            return -1;
        }
    }

    return PermFreqShow("perm freq", subject);
}

/* ====================================================================
 * perm save / perm load — policy snapshots as ordinary files
 *
 * The snapshot is the perm.h binary format (magic "POLY", version, the
 * grant table, then the role table), so it travels as a file a user can
 * copy, keep or diff.  LOAD is all-or-nothing server-side: a rejected
 * snapshot leaves the running policy untouched, which is why this
 * command can say so with confidence.
 * ==================================================================== */

static void PermPolicyUsage(void) {
    ShellWrite("Usage: perm save [url] [all]   (default " PERM_POLICY_URL_DEFAULT ")\n");
    ShellWrite("       perm load [url]         (default " PERM_POLICY_URL_DEFAULT ")\n");
    ShellWrite("       'all' exports expired grants too; both need OWNER/ADMIN\n");
}

int PermCmdSave(int argc, char *argv[]) {
    int base = PermArgsFrom(argc, argv, "save");

    const char *path            = PERM_POLICY_URL_DEFAULT;
    u32         include_expired = 0;
    for (int i = base; i < argc; i++) {
        if (strcmp(argv[i], "all") == 0) {
            include_expired = 1; /* perm.h: 0 = live grants only */
            continue;
        }
        path = argv[i];
    }

    if (PermRequireAdmin("perm save") < 0)
        return -1;

    char url[VFS_PATH_MAX];
    if (ShellResolvePath(path, url, sizeof(url)) < 0) {
        PrintfLine("perm save: bad path '%s'\n", path);
        return -1;
    }

    static perm_req_policy_t req;
    memset(&req, 0, sizeof(req));
    req.op              = PERM_OP_POLICY_SAVE;
    req.include_expired = include_expired;

    static perm_resp_policy_t resp;
    memset(&resp, 0, sizeof(resp));
    int r = PermDo(&req, (int)sizeof(req), &resp, (int)sizeof(resp));
    if (r < 0) {
        PrintfLine("perm save: IPC FAILED (%d)\n", r);
        return -1;
    }
    if (resp.ret < 0) {
        PrintfLine("perm save: engine returned %d\n", resp.ret);
        return -1;
    }

    u32 size = resp.size;
    if (size > PERM_POLICY_MAX)
        size = PERM_POLICY_MAX; /* defensive: never trust a length field */
    if (size < PERM_POLICY_HDR) {
        PrintfLine("perm save: snapshot is %u bytes - too small to be a policy\n",
                   (unsigned)size);
        return -1;
    }

    vfs_handle_t h = 0;
    r = FsOpenItem(url, VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE, VFS_ACCESS_WRITE, &h);
    if (r < 0) {
        PrintfLine("perm save: open '%s' FAILED (%d)\n", url, r);
        return -1;
    }
    r = FsWrite(h, 0, resp.data, size);
    (void)FsClose(h);
    if (r < 0) {
        PrintfLine("perm save: write FAILED (%d)\n", r);
        return -1;
    }

    PrintfLine("perm save: %u-byte snapshot%s -> %s\n",
               (unsigned)size,
               include_expired ? " (expired grants included)" : "",
               url);
    return 0;
}

int PermCmdLoad(int argc, char *argv[]) {
    int base = PermArgsFrom(argc, argv, "load");

    if (base + 1 < argc) {
        PrintfLine("perm load: unexpected argument '%s'\n", argv[base + 1]);
        PermPolicyUsage();
        return -1;
    }
    const char *path = (base < argc) ? argv[base] : PERM_POLICY_URL_DEFAULT;

    if (PermRequireAdmin("perm load") < 0)
        return -1;

    char url[VFS_PATH_MAX];
    if (ShellResolvePath(path, url, sizeof(url)) < 0) {
        PrintfLine("perm load: bad path '%s'\n", path);
        return -1;
    }

    /* Size-check before reading: a snapshot has a fixed ceiling
     * (PERM_POLICY_MAX = 3840, it must fit one IPC message), so a larger
     * file is refused instead of being silently truncated into something
     * that would only fail a magic check later. */
    vfs_item_info_t info;
    int             r = FsGetItem(url, &info);
    if (r < 0) {
        PrintfLine("perm load: stat '%s' FAILED (%d)\n", url, r);
        return -1;
    }
    if (info.size < PERM_POLICY_HDR || info.size > PERM_POLICY_MAX) {
        PrintfLine("perm load: '%s' is %llu bytes - not a policy snapshot (%u..%u)\n",
                   url,
                   (unsigned long long)info.size,
                   (unsigned)PERM_POLICY_HDR,
                   (unsigned)PERM_POLICY_MAX);
        return -1;
    }

    vfs_handle_t h = 0;
    r = FsOpenItem(url, VFS_OPEN_READONLY, VFS_ACCESS_READ, &h);
    if (r < 0) {
        PrintfLine("perm load: open '%s' FAILED (%d)\n", url, r);
        return -1;
    }

    static perm_req_policy_t req;
    memset(&req, 0, sizeof(req));
    req.op = PERM_OP_POLICY_LOAD;

    u32 got = 0;
    r       = FsRead(h, 0, req.data, PERM_POLICY_MAX, &got);
    (void)FsClose(h);
    if (r < 0) {
        PrintfLine("perm load: read FAILED (%d)\n", r);
        return -1;
    }
    if (got < PERM_POLICY_HDR) {
        PrintfLine("perm load: read only %u bytes - snapshot too short\n", (unsigned)got);
        return -1;
    }
    req.size = got;

    static perm_resp_policy_t resp;
    memset(&resp, 0, sizeof(resp));
    r = PermDo(&req, (int)sizeof(req), &resp, (int)sizeof(resp));
    if (r < 0) {
        PrintfLine("perm load: IPC FAILED (%d)\n", r);
        return -1;
    }
    if (resp.ret < 0) {
        PrintfLine("perm load: snapshot rejected (%d) - policy unchanged\n", resp.ret);
        return -1;
    }

    PrintfLine("perm load: %u-byte snapshot from %s applied\n", (unsigned)got, url);
    return 0;
}

/* ====================================================================
 * permguard — the quarantine entry point
 *
 * Registered as a command of its own so the permission model has one
 * obvious console door: who is quarantined, what are their counters, and
 * how is one released.  ("perm", "bm" and "permission" are already taken
 * by shell.c, hence the distinct name.)
 * ==================================================================== */

static void PermGuardUsage(void) {
    ShellWrite("Usage: permguard [list]\n");
    ShellWrite("       permguard <subject-id|pid> [release]\n");
    ShellWrite("         list is read-only; 'release' needs OWNER/ADMIN\n");
}

static void PermGuardHeader(void) {
    ShellWrite("subject  pid   name              ctx  hits  denies  quarantined  left\n");
    ShellWrite("-------  ----  ----------------  ---  ----  ------  -----------  ----\n");
}

/* One table row for one subject.  Returns 0 when the row was printed,
 * -1 when the engine refused to answer — a skipped row is honest, a
 * fabricated "0 denies" is not. */
static int PermGuardRow(u64 subject, int known_fg, u32 fg) {
    static perm_resp_freq_t f;
    if (PermFreqQuery(subject, 0, &f) < 0)
        return -1;

    proc_ident_t ident;
    char         pidbuf[16];
    const char  *pname = "(not live)";
    if (ProcInfoBySubject(subject, &ident) == 0) {
        snprintf(pidbuf, sizeof(pidbuf), "%d", ident.pid);
        pname = ident.name;
    } else {
        snprintf(pidbuf, sizeof(pidbuf), "%s", "-");
    }

    PrintfLine("%-7llu  %-4s  %-16s  %-3s  %-4u  %-6u  %-11s  %u\n",
               (unsigned long long)subject,
               pidbuf,
               pname,
               known_fg ? (fg ? "fg" : "bg") : "-",
               (unsigned)f.count,
               (unsigned)f.denies,
               f.quarantined ? "yes" : "no",
               (unsigned)f.quarantine_ticks);
    return 0;
}

/* permguard [list] — inspect every subject the engine knows.
 *
 * The candidate set is the perm context table plus the caller itself: a
 * freshly booted machine may have no context record at all, while the
 * shell's own subject is known for certain.  Costs one FREQ round trip
 * per candidate (at most PERM_CTX_LIST_MAX + 1). */
static int PermGuardList(void) {
    perm_req_ctx_query_t req;
    memset(&req, 0, sizeof(req));
    req.op = PERM_OP_CTX_QUERY;

    static perm_resp_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    int have_table = 0;
    if (PermDo(&req, (int)sizeof(req), &ctx, (int)sizeof(ctx)) == 0 && ctx.ret == 0)
        have_table = 1;

    u32 n = have_table ? ctx.count : 0;
    if (n > PERM_CTX_LIST_MAX)
        n = PERM_CTX_LIST_MAX;

    PermGuardHeader();

    u32 shown = 0;
    for (u32 i = 0; i < n; i++) {
        if (ctx.entries[i].subject_id == 0)
            continue;
        if (PermGuardRow(ctx.entries[i].subject_id, 1, ctx.entries[i].foreground) == 0)
            shown++;
    }

    u64 self = GetSubject();
    if (self != 0) {
        int listed = 0;
        for (u32 i = 0; i < n; i++)
            if (ctx.entries[i].subject_id == self)
                listed = 1;
        if (!listed && PermGuardRow(self, 0, 0) == 0)
            shown++;
    }

    if (shown == 0) {
        ShellWrite("permguard: no subject to inspect - is perm-manager up?\n");
        return -1;
    }
    PrintfLine("permguard: %u subject(s); quarantine after %u denies inside %u ticks (%u ticks)\n",
               (unsigned)shown,
               (unsigned)PERM_DENY_THRESHOLD,
               (unsigned)PERM_DENY_WINDOW_TICKS,
               (unsigned)PERM_QUARANTINE_TICKS);
    return 0;
}

static int CmdPermGuard(int argc, char *argv[]) {
    if (argc < 2 || strcmp(argv[1], "list") == 0) {
        if (argc > 2) {
            PrintfLine("permguard: unexpected argument '%s'\n", argv[2]);
            PermGuardUsage();
            return -1;
        }
        return PermGuardList();
    }

    /* "permguard release <subject>" is accepted as well as the documented
     * "permguard <subject> release": both read like an order. */
    if (strcmp(argv[1], "release") == 0) {
        if (argc != 3) {
            PermGuardUsage();
            return -1;
        }
        return PermFreqRelease("permguard", argv[2]); /* "all" = everyone */
    }

    if (argc >= 3 && strcmp(argv[2], "release") == 0) {
        if (argc > 3) {
            PrintfLine("permguard: unexpected argument '%s'\n", argv[3]);
            PermGuardUsage();
            return -1;
        }
        return PermFreqRelease("permguard", argv[1]);
    }
    if (argc > 2) {
        PrintfLine("permguard: unexpected argument '%s'\n", argv[2]);
        PermGuardUsage();
        return -1;
    }

    u64 subject = 0;
    if (PermResolveSubject(argv[1], &subject) < 0)
        return -1;

    PermGuardHeader();
    if (PermGuardRow(subject, 0, 0) < 0) {
        PrintfLine("permguard: no counters for subject %llu\n", (unsigned long long)subject);
        return -1;
    }
    ShellWrite("permguard: append 'release' to lift a quarantine (OWNER/ADMIN)\n");
    return 0;
}

/* ====================================================================
 * Registration
 *
 * The shell's own "perm" command keeps its verbs; this module adds the
 * v1.0 surface.  "permguard" is the standalone quarantine entry point,
 * and the perm_* aliases make every verb reachable on its own (and
 * testable) without depending on how the shell dispatches "perm".
 * ==================================================================== */

void ShellRegisterPermCommands(void) {
    ShellRegisterCommand(
        "permguard", "Quarantine admin: permguard [list|<subject|pid> [release]]", CmdPermGuard);
    ShellRegisterCommand(
        "perm_audit",
        "Audit ring: perm_audit [n] [allow|deny] [subject=<id>] | perm_audit save <url>",
        PermCmdAudit);
    ShellRegisterCommand(
        "perm_ctx", "FG/BG context: perm_ctx [list|<subject|pid> fg|bg]", PermCmdCtx);
    ShellRegisterCommand("perm_freq",
                         "Counters: perm_freq [subject|all] | perm_freq release <subject|all>",
                         PermCmdFreq);
    ShellRegisterCommand(
        "perm_save", "Save policy snapshot (OWNER/ADMIN): perm_save [url] [all]", PermCmdSave);
    ShellRegisterCommand("perm_load",
                         "Load policy snapshot (OWNER/ADMIN): perm_load [url]",
                         PermCmdLoad);
}
