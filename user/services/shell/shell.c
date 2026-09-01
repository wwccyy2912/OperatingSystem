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
 * shell.c - Simple terminal shell (TTY-like)
 * Copyright (c) 2026 OpSys Project
 *
 * Runs as its OWN process, spawned by the manager via
 * SYS_PROCESS_CREATE.  All I/O goes through the framebuffer terminal
 * and PS/2 keyboard services:
 *   input  — IpcCall(READ_BLOCK) on the 'keyboard' port (blocking: the
 *            shell thread parks in the kernel until key bytes arrive)
 *   output — IpcCall(WRITE) on the 'term' port (rendered on the
 *            framebuffer by the terminal service)
 * Supports basic line editing.  Commands are registered at runtime
 * into a singly-linked list via ShellRegisterCommand() — the shell
 * registers its built-ins at the top of main().  Architecture:
 *
 *   main()
 *     ├─ ShellRegisterCommand() × 14    (built-in commands)
 *     └─ ShellLoop()
 *          ├─ print prompt
 *          ├─ ReadLine() ← IpcCall(READ_BLOCK)  (blocks until input)
 *          │    ├─ echo / backspace handling
 *          │    └─ enter → returns buffer
 *          └─ Execute()
 *               ├─ parse argv
 *               └─ linked-list dispatch  (strcmp over registered names)
 *
 * ------------------------------------------------------------------
 * Structure (REPL: line edit -> parse -> dispatch):
 *   main() → ShellMain()      (own process, spawned by the manager)
 *     ├─ ShellRegisterCommand() × N   built-ins into the linked list
 *     └─ ShellLoop()
 *          ├─ ReadLine() ← IpcCall(READ_BLOCK)  (kbd port; line
 *          │    editing, history ring, UTF-8, pinyin IME)
 *          └─ Execute() → argv parse → list dispatch → cmd handler
 *               └─ output → IpcCall(WRITE) on the 'term' port
 * How it works:
 *   ShellMain resolves the 'keyboard' and 'term' ports once, registers
 *   the built-ins at runtime via ShellRegisterCommand(), then
 *   ShellLoop blocks in ReadLine until key bytes arrive; Execute
 *   tokenizes the line and dispatches by strcmp over the list, with
 *   command output rendered by the terminal service.
 * Purpose:
 *   Interactive user front end: command parsing/execution over IPC,
 *   plus line editing, history, tab completion, UTF-8/IME input and
 *   TUI tools (fm).
 * Caveats:
 *   READ_BLOCK parks the whole shell thread in the kernel until input
 *   arrives; dispatch is a linear strcmp scan of the command list;
 *   shell output goes to the 'term' port, never to serial (libc
 *   printf is the debug-log exception).
 * ------------------------------------------------------------------
 */

#include "shell.h"              /* cmd_func_t, shell_register_command */
#include "../lib/libc/stdio.h"  /* printf, sscanf -> SYS_DEBUG_LOG (serial) */
#include "../lib/libc/stdlib.h" /* atoi */
#include "../lib/libc/string.h" /* strcmp, strlen, strdup */
#include "../lib/libc/utf8.h"   /* UTF-8-aware line editing */
#include "../lib/libime/ime.h"  /* pinyin IME (Chinese input) */
#include "../lib/libfs/fs.h"    /* libfs VFS client (ls/cat/stat/tee/fallocate/mkdir/rm) */
#include "../lib/libpkg/pkg.h"  /* libpkg pkg-manager client (pkg_*) */
#include "../lib/libos/syscalls.h"
#include "../perm/perm.h" /* Powerbox protocol (perm_answer/perm_revoke) */
#include "../user/user.h" /* user account protocol */
#include "../net/net.h"  /* PCnet NIC protocol (net command) */
#include "../policy/policy.h" /* command policy service (v0.5) */
#include "../lib/libtui/tui.h" /* interactive TUI components */
#include <malloc.h>       /* malloc, free */
#include <stdarg.h>
#include <stdint.h>

typedef uint8_t  u8;
typedef uint32_t u32;
typedef int32_t  i32;

/* ====================================================================
 * Constants
 * ==================================================================== */

#define LINE_BUF_SIZE 256

/* Completion / fm list caps: max candidates shown by Tab completion
 * and max entries per fm screen.  The 64x64 second dim is the per-item
 * name buffer (VFS names are capped at 64 by the server). */
#define COMPLETE_MAX_MATCHES 64
#define FM_MAX_ITEMS         64
#define PROC_MAX_ITEMS       64 /* ps / kill-picker list caps */
#define MAX_ARGS      16

/* Current working directory (v0.5): "/" = volume list view.  All VFS
 * commands accept paths relative to this.  Kept as "/Volumes/X/..." so
 * cd + relative paths compose naturally with the VFS URL grammar. */
static char s_cwd[LINE_BUF_SIZE] = "/";

/* Command history (v1.3): ring of the last N executed lines, navigated
 * with Up/Down in read_line.  The most recent entry is written into
 * the ring on every executed (non-empty) command. */
#define HIST_MAX  16
#define HIST_LEN  LINE_BUF_SIZE
static char s_history[HIST_MAX][HIST_LEN];
static int  s_hist_count; /* entries stored */
static int  s_hist_next;  /* ring write index */
static int  s_hist_view;  /* -1 = live editing; else index being viewed */

/* Identity: the vfs and perm servers derive the caller's identity from
 * its kernel-issued subject (ipc_recv_from), so the shell no longer
 * supplies a self-reported app identity for the Powerbox permission
 * flow (fs_open_item / fs_create_bookmark / perm_revoke).  The shell's
 * own subject comes from GetSubject() when needed. */

/* Terminal service protocol (mirrors user/services/term/term.c) */
#define TERM_OP_WRITE       1
#define TERM_OP_CLEAR       2
#define TERM_OP_STATUS      3 /* {prefix_len,msg_len,prefix,msg} */
#define TERM_CHUNK          32 /* max payload bytes per WRITE */

/* Keyboard service protocol (mirrors user/services/keyboard/keyboard.c) */
#define KBD_OP_READ       1
#define KBD_OP_READ_BLOCK 2
#define KBD_CHUNK         32 /* max payload bytes per READ */

/* Ctrl+Space (KBD_CTRL_BASE+0 = 0x80) toggles the pinyin IME. */
#define KBD_CTRL_SPACE 0x80

/* ====================================================================
 * Pinyin IME state
 *
 * The keyboard service emits only ASCII, so Chinese is entered by
 * typing pinyin (lowercase letters, which accumulate in s_ime_py) and
 * committing a candidate with Space / digits 1-9.  The committed
 * candidate is a UTF-8 sequence inserted into the line buffer, so the
 * editor's codepoint-aware backspace/arrows/redraw all apply to it.
 * The candidate list is shown in the terminal's status bar.
 * ==================================================================== */

static int         s_ime_on; /* 1 = pinyin input mode */
static char        s_ime_py[IME_MAX_PINYIN];
static int         s_ime_plen;
static int         s_ime_cidx; /* selected candidate (0-based) */
static const char *s_ime_cands[IME_MAX_CAND];
static int         s_ime_ncand;

/* ====================================================================
 * Forward declarations for built-in commands
 * ==================================================================== */

static int CmdHelp(int argc, char *argv[]);
static int CmdEcho(int argc, char *argv[]);
static int CmdPid(int argc, char *argv[]);
static int CmdFree(int argc, char *argv[]);
static int CmdClear(int argc, char *argv[]);
static int CmdCap(int argc, char *argv[]);
static int CmdMouse(int argc, char *argv[]);
static int CmdGui(int argc, char *argv[]);
static int CmdNet(int argc, char *argv[]);
static int CmdPorts(int argc, char *argv[]);
static int CmdSleep(int argc, char *argv[]);
static int CmdThreads(int argc, char *argv[]);
static int CmdMutex(int argc, char *argv[]);
static int CmdExec(int argc, char *argv[]);
static int CmdUptime(int argc, char *argv[]);
static int CmdExit(int argc, char *argv[]);
static int CmdReboot(int argc, char *argv[]);
static int CmdIme(int argc, char *argv[]);
static int CmdKill(int argc, char *argv[]);
static int CmdPs(int argc, char *argv[]);
static int CmdLs(int argc, char *argv[]);
static int CmdCat(int argc, char *argv[]);
static int CmdStat(int argc, char *argv[]);
static int CmdTee(int argc, char *argv[]);
static int CmdFallocate(int argc, char *argv[]);
static int CmdMkdir(int argc, char *argv[]);
static int CmdRm(int argc, char *argv[]);
/* Phase 2: bookmarks + Powerbox + mv (design §8) */
static int CmdBmCreate(int argc, char *argv[]);
static int CmdBmResolve(int argc, char *argv[]);
static int CmdBmRevoke(int argc, char *argv[]);
static int CmdPermAnswer(int argc, char *argv[]);
static int CmdPermQuery(int argc, char *argv[]);
static int CmdPermRevoke(int argc, char *argv[]);
static int CmdMv(int argc, char *argv[]);
static int CmdPkg(int argc, char *argv[]);
static int CmdLogin(int argc, char *argv[]);
static int CmdLogout(int argc, char *argv[]);
static int CmdWhoami(int argc, char *argv[]);
static int CmdPasswd(int argc, char *argv[]);
static int CmdUseradd(int argc, char *argv[]);
static int CmdUserdel(int argc, char *argv[]);
static int CmdUserlock(int argc, char *argv[]);
static int CmdUserunlock(int argc, char *argv[]);
static int CmdUsers(int argc, char *argv[]);
static int CmdStop(int argc, char *argv[]);
static int CmdExport(int argc, char *argv[]);
static int CmdUnset(int argc, char *argv[]);
static int CmdEnv(int argc, char *argv[]);
static int CmdPolicySet(int argc, char *argv[]);
static int CmdPolicyDump(int argc, char *argv[]);
static int UserCall(const void *req, int req_len, void *resp, int resp_len);
static int CmdCd(int argc, char *argv[]);
static int CmdPwd(int argc, char *argv[]);
static int CmdScroll(int argc, char *argv[]);
static int CmdDisk(int argc, char *argv[]);
static int CmdShutdown(int argc, char *argv[]);
static int CmdBm(int argc, char *argv[]);
static int CmdPerm(int argc, char *argv[]);
static int CmdPolicy(int argc, char *argv[]);
static int CmdFm(int argc, char *argv[]);
static int ShellResolvePath(const char *path, char *out, size_t outsz);

/* ====================================================================
 * Runtime command registry
 *
 * The command set is a singly-linked list built at runtime via
 * ShellRegisterCommand(), NOT a compile-time table.  The shell
 * registers its 12 built-ins at the top of ShellMain(); other
 * services (e.g. the manager) may register their own commands before
 * the shell starts reading input.
 *
 * Single-writer rule: the list is written only BEFORE the shell loop
 * starts reading it — the shell registers its own built-ins, and the
 * manager registers its "services" command on its own thread before it
 * calls ThreadCreate(ShellMain, ...).  After that neither thread
 * touches the list again while it is being read.  No locking needed.
 * ==================================================================== */

typedef struct cmd_node {
    char            *name; /* strdup'd */
    char            *help; /* strdup'd */
    cmd_func_t       func;
    struct cmd_node *next;
} cmd_node_t;

static cmd_node_t *s_cmd_head; /* list head  (see single-writer rule) */
static cmd_node_t *s_cmd_tail; /* list tail  (O(1) append keeps order) */

/* ====================================================================
 * Command policy filter (v0.5)
 *
 * Three-tier command access (docs/permission_model.md §九·五):
 *   Capability (kernel) -> Policy DB (policy service) -> Shell override.
 * The shell keeps its full static command table, but marks every
 * registered command with a verdict fetched from the policy service at
 * startup (POLICY_DENY -> not executed).  If the policy service is
 * unreachable, a hardcoded rescue list keeps the shell usable.
 * Environment variables NEVER gate commands (env = user prefs only).
 * ==================================================================== */

/* Per-command verdict table, indexed by command name hash. */
#define CMD_FILTER_SLOTS 128

typedef struct {
    char name[POLICY_CMD_MAX];
    u8   deny; /* 1 = command blocked by policy */
} cmd_filter_t;

static cmd_filter_t s_cmd_filter[CMD_FILTER_SLOTS];
static int          s_filter_ready; /* 1 after the policy query */

/* FNV-1a over the command name -> slot. */
static u32 CmdFilterHash(const char *s) {
    u32 h = 2166136261u;
    while (*s) {
        h ^= (u8)*s++;
        h *= 16777619u;
    }
    return h % CMD_FILTER_SLOTS;
}

/* Record a verdict (deny=1/0) for a command. */
static void CmdFilterSet(const char *name, int deny) {
    u32 slot = CmdFilterHash(name);
    for (u32 i = 0; i < CMD_FILTER_SLOTS; i++) {
        u32 idx = (slot + i) % CMD_FILTER_SLOTS;
        if (s_cmd_filter[idx].name[0] == '\0' ||
            strcmp(s_cmd_filter[idx].name, name) == 0) {
            strncpy(s_cmd_filter[idx].name, name, sizeof(s_cmd_filter[idx].name) - 1);
            s_cmd_filter[idx].name[sizeof(s_cmd_filter[idx].name) - 1] = '\0';
            s_cmd_filter[idx].deny                                    = (u8)deny;
            return;
        }
    }
    /* Table full: leave unrecorded (default allow). */
}

/* 1 when the command is blocked by policy (deny); 0 otherwise. */
static int CmdFilterDenied(const char *name) {
    if (!s_filter_ready)
        return 0; /* no policy loaded -> allow (capability still gates) */
    u32 slot = CmdFilterHash(name);
    for (u32 i = 0; i < CMD_FILTER_SLOTS; i++) {
        u32 idx = (slot + i) % CMD_FILTER_SLOTS;
        if (s_cmd_filter[idx].name[0] == '\0')
            return 0;
        if (strcmp(s_cmd_filter[idx].name, name) == 0)
            return s_cmd_filter[idx].deny;
    }
    return 0;
}

/* Rescue list: commands guaranteed to work even if the policy service
 * is down (admin recovery path).  These are force-ALLOWED. */
static const char *const s_rescue_cmds[] = {
    "help", "ls", "cat", "echo", "env", "export", "unset",
    "login", "whoami", "exit", "reboot",
};

static int CmdIsRescue(const char *name) {
    for (u32 i = 0; i < sizeof(s_rescue_cmds) / sizeof(s_rescue_cmds[0]); i++)
        if (strcmp(s_rescue_cmds[i], name) == 0)
            return 1;
    return 0;
}

/* Query the policy service for the caller's role and mark the command
 * table.  Falls back to rescue-only mode on any failure. */
static void CmdFilterLoad(void) {
    memset(s_cmd_filter, 0, sizeof(s_cmd_filter));
    s_filter_ready = 0;

    int port = PortGet(POLICY_PORT_NAME);
    if (port < 0)
        return; /* service down: no policy -> all allowed (rescue implicit) */

    /* Resolve the caller's role via the user service (WHOAMI). */
    u32 role = 2; /* PERM_ROLE_STANDARD default */
    {
        int uport = PortGet("user");
        if (uport >= 0) {
            user_req_login_t req;
            memset(&req, 0, sizeof(req));
            req.op = USER_OP_WHOAMI;
            user_resp_login_t resp;
            memset(&resp, 0, sizeof(resp));
            int rlen = (int)sizeof(resp);
            if (IpcCall(uport, &req, (int)sizeof(req), &resp, &rlen) == 0 && resp.ret == 0)
                role = resp.role;
        }
    }

    /* Build the command list from the registry (bounded). */
    policy_req_query_t q;
    memset(&q, 0, sizeof(q));
    q.op    = POLICY_OP_QUERY;
    q.role  = role;
    q.count = 0;
    for (cmd_node_t *n = s_cmd_head; n && q.count < POLICY_MAX_CMDS; n = n->next) {
        strncpy(q.cmds[q.count], n->name, POLICY_CMD_MAX - 1);
        q.cmds[q.count][POLICY_CMD_MAX - 1] = '\0';
        q.count++;
    }
    if (q.count == 0)
        return;

    policy_resp_query_t resp;
    memset(&resp, 0, sizeof(resp));
    int rlen = (int)sizeof(resp);
    if (IpcCall(port, &q, (int)sizeof(q), &resp, &rlen) < 0 || resp.ret < 0)
        return;

    /* Apply verdicts; rescue commands are force-allowed. */
    u32 n = 0;
    for (cmd_node_t *node = s_cmd_head; node && n < resp.count; node = node->next, n++) {
        u8 v = resp.verdicts[n];
        if (v == POLICY_DENY && !CmdIsRescue(node->name))
            CmdFilterSet(node->name, 1);
        else
            CmdFilterSet(node->name, 0);
    }
    s_filter_ready = 1;
}

/*
 * Register a command with the shell.  Both name and help are strdup'd
 * into freshly malloc'd storage; the node is appended at the TAIL so
 * `help` output preserves registration order.  Duplicate names are
 * rejected.
 *
 * Returns OK (0) on success; ERR_NOMEM if any allocation fails;
 * ERR_INVAL if name/help/func is NULL or the name is already
 * registered.
 */
int ShellRegisterCommand(const char *name, const char *help, cmd_func_t func) {
    if (name == NULL || help == NULL || func == NULL)
        return ERR_INVAL;

    /* Reject duplicates: a name may only be registered once. */
    for (cmd_node_t *n = s_cmd_head; n != NULL; n = n->next) {
        if (strcmp(n->name, name) == 0)
            return ERR_INVAL;
    }

    cmd_node_t *node = (cmd_node_t *)malloc(sizeof(cmd_node_t));
    if (node == NULL)
        return ERR_NOMEM;

    node->name = strdup(name);
    if (node->name == NULL) {
        free(node);
        return ERR_NOMEM;
    }
    node->help = strdup(help);
    if (node->help == NULL) {
        free(node->name);
        free(node);
        return ERR_NOMEM;
    }

    node->func = func;
    node->next = NULL;

    /* Append at tail: `help` output stays in registration order. */
    if (s_cmd_head == NULL)
        s_cmd_head = node;
    else
        s_cmd_tail->next = node;
    s_cmd_tail = node;

    return OK;
}

/* ====================================================================
 * Terminal service I/O
 *
 * Every byte the shell prints goes out to the framebuffer through the
 * terminal service (WRITE op); every key the user presses comes back
 * from the keyboard service (READ_BLOCK op, blocking — 0 bytes only
 * when the single pending-read slot is already taken).
 * ==================================================================== */

static int s_term_port = -1; /* resolved once in ShellMain()  */
static int s_kbd_port  = -1; /* resolved once in ShellMain()  */

/* Send one byte to the terminal service (WRITE op). */
static void ShellPutc(char c) {
    if (s_term_port < 0)
        return;

    u32 req[2 + 1]; /* { op; len; data[1] } */
    u32 resp[1];    /* { ret }             */
    req[0]         = TERM_OP_WRITE;
    req[1]         = 1;
    ((u8 *)req)[8] = (u8)c;
    int resp_len   = (int)sizeof(resp);
    IpcCall(s_term_port, (const void *)req, 9, (void *)resp, &resp_len);
}

/* Send a NUL-terminated string to the terminal service (WRITE op),
 * chunked so the request buffer stays small and bounded. */
static void ShellWrite(const char *s) {
    if (s_term_port < 0)
        return;

    u32 req[2 + 8]; /* { op; len; data[32] } */
    u32 resp[1];    /* { ret }              */
    while (*s != '\0') {
        int n = 0;
        while (n < TERM_CHUNK && s[n] != '\0') {
            ((u8 *)req)[8 + n] = (u8)s[n];
            n++;
        }
        req[0]       = TERM_OP_WRITE;
        req[1]       = (u32)n;
        int resp_len = (int)sizeof(resp);
        IpcCall(s_term_port, (const void *)req, 8 + n, (void *)resp, &resp_len);
        s += n;
    }
}

/* Local int -> decimal string (the libc has no itoa). */
static void ShellItoa(int v, char *buf) {
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

/* Local u32 -> lowercase hex string. */
static void ShellUtoaHex(u32 v, char *buf) {
    char              tmp[9];
    int               i = 0, j = 0;
    static const char hex[] = "0123456789abcdef";

    if (v == 0)
        tmp[i++] = '0';
    while (v > 0) {
        tmp[i++] = hex[v & 0xf];
        v >>= 4;
    }
    while (i > 0)
        buf[j++] = tmp[--i];
    buf[j] = '\0';
}

/* Minimal formatted output through the serial service.
 * Supports %d, %x, %s, %c, %% — everything the shell commands need. */
static void ShellPrintf(const char *fmt, ...) {
    va_list ap;
    char    num[12];

    va_start(ap, fmt);
    for (const char *p = fmt; *p != '\0'; p++) {
        if (*p != '%') {
            ShellPutc(*p);
            continue;
        }
        p++;
        if (*p == '\0')
            break;
        /* Minimal flag/width support: %0Nd / %0Nx (zero-pad to N). */
        int zero = 0;
        int width = 0;
        if (*p == '0') {
            zero = 1;
            p++;
        }
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }
        switch (*p) {
        case '%':
            ShellPutc('%');
            break;
        case 'c':
            ShellPutc((char)va_arg(ap, int));
            break;
        case 's':
            ShellWrite(va_arg(ap, const char *));
            break;
        case 'd': {
            ShellItoa(va_arg(ap, int), num);
            int n = (int)strlen(num);
            while (zero && width > n) {
                ShellPutc('0');
                width--;
            }
            ShellWrite(num);
            break;
        }
        case 'x': {
            ShellUtoaHex(va_arg(ap, u32), num);
            int n = (int)strlen(num);
            while (zero && width > n) {
                ShellPutc('0');
                width--;
            }
            ShellWrite(num);
            break;
        }
        default:
            ShellPutc('%');
            ShellPutc(*p);
            break;
        }
    }
    va_end(ap);
}

/* ====================================================================
 * Line editor (keyboard service input)
 * ==================================================================== */

/* Build the prompt string: PS1 if set, else "opsys:<cwd>$ " (bash-style
 * cwd-aware default).  Shared by shell_loop and shell_redraw_line so the
 * on-screen prompt always matches the redraw cursor math. */
static void ShellPrompt(char *out, size_t outsz) {
    const char *ps1 = getenv("PS1");
    if (ps1 && ps1[0] != '\0') {
        strncpy(out, ps1, outsz - 1);
        out[outsz - 1] = '\0';
        return;
    }
    snprintf(out, outsz, "opsys:%s$ ", s_cwd);
}

/* Redraw the whole current line on screen (used after history recall /
 * tab completion, which rewrite the buffer in place).  Uses the term's
 * cursor API for reliable placement. */
static void ShellRedrawLine(const char *line, int pos) {
    char prompt[LINE_BUF_SIZE + 16];
    ShellPrompt(prompt, sizeof(prompt));
    /* DISPLAY widths (columns): CJK counts 2.  The terminal cursor API
     * and the erase run are column-based, so a UTF-8 prompt (e.g. a cwd
     * with Chinese) must be measured by width, not by byte length. */
    int pcols  = Utf8StrWidth(prompt, (int)strlen(prompt));
    int lwidth = Utf8StrWidth(line, (int)strlen(line));
    int pwidth = Utf8StrWidth(line, pos);

    /* Current row: query the term cursor, then set back to it after
     * re-printing (the shell's output cursor is on the prompt row). */
    u32 row = 0;
    if (s_term_port >= 0) {
        u32 req[2];
        u8  resp[16];
        req[0] = 7; /* TERM_OP_GET_CURSOR */
        req[1] = 0;
        int rlen = (int)sizeof(resp);
        if (IpcCall(s_term_port, req, 8, resp, &rlen) == 0 && rlen >= 12)
            row = ((u32 *)resp)[2];
    }

    /* Erase the current row and re-print prompt + line.  The extra
     * ERASE_MARGIN cells cover the row's right edge past the text
     * (leftovers from a longer previous line). */
    const int erase_margin = 4;
    ShellWrite("\r");
    for (int i = 0; i < pcols + lwidth + erase_margin; i++)
        ShellWrite(" ");
    ShellWrite("\r");
    ShellWrite(prompt);
    ShellWrite(line);

    /* Place the cursor at (pcols + pwidth, row) — columns, so wide CJK
     * characters keep the cursor aligned. */
    if (s_term_port >= 0) {
        u32 req[4];
        req[0] = 6; /* TERM_OP_SET_CURSOR */
        req[1] = 8;
        req[2] = (u32)(pcols + pwidth);
        req[3] = row;
        int resp_len = 8;
        u8  resp[8];
        (void)IpcCall(s_term_port, req, 8 + 8, resp, &resp_len);
    } else {
        int llen = (int)strlen(line);
        for (int i = llen; i > pos; i--)
            ShellWrite("\b");
    }
}

/* ====================================================================
 * Pinyin IME helpers
 *
 * The composition letters live IN the line buffer (typed normally), so
 * commit replaces the trailing pinyin bytes with the chosen candidate's
 * UTF-8 bytes.  Candidates are shown in the term status bar.
 * ==================================================================== */

static void ImeClearComposition(void) {
    s_ime_plen  = 0;
    s_ime_py[0] = '\0';
    s_ime_ncand = 0;
    s_ime_cidx  = 0;
}

/* Recompute the candidate list for the current composition. */
static void ImeRefreshCands(void) {
    s_ime_py[s_ime_plen] = '\0';
    s_ime_ncand          = ImeLookup(s_ime_py, s_ime_cands);
    if (s_ime_cidx >= s_ime_ncand)
        s_ime_cidx = 0;
}

/* Draw the IME mode + composition + candidates in the terminal's
 * status bar (TERM_OP_STATUS). */
static void ImeShowStatus(void) {
    if (s_term_port < 0)
        return;
    char msg[128];
    int  mlen;
    if (!s_ime_on) {
        mlen = snprintf(msg, sizeof(msg), "off (Ctrl+Space)");
    } else if (s_ime_plen == 0) {
        mlen = snprintf(msg, sizeof(msg), "on (Ctrl+Space) — pinyin");
    } else {
        int cn = snprintf(msg, sizeof(msg), "%s", s_ime_py);
        for (int i = 0; i < s_ime_ncand && cn < (int)sizeof(msg) - 8; i++) {
            int add = snprintf(msg + cn, (size_t)(sizeof(msg) - cn),
                               " %d%s", i + 1, s_ime_cands[i]);
            if (add <= 0)
                break;
            cn += add;
        }
        mlen = cn;
    }
    if (mlen < 0)
        mlen = 0;
    if (mlen > (int)sizeof(msg) - 1)
        mlen = (int)sizeof(msg) - 1;
    static const char prefix[] = "IME";
    const int         plen     = 3;
    u8                req[16 + 128];
    u32              *hdr = (u32 *)req;
    hdr[0] = TERM_OP_STATUS;
    hdr[1] = (u32)(8 + plen + mlen);
    hdr[2] = (u32)plen;
    hdr[3] = (u32)mlen;
    memcpy(req + 16, prefix, (size_t)plen);
    memcpy(req + 16 + plen, msg, (size_t)mlen);
    u32 resp[1];
    int resp_len = (int)sizeof(resp);
    (void)IpcCall(s_term_port, req, 16 + plen + mlen, resp, &resp_len);
}

/* Commit candidate k: replace the composition letters (sitting in the
 * line at [pos - plen, pos)) with the candidate's UTF-8 bytes. */
static int ImeCommit(int k, char *buf, int pos, int maxlen) {
    if (s_ime_plen == 0 || k < 0 || k >= s_ime_ncand)
        return pos;
    int plen = s_ime_plen;
    if (pos < plen)
        return pos;
    const char *txt  = s_ime_cands[k];
    int         tlen = Utf8SeqLen(txt);
    if (tlen <= 0)
        return pos;
    int l = (int)strlen(buf);
    if (l - plen + tlen >= maxlen)
        return pos;
    memmove(buf + (pos - plen) + tlen, buf + pos, (size_t)(l - pos) + 1);
    memcpy(buf + (pos - plen), txt, (size_t)tlen);
    int npos = pos - plen + tlen;
    /* Serial debug via libc printf (SYS_DEBUG_LOG): the shell's screen
     * is not mirrored to serial, so this is how tests verify that a
     * pinyin composition committed the right code point.  The
     * candidate pointer is NOT NUL-terminated at the character
     * boundary, so print through a bounded copy. */
    {
        uint32_t cp = 0;
        (void)Utf8Decode(txt, &cp);
        char tmp[5];
        memcpy(tmp, txt, (size_t)tlen);
        tmp[tlen] = '\0';
        printf("ime: commit '%s' U+%04X\n", tmp, (unsigned)cp);
    }
    ImeClearComposition();
    ImeShowStatus();
    return npos;
}

/* ime [on|off] — show or change the pinyin IME mode (also Ctrl+Space). */
static int CmdIme(int argc, char *argv[]) {
    if (argc >= 2) {
        if (strcmp(argv[1], "on") == 0) {
            s_ime_on = 1;
        } else if (strcmp(argv[1], "off") == 0) {
            s_ime_on = 0;
            ImeClearComposition();
        } else {
            ShellWrite("Usage: ime [on|off]\n");
            return -1;
        }
    }
    ShellPrintf("IME %s (Ctrl+Space toggles; pinyin + Space/digits 1-9)\n",
                 s_ime_on ? "on" : "off");
    ImeShowStatus();
    return 0;
}

/* Byte offset of the start of the word containing/left of pos.  A word
 * is a run of non-space code points; both bounds move by whole UTF-8
 * characters so CJK words are never split. */
static int ShellWordStart(const char *buf, int pos) {
    int p = pos;
    while (p > 0 && buf[p - 1] == ' ')
        p = Utf8Prev(buf, p);
    while (p > 0 && buf[p - 1] != ' ')
        p = Utf8Prev(buf, p);
    return p;
}

/* Byte offset of the first code point after the word at/right of pos. */
static int ShellWordEnd(const char *buf, int pos, int len) {
    int p = pos;
    while (p < len && buf[p] != ' ')
        p = Utf8Next(buf, p, len);
    while (p < len && buf[p] == ' ')
        p = Utf8Next(buf, p, len);
    return p;
}

/* Any cursor-movement / line-rewriting edit commits (drops) the pinyin
 * composition so the "letters at [pos-plen, pos)" invariant stays true. */
static void ImeDiscardOnEdit(void) {
    if (s_ime_plen > 0) {
        ImeClearComposition();
        ImeShowStatus();
    }
}

/* Tab completion: first token -> command names; otherwise a path
 * fragment -> directory entries (absolute or cwd-relative).  Completes
 * to the longest common prefix; when no unique prefix, lists matches.
 * Returns 1 if the buffer changed. */
static void FmStrncpyUtf8(char *dst, const char *src, int cap); /* below */
static int ShellComplete(char *buf, int *pos, int maxlen) {
    int tok_start = *pos;
    while (tok_start > 0 && buf[tok_start - 1] != ' ')
        tok_start--;
    int is_first = 1;
    for (int i = 0; i < tok_start; i++) {
        if (buf[i] != ' ')
            is_first = 0;
    }
    const char *tok    = buf + tok_start;
    int         toklen = *pos - tok_start;

    if (is_first) {
        /* Command completion. */
        static const char *matches[COMPLETE_MAX_MATCHES];
        int                nm = 0, common = -1;
        for (cmd_node_t *n = s_cmd_head; n; n = n->next) {
            if (strncmp(n->name, tok, (size_t)toklen) == 0) {
                if (nm < COMPLETE_MAX_MATCHES)
                    matches[nm++] = n->name;
                if (common < 0)
                    common = (int)strlen(n->name);
                else {
                    int k = 0;
                    while (k < common && matches[0][k] == n->name[k])
                        k++;
                    common = k;
                }
            }
        }
        if (nm == 1) {
            int need = common + 1;
            if (tok_start + need < maxlen) {
                int oldpos = *pos;
                int tail   = (int)strlen(buf);
                int shift  = (common - toklen) + 1; /* new token + space */
                if (tail + shift < maxlen) {
                    /* Preserve the text after the completed token
                     * (mid-line completion must not truncate the line). */
                    memmove(buf + oldpos + shift, buf + oldpos,
                            (size_t)(tail - oldpos) + 1);
                    for (int i = 0; i < common; i++)
                        buf[tok_start + i] = matches[0][i];
                    buf[tok_start + common] = ' ';
                    *pos = tok_start + common + 1;
                }
                ShellRedrawLine(buf, *pos);
                return 1;
            }
        } else if (nm > 1 && common > toklen) {
            int oldpos = *pos;
            int tail   = (int)strlen(buf);
            int shift  = common - toklen;
            if (tail + shift < maxlen) {
                memmove(buf + oldpos + shift, buf + oldpos,
                        (size_t)(tail - oldpos) + 1);
                for (int i = 0; i < common; i++)
                    buf[tok_start + i] = matches[0][i];
                *pos = tok_start + common;
            }
            ShellRedrawLine(buf, *pos);
            return 1;
        } else if (nm > 1) {
            ShellWrite("\n");
            for (int i = 0; i < nm; i++) {
                ShellWrite("  ");
                ShellWrite(matches[i]);
                ShellWrite("\n");
            }
            ShellRedrawLine(buf, *pos);
        }
        return 0;
    }

    /* Path completion. */
    char dir[LINE_BUF_SIZE];
    char frag[LINE_BUF_SIZE];
    const char *slash = NULL;
    for (const char *p = tok; *p; p++)
        if (*p == '/')
            slash = p;
    if (slash) {
        int dlen = (int)(slash - tok);
        if (dlen == 0) {
            strncpy(dir, "/", sizeof(dir) - 1);
            dir[sizeof(dir) - 1] = '\0';
        } else {
            char tmp[LINE_BUF_SIZE];
            memcpy(tmp, tok, (size_t)dlen);
            tmp[dlen] = '\0';
            if (ShellResolvePath(tmp, dir, sizeof(dir)) < 0)
                return 0;
        }
        strncpy(frag, slash + 1, sizeof(frag) - 1);
        frag[sizeof(frag) - 1] = '\0';
    } else {
        if (ShellResolvePath("", dir, sizeof(dir)) < 0)
            return 0;
        strncpy(frag, tok, sizeof(frag) - 1);
        frag[sizeof(frag) - 1] = '\0';
    }

    static vfs_enum_batch_t batch;
    static char matches[COMPLETE_MAX_MATCHES][256];
    int                    nm = 0, common = -1;
    vfs_handle_t           e;
    int                    r = FsEnumBegin(dir, &e);
    if (r < 0)
        return 0;
    for (;;) {
        r = FsEnumNext(e, &batch);
        if (r < 0 || batch.batch_count == 0)
            break;
        for (u32 i = 0; i < batch.batch_count && nm < COMPLETE_MAX_MATCHES; i++) {
            if (strncmp(batch.batch[i], frag, strlen(frag)) == 0) {
                /* UTF-8-safe copy: a long CJK name is not cut mid-char. */
                FmStrncpyUtf8(matches[nm], batch.batch[i], 256);
                if (common < 0)
                    common = (int)strlen(matches[nm]);
                else {
                    int k = 0;
                    while (k < common && matches[0][k] == matches[nm][k])
                        k++;
                    common = k;
                }
                nm++;
            }
        }
    }
    FsEnumEnd(e);

    if (nm == 1 || (nm > 1 && common > (int)strlen(frag))) {
        if (tok_start + common < maxlen) {
            int oldpos = *pos;
            int tail   = (int)strlen(buf);
            int shift  = common - (int)strlen(frag);
            if (tail + shift < maxlen) {
                memmove(buf + oldpos + shift, buf + oldpos,
                        (size_t)(tail - oldpos) + 1);
                for (int i = 0; i < common; i++)
                    buf[tok_start + i] = matches[0][i];
                *pos = tok_start + common;
            }
            ShellRedrawLine(buf, *pos);
        }
        return 1;
    }
    if (nm > 1) {
        ShellWrite("\n");
        for (int i = 0; i < nm; i++) {
            ShellWrite("  ");
            ShellWrite(matches[i]);
            ShellWrite("\n");
        }
        ShellRedrawLine(buf, *pos);
    }
    return 0;
}

/*
 * Read one line from keyboard input.
 * Uses a blocking READ_BLOCK: the calling thread parks inside ipc_call
 * until the keyboard service delivers key bytes (the service parks the
 * call and the IRQ thread completes it when PS/2 scancodes arrive), so
 * there is no poll/yield busy loop.  Echoes characters, handles
 * backspace and basic editing.  Returns the number of characters read
 * (excluding null terminator), or -1 on error.
 */
static int ReadLineImpl(char *buf, int maxlen, int mask);

static int ReadLine(char *buf, int maxlen) {
    return ReadLineImpl(buf, maxlen, 0);
}

/* Password entry: echo '*' instead of the typed characters. */
static int ReadLineMasked(char *buf, int maxlen) {
    return ReadLineImpl(buf, maxlen, 1);
}

/* Shared line reader: mask=1 echoes '*' (password entry). */
static int ReadLineImpl(char *buf, int maxlen, int mask) {
    int pos = 0;
    u32 req[2 + 1];  /* { op; len }        */
    u32 resp[1 + 8]; /* { ret; data[32] }  */

    /* The caller's buffer is reused across commands (shell_loop's
     * `line`): start NUL-terminated and keep it that way after every
     * edit, or a short second command would carry the tail of the
     * previous one ("w" typed after "xyz" becoming "wxyz"). */
    buf[0] = '\0';

    for (;;) {
        req[0]       = KBD_OP_READ_BLOCK;
        req[1]       = KBD_CHUNK;
        int resp_len = (int)sizeof(resp);
        int ret      = IpcCall(s_kbd_port, (const void *)req, 8, (void *)resp, &resp_len);
        if (ret < 0)
            return -1;
        i32 n = (i32)resp[0];
        if (n < 0)
            return -1;
        if (n > KBD_CHUNK)
            n = KBD_CHUNK;
        if (n == 0) {
            /* Defensive only: the service replies 0 when the single
             * blocking-read slot is already parked (never in practice).
             * Yield instead of hot-spinning. */
            ThreadYield();
            continue;
        }

        for (int i = 0; i < n; i++) {
            unsigned char ch = (unsigned char)((u8 *)resp)[4 + i];

            /* Any editing key other than the composition keys (letters,
             * space/digits/Enter while composing, backspace, the IME
             * toggle) drops the pinyin composition — that keeps the
             * "composition letters at [pos - plen, pos)" invariant that
             * ime_commit relies on (cursor movement / line rewrites
             * would otherwise break the byte-offset correspondence). */
            if (s_ime_plen > 0 && s_ime_on && !mask &&
                !(ch >= 'a' && ch <= 'z') && ch != ' ' &&
                !(ch >= '1' && ch <= '9') && ch != '\b' && ch != 0x7F &&
                ch != 0x80 && ch != '\r' && ch != '\n')
                ImeDiscardOnEdit();

            switch (ch) {
            case '\r':
            case '\n':
                /* IME Enter: commit the current candidate into the line
                 * and keep editing — press Enter again to run it. */
                if (s_ime_plen > 0 && s_ime_ncand > 0) {
                    pos = ImeCommit(s_ime_cidx, buf, pos, maxlen);
                    ShellRedrawLine(buf, pos);
                    break;
                }
                /* Enter — terminate line */
                if (pos == 0) {
                    /* Stray Enter on an empty line: a '\n' that was
                     * buffered in the keyboard RX ring while the shell
                     * was busy executing a previous command (no READ
                     * parked, e.g. during spawn's multi-second run) and
                     * is delivered after the command finishes.  Echoing
                     * it and returning 0 would make shell_loop re-print
                     * the prompt, producing a spurious empty 'opsys$'
                     * line (the phantom prompt).  Skip it silently. */
                    break;
                }
                ShellWrite("\r\n");
                s_hist_view = -1; /* back to live editing */
                /* Commit the FULL buffer: never NUL-cut at the cursor
                 * (pos) — after a mid-line insert/delete pos != strlen,
                 * and truncating there would chop the tail off the
                 * recalled/edited command.  The buffer is already
                 * NUL-terminated at its true end. */
                return (int)strlen(buf);

            case '\b':
            case 0x7F: /* DEL — Backspace */
                if (pos > 0) {
                    /* While composing pinyin, the backspace also pops
                     * one letter off the composition. */
                    if (s_ime_plen > 0) {
                        s_ime_plen--;
                        ImeRefreshCands();
                        ImeShowStatus();
                    }
                    /* Back up over a whole UTF-8 code point so a
                     * multi-byte character is never left half-deleted. */
                    int prev = Utf8Prev(buf, pos);
                    int del  = pos - prev;
                    if (prev < (int)strlen(buf)) {
                        /* Deleting in the middle: shift the tail left. */
                        for (int k = prev; buf[k] != '\0'; k++)
                            buf[k] = buf[k + del];
                        pos = prev;
                        ShellRedrawLine(buf, pos);
                    } else {
                        /* Erase at end of line: one narrow column, or
                         * two for a wide (CJK) character. */
                        uint32_t cp;
                        (void)Utf8Decode(buf + prev, &cp);
                        if (Utf8CharWidth(cp) == 2)
                            ShellWrite("\b  \b");
                        else
                            ShellWrite("\b \b");
                        buf[prev] = '\0';
                        pos = prev;
                    }
                }
                break;

            case '\t':
                /* Tab — command / path completion (v1.3). */
                (void)ShellComplete(buf, &pos, maxlen);
                break;

            case 0x0B: /* Up arrow — history back */
                if (s_hist_count > 0) {
                    if (s_hist_view < 0)
                        s_hist_view = (s_hist_next + HIST_MAX - 1) % HIST_MAX;
                    else
                        s_hist_view = (s_hist_view + HIST_MAX - 1) % HIST_MAX;
                    /* Stop at the oldest entry. */
                    int oldest = (s_hist_next + HIST_MAX - s_hist_count) % HIST_MAX;
                    if (s_hist_view == (s_hist_next + HIST_MAX - 1) % HIST_MAX &&
                        s_hist_count == 1) {
                        /* single entry: stay */
                    }
                    /* Clamp: if we wrapped past the oldest, go back down. */
                    int wrapped = 0;
                    if (oldest < (s_hist_next + HIST_MAX - 1) % HIST_MAX) {
                        if (s_hist_view < oldest || s_hist_view > (s_hist_next + HIST_MAX - 1) % HIST_MAX)
                            wrapped = 1;
                    } else {
                        if (s_hist_view < oldest && s_hist_view >= (s_hist_next + HIST_MAX - 1) % HIST_MAX)
                            wrapped = 1;
                    }
                    if (wrapped)
                        s_hist_view = oldest;
                    strncpy(buf, s_history[s_hist_view], (size_t)maxlen - 1);
                    buf[maxlen - 1] = '\0';
                    pos = (int)strlen(buf);
                    ShellRedrawLine(buf, pos);
                }
                break;

            case 0x0C: /* Down arrow — history forward / live edit */
                if (s_hist_view >= 0) {
                    s_hist_view = (s_hist_view + 1) % HIST_MAX;
                    /* Wrapped past newest -> back to live editing. */
                    if (s_hist_view == s_hist_next) {
                        s_hist_view = -1;
                        buf[0] = '\0';
                        pos    = 0;
                    } else {
                        /* Skip gaps beyond count. */
                        int oldest = (s_hist_next + HIST_MAX - s_hist_count) % HIST_MAX;
                        if (s_hist_view == (oldest + HIST_MAX - 1) % HIST_MAX && s_hist_count == 1) {
                            /* only one entry: back to live */
                            s_hist_view = -1;
                            buf[0] = '\0';
                            pos    = 0;
                        } else {
                            strncpy(buf, s_history[s_hist_view], (size_t)maxlen - 1);
                            buf[maxlen - 1] = '\0';
                            pos = (int)strlen(buf);
                        }
                    }
                    ShellRedrawLine(buf, pos);
                }
                break;

            case 0x01: /* Home */
                pos = 0;
                ShellRedrawLine(buf, pos);
                break;

            case 0x05: /* End */
                pos = (int)strlen(buf);
                ShellRedrawLine(buf, pos);
                break;

            case 0x02: /* PgUp — first history entry */
                if (s_hist_count > 0) {
                    s_hist_view = (s_hist_next + HIST_MAX - s_hist_count) % HIST_MAX;
                    strncpy(buf, s_history[s_hist_view], (size_t)maxlen - 1);
                    buf[maxlen - 1] = '\0';
                    pos = (int)strlen(buf);
                    ShellRedrawLine(buf, pos);
                }
                break;

            case 0x06: /* PgDn — newest history entry */
                if (s_hist_count > 0) {
                    s_hist_view = (s_hist_next + HIST_MAX - 1) % HIST_MAX;
                    strncpy(buf, s_history[s_hist_view], (size_t)maxlen - 1);
                    buf[maxlen - 1] = '\0';
                    pos = (int)strlen(buf);
                    ShellRedrawLine(buf, pos);
                }
                break;

            case 0x10: /* Left arrow (DLE) — move cursor left by one
                        * code point (skips UTF-8 continuation bytes) */
                if (pos > 0) {
                    pos = Utf8Prev(buf, pos);
                    ShellRedrawLine(buf, pos);
                }
                break;

            case 0x14: /* Right arrow (DC4) — move cursor right by one
                        * code point */
                if (pos < (int)strlen(buf)) {
                    pos = Utf8Next(buf, pos, (int)strlen(buf));
                    ShellRedrawLine(buf, pos);
                }
                break;

            /* ---- Ctrl / Alt editing keys (0x80+/0xE0+ bands) ---- */
            case 0x81: /* Ctrl-A — line start (same as Home) */
                pos = 0;
                ShellRedrawLine(buf, pos);
                break;
            case 0x85: /* Ctrl-E — line end (same as End) */
                pos = (int)strlen(buf);
                ShellRedrawLine(buf, pos);
                break;
            case 0x91: { /* Ctrl-U — kill to line start */
                int l = (int)strlen(buf);
                for (int k = pos; k < l; k++)
                    buf[k - pos] = buf[k];
                buf[l - pos] = '\0';
                pos = 0;
                ShellRedrawLine(buf, pos);
                break;
            }
            case 0x8B: /* Ctrl-K — kill to line end */
                buf[pos] = '\0';
                ShellRedrawLine(buf, pos);
                break;
            case KBD_CTRL_SPACE: /* Ctrl+Space — toggle pinyin IME */
                s_ime_on = !s_ime_on;
                ImeClearComposition();
                ImeShowStatus();
                ShellRedrawLine(buf, pos);
                break;
            case 0x97: { /* Ctrl-W — delete previous word (UTF-8 aware) */
                int p = ShellWordStart(buf, pos);
                for (int k = pos; buf[k] != '\0'; k++)
                    buf[p + (k - pos)] = buf[k];
                buf[p + ((int)strlen(buf) - pos)] = '\0';
                pos = p;
                ShellRedrawLine(buf, pos);
                break;
            }
            case 0x8C: /* Ctrl-L — clear screen, redraw line */
                ShellWrite("\033[2J\033[H");
                ShellRedrawLine(buf, pos);
                break;
            case 0x83: /* Ctrl-C — cancel the current line */
                ShellWrite("^C\r\n");
                buf[0] = '\0';
                pos    = 0;
                s_hist_view = -1;
                break;
            case 0x84: /* Ctrl-D — EOF on an empty line exits the shell */
                if (buf[0] == '\0') {
                    ShellWrite("\r\n");
                    return -1;
                }
                break;
            case 0xE2: { /* Alt-B — move back one word (UTF-8 aware) */
                pos = ShellWordStart(buf, pos);
                ShellRedrawLine(buf, pos);
                break;
            }
            case 0xE6: { /* Alt-F — move forward one word (UTF-8 aware) */
                pos = ShellWordEnd(buf, pos, (int)strlen(buf));
                ShellRedrawLine(buf, pos);
                break;
            }
            case 0xE4: { /* Alt-D — delete word after cursor (UTF-8 aware) */
                int l = (int)strlen(buf);
                int p = ShellWordEnd(buf, pos, l);
                for (int k = p; buf[k] != '\0'; k++)
                    buf[pos + (k - p)] = buf[k];
                buf[pos + (l - p)] = '\0';
                ShellRedrawLine(buf, pos);
                break;
            }

            default:
                if (ch >= ' ' && ch < 0x7F) {
                    /* ---- Pinyin IME interception (not for passwords) ---- */
                    if (s_ime_on && !mask) {
                        if (s_ime_plen > 0 && ch >= '1' && ch <= '9') {
                            /* digit 1-9 selects candidate N */
                            if (s_ime_ncand > ch - '1') {
                                pos = ImeCommit(ch - '1', buf, pos, maxlen);
                                ShellRedrawLine(buf, pos);
                                break;
                            }
                            /* candidate N absent: the digit is literal */
                            ImeClearComposition();
                            ImeShowStatus();
                        } else if (s_ime_plen > 0 && ch == ' ') {
                            /* space commits the first candidate */
                            if (s_ime_ncand > 0) {
                                pos = ImeCommit(0, buf, pos, maxlen);
                                ShellRedrawLine(buf, pos);
                                break;
                            }
                            /* no candidate: the space is literal */
                            ImeClearComposition();
                            ImeShowStatus();
                        } else if (ch >= 'a' && ch <= 'z') {
                            /* Only accumulate letters while they stay a
                             * valid pinyin PREFIX; once the run stops
                             * matching (English words like "tee" or
                             * "txt"), the composition is over and the
                             * letters are plain text. */
                            if (s_ime_plen < IME_MAX_PINYIN - 1) {
                                s_ime_py[s_ime_plen]     = (char)ch;
                                s_ime_py[s_ime_plen + 1] = '\0';
                                if (ImePrefix(s_ime_py)) {
                                    s_ime_plen++;
                                    ImeRefreshCands();
                                    ImeShowStatus();
                                } else {
                                    ImeClearComposition();
                                    ImeShowStatus();
                                }
                            }
                            /* fall through: insert the letter normally */
                        } else if (s_ime_plen > 0) {
                            /* other printable: drop the composition, then
                             * insert the key normally (the pinyin letters
                             * already in the buffer stay as literal text) */
                            ImeClearComposition();
                            ImeShowStatus();
                        }
                    }
                    /* ---- normal insert ---- */
                    int l = (int)strlen(buf);
                    if (pos < maxlen - 1 && pos <= l) {
                        if (pos < l) {
                            /* Insert in the middle: shift the tail right
                             * and redraw — a bare echo would OVERWRITE
                             * the following characters (the v1.3 "can't
                             * edit a recalled command" bug). */
                            for (int k = l + 1; k > pos; k--)
                                buf[k] = buf[k - 1];
                            buf[pos] = (char)ch;
                            pos++;
                            ShellRedrawLine(buf, pos);
                        } else {
                            buf[pos++] = (char)ch;
                            /* The function name is the mask flag: echo '*'
                             * for passwords, the char itself otherwise. */
                            ShellPutc(mask ? '*' : (char)ch);
                        }
                        buf[pos] = '\0';
                    }
                    /* else: buffer full, silently ignore */
                }
                /* Non-printable: ignore */
                break;
            }
        }
    }
}

/* ====================================================================
 * Command parser
 * ==================================================================== */

static int ParseLine(char *line, char *argv[], int max_args) {
    int   argc = 0;
    char *p    = line;

    while (*p != '\0' && argc < max_args) {
        /* Skip leading whitespace */
        while (*p == ' ' || *p == '\t')
            p++;

        if (*p == '\0')
            break;

        /* Check for quoted string */
        if (*p == '"') {
            p++; /* skip opening quote */
            argv[argc++] = p;
            while (*p != '\0' && *p != '"')
                p++;
            if (*p == '"')
                *p++ = '\0'; /* terminate and skip closing quote */
        } else {
            argv[argc++] = p;
            while (*p != '\0' && *p != ' ' && *p != '\t')
                p++;
            if (*p != '\0')
                *p++ = '\0';
        }
    }

    return argc;
}

/* Forward declaration */
static void WorkerFunc(void *arg);

/* ====================================================================
 * Command execution
 * ==================================================================== */

static int Execute(char *line) {
    char *argv[MAX_ARGS];
    int   argc = ParseLine(line, argv, MAX_ARGS);

    if (argc == 0)
        return 0;

    /* Policy gate (v0.5): a DENY verdict blocks execution. */
    if (CmdFilterDenied(argv[0])) {
        ShellPrintf("shell: command '%s' denied by policy\n", argv[0]);
        return -9; /* ERR_DENIED */
    }

    /* v0.5: resolve relative paths for VFS commands against s_cwd.
     * Commands whose FIRST argument is a path: ls/cat/tee/mkdir/rm/
     * stat/fallocate/mv(src).  bm_create also takes a path.  "cd" and
     * "pwd" handle their own resolution; everything else is untouched.
     * The rewrite happens in place on the parsed argv[] strings. */
    {
        static char pbuf[2][LINE_BUF_SIZE];
        int         n = 0;
        int         is_path_cmd =
            strcmp(argv[0], "ls") == 0 || strcmp(argv[0], "cat") == 0 ||
            strcmp(argv[0], "tee") == 0 || strcmp(argv[0], "mkdir") == 0 ||
            strcmp(argv[0], "rm") == 0 || strcmp(argv[0], "stat") == 0 ||
            strcmp(argv[0], "fallocate") == 0 || strcmp(argv[0], "mv") == 0 ||
            strcmp(argv[0], "bm_create") == 0;
        if (is_path_cmd && argc >= 2) {
            for (int i = 1; i < argc && n < 2; i++) {
                /* Skip mv's optional 3rd arg (new name) — it is not a
                 * path.  bm_create's 2nd arg is an access mode.  tee's
                 * 2nd arg is the TEXT to write, never a path. */
                if (strcmp(argv[0], "mv") == 0 && i >= 3)
                    break;
                if (strcmp(argv[0], "bm_create") == 0 && i >= 2)
                    break;
                if (strcmp(argv[0], "tee") == 0 && i >= 2)
                    break;
                if (argv[i][0] != '/' && argv[i][0] != '\0') {
                    if (ShellResolvePath(argv[i], pbuf[n], sizeof(pbuf[n])) == 0) {
                        argv[i] = pbuf[n];
                        n++;
                    }
                }
            }
        } else if (strcmp(argv[0], "ls") == 0) {
            /* v0.7.1: with no path argument, list the CURRENT
             * directory instead of demanding a path.  (stat keeps its
             * existing no-arg default: both configured volumes.) */
            if (argc == 1) {
                argv[1] = s_cwd;
                argc    = 2;
            }
        }
    }

    /* Look up command in the runtime-registered list */
    for (cmd_node_t *n = s_cmd_head; n != NULL; n = n->next) {
        if (strcmp(argv[0], n->name) == 0)
            return n->func(argc, argv);
    }

    ShellPrintf("shell: unknown command '%s' (try 'help')\n", argv[0]);
    return -1;
}

/* ====================================================================
 * Main loop
 * ==================================================================== */

static void ShellLoop(void) {
    /* Banner (ASCII only - UTF-8 box chars break VGA/vc terminals) */
    ShellWrite("\n");
    ShellWrite("Welcome to OpSys \n");
    ShellWrite("  Copyright (c) 2026 OpSys Project \n");
    ShellWrite("  shell.c - Simple terminal shell (TTY-like) \n");
    ShellWrite("  Type 'help' for a command list. \n");
    ShellWrite("\n");

    char line[LINE_BUF_SIZE];

    for (;;) {
        /* Prompt: PS1 override, else "opsys:<cwd>$ " (bash-style).
         * Environment is user preference only; it never carries
         * security policy. */
        char prompt[LINE_BUF_SIZE + 16];
        ShellPrompt(prompt, sizeof(prompt));
        ShellWrite(prompt);
        int len = ReadLine(line, LINE_BUF_SIZE);
        if (len < 0) {
            ThreadYield(); /* serial service unavailable */
            continue;
        }
        if (len > 0) {
            /* Record in the history ring (skip duplicate of the last). */
            int dup = (s_hist_count > 0 &&
                       strcmp(s_history[(s_hist_next + HIST_MAX - 1) % HIST_MAX], line) == 0);
            if (!dup) {
                strncpy(s_history[s_hist_next], line, HIST_LEN - 1);
                s_history[s_hist_next][HIST_LEN - 1] = '\0';
                s_hist_next = (s_hist_next + 1) % HIST_MAX;
                if (s_hist_count < HIST_MAX)
                    s_hist_count++;
            }
            Execute(line);
        }
    }
}

/* ====================================================================
 * Built-in commands
 * ==================================================================== */

static int CmdHelp(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    ShellWrite("Available commands:\n");
    for (cmd_node_t *n = s_cmd_head; n != NULL; n = n->next) {
        ShellWrite("  ");
        ShellWrite(n->name);
        int pad = 12 - (int)strlen(n->name); /* %-12s */
        while (pad > 0) {
            ShellPutc(' ');
            pad--;
        }
        ShellWrite("  ");
        ShellWrite(n->help);
        ShellWrite("\n");
    }
    return 0;
}

static int CmdEcho(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (i > 1)
            ShellWrite(" ");
        ShellWrite(argv[i]);
    }
    ShellWrite("\n");
    return 0;
}

/* export NAME=value | export NAME — set an environment variable (or
 * print its current value).  The environment carries per-process user
 * preferences (PS1/EDITOR/LANG); it never carries security policy. */
static int CmdExport(int argc, char *argv[]) {
    if (argc < 2) {
        ShellWrite("Usage: export NAME=value | export NAME\n");
        return -1;
    }
    const char *arg = argv[1];
    char       *eq  = strchr(arg, '=');
    if (eq) {
        char saved = *eq;
        *eq        = '\0';
        int r      = Setenv(arg, eq + 1, 1);
        *eq        = saved;
        if (r < 0) {
            ShellPrintf("export: Setenv(%s) failed\n", arg);
            return -1;
        }
        ShellPrintf("export: %s\n", arg);
        return 0;
    }
    const char *v = getenv(arg);
    if (v)
        ShellPrintf("%s=%s\n", arg, v);
    else
        ShellPrintf("%s: not set\n", arg);
    return 0;
}

/* unset NAME — remove an environment variable. */
static int CmdUnset(int argc, char *argv[]) {
    if (argc < 2) {
        ShellWrite("Usage: unset NAME\n");
        return -1;
    }
    int r = Unsetenv(argv[1]);
    if (r < 0) {
        ShellPrintf("unset: invalid name '%s'\n", argv[1]);
        return -1;
    }
    ShellPrintf("unset: %s\n", argv[1]);
    return 0;
}

/* env — print the whole environment, one "NAME=value" per line. */
static int CmdEnv(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    if (!environ) {
        ShellWrite("env: (empty)\n");
        return 0;
    }
    for (char **e = environ; *e; e++) {
        ShellWrite(*e);
        ShellWrite("\n");
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  cd / pwd (v0.5)                                                   */
/* ------------------------------------------------------------------ */

/* Normalize an ABSOLUTE path in place: collapse "//", drop "." and
 * resolve ".." segments (clamped at the root).  "cd .." from "/Disk/d1"
 * yields "/Disk", "cd ." stays put.  The result never ends in '/' except
 * for the root itself.  Writes into out (LINE_BUF_SIZE). */
static void PathNormalize(const char *in, char *out, size_t outsz) {
    /* Segment buffer matches the VFS name limit (256), so a long UTF-8
     * name(up to 85 CJK chars on the mem volume) is never silently
     * dropped — the old 64-byte cap cut every ≥64-byte segment and
     * silently resolved such paths to the PARENT directory. */
    enum { PN_MAX_DEPTH = 24, PN_SEG_MAX = 256 };
    static char segs[PN_MAX_DEPTH][PN_SEG_MAX];
    int         n = 0;

    const char *p = in;
    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        const char *start = p;
        while (*p && *p != '/')
            p++;
        int len = (int)(p - start);
        if (len == 1 && start[0] == '.')
            continue; /* ".": stay */
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (n > 0)
                n--; /* "..": up one level (clamped at root) */
            continue;
        }
        if (n < PN_MAX_DEPTH && len < PN_SEG_MAX) {
            memcpy(segs[n], start, (size_t)len);
            segs[n][len] = '\0';
            n++;
        } else {
            /* Too deep or a segment beyond the cap: keep the caller's
             * bytes (no silent drop) — copy what fits so the path still
             * resolves to something deterministic. */
            if (n < PN_MAX_DEPTH) {
                int clen = (len < PN_SEG_MAX - 1) ? len : PN_SEG_MAX - 1;
                memcpy(segs[n], start, (size_t)clen);
                segs[n][clen] = '\0';
                n++;
            }
        }
    }

    size_t o = 0;
    if (n == 0) {
        out[0] = '/';
        out[1] = '\0';
        return;
    }
    for (int i = 0; i < n; i++) {
        size_t sl = strlen(segs[i]);
        if (o + sl + 2 > outsz) {
            /* Out of room: fit what we can, cut at a UTF-8 character
             * boundary (never split a multi-byte name at the edge), and
             * stop appending. */
            char tmp[PN_SEG_MAX];
            FmStrncpyUtf8(tmp, segs[i], (int)(outsz - o));
            size_t cl = strlen(tmp);
            if (cl > 0) {
                out[o++] = '/';
                memcpy(out + o, tmp, cl);
                o += cl;
            }
            break;
        }
        out[o++] = '/';
        memcpy(out + o, segs[i], sl);
        o += sl;
    }
    out[o] = '\0';
}

/* Resolve a user-supplied path against s_cwd into a canonical URL.
 *   - "/" or empty  -> s_cwd itself (volume-list view)
 *   - starting with "/" -> absolute path
 *   - otherwise -> s_cwd + "/" + path (relative)
 * Every result is normalized (".", "..", "//" resolved) — so "cd ..",
 * "cd .", "ls ../x", "cd /Disk/d1/.." all behave like a real shell.
 * Writes into out (LINE_BUF_SIZE).  Returns 0 on success. */
static int ShellResolvePath(const char *path, char *out, size_t outsz) {
    char raw[LINE_BUF_SIZE];
    if (!path || path[0] == '\0') {
        strncpy(raw, s_cwd, sizeof(raw) - 1);
        raw[sizeof(raw) - 1] = '\0';
    } else if (path[0] == '/') {
        strncpy(raw, path, sizeof(raw) - 1);
        raw[sizeof(raw) - 1] = '\0';
    } else {
        /* Relative: compose s_cwd + "/" + path. */
        int need = (int)strlen(s_cwd) + 1 + (int)strlen(path) + 1;
        if ((size_t)need > (int)sizeof(raw)) {
            ShellPrintf("path: too long\n");
            return -1;
        }
        if (strcmp(s_cwd, "/") == 0) {
            snprintf(raw, sizeof(raw), "/%s", path);
        } else {
            snprintf(raw, sizeof(raw), "%s/%s", s_cwd, path);
        }
    }
    PathNormalize(raw, out, outsz);
    return 0;
}

/* cd [dir] — change the working directory.  No argument -> "/".  The
 * target must exist and be a directory (fs_get_item + type check). */
static int CmdCd(int argc, char *argv[]) {
    const char *arg = (argc >= 2) ? argv[1] : "/";

    char url[LINE_BUF_SIZE];
    if (ShellResolvePath(arg, url, sizeof(url)) < 0)
        return -1;

    /* Normalize trailing "/" (e.g. "cd /Volumes/" -> "/Volumes"). */
    size_t ulen = strlen(url);
    while (ulen > 1 && url[ulen - 1] == '/')
        url[--ulen] = '\0';

    /* "/" (volume view) is always valid. */
    if (strcmp(url, "/") == 0 || strcmp(url, "") == 0) {
        strncpy(s_cwd, "/", sizeof(s_cwd) - 1);
        s_cwd[sizeof(s_cwd) - 1] = '\0';
        return 0;
    }

    vfs_item_info_t info;
    int             r = FsGetItem(url, &info);
    if (r < 0) {
        ShellPrintf("cd: %s FAILED (%d)\n", url, r);
        return -1;
    }
    if (info.type != VFS_ITEM_DIR) {
        ShellPrintf("cd: %s: not a directory\n", url);
        return -1;
    }
    strncpy(s_cwd, url, sizeof(s_cwd) - 1);
    s_cwd[sizeof(s_cwd) - 1] = '\0';
    return 0;
}

/* pwd — print the working directory. */
static int CmdPwd(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    ShellWrite(s_cwd);
    ShellWrite("\n");
    return 0;
}

/* scroll [lines|end] — page the terminal view through the term's
 * scrollback buffer (v0.7 Track 4).  Positive delta pages back (older
 * lines), negative forward; "end" or 0 returns to the live screen.
 * The next shell output (a write to the term) resets the view. */
static int CmdScroll(int argc, char *argv[]) {
    if (s_term_port < 0) {
        ShellWrite("scroll: terminal unavailable\n");
        return -1;
    }
    /* Pager: enter the scrollback view and loop on keys until the user
     * quits (q/Enter) — the WRITE-to-live reset in term only fires on
     * text output, so the view stays up while we read keys. */
    i32 delta = 20;
    if (argc >= 2) {
        if (strcmp(argv[1], "end") == 0)
            delta = 0;
        else {
            char *end = NULL;
            long  v   = strtol(argv[1], &end, 10);
            if (!end || *end != '\0') {
                ShellWrite("scroll: invalid line count (use a number or 'end')\n");
                return -1;
            }
            delta = (i32)v;
        }
    }
    /* Enter the view. */
    u32 req[3];
    req[0] = 10; /* TERM_OP_SCROLLVIEW */
    req[1] = 4;
    req[2] = (u32)delta;
    int resp_len = 8;
    u8  resp[8];
    int r = IpcCall(s_term_port, req, 12, resp, &resp_len);
    if (r < 0 || (i32)((u32 *)resp)[0] < 0) {
        ShellWrite("scroll: term op failed\n");
        return -1;
    }
    /* NOTE: no shell_write here — any WRITE to the terminal resets the
     * scrollback view to live (term.c).  The pager is silent; only the
     * key loop runs until the user quits. */

    /* Pager loop: keys come from the keyboard service. */
    for (;;) {
        u32 kreq[2] = {2, 8}; /* KBD_OP_READ_BLOCK */
        u8  kresp[4 + 8];
        int krl = (int)sizeof(kresp);
        if (IpcCall(s_kbd_port, kreq, 8, kresp, &krl) < 0 || krl < 4)
            break;
        i32 n = (i32)((u32 *)kresp)[0];
        if (n <= 0)
            continue;
        u8 key = kresp[4];
        if (key == 'q' || key == 'Q' || key == '\r' || key == '\n')
            break;
        i32 step = 0;
        if (key == 0x02 || key == 0x0B) /* PgUp / Up */
            step = 20;
        else if (key == 0x06 || key == 0x0C) /* PgDn / Down */
            step = -20;
        else
            continue;
        req[0] = 10;
        req[1] = 4;
        req[2] = (u32)step;
        (void)IpcCall(s_term_port, req, 12, resp, &resp_len);
    }

    /* Return to live. */
    req[0] = 10;
    req[1] = 4;
    req[2] = 0;
    (void)IpcCall(s_term_port, req, 12, resp, &resp_len);
    return 0;
}

/* ====================================================================
 * disk — block-device management (v0.7.1)
 *
 *   disk list                     volumes + capacity/used (df-like)
 *   disk mount <vol>              mount the volume
 *   disk unmount <vol>            unmount it
 *   disk format <vol>             wipe + re-format (DESTRUCTIVE, asks)
 *   disk fill <vol> [bytes]       write fill.bin until NOSPC or budget
 *
 * mount/unmount/format/fill go through the user service (admin proxy):
 * it holds ATOM_SERVICE_MANAGE (the driver's control plane requires
 * it) and re-checks the caller is OWNER/ADMIN — the shell never talks
 * to the driver directly.
 * ==================================================================== */
static int CmdDisk(int argc, char *argv[]) {
    if (argc < 2) {
        ShellWrite("Usage: disk list | mount <vol> | unmount <vol> | "
                    "format <vol> | fill <vol> [bytes]\n");
        return -1;
    }

    if (strcmp(argv[1], "list") == 0) {
        static vfs_vol_info_t vols[VFS_MAX_VOLS];
        u32                   count = 0;
        int                   r     = FsListVolumes(vols, &count);
        if (r < 0) {
            ShellPrintf("disk: list FAILED (%d)\n", r);
            return -1;
        }
        if (count == 0) {
            ShellWrite("disk: no volumes mounted\n");
            return 0;
        }
        for (u32 i = 0; i < count; i++) {
            char url[80];
            snprintf(url, sizeof(url), "/%s", vols[i].mount_name);
            u64 total = 0, used = 0;
            u32 ro   = 0;
            int  sr  = FsStatVolume(url, &total, &used, &ro);
            ShellWrite(vols[i].mount_name);
            ShellWrite("  ");
            if (sr == 0)
                ShellPrintf("%d KiB used / %d KiB total%s\n", (int)(used / 1024u),
                             (int)(total / 1024u), ro ? " (ro)" : "");
            else
                ShellWrite("(stat unavailable)\n");
        }
        ShellPrintf("disk: %d volume(s)\n", (int)count);
        return 0;
    }

    if (argc < 3) {
        ShellWrite("Usage: disk list | mount <vol> | unmount <vol> | "
                    "format <vol> | fill <vol> [bytes]\n");
        return -1;
    }
    if (strlen(argv[2]) >= 64) {
        ShellPrintf("disk: volume name too long\n");
        return -1;
    }

    user_req_disk_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.volume, argv[2], sizeof(req.volume) - 1);
    req.volume[sizeof(req.volume) - 1] = '\0';

    if (strcmp(argv[1], "mount") == 0) {
        req.op = USER_OP_DISK_MOUNT;
    } else if (strcmp(argv[1], "unmount") == 0) {
        req.op = USER_OP_DISK_UNMOUNT;
    } else if (strcmp(argv[1], "format") == 0) {
        req.op = USER_OP_DISK_FORMAT;
    } else if (strcmp(argv[1], "fill") == 0) {
        req.op = USER_OP_DISK_FILL;
        if (argc >= 4) {
            u32 v = 0;
            for (const char *p = argv[3]; *p >= '0' && *p <= '9'; p++)
                v = v * 10u + (u32)(*p - '0');
            req.size = v; /* 0 (explicit) = fill until NOSPC */
        } else {
            req.size = 0; /* fill until NOSPC */
        }
    } else {
        ShellPrintf("disk: unknown subcommand '%s'\n", argv[1]);
        return -1;
    }

    /* Format is destructive: require an explicit confirmation word. */
    if (req.op == USER_OP_DISK_FORMAT) {
        ShellPrintf("disk: formatting '%s' destroys ALL data on it.\n", req.volume);
        ShellWrite("Type YES to continue: ");
        char conf[8];
        if (ReadLine(conf, sizeof(conf)) < 0)
            return -1;
        if (strcmp(conf, "YES") != 0) {
            ShellWrite("disk: format cancelled\n");
            return 0;
        }
    }

    user_resp_disk_t resp;
    memset(&resp, 0, sizeof(resp));
    int r = UserCall(&req, (int)sizeof(req), &resp, (int)sizeof(resp));
    if (r < 0) {
        ShellPrintf("disk: ipc FAILED (%d)\n", r);
        return -1;
    }
    if (resp.ret < 0) {
        ShellPrintf("disk: %s '%s' FAILED (%d)", argv[1], req.volume, resp.ret);
        if (resp.detail[0])
            ShellPrintf(" - %s", resp.detail);
        ShellWrite("\n");
        return -1;
    }
    if (req.op == USER_OP_DISK_FILL)
        ShellPrintf("disk: %d KiB written to %s/fill.bin\n", (int)(resp.bytes / 1024u),
                     req.volume);
    else
        ShellPrintf("disk: %s '%s' ok\n", argv[1], req.volume);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  UNIX-style subcommand dispatchers (v0.5)                          */
/*                                                                   */
/*  The legacy underscore commands (bm_resolve, perm_revoke, ...) are
 *  kept as aliases; the UNIX-style names are the primary interface:
 *    bm     create|resolve|revoke
 *    perm   answer|query|revoke
 *    policy set|dump
 *  userlock/userunlock are single-word UNIX-style names.            */
/* ------------------------------------------------------------------ */

static int CmdBm(int argc, char *argv[]) {
    if (argc < 2) {
        ShellWrite("Usage: bm <create|resolve|revoke> ...\n");
        return -1;
    }
    if (strcmp(argv[1], "create") == 0)
        return CmdBmCreate(argc - 1, argv + 1);
    if (strcmp(argv[1], "resolve") == 0)
        return CmdBmResolve(argc - 1, argv + 1);
    if (strcmp(argv[1], "revoke") == 0)
        return CmdBmRevoke(argc - 1, argv + 1);
    ShellPrintf("bm: unknown subcommand '%s'\n", argv[1]);
    return -1;
}

static int CmdPerm(int argc, char *argv[]) {
    if (argc < 2) {
        ShellWrite("Usage: perm <answer|query|revoke> ...\n");
        return -1;
    }
    if (strcmp(argv[1], "answer") == 0)
        return CmdPermAnswer(argc - 1, argv + 1);
    if (strcmp(argv[1], "query") == 0)
        return CmdPermQuery(argc - 1, argv + 1);
    if (strcmp(argv[1], "revoke") == 0)
        return CmdPermRevoke(argc - 1, argv + 1);
    ShellPrintf("perm: unknown subcommand '%s'\n", argv[1]);
    return -1;
}

static int CmdPolicy(int argc, char *argv[]) {
    if (argc < 2) {
        ShellWrite("Usage: policy <set|dump> ...\n");
        return -1;
    }
    if (strcmp(argv[1], "set") == 0)
        return CmdPolicySet(argc - 1, argv + 1);
    if (strcmp(argv[1], "dump") == 0)
        return CmdPolicyDump(argc - 1, argv + 1);
    ShellPrintf("policy: unknown subcommand '%s'\n", argv[1]);
    return -1;
}

/* ------------------------------------------------------------------ */
/*  fm — TUI file manager (v1.3)                                      */
/*                                                                   */
/*  Browses the current directory (or an absolute/relative path) with
 *  a tui_menu.  Keys: j/k move, Enter enter a directory / select a
 *  file, 'v' view a file (cat), 'd' delete (confirm), q quit.  The
 *  menu is a non-destructive overlay, so the shell prompt stays put
 *  underneath.                                                        */
/* ------------------------------------------------------------------ */

/* UTF-8-safe bounded copy: copies at most cap-1 bytes, then retreats
 * to a character boundary so a long multi-byte name is never cut in
 * the middle of a character. */
static void FmStrncpyUtf8(char *dst, const char *src, int cap) {
    int len = (int)strlen(src);
    if (len >= cap)
        len = cap - 1;
    int back = 0;
    while (back < 3 && len - 1 - back >= 0 &&
           ((unsigned char)src[len - 1 - back] & 0xC0) == 0x80)
        back++;
    if (back > 0) {
        int need = Utf8SeqLen(src + (len - 1 - back));
        if (need == 0 || need > back + 1)
            len -= back; /* drop the incomplete trailing character */
    }
    memcpy(dst, src, (size_t)len);
    dst[len] = '\0';
}

/* Enumerate `dir` (an absolute URL) into items[]; returns count. */
static int FmEnum(const char *dir, char items[][256], int cap) {
    static vfs_enum_batch_t batch; /* ~16.5 KB — keep off the stack */
    vfs_handle_t            e;
    int                     r = FsEnumBegin(dir, &e);
    if (r < 0)
        return r;
    int total = 0;
    for (;;) {
        r = FsEnumNext(e, &batch);
        if (r < 0) {
            FsEnumEnd(e);
            return r;
        }
        if (batch.batch_count == 0)
            break;
        for (u32 i = 0; i < batch.batch_count && total < cap; i++) {
            FmStrncpyUtf8(items[total], batch.batch[i], 256);
            total++;
        }
    }
    FsEnumEnd(e);
    return total;
}

/* Compose dir + "/" + name into out. */
static void FmJoin(const char *dir, const char *name, char *out, size_t outsz) {
    if (strcmp(dir, "/") == 0)
        snprintf(out, outsz, "/%s", name);
    else
        snprintf(out, outsz, "%s/%s", dir, name);
}

static int CmdFm(int argc, char *argv[]) {
    char dir[LINE_BUF_SIZE];
    if (argc >= 2) {
        if (ShellResolvePath(argv[1], dir, sizeof(dir)) < 0)
            return -1;
    } else {
        strncpy(dir, s_cwd, sizeof(dir) - 1);
        dir[sizeof(dir) - 1] = '\0';
    }
    /* "/" shows the volume list — enumerate the root view. */
    if (strcmp(dir, "/") == 0) {
        static vfs_vol_info_t vols[VFS_MAX_VOLS];
        u32                   vcount = 0;
        int                   r      = FsListVolumes(vols, &vcount);
        if (r < 0) {
            ShellPrintf("fm: volume list FAILED (%d)\n", r);
            return -1;
        }
        /* tui_menu needs a stable items array; reuse a static one. */
        static char items[FM_MAX_ITEMS][64];
        for (u32 i = 0; i < vcount && i < FM_MAX_ITEMS; i++) {
            snprintf(items[i], 64, "%s%s", vols[i].mount_name,
                     vols[i].read_only ? " (ro)" : "");
        }
        const char *ptrs[FM_MAX_ITEMS];
        for (u32 i = 0; i < vcount && i < 64; i++)
            ptrs[i] = items[i];
        int sel = TuiMenu(30, 8, 50, (int)vcount + 2, "Volumes (j/k, Enter, q)",
                           ptrs, (int)vcount, NULL);
        if (sel < 0)
            return 0; /* cancelled */
        /* Enter a volume: cd to /Volumes/<name> (strip (ro) suffix). */
        char *space = strchr(items[sel], ' ');
        if (space)
            *space = '\0';
        snprintf(dir, sizeof(dir), "/Volumes/%s", items[sel]);
    }

    for (;;) {
        static char items[FM_MAX_ITEMS][256];
        static const char *ptrs[FM_MAX_ITEMS];
        int n = FmEnum(dir, items, FM_MAX_ITEMS);
        if (n < 0) {
            ShellPrintf("fm: %s FAILED (%d)\n", dir, n);
            return -1;
        }
        for (int i = 0; i < n; i++)
            ptrs[i] = items[i];

        char title[128];
        snprintf(title, sizeof(title), "fm: %s (j/k Enter v d q)", dir);
        int rows = (n + 2 < 20) ? n + 2 : 20;
        int sel  = TuiMenu(4, 4, 80, rows, title, ptrs, n, NULL);
        if (sel < 0)
            break; /* q = quit */

        char full[LINE_BUF_SIZE];
        FmJoin(dir, items[sel], full, sizeof(full));

        vfs_item_info_t info;
        int             r = FsGetItem(full, &info);
        if (r < 0) {
            ShellPrintf("fm: %s FAILED (%d)\n", full, r);
            continue;
        }
        if (info.type == VFS_ITEM_DIR) {
            /* Enter directory. */
            strncpy(dir, full, sizeof(dir) - 1);
            dir[sizeof(dir) - 1] = '\0';
            strncpy(s_cwd, dir, sizeof(s_cwd) - 1);
            s_cwd[sizeof(s_cwd) - 1] = '\0';
            continue;
        }

        /* File: wait for an action key after Enter. */
        ShellPrintf("fm: %s (%d bytes) - v=view d=delete r=rename c=copy q=back\n",
                     items[sel], (int)info.size);
        /* Read one key directly (READ_BLOCK on the keyboard port). */
        u32 req[2];
        u8  resp[8];
        req[0]       = KBD_OP_READ_BLOCK;
        req[1]       = 1;
        int resp_len = (int)sizeof(resp);
        u8  key      = 0;
        if (IpcCall(s_kbd_port, req, 8, resp, &resp_len) == 0 && resp_len >= 4)
            key = resp[4];

        if (key == 'v' || key == 'V') {
            /* View: reuse cmd_cat logic via a fresh argv. */
            char *vargv[2] = {(char *)"cat", full};
            (void)CmdCat(2, vargv);
        } else if (key == 'd' || key == 'D') {
            char msg[128];
            snprintf(msg, sizeof(msg), "Delete '%s'?", items[sel]);
            int yes = TuiConfirm(20, 14, 60, "Delete", msg, "y = delete, n = cancel");
            if (yes > 0) {
                int dr = FsDeleteItem(full, 0);
                ShellPrintf("fm: delete %s (%d)\n", items[sel], dr);
            }
        } else if (key == 'r' || key == 'R') {
            /* Rename in place: TUI input line for the new name. */
            char newname[64];
            if (TuiInputLine(5, 30, "New name: ", newname, sizeof(newname), 0) >= 0 &&
                newname[0] != '\0') {
                vfs_item_info_t ri;
                int mr = FsMoveItem(full, dir, newname, &ri);
                ShellPrintf("fm: rename %s -> %s (%d)\n", items[sel], newname, mr);
            }
        } else if (key == 'c' || key == 'C') {
            /* Copy: read the file and write a "copy" sibling. */
            vfs_handle_t h;
            int          or = FsOpenItem(full, VFS_OPEN_READONLY, VFS_ACCESS_READ, &h);
            if (or < 0) {
                ShellPrintf("fm: copy open FAILED (%d)\n", or);
            } else {
                char dst[LINE_BUF_SIZE];
                char copy_name[96];
                snprintf(copy_name, sizeof(copy_name), "%s.copy", items[sel]);
                FmJoin(dir, copy_name, dst, sizeof(dst));
                vfs_handle_t dh;
                int          wr = FsOpenItem(dst, VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE,
                                               VFS_ACCESS_WRITE, &dh);
                if (wr < 0) {
                    ShellPrintf("fm: copy create FAILED (%d)\n", wr);
                } else {
                    static u8 cbuf[1024];
                    u64       off = 0;
                    int       cr  = 0;
                    for (;;) {
                        u32 got = 0;
                        cr = FsRead(h, off, cbuf, sizeof(cbuf), &got);
                        if (cr < 0 || got == 0)
                            break;
                        if (FsWrite(dh, off, cbuf, got) < 0)
                            break;
                        off += (u64)got;
                    }
                    FsClose(dh);
                    FsClose(h);
                    ShellPrintf("fm: copied to %s (%d)\n", copy_name, cr < 0 ? cr : (int)off);
                }
            }
        }
    }
    return 0;
}

/* policy_set <role> <cmd> <allow|deny|unset> — admin hot-updates one
 * command's verdict for a role.  The mutation goes through the USER
 * service (the trusted management proxy: it holds ATOM_SERVICE_MANAGE
 * and verifies the caller is OWNER/ADMIN by account) and is applied
 * live in the policy service.  This is the "system dynamic mechanism"
 * for command policy: no rebuild, no restart — runtime adjustment. */
static int CmdPolicySet(int argc, char *argv[]) {
    if (argc < 4) {
        ShellWrite("Usage: policy_set <role> <cmd> <allow|deny|unset>\n");
        return -1;
    }
    int port = PortGet("user");
    if (port < 0) {
        ShellPrintf("policy_set: user service unavailable (%d)\n", port);
        return -1;
    }

    /* Role name -> PERM_ROLE_* (mirror of cmd_useradd). */
    uint32_t role;
    if (strcmp(argv[1], "owner") == 0) role = PERM_ROLE_OWNER;
    else if (strcmp(argv[1], "admin") == 0) role = PERM_ROLE_ADMIN;
    else if (strcmp(argv[1], "standard") == 0) role = PERM_ROLE_STANDARD;
    else if (strcmp(argv[1], "child") == 0) role = PERM_ROLE_CHILD;
    else if (strcmp(argv[1], "guest") == 0) role = PERM_ROLE_GUEST;
    else if (strcmp(argv[1], "auditor") == 0) role = PERM_ROLE_AUDITOR;
    else {
        ShellPrintf("policy_set: invalid role '%s'\n", argv[1]);
        return -2;
    }

    uint32_t verdict;
    if (strcmp(argv[3], "allow") == 0) verdict = POLICY_ALLOW;
    else if (strcmp(argv[3], "deny") == 0) verdict = POLICY_DENY;
    else if (strcmp(argv[3], "unset") == 0) verdict = POLICY_UNSET;
    else {
        ShellPrintf("policy_set: invalid verdict '%s' (allow|deny|unset)\n", argv[3]);
        return -2;
    }

    user_req_policy_t req;
    memset(&req, 0, sizeof(req));
    req.op      = USER_OP_POLICY_SET;
    req.role    = role;
    req.verdict = verdict;
    strncpy(req.cmd, argv[2], sizeof(req.cmd) - 1);
    req.cmd[sizeof(req.cmd) - 1] = '\0';

    user_resp_policy_t resp;
    memset(&resp, 0, sizeof(resp));
    int rlen = (int)sizeof(resp);
    int r    = IpcCall(port, &req, (int)sizeof(req), &resp, &rlen);
    if (r < 0) {
        ShellPrintf("policy_set: ipc FAILED (%d)\n", r);
        return -1;
    }
    if (resp.ret < 0) {
        ShellPrintf("policy_set: FAILED (%d) (admin only)\n", resp.ret);
        return -1;
    }
    ShellPrintf("policy_set: role=%s cmd=%s -> %s\n",
                 argv[1], argv[2], argv[3]);
    return 0;
}

/* policy_dump — print the full command-policy table (admin, proxied). */
static int CmdPolicyDump(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int port = PortGet("user");
    if (port < 0) {
        ShellPrintf("policy_dump: user service unavailable (%d)\n", port);
        return -1;
    }
    user_req_policy_t req;
    memset(&req, 0, sizeof(req));
    req.op = USER_OP_POLICY_DUMP;
    user_resp_policy_t resp;
    memset(&resp, 0, sizeof(resp));
    int rlen = (int)sizeof(resp);
    int r    = IpcCall(port, &req, (int)sizeof(req), &resp, &rlen);
    if (r < 0) {
        ShellPrintf("policy_dump: ipc FAILED (%d)\n", r);
        return -1;
    }
    if (resp.ret < 0) {
        ShellPrintf("policy_dump: FAILED (%d) (admin only)\n", resp.ret);
        return -1;
    }
    ShellPrintf("policy_dump: %u rule(s)\n", (unsigned)resp.count);
    for (uint32_t i = 0; i < resp.count && i < 64; i++) {
        ShellWrite("  ");
        ShellWrite(resp.lines[i]);
        ShellWrite("\n");
    }
    return 0;
}

static int CmdPid(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    ShellPrintf("PID=%d\n", GetPid());
    return 0;
}

/*
 * Spawn an embedded demo/service ELF.  Blob name comes from argv[1]
 * (defaults to "hello" for backward compatibility); every registered
 * image (hello, runtime_demo, sbox_demo, ...) is fetchable via
 * SYS_BLOB_GET and spawnable via SYS_PROCESS_CREATE.
 */
static int CmdExec(int argc, char *argv[]) {
    const char *name = (argc > 1) ? argv[1] : "hello";
    static char blob_buf[262144]; /* must hold the largest ELF */
    int         size;

    if (strchr(name, '/') != NULL || name[0] == '.') {
        /* Path argument: run an ELF FILE from the VFS (e.g. exec /Disk/
         * app.elf or exec ./app.elf in the cwd).  Read it in full, then
         * spawn it like an embedded blob. */
        char url[LINE_BUF_SIZE];
        if (ShellResolvePath(name, url, sizeof(url)) < 0)
            return 1;
        vfs_handle_t h;
        int          r = FsOpenItem(url, VFS_OPEN_READONLY, VFS_ACCESS_READ, &h);
        if (r < 0) {
            ShellPrintf("exec: open %s FAILED (%d)\n", url, r);
            return 1;
        }
        size = 0;
        for (;;) {
            u32 got = 0;
            r       = FsRead(h, (u64)size, blob_buf + size, (u32)(sizeof(blob_buf) - (u64)size), &got);
            if (r < 0 || got == 0)
                break;
            size += (int)got;
        }
        FsClose(h);
        if (size <= 0) {
            ShellPrintf("exec: %s is empty or unreadable\n", url);
            return 1;
        }
        /* Process name = basename of the path. */
        const char *base = strrchr(url, '/');
        name             = base ? base + 1 : url;
        ShellPrintf("exec: read %s (%d bytes)\n", url, size);
    } else {
        size = BlobGet(name, blob_buf, sizeof(blob_buf));
        if (size < 0) {
            ShellPrintf("exec: BlobGet(%s) FAILED (%d)\n", name, size);
            return 1;
        }
        ShellPrintf("exec: fetched %s.elf blob from kernel (%d bytes)\n", name, size);
    }
    int pid = ProcessCreate(name, blob_buf, size);
    if (pid < 0) {
        ShellPrintf("exec: FAILED (%d)\n", pid);
        return 1;
    }
    ShellPrintf("exec: created PID %d\n", pid);
    return 0;
}

static int CmdFree(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int free_pages = GetFreePages();
    ShellPrintf(
        "Free memory: %d pages (%d KB, %d MB)\n", free_pages, free_pages * 4, free_pages / 256);
    return 0;
}

static int CmdClear(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    /* Ask the terminal service to blank the screen and home the cursor
     * (the terminal owns the framebuffer, so only it can clear it). */
    if (s_term_port >= 0) {
        u32 req[2];
        u32 resp[1];
        req[0]       = TERM_OP_CLEAR;
        req[1]       = 0;
        int resp_len = (int)sizeof(resp);
        IpcCall(s_term_port, (const void *)req, 8, (void *)resp, &resp_len);
    }
    return 0;
}

static int CmdCap(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    /* Create a dummy MEM capability as a test */
    int cap = CapCreate(CAP_TYPE_MEM, RIGHT_WRITE);
    if (cap > 0) {
        ShellPrintf("Created MEM cap: handle=%d\n", cap);
        CapRevoke(cap);
        ShellWrite("Revoked\n");
    } else {
        ShellPrintf("cap_create failed: %d\n", cap);
    }
    return 0;
}

/* mouse — read the PS/2 mouse state once ({dx, dy, buttons} deltas
 * since the last read; buttons bit0=left bit1=right bit2=middle). */
static int CmdMouse(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    if (s_kbd_port < 0) {
        ShellWrite("mouse: keyboard service unavailable\n");
        return -1;
    }
    u32 req[2];
    u8  resp[16];
    req[0] = 5; /* KBD_OP_MOUSE_READ */
    req[1] = 12;
    int resp_len = (int)sizeof(resp);
    int r        = IpcCall(s_kbd_port, req, 8, resp, &resp_len);
    if (r < 0 || resp_len < 4 + 12) {
        ShellPrintf("mouse: FAILED (%d)\n", r);
        return -1;
    }
    i32 *d = (i32 *)(resp + 4);
    ShellPrintf("mouse: dx=%d dy=%d buttons=%d\n", d[0], d[1], d[2]);
    return 0;
}

/* gui — enter the pixel desktop: spawn gui_demo (it activates the
 * compositor and takes the keyboard focus), wait for it to exit, then
 * return to the text shell. */
static int CmdGui(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    static char blob_buf[262144];
    int         size = BlobGet("gui_demo", blob_buf, sizeof(blob_buf));
    if (size < 0) {
        ShellPrintf("gui: gui_demo blob unavailable (%d)\n", size);
        return 1;
    }
    int pid = ProcessCreate("gui_demo", blob_buf, size);
    if (pid < 0) {
        ShellPrintf("gui: spawn FAILED (%d)\n", pid);
        return 1;
    }
    ShellPrintf("gui: desktop up (PID %d) - press q inside to quit\n", pid);
    int exit_code = 0;
    int r         = ProcessWait(pid, &exit_code);
    if (r != pid) {
        ShellPrintf("gui: wait FAILED (%d)\n", r);
        return 1;
    }
    ShellPrintf("gui: desktop closed (exit %d)\n", exit_code);
    return 0;
}

/* net — talk to the PCnet driver: mac | arp | recv | stats.
 * `net arp` sends an ARP who-has to the slirp gateway (10.0.2.2) and
 * prints the reply it receives — an end-to-end Tx+Rx proof. */
static int CmdNet(int argc, char *argv[]) {
    if (argc < 2) {
        ShellWrite("Usage: net mac | arp | recv | stats\n");
        return -1;
    }
    int port = PortGet("net");
    if (port < 0) {
        ShellPrintf("net: 'net' port unavailable (%d)\n", port);
        return -1;
    }
    if (strcmp(argv[1], "mac") == 0) {
        net_req_t req;
        memset(&req, 0, sizeof(req));
        req.op = 1; /* NET_OP_GET_MAC */
        net_resp_t resp;
        memset(&resp, 0, sizeof(resp));
        int rl = (int)sizeof(resp);
        if (IpcCall(port, &req, 8, &resp, &rl) < 0 || resp.ret < 0) {
            ShellPrintf("net: GET_MAC FAILED (%d)\n", resp.ret);
            return -1;
        }
        ShellPrintf("net: MAC %x:%x:%x:%x:%x:%x\n",
                     resp.data[0], resp.data[1], resp.data[2],
                     resp.data[3], resp.data[4], resp.data[5]);
        return 0;
    }
    if (strcmp(argv[1], "arp") == 0) {
        /* Fetch our MAC, then build an ARP who-has 10.0.2.2. */
        net_req_t req;
        memset(&req, 0, sizeof(req));
        req.op = 1;
        net_resp_t resp;
        memset(&resp, 0, sizeof(resp));
        int rl = (int)sizeof(resp);
        if (IpcCall(port, &req, 8, &resp, &rl) < 0 || resp.ret < 0) {
            ShellPrintf("net: GET_MAC FAILED (%d)\n", resp.ret);
            return -1;
        }
        u8 mac[6];
        memcpy(mac, resp.data, 6);

        /* Ethernet frame: dst bcast, src mac, type ARP. */
        u8 frame[42];
        memset(frame, 0xFF, 6); /* dst broadcast */
        memcpy(frame + 6, mac, 6);
        frame[12] = 0x08;
        frame[13] = 0x06; /* ARP */
        /* ARP header */
        frame[14] = 0x00; frame[15] = 0x01; /* hw ether */
        frame[16] = 0x08; frame[17] = 0x00; /* proto IP */
        frame[18] = 6; frame[19] = 4;       /* hlen plen */
        frame[20] = 0x00; frame[21] = 0x01; /* op request */
        memcpy(frame + 22, mac, 6);          /* sender MAC */
        frame[28] = 10; frame[29] = 0; frame[30] = 2; frame[31] = 15; /* spa 10.0.2.15 */
        memset(frame + 32, 0, 6);            /* target MAC 0 */
        frame[38] = 10; frame[39] = 0; frame[40] = 2; frame[41] = 2;  /* tpa 10.0.2.2 */

        memset(&req, 0, sizeof(req));
        req.op  = 2; /* NET_OP_SEND */
        req.len = 42;
        memcpy(req.data, frame, 42);
        if (IpcCall(port, &req, 8 + 42, &resp, &rl) < 0 || resp.ret < 0) {
            ShellPrintf("net: ARP send FAILED (%d)\n", resp.ret);
            return -1;
        }
        ShellWrite("net: ARP who-has 10.0.2.2 sent, waiting reply...\n");
        /* Poll RECV for a reply (up to ~3s).  A frame only counts as
         * the ARP reply when EtherType is ARP, opcode is REPLY and the
         * sender IP is the slirp gateway — anything else is noise. */
        for (int i = 0; i < 300; i++) {
            memset(&req, 0, sizeof(req));
            req.op = 3; /* NET_OP_RECV */
            memset(&resp, 0, sizeof(resp));
            if (IpcCall(port, &req, 8, &resp, &rl) == 0 && resp.ret == 0 && resp.len >= 42) {
                u8 *f = resp.data;
                ShellPrintf("net: RX %d bytes dst %x:%x:%x:%x:%x:%x type %x%x\n",
                             resp.len, f[0], f[1], f[2], f[3], f[4], f[5], f[12], f[13]);
                /* ARP reply: ethertype 0x0806, opcode 0x0002, sender
                 * 10.0.2.2 (f[28..31] = spa in the ARP payload). */
                if (f[12] == 0x08 && f[13] == 0x06 && f[20] == 0 && f[21] == 2 &&
                    f[28] == 10 && f[29] == 0 && f[30] == 2 && f[31] == 2) {
                    ShellPrintf("net: ARP reply! sender=%x:%x:%x:%x:%x:%x\n",
                                 f[22], f[23], f[24], f[25], f[26], f[27]);
                    return 0;
                }
            }
            (void)Sleep(1); /* 10 ms */
        }
        ShellWrite("net: no reply within timeout\n");
        return -1;
    }
    if (strcmp(argv[1], "recv") == 0) {
        net_req_t req;
        memset(&req, 0, sizeof(req));
        req.op = 3;
        net_resp_t resp;
        memset(&resp, 0, sizeof(resp));
        int rl = (int)sizeof(resp);
        if (IpcCall(port, &req, 8, &resp, &rl) == 0 && resp.ret == 0) {
            ShellPrintf("net: RX %d bytes\n", resp.len);
            for (u32 i = 0; i < resp.len && i < 64; i++) {
                ShellPrintf("%02x ", resp.data[i]);
                if ((i & 15) == 15)
                    ShellWrite("\n");
            }
            ShellWrite("\n");
            return 0;
        }
        ShellWrite("net: no packet pending\n");
        return -1;
    }
    if (strcmp(argv[1], "stats") == 0) {
        net_req_t req;
        memset(&req, 0, sizeof(req));
        req.op = 4;
        net_resp_t resp;
        memset(&resp, 0, sizeof(resp));
        int rl = (int)sizeof(resp);
        if (IpcCall(port, &req, 8, &resp, &rl) == 0 && resp.ret == 0) {
            u32 *st = (u32 *)resp.data;
            ShellPrintf("net: rx=%d tx=%d err=%d\n", st[0], st[1], st[2]);
            return 0;
        }
        return -1;
    }
    if (strcmp(argv[1], "ping") == 0 && argc >= 3) {
        /* ICMP echo to a dotted-quad address (e.g. 10.0.2.2). */
        u8 ip[4] = {0, 0, 0, 0};
        const char *s = argv[2];
        int ok = 1;
        for (int i = 0; i < 4; i++) {
            char *end = NULL;
            long  v   = strtol(s, &end, 10);
            if (end == s || v < 0 || v > 255 ||
                (i < 3 && *end != '.')) {
                ok = 0;
                break;
            }
            ip[i] = (u8)v;
            if (i < 3)
                s = end + 1;
            else if (*end != '\0')
                ok = 0;
        }
        if (!ok) {
            ShellWrite("net: bad address\n");
            return -1;
        }
        net_req_t req;
        memset(&req, 0, sizeof(req));
        req.op = 7; /* NET_OP_PING */
        req.len = 4;
        memcpy(req.data, ip, 4);
        net_resp_t resp;
        memset(&resp, 0, sizeof(resp));
        int rl = (int)sizeof(resp);
        ShellPrintf("net: ping %d.%d.%d.%d ...\n", ip[0], ip[1], ip[2], ip[3]);
        if (IpcCall(port, &req, 8 + 4, &resp, &rl) == 0 && resp.ret == 0) {
            ShellWrite("net: reply OK\n");
            return 0;
        }
        ShellPrintf("net: no reply (ret=%d)\n", resp.ret);
        return -1;
    }
    if (strcmp(argv[1], "tcp") == 0 && argc >= 3) {
        /* TCP echo test (server role): net tcp <port>.  Listens on the
         * given port and waits for a host-side connection (QEMU
         * hostfwd), receives one segment, echoes it back and closes.
         */
        long portn = strtol(argv[2], NULL, 10);
        if (portn < 16 || portn > 65535) {
            ShellWrite("net: bad port\n");
            return -1;
        }
        net_req_t req;
        memset(&req, 0, sizeof(req));
        net_resp_t resp;
        memset(&resp, 0, sizeof(resp));
        int rl = (int)sizeof(resp);

        req.op = 12; /* NET_OP_TCP_LISTEN */
        req.len = 2;
        req.data[0] = (u8)(portn >> 8);
        req.data[1] = (u8)(portn & 0xFF);
        if (IpcCall(port, &req, 8 + 2, &resp, &rl) < 0 || resp.ret < 0) {
            ShellPrintf("net: tcp listen failed (%d)\n", resp.ret);
            return -1;
        }
        ShellPrintf("net: tcp listening on %d ...\n", (int)portn);
        req.op = 13; /* NET_OP_TCP_ACCEPT */
        req.len = 0;
        if (IpcCall(port, &req, 8, &resp, &rl) < 0 || resp.ret < 0) {
            ShellPrintf("net: tcp accept failed (%d)\n", resp.ret);
            return -1;
        }
        ShellPrintf("net: tcp accepted %d.%d.%d.%d:%d\n",
                     resp.data[0], resp.data[1], resp.data[2], resp.data[3],
                     (resp.data[4] << 8) | resp.data[5]);
        req.op = 15; /* NET_OP_TCP_RECV */
        req.len = 0;
        if (IpcCall(port, &req, 8, &resp, &rl) == 0 && resp.ret >= 0 && resp.len > 0) {
            ShellPrintf("net: tcp recv %d bytes: ", resp.len);
            ShellWrite((const char *)resp.data);
            ShellWrite("\n");
            /* echo it back */
            req.op = 14; /* NET_OP_TCP_SEND */
            req.len = resp.len;
            memcpy(req.data, resp.data, req.len);
            (void)IpcCall(port, &req, 8 + req.len, &resp, &rl);
            req.op = 16; /* NET_OP_TCP_CLOSE */
            req.len = 0;
            (void)IpcCall(port, &req, 8, &resp, &rl);
            return 0;
        }
        ShellPrintf("net: tcp recv failed (%d)\n", resp.ret);
        return -1;
    }
    ShellWrite("Usage: net mac | arp | ping <ip> | tcp <ip> <port> <msg> | recv | stats\n");
    return -1;
}

static int CmdPorts(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    /* Try to look up some well-known ports */
    int port = PortGet("init");
    if (port > 0)
        ShellPrintf("'init' port: %d\n", port);
    else
        ShellWrite("'init' port not found\n");

    port = PortGet("shell");
    if (port > 0)
        ShellPrintf("'shell' port: %d\n", port);
    else
        ShellWrite("'shell' port not registered\n");

    /* Create a test port + register it */
    port = IpcPortCreate();
    if (port > 0) {
        ShellPrintf("Created test port: %d\n", port);
        int ret = PortRegister("shell_test", port);
        ShellPrintf("register 'shell_test' -> %d\n", ret);
    }
    return 0;
}

static int CmdSleep(int argc, char *argv[]) {
    if (argc < 2) {
        ShellWrite("Usage: sleep <ticks>\n");
        return -1;
    }
    int ticks = atoi(argv[1]);
    if (ticks <= 0) {
        ShellWrite("sleep: tick count must be positive\n");
        return -1;
    }
    ShellPrintf("Sleeping for %d ticks...\n", ticks);
    int ret = Sleep(ticks);
    if (ret < 0) {
        ShellPrintf("sleep: syscall failed (%d)\n", ret);
        return -1;
    }
    ShellWrite("Done\n");
    return 0;
}

static int CmdThreads(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    ShellWrite("Spawning worker thread...\n");
    int tid = ThreadCreate(WorkerFunc, NULL, 10);
    if (tid > 0) {
        ShellPrintf("Worker TID=%d, joining...\n", tid);
        int exit_code;
        ThreadJoin(tid, &exit_code);
        ShellPrintf("Worker joined, exit_code=%d\n", exit_code);
    } else {
        ShellPrintf("thread_create failed: %d\n", tid);
    }
    return 0;
}

/* Worker thread function for cmd_threads */
static void WorkerFunc(void *arg) {
    (void)arg;
    ShellWrite("  [worker] hello from thread!\n");
    ThreadExit(42);
}

/* ====================================================================
 * Mutex demo (cmd_mutex)
 *
 * Spawns N worker threads that each increment a shared counter
 * s_demo_iters times under a kernel mutex.  A lost-update would leave
 * the final counter below N * s_demo_iters — so an exact match proves
 * the mutex serialized every critical section.
 * ==================================================================== */

#define MUTEX_DEMO_ITERS 1000

static int s_demo_mutex   = -1; /* kernel mutex handle */
static int s_demo_counter = 0;  /* shared, mutex-protected */

/* Worker: increment the shared counter under the demo mutex. */
static void MutexWorkerFunc(void *arg) {
    (void)arg;
    for (int i = 0; i < MUTEX_DEMO_ITERS; i++) {
        int r = MutexLock(s_demo_mutex);
        if (r < 0) {
            ThreadExit(r); /* lock failed — propagate error */
        }
        s_demo_counter++;
        MutexUnlock(s_demo_mutex);
    }
    ThreadExit(0);
}

static int CmdMutex(int argc, char *argv[]) {
    int n = 4;
    if (argc >= 2) {
        n = atoi(argv[1]);
        if (n < 1)
            n = 1;
        if (n > 16)
            n = 16;
    }

    ShellWrite("mutex demo: creating mutex...\n");
    s_demo_mutex = MutexCreate();
    if (s_demo_mutex < 0) {
        ShellPrintf("mutex_create failed: %d\n", s_demo_mutex);
        return -1;
    }
    ShellPrintf("mutex handle=%d\n", s_demo_mutex);

    /* Round-trip: uncontended lock/unlock */
    int ret = MutexLock(s_demo_mutex);
    ShellPrintf("lock -> %d\n", ret);
    ret = MutexUnlock(s_demo_mutex);
    ShellPrintf("unlock -> %d\n", ret);

    /* Contended: N workers racing on the shared counter */
    s_demo_counter = 0;
    ShellPrintf("spawning %d workers x %d iters\n", n, MUTEX_DEMO_ITERS);

    int tids[16];
    for (int i = 0; i < n; i++) {
        tids[i] = ThreadCreate(MutexWorkerFunc, NULL, 10);
        if (tids[i] < 0) {
            ShellPrintf("thread_create failed: %d\n", tids[i]);
            MutexDestroy(s_demo_mutex);
            return -1;
        }
    }
    for (int i = 0; i < n; i++) {
        int code;
        ThreadJoin(tids[i], &code);
        if (code != 0)
            ShellPrintf("worker %d exited with %d\n", i, code);
    }

    int expect = n * MUTEX_DEMO_ITERS;
    if (s_demo_counter == expect)
        ShellPrintf("MUTEX PASS: counter=%d expected=%d\n", s_demo_counter, expect);
    else
        ShellPrintf("MUTEX FAIL: counter=%d expected=%d\n", s_demo_counter, expect);

    ret = MutexDestroy(s_demo_mutex);
    ShellPrintf("destroy -> %d\n", ret);
    return 0;
}

static int CmdUptime(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int ticks = GetTime();
    ShellPrintf("System ticks: %d\n", ticks);
    return 0;
}

static int CmdExit(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    ShellWrite("shell: exiting thread\n");
    ThreadExit(0);
    /* unreachable */
    return 0;
}

static int CmdReboot(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    ShellWrite("Halting system...\n");
    (void)sys_reboot();
    /* Only reached if the reboot syscall failed. */
    ShellWrite("reboot: syscall failed, system still running\n");
    return 0;
}

/* shutdown — power off the machine (ACPI S5 via the kernel; falls back
 * to reboot if the platform has no ACPI power button). */
static int CmdShutdown(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    ShellWrite("Shutting down...\n");
    (void)sys_shutdown();
    /* Only reached if the shutdown syscall failed (e.g. ERR_NOCAP). */
    ShellWrite("shutdown: syscall failed, system still running\n");
    return 0;
}


/*
 * Column helpers for cmd_ps.  The file-local ShellPrintf() has no
 * width/precision specifiers, so columns are padded manually exactly
 * like cmd_help does (write, then pad with trailing spaces).
 */
static void ShellPadInt(int v, int width) {
    char num[12];
    ShellItoa(v, num);
    ShellWrite(num);
    int pad = width - (int)strlen(num);
    while (pad > 0) {
        ShellPutc(' ');
        pad--;
    }
}

static void ShellPadStr(const char *s, int width) {
    ShellWrite(s);
    int pad = width - (int)strlen(s);
    while (pad > 0) {
        ShellPutc(' ');
        pad--;
    }
}

/*
 * ps
 *
 * List all user processes as a padded table.  Uses a static BSS
 * buffer (64 x 84-byte records); the kernel table cap is 256 but a
 * demo never approaches it.
 */
static int CmdPs(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    static proc_info_t s_ps_info[PROC_MAX_ITEMS];

    int n = ProcessList(s_ps_info, PROC_MAX_ITEMS);
    if (n < 0) {
        ShellPrintf("ps: process_list failed (%d)\n", n);
        return -1;
    }

    ShellWrite("PID  STATE    THR EXIT  NAME\n");
    for (int i = 0; i < n; i++) {
        const proc_info_t *p = &s_ps_info[i];
        const char        *state_name;
        switch (p->state) {
        case 0:
            state_name = "CREATED";
            break;
        case 1:
            state_name = "READY";
            break;
        case 2:
            state_name = "RUNNING";
            break;
        case 3:
            state_name = "ZOMBIE";
            break;
        case 4:
            state_name = "FINISHED";
            break;
        default:
            state_name = "?";
            break;
        }
        ShellPadInt(p->pid, 4);
        ShellWrite(" ");
        ShellPadStr(state_name, 8);
        ShellWrite(" ");
        ShellPadInt((int)p->thread_count, 3);
        ShellWrite(" ");
        ShellPadInt(p->exit_code, 4);
        ShellWrite("  ");
        ShellWrite(p->name);
        ShellWrite("\n");
    }
    return 0;
}

/*
 * kill <pid> [signum]
 *
 * Send a signal to a process.  signum defaults to SIGKILL (9).
 * SIGKILL force-terminates the target process (every thread gets
 * force_exit and blocked ones are woken); other signals follow the
 * registered handler / default action.
 */
static int CmdKill(int argc, char *argv[]) {
    int pid = 0;
    if (argc < 2) {
        /* v1.3: no PID -> TUI process picker. */
        static proc_info_t plist[PROC_MAX_ITEMS];
        static char        lines[PROC_MAX_ITEMS][32];
        static const char *ptrs[PROC_MAX_ITEMS];
        int                n = ProcessList(plist, PROC_MAX_ITEMS);
        if (n <= 0) {
            ShellWrite("kill: no processes\n");
            return -1;
        }
        int shown = 0;
        for (int i = 0; i < n && shown < 64; i++) {
            snprintf(lines[shown], 32, "%d  %s", (int)plist[i].pid, plist[i].name);
            ptrs[shown] = lines[shown];
            shown++;
        }
        int sel = TuiMenu(30, 6, 44, (shown + 2 < 20) ? shown + 2 : 20,
                           "Processes (j/k Enter q)", ptrs, shown, NULL);
        if (sel < 0)
            return 0; /* cancelled */
        pid = (int)plist[sel].pid;
    } else {
        pid = atoi(argv[1]);
        if (pid <= 0) {
            ShellPrintf("kill: invalid pid '%s'\n", argv[1]);
            return -1;
        }
    }
    int signum = SIGKILL;
    if (argc >= 3) {
        signum = atoi(argv[2]);
        if (signum <= 0 || signum >= NSIG) {
            ShellPrintf("kill: invalid signum '%s' (1..%d)\n", argv[2], NSIG - 1);
            return -1;
        }
    }

    /* v0.5: if the caller is logged in as OWNER/ADMIN, SIGKILL other
     * processes via the user-service proxy (the shell lacks the
     * kernel's ATOM_SERVICE_MANAGE, so direct cross-process SIGKILL
     * returns ERR_NOCAP).  The proxy re-checks admin + protects
     * system-critical services.  Non-admin callers keep the direct
     * path (self-signal still works; foreign PIDs are rejected by the
     * kernel gate with a clear message). */
    if (signum == SIGKILL) {
        user_req_login_t wq;
        memset(&wq, 0, sizeof(wq));
        wq.op = USER_OP_WHOAMI;
        user_resp_login_t who;
        memset(&who, 0, sizeof(who));
        int r = UserCall(&wq, (int)sizeof(wq), &who, (int)sizeof(who));
        if (r == 0 && who.ret == 0 &&
            (who.role == PERM_ROLE_OWNER || who.role == PERM_ROLE_ADMIN)) {
            user_req_kill_t kq;
            memset(&kq, 0, sizeof(kq));
            kq.op  = USER_OP_KILL;
            kq.pid = pid;
            user_resp_kill_t kr;
            memset(&kr, 0, sizeof(kr));
            r = UserCall(&kq, (int)sizeof(kq), &kr, (int)sizeof(kr));
            if (r < 0 || kr.ret < 0) {
                ShellPrintf("kill: PID %d SIG %d FAILED (%d)%s%s\n",
                             pid, signum, r < 0 ? r : kr.ret,
                             kr.detail[0] ? " - " : "", kr.detail);
                return -1;
            }
            ShellPrintf("kill: %s\n", kr.detail);
            return 0;
        }
    }

    int ret = Kill(pid, signum);
    if (ret < 0) {
        ShellPrintf("kill: PID %d SIG %d FAILED (%d) "
                     "(foreign PIDs need OWNER/ADMIN login)\n",
                     pid, signum, ret);
        return -1;
    }
    ShellPrintf("kill: sent SIG %d to PID %d\n", signum, pid);
    return 0;
}

/* ====================================================================
 * VFS client commands (libfs over the "vfs" server)
 * ==================================================================== */

/*
 * ls <url> — enumerate a directory.  URL like "/Volumes/System/"
 * or "Users".  "/" and "/Volumes" enumerate the mounted volumes (the
 * root view: there is no root item, the server answers from the mount
 * table).  Prints one child name per line, then the entry count.
 */
static int CmdLs(int argc, char *argv[]) {
    if (argc < 2) {
        ShellWrite("Usage: ls <dir-url>\n");
        return -1;
    }
    if (strcmp(argv[1], "/") == 0 || strcmp(argv[1], "/Volumes") == 0 ||
        strcmp(argv[1], "/Volumes/") == 0) {
        static vfs_vol_info_t vols[VFS_MAX_VOLS]; /* small, static */
        u32                   count = 0;
        int                   r     = FsListVolumes(vols, &count);
        if (r < 0) {
            ShellPrintf("ls: %s FAILED (%d)\n", argv[1], r);
            return -1;
        }
        for (u32 i = 0; i < count; i++) {
            ShellWrite(vols[i].mount_name);
            ShellWrite(vols[i].read_only ? " (ro)\n" : "\n");
        }
        ShellPrintf("ls: %d volumes\n", count);
        return 0;
    }
    static vfs_enum_batch_t batch; /* ~16.5 KB — keep off the stack */
    vfs_handle_t            e;
    int                     r = FsEnumBegin(argv[1], &e);
    if (r < 0) {
        ShellPrintf("ls: %s FAILED (%d)\n", argv[1], r);
        return -1;
    }
    int total = 0;
    for (;;) {
        r = FsEnumNext(e, &batch);
        if (r < 0) {
            ShellPrintf("ls: enum FAILED (%d)\n", r);
            FsEnumEnd(e);
            return -1;
        }
        if (batch.batch_count == 0)
            break;
        for (u32 i = 0; i < batch.batch_count; i++) {
            ShellWrite(batch.batch[i]);
            ShellWrite("\n");
            total++;
        }
    }
    FsEnumEnd(e);
    ShellPrintf("ls: %d entries\n", total);
    return 0;
}

/*
 * cat <url> — read and display a file.  Non-printable bytes are
 * shown as '.' so binary blobs do not corrupt the terminal.  Ends with
 * a byte-count line so the content length can be verified against the
 * source blob.
 */
/*
 * UTF-8 passthrough for cat: emit valid multi-byte sequences verbatim
 * (a binary file's UTF-8 text is no longer dotted into oblivion), and
 * replace stray/invalid bytes with '.'.  `pend`/`pend_n` carry an
 * incomplete sequence tail across 1024-byte read chunks.
 */
static void CatEmitText(const u8 *data, u32 len, u8 *pend, int *pend_n) {
    u32 i = 0;
    while (i < len || *pend_n > 0) {
        int need;
        if (*pend_n > 0) {
            need = Utf8SeqLen((const char *)pend);
        } else {
            u8 c = data[i];
            need = Utf8SeqLen((const char *)&c);
            if (need <= 0) {
                /* ASCII printable, or a stray byte */
                ShellPutc((c >= 32 && c < 127) ? (char)c : '.');
                i++;
                continue;
            }
        }
        if (need <= 0) { /* invalid lead already parked: flush dots */
            for (int k = 0; k < *pend_n; k++)
                ShellPutc('.');
            *pend_n = 0;
            continue;
        }
        while (*pend_n < need && *pend_n < 4 && i < len)
            pend[(*pend_n)++] = data[i++];
        if (*pend_n >= need) {
            uint32_t cp;
            int      n = Utf8Decode((const char *)pend, &cp);
            if (n == need) {
                for (int k = 0; k < n; k++)
                    ShellPutc((char)pend[k]);
            } else {
                for (int k = 0; k < *pend_n; k++)
                    ShellPutc('.');
            }
            *pend_n = 0;
            continue;
        }
        break; /* incomplete sequence: input exhausted, park for next read */
    }
}

static int CmdCat(int argc, char *argv[]) {
    if (argc < 2) {
        ShellWrite("Usage: cat <file-url>\n");
        return -1;
    }
    vfs_item_info_t info;
    int             r = FsGetItem(argv[1], &info);
    if (r < 0) {
        ShellPrintf("cat: %s FAILED (%d)\n", argv[1], r);
        return -1;
    }
    ShellPrintf("== %s (%d bytes) ==\n", argv[1], (int)info.size);

    vfs_handle_t h;
    r = FsOpenItem(argv[1], VFS_OPEN_READONLY, VFS_ACCESS_READ, &h);
    if (r < 0) {
        ShellPrintf("cat: open FAILED (%d)\n", r);
        return -1;
    }
    static u8 buf[1024];
    u8        pend[4];
    int       pend_n = 0;
    u32       total  = 0;
    for (;;) {
        u32 got = 0;
        r       = FsRead(h, total, buf, sizeof(buf), &got);
        if (r < 0) {
            ShellPrintf("cat: read FAILED (%d)\n", r);
            FsClose(h);
            return -1;
        }
        if (got == 0)
            break;
        CatEmitText(buf, got, pend, &pend_n);
        total += got;
        if (got < sizeof(buf))
            break; /* EOF */
    }
    if (pend_n > 0) { /* flush an unterminated tail as dots */
        for (int k = 0; k < pend_n; k++)
            ShellPutc('.');
        pend_n = 0;
    }
    FsClose(h);
    ShellPrintf("\n== read %d bytes ==\n", (int)total);
    return 0;
}

/*
 * stat [url] — volume capacity/usage.  With no argument, stats
 * both configured volumes (System read-only, Users 32 MiB RAM).
 */
static int CmdStat(int argc, char *argv[]) {
    static const char *vols[2] = {"/Volumes/System", "/Volumes/Users"};
    int                count   = (argc >= 2) ? 1 : 2;
    for (int v = 0; v < count; v++) {
        const char *url   = (argc >= 2) ? argv[1] : vols[v];
        u64         total = 0, used = 0;
        u32         ro = 0;
        int         r  = FsStatVolume(url, &total, &used, &ro);
        if (r < 0) {
            ShellPrintf("stat: %s FAILED (%d)\n", url, r);
            continue;
        }
        ShellPrintf("%s: %d KB total, %d KB used, %s\n",
                     url,
                     (int)(total / 1024),
                     (int)(used / 1024),
                     ro ? "read-only" : "read-write");
    }
    return 0;
}

/*
 * tee <url> <text> — write text to a file (create/truncate,
 * VFS_ACCESS_WRITE).  Exercises the write path: on the read-only System
 * volume the server must reject the open with VFS_ERR_READONLY (-100);
 * on the RAM volume the bytes must land so cat can read them back.
 */
static int CmdTee(int argc, char *argv[]) {
    if (argc < 3) {
        ShellWrite("Usage: tee <file-url> <text>\n");
        return -1;
    }
    const char *url  = argv[1];
    const char *text = argv[2];
    u32         len  = (u32)strlen(text);

    vfs_handle_t h;
    int          r = FsOpenItem(url, VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE, VFS_ACCESS_WRITE, &h);
    if (r < 0) {
        ShellPrintf("tee: open %s FAILED (%d)\n", url, r);
        return -1;
    }
    r = FsWrite(h, 0, text, len);
    if (r < 0) {
        ShellPrintf("tee: write FAILED (%d)\n", r);
        FsClose(h);
        return -1;
    }
    FsClose(h);
    ShellPrintf("tee: %d bytes written to %s\n", (int)len, url);
    return 0;
}

/*
 * fallocate <url> — grow a file in 4 KiB chunks until the volume is
 * full.  Exercises the ENOSPC path: the fs_mem_driver capacity check
 * must reject the write that would exceed the 32 MiB Users volume with
 * VFS_ERR_NOSPC (-101).  Prints progress every 8 MiB and the failing
 * offset + error code.
 */
static int CmdFallocate(int argc, char *argv[]) {
    if (argc < 2) {
        ShellWrite("Usage: fallocate <file-url>\n");
        return -1;
    }
    const char *url = argv[1];

    vfs_handle_t h;
    int          r = FsOpenItem(url, VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE, VFS_ACCESS_WRITE, &h);
    if (r < 0) {
        ShellPrintf("fallocate: open %s FAILED (%d)\n", url, r);
        return -1;
    }

    static u8 s_fill_buf[4096];
    memset(s_fill_buf, 0xAB, sizeof(s_fill_buf));

    u64 off = 0;
    for (;;) {
        r = FsWrite(h, off, s_fill_buf, sizeof(s_fill_buf));
        if (r < 0) {
            /* Only a full volume is NOSPC; other failures (e.g. a dead
             * driver returning ERR_NOENT) are labelled accurately. */
            if (r == VFS_ERR_NOSPC)
                ShellPrintf("fallocate: NOSPC at %d MiB (err %d)\n", (int)(off >> 20), r);
            else
                ShellPrintf("fallocate: write FAILED at %d MiB (err %d)\n",
                             (int)(off >> 20), r);
            FsClose(h);
            return 0;
        }
        off += sizeof(s_fill_buf);
        if ((off & 0x7FFFFF) == 0) /* every 8 MiB */
            ShellPrintf("fallocate: %d MiB\n", (int)(off >> 20));
    }
}

/*
 * mkdir <url> — create an empty directory (fs_create_dir).
 * Fails with VFS_ERR_EXISTS (-104) if the directory already exists.
 */
static int CmdMkdir(int argc, char *argv[]) {
    if (argc < 2) {
        ShellWrite("Usage: mkdir <dir-url>\n");
        return -1;
    }
    int r = FsCreateDir(argv[1]);
    if (r < 0) {
        ShellPrintf("mkdir: %s FAILED (%d)\n", argv[1], r);
        return -1;
    }
    ShellPrintf("mkdir: created %s\n", argv[1]);
    return 0;
}

/*
 * rm <url> — delete an item (FsDeleteItem, non-recursive).  A
 * non-empty directory fails with ERR_BUSY.
 */
static int CmdRm(int argc, char *argv[]) {
    if (argc < 2) {
        ShellWrite("Usage: rm <url>\n");
        return -1;
    }
    /* v1.3: destructive command -> TUI confirm dialog. */
    char msg[160];
    snprintf(msg, sizeof(msg), "Delete '%s'?", argv[1]);
    int yes = TuiConfirm(20, 14, 60, "Confirm Delete", msg,
                          "Type y to confirm, n to cancel");
    if (yes < 0)
        return -1;
    if (!yes) {
        ShellWrite("rm: cancelled\n");
        return 0;
    }
    int r = FsDeleteItem(argv[1], 0);
    if (r < 0) {
        ShellPrintf("rm: %s FAILED (%d)\n", argv[1], r);
        return -1;
    }
    ShellPrintf("rm: removed %s\n", argv[1]);
    return 0;
}

/* ====================================================================
 * Phase 2 test commands: security-scoped bookmarks + Powerbox + mv
 * (design §8).  Acceptance flow:
 *   bm_create <url> r        → -105 (no grant; term shows the prompt)
 *   perm_answer <id> y       → grant upserted, UI_SHOW update pushed
 *   bm_create <url> r        → ok, blob cached
 *   bm_resolve               → handle (授权后 → 句柄)
 *   mv <url> <dst> [name]  → itemID stable, bookmark still valid
 *   bm_resolve               → still resolves (移动后仍有效)
 *   perm_revoke [subject_id] → grants dropped (or bm_revoke)
 *   bm_resolve               → -105 again (撤销后 → -EACCES)
 * ==================================================================== */

static u8  s_bm_blob[VFS_BOOKMARK_MAX]; /* cached bookmark blob     */
static u32 s_bm_len    = 0;             /* 0 = no cached bookmark   */
static int s_perm_port = -1;            /* "perm" port, lazy        */

static int perm_port(void) {
    if (s_perm_port < 0)
        s_perm_port = PortGet(PERM_PORT_NAME);
    return s_perm_port;
}

/* Parse "r"/"w"/"rw" → VFS_ACCESS_* mask (default "r"). */
static u32 BmAccess(const char *s) {
    u32 a = 0;
    if (!s || *s == '\0')
        s = "r";
    for (; *s; s++) {
        if (*s == 'r' || *s == 'R')
            a |= VFS_ACCESS_READ;
        if (*s == 'w' || *s == 'W')
            a |= VFS_ACCESS_WRITE;
        if (*s == 'x' || *s == 'X')
            a |= VFS_ACCESS_EXEC;
    }
    return a;
}

/* bm_create <url> [r|w|rw] — request a Powerbox-gated bookmark.
 * The first attempt normally returns VFS_ERR_ACCESS (-105) and the
 * term prints "perm: app ... - perm_answer <id> y/n"; answer it, then
 * retry.  On success the blob is cached for bm_resolve/bm_revoke. */
static int CmdBmCreate(int argc, char *argv[]) {
    if (argc < 2) {
        ShellWrite("Usage: bm_create <url> [r|w|rw]\n");
        return -1;
    }
    const char *url    = argv[1];
    u32         access = BmAccess((argc >= 3) ? argv[2] : "r");

    u32 len = 0;
    int r   = FsCreateBookmark(url, access, 0, s_bm_blob, &len);
    if (r < 0) {
        ShellPrintf("bm_create: FAILED (%d)", r);
        if (r == VFS_ERR_ACCESS)
            ShellWrite(" (denied - see term, then perm_answer <id> y, "
                        "and retry)\n");
        else
            ShellWrite("\n");
        return -1;
    }
    s_bm_len = len;
    ShellPrintf("bm_create: ok, %d-byte bookmark cached\n", (int)len);
    return 0;
}

/* bm_resolve — blob → FileHandle (authorization re-checked server-side
 * on every resolve).  Prints handle/item/access, then closes. */
static int CmdBmResolve(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    if (s_bm_len == 0) {
        ShellWrite("bm_resolve: no cached bookmark (bm_create first)\n");
        return -1;
    }
    vfs_handle_t    h = 0;
    vfs_item_info_t item;
    u32             access = 0;
    int             r      = FsResolveBookmark(s_bm_blob, s_bm_len, &h, &item, &access);
    if (r < 0) {
        ShellPrintf("bm_resolve: FAILED (%d)", r);
        if (r == VFS_ERR_ACCESS)
            ShellWrite(" (EACCES)\n");
        else
            ShellWrite("\n");
        return -1;
    }
    ShellPrintf("bm_resolve: handle %d, item '%s' (id %d), access %d\n",
                 (int)h,
                 item.name,
                 (int)item.item_id,
                 (int)access);
    FsClose(h);
    return 0;
}

/* bm_revoke — drop the bookmark record server-side (idempotent). */
static int CmdBmRevoke(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    if (s_bm_len == 0) {
        ShellWrite("bm_revoke: no cached bookmark (bm_create first)\n");
        return -1;
    }
    int r = FsRevokeBookmark(s_bm_blob, s_bm_len);
    ShellPrintf("bm_revoke: %s (%d)\n", r < 0 ? "FAILED" : "ok", r);
    if (r == 0)
        s_bm_len = 0; /* record gone */
    return r < 0 ? -1 : 0;
}

/* perm_answer <query_id> <y|n> — user verdict on a pending Powerbox
 * query.  y → grant upserted; n → denied (default deny). */
static int CmdPermAnswer(int argc, char *argv[]) {
    if (argc < 3) {
        ShellWrite("Usage: perm_answer <query-id> <y|n>\n");
        return -1;
    }
    int port = perm_port();
    if (port < 0) {
        ShellPrintf("perm_answer: perm port unavailable (%d)\n", port);
        return -1;
    }

    perm_req_answer_t req;
    memset(&req, 0, sizeof(req));
    req.op       = PERM_OP_ANSWER;
    req.query_id = (u32)atoi(argv[1]);
    req.allow    = (argv[2][0] == 'y' || argv[2][0] == 'Y') ? 1 : 0;

    perm_resp_answer_t resp;
    memset(&resp, 0, sizeof(resp));
    int resp_len = (int)sizeof(resp);
    int r        = IpcCall(port, &req, (int)sizeof(req), &resp, &resp_len);
    if (r < 0) {
        ShellPrintf("perm_answer: ipc FAILED (%d)\n", r);
        return -1;
    }
    ShellPrintf("perm_answer: query %d -> %s (%d)\n",
                 (int)req.query_id,
                 req.allow ? "ALLOWED" : "DENIED",
                 resp.ret);
    return 0;
}

/* VFS_ACCESS_* mask → short string ("R", "W", "RW", ...).  Static
 * storage; the result is consumed immediately by shell_printf. */
static const char *perm_access_str(u32 access) {
    static char buf[4];
    int         i = 0;
    if (access & VFS_ACCESS_READ)
        buf[i++] = 'R';
    if (access & VFS_ACCESS_WRITE)
        buf[i++] = 'W';
    if (access & VFS_ACCESS_EXEC)
        buf[i++] = 'X';
    if (i == 0)
        buf[i++] = '-';
    buf[i] = '\0';
    return buf;
}

/* perm_query [query_id] — fetch a pending Powerbox query (0 = first
 * pending, FIFO).  Prints id/name/pid/url/access/label/state so the
 * test flow can discover the query id that bm_create left pending. */
static int CmdPermQuery(int argc, char *argv[]) {
    int port = perm_port();
    if (port < 0) {
        ShellPrintf("perm_query: perm port unavailable (%d)\n", port);
        return -1;
    }
    u32 qid = (argc >= 2) ? (u32)atoi(argv[1]) : 0;

    perm_req_query_t req;
    memset(&req, 0, sizeof(req));
    req.op       = PERM_OP_QUERY;
    req.query_id = qid;

    perm_resp_query_t resp;
    memset(&resp, 0, sizeof(resp));
    int resp_len = (int)sizeof(resp);
    int r        = IpcCall(port, &req, (int)sizeof(req), &resp, &resp_len);
    if (r < 0) {
        ShellPrintf("perm_query: ipc FAILED (%d)\n", r);
        return -1;
    }
    if (resp.ret < 0) {
        ShellPrintf("perm_query: no pending query (%d)\n", resp.ret);
        return -1;
    }
    ShellPrintf("perm_query: query %d: %s (PID %d) requests %s (%s) - %s "
                 "[state %d]\n",
                 (int)resp.query_id,
                 resp.name,
                 (int)resp.pid,
                 resp.url,
                 perm_access_str(resp.access),
                 resp.label,
                 (int)resp.state);
    return 0;
}

/* perm_revoke [subject_id] — drop grants for a subject (0 = all
 * subjects, all resources).  Default = our own subject (get_subject). */
static int CmdPermRevoke(int argc, char *argv[]) {
    int port = perm_port();
    if (port < 0) {
        ShellPrintf("perm_revoke: perm port unavailable (%d)\n", port);
        return -1;
    }
    u64 subject = (argc >= 2) ? (u64)strtoull(argv[1], NULL, 10) : GetSubject();

    perm_req_revoke_t req;
    memset(&req, 0, sizeof(req)); /* zero resource = all resources */
    req.op         = PERM_OP_REVOKE;
    req.subject_id = subject;

    perm_resp_revoke_t resp;
    memset(&resp, 0, sizeof(resp));
    int resp_len = (int)sizeof(resp);
    int r        = IpcCall(port, &req, (int)sizeof(req), &resp, &resp_len);
    if (r < 0) {
        ShellPrintf("perm_revoke: ipc FAILED (%d)\n", r);
        return -1;
    }
    ShellPrintf("perm_revoke: %d grant(s) dropped\n", (int)resp.revoked);
    return 0;
}

/* move <src-url> <dst-dir-url> [new-name] — move/rename an item.  The
 * fs_mem_driver keeps itemID stable, so cached bookmarks survive. */
static int CmdMv(int argc, char *argv[]) {
    if (argc < 3) {
        ShellWrite("Usage: mv <src-url> <dst-dir-url|?> [new-name]\n");
        return -1;
    }
    char dst[LINE_BUF_SIZE];
    if (strcmp(argv[2], "?") == 0) {
        /* v1.3: TUI directory picker for the destination. */
        static char items[FM_MAX_ITEMS][256];
        static const char *ptrs[FM_MAX_ITEMS];
        int n = FmEnum(s_cwd, items, FM_MAX_ITEMS);
        if (n <= 0) {
            ShellPrintf("mv: no directories in %s\n", s_cwd);
            return -1;
        }
        int shown = 0;
        for (int i = 0; i < n && shown < 64; i++) {
            /* Only directories are valid move targets. */
            char full[LINE_BUF_SIZE];
            FmJoin(s_cwd, items[i], full, sizeof(full));
            vfs_item_info_t it;
            if (FsGetItem(full, &it) == 0 && it.type == VFS_ITEM_DIR) {
                ptrs[shown] = items[i];
                shown++;
            }
        }
        if (shown == 0) {
            ShellPrintf("mv: no directories in %s\n", s_cwd);
            return -1;
        }
        int sel = TuiMenu(30, 8, 50, (shown + 2 < 16) ? shown + 2 : 16,
                           "Move to dir (j/k Enter q)", ptrs, shown, NULL);
        if (sel < 0)
            return 0; /* cancelled */
        FmJoin(s_cwd, ptrs[sel], dst, sizeof(dst));
    } else {
        strncpy(dst, argv[2], sizeof(dst) - 1);
        dst[sizeof(dst) - 1] = '\0';
    }
    /* v1.3: mutating command -> TUI confirm dialog. */
    char msg[200];
    snprintf(msg, sizeof(msg), "Move '%s' to '%s'?", argv[1], dst);
    int yes = TuiConfirm(20, 14, 60, "Confirm Move", msg,
                          "Type y to confirm, n to cancel");
    if (yes < 0)
        return -1;
    if (!yes) {
        ShellWrite("mv: cancelled\n");
        return 0;
    }
    vfs_item_info_t item;
    int             r = FsMoveItem(argv[1], dst, (argc >= 4) ? argv[3] : "", &item);
    if (r < 0) {
        ShellPrintf("mv: FAILED (%d)\n", r);
        return -1;
    }
    ShellPrintf("mv: '%s' -> item %d (size %d)\n", item.name, (int)item.item_id, (int)item.size);
    return 0;
}

/*
 * pkg <install|list|run|remove> — .ops application container
 * (docs/ops_format.md).  Talks to the pkg-manager service through the
 * libpkg client (user/lib/libpkg).  Every operation returns 0 on
 * success or a negative error code, printed on failure.
 */
static int CmdPkg(int argc, char *argv[]) {
    if (argc < 2) {
        ShellWrite("Usage: pkg <install <name> [--perms=a,b,c] | list | "
                    "run <app_id> | remove <app_id>>\n");
        return -1;
    }
    const char *sub = argv[1];

    if (strcmp(sub, "install") == 0) {
        if (argc < 3) {
            ShellWrite("Usage: pkg install <name> [--perms=a,b,c]\n");
            return -1;
        }
        const char *perms = "";
        if (argc >= 4 && strncmp(argv[3], "--perms=", 8) == 0)
            perms = argv[3] + 8;
        int ret = PkgInstall(argv[2], perms);
        if (ret < 0) {
            ShellPrintf("pkg install: FAILED (%d)\n", ret);
            return -1;
        }
        ShellPrintf("pkg install: '%s' installed\n", argv[2]);
        return 0;
    }

    if (strcmp(sub, "list") == 0) {
        char     apps[PKG_MAX_APPS][PKG_NAME_MAX];
        uint32_t count = 0;
        int      ret   = PkgList(apps, &count);
        if (ret < 0) {
            ShellPrintf("pkg list: FAILED (%d)\n", ret);
            return -1;
        }
        ShellPrintf("pkg list: %d app(s) installed\n", (int)count);
        for (uint32_t i = 0; i < count; i++)
            ShellPrintf("  %s\n", apps[i]);
        return 0;
    }

    if (strcmp(sub, "run") == 0) {
        if (argc < 3) {
            ShellWrite("Usage: pkg run <app_id>\n");
            return -1;
        }
        int32_t pid = 0;
        int     ret = PkgRun(argv[2], &pid);
        if (ret < 0) {
            ShellPrintf("pkg run: FAILED (%d)\n", ret);
            return -1;
        }
        ShellPrintf("pkg run: '%s' spawned (PID=%d)\n", argv[2], (int)pid);
        return 0;
    }

    if (strcmp(sub, "remove") == 0) {
        if (argc < 3) {
            ShellWrite("Usage: pkg remove <app_id>\n");
            return -1;
        }
        int ret = PkgRemove(argv[2]);
        if (ret < 0) {
            ShellPrintf("pkg remove: FAILED (%d)\n", ret);
            return -1;
        }
        ShellPrintf("pkg remove: '%s' removed\n", argv[2]);
        return 0;
    }

    ShellPrintf("pkg: unknown subcommand '%s'\n", sub);
    return -1;
}

/* ====================================================================
 * Entry point (shell process main)
 * ==================================================================== */

static void ShellMain(void *arg) {
    (void)arg;
    s_term_port = PortGet("term");
    s_kbd_port  = PortGet("keyboard");

    /* Register the built-in commands BEFORE the loop reads any input.
     * Registration order defines `help` output order.  Return values
     * are ignored: these are static entries, so the only failure mode
     * is heap exhaustion, which leaves the shell with fewer commands
     * but still running. */
    ShellRegisterCommand("help", "Show this help", CmdHelp);
    ShellRegisterCommand("echo", "Print text: echo <message>", CmdEcho);
    ShellRegisterCommand("pid", "Show current process PID", CmdPid);
    ShellRegisterCommand("free", "Show free physical memory", CmdFree);
    ShellRegisterCommand("clear", "Clear the terminal", CmdClear);
    ShellRegisterCommand("cap", "Create a capability (test)", CmdCap);
    ShellRegisterCommand("mouse", "Read the PS/2 mouse state (dx dy buttons)", CmdMouse);
    ShellRegisterCommand("gui", "Enter the pixel desktop (gui_demo)", CmdGui);
    ShellRegisterCommand("net", "PCnet NIC: net mac|arp|recv|stats", CmdNet);
    ShellRegisterCommand("ports", "List registered IPC ports", CmdPorts);
    ShellRegisterCommand("sleep", "Sleep for N ticks: sleep <ticks>", CmdSleep);
    ShellRegisterCommand("threads", "Spawn a test worker thread", CmdThreads);
    ShellRegisterCommand("mutex", "Mutex demo: N threads on a counter", CmdMutex);
    ShellRegisterCommand("exec", "Spawn an embedded demo (exec [blob_name])", CmdExec);
    ShellRegisterCommand("uptime", "Show system tick count", CmdUptime);
    ShellRegisterCommand("exit", "Exit the shell", CmdExit);
    ShellRegisterCommand("reboot", "Halt the system", CmdReboot);
    ShellRegisterCommand("shutdown", "Power off the machine", CmdShutdown);
    ShellRegisterCommand("ime", "Pinyin IME: ime [on|off] (Ctrl+Space)", CmdIme);
    ShellRegisterCommand("kill", "Send a signal: kill <pid> [signum]", CmdKill);
    ShellRegisterCommand("ps", "List running processes", CmdPs);
    ShellRegisterCommand(
        "ls", "List a VFS dir: ls <url> (/ or /Volumes = volumes)", CmdLs);
    ShellRegisterCommand("cat", "Show a VFS file: cat <url>", CmdCat);
    ShellRegisterCommand("stat", "VFS volume stats: stat [url]", CmdStat);
    ShellRegisterCommand("tee", "Write a VFS file: tee <url> <text>", CmdTee);
    ShellRegisterCommand("fallocate", "Fill a volume until ENOSPC: fallocate <url>", CmdFallocate);
    ShellRegisterCommand("mkdir", "Create a VFS dir: mkdir <url>", CmdMkdir);
    ShellRegisterCommand("rm", "Delete a VFS item: rm <url>", CmdRm);
    ShellRegisterCommand(
        "bm_create", "Powerbox-gated bookmark: bm_create <url> [r|w|rw]", CmdBmCreate);
    ShellRegisterCommand("bm_resolve", "Resolve cached bookmark to a handle", CmdBmResolve);
    ShellRegisterCommand("bm_revoke", "Drop the cached bookmark server-side", CmdBmRevoke);
    ShellRegisterCommand(
        "perm_answer", "Answer a Powerbox query: perm_answer <id> y|n", CmdPermAnswer);
    ShellRegisterCommand(
        "perm_query", "Show pending Powerbox query: perm_query [id]", CmdPermQuery);
    ShellRegisterCommand("perm_revoke", "Drop grants: perm_revoke [subject_id]", CmdPermRevoke);
    /* UNIX-style aliases: bm <create|resolve|revoke>, perm <answer|query|revoke>. */
    ShellRegisterCommand("bm", "Bookmarks: bm <create|resolve|revoke> ...", CmdBm);
    ShellRegisterCommand("perm", "Powerbox: perm <answer|query|revoke> ...", CmdPerm);
    ShellRegisterCommand("mv", "Move/rename: mv <src> <dst-dir> [new-name]", CmdMv);
    ShellRegisterCommand("pkg", "pkg-manager: pkg <install|list|run|remove>", CmdPkg);

    /* User accounts + exit guard */
    ShellRegisterCommand("login", "Log in: login [name] [password]", CmdLogin);
    ShellRegisterCommand("logout", "Log out the current account", CmdLogout);
    ShellRegisterCommand("whoami", "Show the logged-in account", CmdWhoami);
    ShellRegisterCommand("passwd", "Change password: passwd [name]", CmdPasswd);
    ShellRegisterCommand("useradd", "Create account (admin): useradd <name> <role> [pw]", CmdUseradd);
    ShellRegisterCommand("userdel", "Delete account (admin): userdel <name>", CmdUserdel);
    ShellRegisterCommand("user_lock", "Disable account (admin): user_lock <name>", CmdUserlock);
    ShellRegisterCommand("user_unlock", "Enable account (admin): user_unlock <name>", CmdUserunlock);
    ShellRegisterCommand("userlock", "Disable account (admin): userlock <name>", CmdUserlock);
    ShellRegisterCommand("userunlock", "Enable account (admin): userunlock <name>", CmdUserunlock);
    ShellRegisterCommand("users", "List accounts (admin)", CmdUsers);
    ShellRegisterCommand("stop", "Stop a system program (admin, confirmed): stop <svc>", CmdStop);
    ShellRegisterCommand("export", "Set env var: export NAME=value (user prefs only)", CmdExport);
    ShellRegisterCommand("unset", "Remove env var: unset NAME", CmdUnset);
    ShellRegisterCommand("env", "Print the environment", CmdEnv);
    ShellRegisterCommand("policy_set", "Hot-update cmd policy (admin): policy_set <role> <cmd> <allow|deny|unset>", CmdPolicySet);
    ShellRegisterCommand("policy_dump", "Show cmd policy table (admin)", CmdPolicyDump);
    ShellRegisterCommand("policy", "Cmd policy: policy <set|dump> ...", CmdPolicy);
    ShellRegisterCommand("cd", "Change directory: cd [dir]", CmdCd);
    ShellRegisterCommand("pwd", "Print working directory", CmdPwd);
    ShellRegisterCommand("scroll",
                           "Page through terminal scrollback: scroll [lines] | scroll end", CmdScroll);
    ShellRegisterCommand("disk",
                           "Disk mgmt: disk list|mount|unmount|format|fill <vol> [bytes]", CmdDisk);
    ShellRegisterCommand("fm", "TUI file manager (j/k Enter v d q)", CmdFm);

    /* v0.5: load the command policy filter (rescue list on failure). */
    CmdFilterLoad();

    ShellLoop();
}

/* ====================================================================
 * Process entry point (crt0 calls main())
 * ==================================================================== */

/*
 * The shell process's main thread.  Defers to ShellMain(), which runs
 * the REPL forever; this function never returns.
 */
int main(void) {
    ShellMain(NULL);
    return 0; /* unreachable */
}

/* ====================================================================
 * User account commands (user service, user.h)
 * ==================================================================== */

static int UserPort(void) {
    static int s_port = -2;
    if (s_port >= -1)
        return s_port;
    s_port = PortGet(USER_PORT_NAME);
    return s_port;
}

/* user_call: simple request/reply over the "user" port. */
static int UserCall(const void *req, int req_len, void *resp, int resp_len) {
    int port = UserPort();
    if (port < 0)
        return port;
    int rlen = resp_len;
    int r    = IpcCall(port, req, req_len, resp, &rlen);
    if (r < 0)
        return r;
    return 0;
}

static const char *role_name(uint32_t role) {
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

/* login [name] — bind this shell to an account (password via masked
 * TUI input unless given as the second argument). */
static int CmdLogin(int argc, char *argv[]) {
    char name[USER_NAME_MAX];
    char pw[USER_PW_MAX];

    if (argc >= 2) {
        strncpy(name, argv[1], sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
    } else {
        /* Prompt at the CURRENT cursor (no absolute-coordinate overlay):
         * the line editor echoes as usual. */
        ShellWrite("User: ");
        if (ReadLine(name, sizeof(name)) < 0)
            return -1;
    }
    if (argc >= 3) {
        strncpy(pw, argv[2], sizeof(pw) - 1);
        pw[sizeof(pw) - 1] = '\0';
    } else {
        /* Masked entry: echo '*' (password). */
        ShellWrite("Password: ");
        if (ReadLineMasked(pw, sizeof(pw)) < 0)
            return -1;
    }

    user_req_login_t req;
    memset(&req, 0, sizeof(req));
    req.op = USER_OP_LOGIN;
    strncpy(req.name, name, sizeof(req.name) - 1);
    strncpy(req.password, pw, sizeof(req.password) - 1);
    user_resp_login_t resp;
    memset(&resp, 0, sizeof(resp));
    int r = UserCall(&req, (int)sizeof(req), &resp, (int)sizeof(resp));
    if (r < 0) {
        ShellPrintf("login: ipc FAILED (%d)\n", r);
        return -1;
    }
    if (resp.ret < 0) {
        ShellPrintf("login: FAILED (%d) - bad name or password\n", resp.ret);
        return -1;
    }
    ShellPrintf("login: ok - '%s' (%s)\n", resp.name, role_name(resp.role));

    /* The command policy is role-dependent: reload it so the new
     * role's allow/deny verdicts apply immediately. */
    CmdFilterLoad();
    return 0;
}

static int CmdLogout(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    user_req_login_t req;
    memset(&req, 0, sizeof(req));
    req.op = USER_OP_LOGOUT;
    user_resp_login_t resp;
    memset(&resp, 0, sizeof(resp));
    int r = UserCall(&req, (int)sizeof(req), &resp, (int)sizeof(resp));
    if (r < 0 || resp.ret < 0) {
        ShellPrintf("logout: FAILED (%d)\n", r < 0 ? r : resp.ret);
        return -1;
    }
    ShellPrintf("logout: ok\n");
    /* Role reverts on logout: reload the command policy. */
    CmdFilterLoad();
    return 0;
}

static int CmdWhoami(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    user_req_login_t req;
    memset(&req, 0, sizeof(req));
    req.op = USER_OP_WHOAMI;
    user_resp_login_t resp;
    memset(&resp, 0, sizeof(resp));
    int r = UserCall(&req, (int)sizeof(req), &resp, (int)sizeof(resp));
    if (r < 0) {
        ShellPrintf("whoami: ipc FAILED (%d)\n", r);
        return -1;
    }
    if (resp.ret < 0) {
        /* UNIX-style: not logged in -> "nobody" (exit 0). */
        ShellWrite("nobody\n");
        return 0;
    }
    ShellPrintf("%s (%s)\n", resp.name, role_name(resp.role));
    return 0;
}

/* passwd [name] — change own password (TUI masked input), or another
 * user's when given a name(requires admin re-auth). */
static int CmdPasswd(int argc, char *argv[]) {
    char oldpw[USER_PW_MAX];
    char newpw[USER_PW_MAX];

    if (TuiInputLine(5, 30, "Current password: ", oldpw, sizeof(oldpw), 1) < 0)
        return -1;
    if (TuiInputLine(5, 31, "New password: ", newpw, sizeof(newpw), 1) < 0)
        return -1;

    user_req_passwd_t req;
    memset(&req, 0, sizeof(req));
    req.op = USER_OP_PASSWD;
    strncpy(req.old_password, oldpw, sizeof(req.old_password) - 1);
    strncpy(req.new_password, newpw, sizeof(req.new_password) - 1);
    if (argc >= 2)
        strncpy(req.name, argv[1], sizeof(req.name) - 1);

    user_resp_passwd_t resp;
    memset(&resp, 0, sizeof(resp));
    int r = UserCall(&req, (int)sizeof(req), &resp, (int)sizeof(resp));
    if (r < 0 || resp.ret < 0) {
        ShellPrintf("passwd: FAILED (%d)\n", r < 0 ? r : resp.ret);
        return -1;
    }
    ShellPrintf("passwd: ok\n");
    return 0;
}

/* useradd <name> <role> [password] — create an account (admin only).
 * Role: owner|admin|standard|child|guest|auditor (or numeric).  An
 * unrecognized role name is an ERROR (never silently 0/OWNER). */
static int CmdUseradd(int argc, char *argv[]) {
    if (argc < 3) {
        ShellWrite("Usage: useradd <name> <role> [password]\n");
        return -1;
    }
    uint32_t role;
    int      role_ok = 1;
    if (strcmp(argv[2], "owner") == 0) role = PERM_ROLE_OWNER;
    else if (strcmp(argv[2], "admin") == 0) role = PERM_ROLE_ADMIN;
    else if (strcmp(argv[2], "standard") == 0) role = PERM_ROLE_STANDARD;
    else if (strcmp(argv[2], "child") == 0) role = PERM_ROLE_CHILD;
    else if (strcmp(argv[2], "guest") == 0) role = PERM_ROLE_GUEST;
    else if (strcmp(argv[2], "auditor") == 0) role = PERM_ROLE_AUDITOR;
    else {
        /* Numeric role: accept only a pure number in [0, PERM_ROLE_MAX). */
        char *end = NULL;
        long  v   = strtol(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || v < 0 || v >= PERM_ROLE_MAX) {
            role_ok = 0;
            role    = 0;
        } else {
            role = (uint32_t)v;
        }
    }
    if (!role_ok) {
        ShellPrintf("useradd: invalid role '%s' "
                     "(owner|admin|standard|child|guest|auditor)\n",
                     argv[2]);
        return -2; /* ERR_INVAL */
    }

    char pw[USER_PW_MAX];
    if (argc >= 4) {
        strncpy(pw, argv[3], sizeof(pw) - 1);
        pw[sizeof(pw) - 1] = '\0';
    } else {
        if (TuiInputLine(5, 30, "Password for new user: ", pw, sizeof(pw), 1) < 0)
            return -1;
    }

    user_req_login_t req;
    memset(&req, 0, sizeof(req));
    req.op = USER_OP_USERADD;
    strncpy(req.name, argv[1], sizeof(req.name) - 1);
    strncpy(req.password, pw, sizeof(req.password) - 1);
    req.role = role;
    user_resp_login_t resp;
    memset(&resp, 0, sizeof(resp));
    int r = UserCall(&req, (int)sizeof(req), &resp, (int)sizeof(resp));
    if (r < 0 || resp.ret < 0) {
        ShellPrintf("useradd: FAILED (%d)\n", r < 0 ? r : resp.ret);
        return -1;
    }
    ShellPrintf("useradd: ok - '%s' (%s)\n", argv[1], role_name(role));
    return 0;
}

static int CmdUserdel(int argc, char *argv[]) {
    if (argc < 2) {
        ShellWrite("Usage: userdel <name>\n");
        return -1;
    }
    user_req_login_t req;
    memset(&req, 0, sizeof(req));
    req.op = USER_OP_USERDEL;
    strncpy(req.name, argv[1], sizeof(req.name) - 1);
    user_resp_login_t resp;
    memset(&resp, 0, sizeof(resp));
    int r = UserCall(&req, (int)sizeof(req), &resp, (int)sizeof(resp));
    if (r < 0 || resp.ret < 0) {
        ShellPrintf("userdel: FAILED (%d)\n", r < 0 ? r : resp.ret);
        return -1;
    }
    ShellPrintf("userdel: ok\n");
    return 0;
}

/* user_lock <name> / user_unlock <name> — admin disables or re-enables
 * an account (also resets its lockout counter). */
static int CmdUserlock(int argc, char *argv[]) {
    if (argc < 2) {
        ShellWrite("Usage: user_lock <name>\n");
        return -1;
    }
    user_req_login_t req;
    memset(&req, 0, sizeof(req));
    req.op = USER_OP_LOCK;
    strncpy(req.name, argv[1], sizeof(req.name) - 1);
    user_resp_login_t resp;
    memset(&resp, 0, sizeof(resp));
    int r = UserCall(&req, (int)sizeof(req), &resp, (int)sizeof(resp));
    if (r < 0 || resp.ret < 0) {
        ShellPrintf("user_lock: FAILED (%d)\n", r < 0 ? r : resp.ret);
        return -1;
    }
    ShellPrintf("user_lock: '%s' locked\n", argv[1]);
    return 0;
}

static int CmdUserunlock(int argc, char *argv[]) {
    if (argc < 2) {
        ShellWrite("Usage: user_unlock <name>\n");
        return -1;
    }
    user_req_login_t req;
    memset(&req, 0, sizeof(req));
    req.op = USER_OP_UNLOCK;
    strncpy(req.name, argv[1], sizeof(req.name) - 1);
    user_resp_login_t resp;
    memset(&resp, 0, sizeof(resp));
    int r = UserCall(&req, (int)sizeof(req), &resp, (int)sizeof(resp));
    if (r < 0 || resp.ret < 0) {
        ShellPrintf("user_unlock: FAILED (%d)\n", r < 0 ? r : resp.ret);
        return -1;
    }
    ShellPrintf("user_unlock: '%s' unlocked\n", argv[1]);
    return 0;
}

static int CmdUsers(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    user_req_login_t req;
    memset(&req, 0, sizeof(req));
    req.op = USER_OP_USERS;
    user_resp_login_t resp;
    memset(&resp, 0, sizeof(resp));
    int r = UserCall(&req, (int)sizeof(req), &resp, (int)sizeof(resp));
    if (r < 0 || resp.ret < 0) {
        if (resp.ret == -9) /* ERR_DENIED */
            ShellWrite("users: permission denied - OWNER/ADMIN login required\n");
        else
            ShellPrintf("users: FAILED (%d) - run 'login' first\n", r < 0 ? r : resp.ret);
        return -1;
    }
    /* reason holds "name role;name role;..." lines. */
    char *p = resp.reason;
    if (resp.count == 0) {
        ShellWrite("Accounts: (none)\n");
        return 0;
    }
    /* v1.3: TUI popup listing the accounts (Enter/q to dismiss). */
    static char lines[USER_MAX_ACCOUNTS][40];
    static const char *ptrs[USER_MAX_ACCOUNTS];
    int shown = 0;
    while (*p && shown < USER_MAX_ACCOUNTS) {
        char *semi = strchr(p, ';');
        if (!semi)
            break;
        *semi = '\0';
        strncpy(lines[shown], p, sizeof(lines[shown]) - 1);
        lines[shown][sizeof(lines[shown]) - 1] = '\0';
        ptrs[shown] = lines[shown];
        shown++;
        p = semi + 1;
    }
    /* Clear first: the popup must not overlay leftover shell output
     * (that made the TUI look broken/stuck).  The shell loop reprints
     * the prompt after the menu is dismissed. */
    if (s_term_port >= 0) {
        u32 req[2];
        u32 resp[1];
        req[0]       = TERM_OP_CLEAR;
        req[1]       = 0;
        int resp_len = (int)sizeof(resp);
        IpcCall(s_term_port, (const void *)req, 8, (void *)resp, &resp_len);
    }
    (void)TuiMenu(38, 6, 40, shown + 2, "Accounts (Enter/q)", ptrs, shown, NULL);
    return 0;
}

/* ====================================================================
 * Exit guard: stop <svc>
 * 1. TUI confirm dialog 2. admin password (masked) 3. USER_OP_STOP.
 * The user service re-checks OWNER/ADMIN and refuses system-critical
 * services; the kill is executed there (shell is not management-plane).
 * ==================================================================== */

static int CmdStop(int argc, char *argv[]) {
    if (argc < 2) {
        ShellWrite("Usage: stop <svc-name>\n");
        return -1;
    }

    /* 1. Confirm dialog. */
    char msg[96];
    snprintf(msg, sizeof(msg), "Stop system program '%s'?", argv[1]);
    int yes = TuiConfirm(20, 14, 60, "Confirm Stop", msg,
                          "Type y to confirm, n to cancel");
    if (yes < 0) {
        ShellPrintf("stop: dialog error (%d)\n", yes);
        return -1;
    }
    if (!yes) {
        ShellPrintf("stop: cancelled\n");
        return 0;
    }

    /* 2. Admin password (masked TUI input), verified against the
     * CURRENTLY LOGGED-IN account. */
    user_resp_login_t who;
    memset(&who, 0, sizeof(who));
    {
        user_req_login_t wq;
        memset(&wq, 0, sizeof(wq));
        wq.op = USER_OP_WHOAMI;
        int r = UserCall(&wq, (int)sizeof(wq), &who, (int)sizeof(who));
        if (r < 0 || who.ret < 0) {
            ShellPrintf("stop: not logged in - run 'login' first (%d)\n", r < 0 ? r : who.ret);
            return -1;
        }
    }

    char pw[USER_PW_MAX];
    if (TuiInputLine(5, 31, "Admin password: ", pw, sizeof(pw), 1) < 0)
        return -1;

    user_req_login_t vq;
    memset(&vq, 0, sizeof(vq));
    vq.op = USER_OP_VERIFY;
    strncpy(vq.name, who.name, sizeof(vq.name) - 1);
    strncpy(vq.password, pw, sizeof(vq.password) - 1);
    user_resp_login_t vr;
    memset(&vr, 0, sizeof(vr));
    int r = UserCall(&vq, (int)sizeof(vq), &vr, (int)sizeof(vr));
    if (r < 0 || vr.ret < 0) {
        ShellPrintf("stop: wrong password or verification failed (%d)\n", r < 0 ? r : vr.ret);
        return -1;
    }

    /* 3. Execute the stop (service re-checks OWNER/ADMIN + critical). */
    user_req_stop_t sq;
    memset(&sq, 0, sizeof(sq));
    sq.op = USER_OP_STOP;
    strncpy(sq.svc, argv[1], sizeof(sq.svc) - 1);
    user_resp_stop_t sr;
    memset(&sr, 0, sizeof(sr));
    r = UserCall(&sq, (int)sizeof(sq), &sr, (int)sizeof(sr));
    if (r < 0 || sr.ret < 0) {
        ShellPrintf("stop: FAILED (%d)%s%s\n",
                     r < 0 ? r : sr.ret,
                     sr.detail[0] ? " - " : "",
                     sr.detail);
        return -1;
    }
    ShellPrintf("stop: %s\n", sr.detail);
    return 0;
}
