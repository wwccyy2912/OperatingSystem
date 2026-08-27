/*
 * shell.c - Simple terminal shell (TTY-like)
 * Copyright (c) 2026 OpSys Project
 *
 * Runs as its OWN process, spawned by the manager via
 * SYS_PROCESS_CREATE.  All I/O goes through the framebuffer terminal
 * and PS/2 keyboard services:
 *   input  — ipc_call(READ_BLOCK) on the 'keyboard' port (blocking: the
 *            shell thread parks in the kernel until key bytes arrive)
 *   output — ipc_call(WRITE) on the 'term' port (rendered on the
 *            framebuffer by the terminal service)
 * Supports basic line editing.  Commands are registered at runtime
 * into a singly-linked list via shell_register_command() — the shell
 * registers its built-ins at the top of main().  Architecture:
 *
 *   main()
 *     ├─ shell_register_command() × 14    (built-in commands)
 *     └─ shell_loop()
 *          ├─ print prompt
 *          ├─ read_line() ← ipc_call(READ_BLOCK)  (blocks until input)
 *          │    ├─ echo / backspace handling
 *          │    └─ enter → returns buffer
 *          └─ execute()
 *               ├─ parse argv
 *               └─ linked-list dispatch  (strcmp over registered names)
 */

#include "shell.h"              /* cmd_func_t, shell_register_command */
#include "../lib/libc/stdio.h"  /* printf -> SYS_DEBUG_LOG (serial) */
#include "../lib/libc/stdlib.h" /* atoi */
#include "../lib/libc/string.h" /* strcmp, strlen, strdup */
#include "../lib/libfs/fs.h"    /* libfs VFS client (ls/cat/stat/tee/fallocate/mkdir/rm) */
#include "../lib/libpkg/pkg.h"  /* libpkg pkg-manager client (pkg_*) */
#include "../lib/libos/syscalls.h"
#include "../perm/perm.h" /* Powerbox protocol (perm_answer/perm_revoke) */
#include "../user/user.h" /* user account protocol */
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
 * own subject comes from get_subject() when needed. */

/* Terminal service protocol (mirrors user/services/term/term.c) */
#define TERM_OP_WRITE 1
#define TERM_OP_CLEAR 2
#define TERM_CHUNK    32 /* max payload bytes per WRITE */

/* Keyboard service protocol (mirrors user/services/keyboard/keyboard.c) */
#define KBD_OP_READ       1
#define KBD_OP_READ_BLOCK 2
#define KBD_CHUNK         32 /* max payload bytes per READ */

/* ====================================================================
 * Forward declarations for built-in commands
 * ==================================================================== */

static int cmd_help(int argc, char *argv[]);
static int cmd_echo(int argc, char *argv[]);
static int cmd_pid(int argc, char *argv[]);
static int cmd_free(int argc, char *argv[]);
static int cmd_clear(int argc, char *argv[]);
static int cmd_cap(int argc, char *argv[]);
static int cmd_mouse(int argc, char *argv[]);
static int cmd_ports(int argc, char *argv[]);
static int cmd_sleep(int argc, char *argv[]);
static int cmd_threads(int argc, char *argv[]);
static int cmd_mutex(int argc, char *argv[]);
static int cmd_exec(int argc, char *argv[]);
static int cmd_uptime(int argc, char *argv[]);
static int cmd_exit(int argc, char *argv[]);
static int cmd_reboot(int argc, char *argv[]);
static int cmd_kill(int argc, char *argv[]);
static int cmd_ps(int argc, char *argv[]);
static int cmd_ls(int argc, char *argv[]);
static int cmd_cat(int argc, char *argv[]);
static int cmd_stat(int argc, char *argv[]);
static int cmd_tee(int argc, char *argv[]);
static int cmd_fallocate(int argc, char *argv[]);
static int cmd_mkdir(int argc, char *argv[]);
static int cmd_rm(int argc, char *argv[]);
/* Phase 2: bookmarks + Powerbox + mv (design §8) */
static int cmd_bm_create(int argc, char *argv[]);
static int cmd_bm_resolve(int argc, char *argv[]);
static int cmd_bm_revoke(int argc, char *argv[]);
static int cmd_perm_answer(int argc, char *argv[]);
static int cmd_perm_query(int argc, char *argv[]);
static int cmd_perm_revoke(int argc, char *argv[]);
static int cmd_mv(int argc, char *argv[]);
static int cmd_pkg(int argc, char *argv[]);
static int cmd_login(int argc, char *argv[]);
static int cmd_logout(int argc, char *argv[]);
static int cmd_whoami(int argc, char *argv[]);
static int cmd_passwd(int argc, char *argv[]);
static int cmd_useradd(int argc, char *argv[]);
static int cmd_userdel(int argc, char *argv[]);
static int cmd_userlock(int argc, char *argv[]);
static int cmd_userunlock(int argc, char *argv[]);
static int cmd_users(int argc, char *argv[]);
static int cmd_stop(int argc, char *argv[]);
static int cmd_export(int argc, char *argv[]);
static int cmd_unset(int argc, char *argv[]);
static int cmd_env(int argc, char *argv[]);
static int cmd_policy_set(int argc, char *argv[]);
static int cmd_policy_dump(int argc, char *argv[]);
static int user_call(const void *req, int req_len, void *resp, int resp_len);
static int cmd_cd(int argc, char *argv[]);
static int cmd_pwd(int argc, char *argv[]);
static int cmd_scroll(int argc, char *argv[]);
static int cmd_disk(int argc, char *argv[]);
static int cmd_shutdown(int argc, char *argv[]);
static int cmd_bm(int argc, char *argv[]);
static int cmd_perm(int argc, char *argv[]);
static int cmd_policy(int argc, char *argv[]);
static int cmd_fm(int argc, char *argv[]);
static int shell_resolve_path(const char *path, char *out, size_t outsz);

/* ====================================================================
 * Runtime command registry
 *
 * The command set is a singly-linked list built at runtime via
 * shell_register_command(), NOT a compile-time table.  The shell
 * registers its 12 built-ins at the top of shell_main(); other
 * services (e.g. the manager) may register their own commands before
 * the shell starts reading input.
 *
 * Single-writer rule: the list is written only BEFORE the shell loop
 * starts reading it — the shell registers its own built-ins, and the
 * manager registers its "services" command on its own thread before it
 * calls thread_create(shell_main, ...).  After that neither thread
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
static u32 cmd_filter_hash(const char *s) {
    u32 h = 2166136261u;
    while (*s) {
        h ^= (u8)*s++;
        h *= 16777619u;
    }
    return h % CMD_FILTER_SLOTS;
}

/* Record a verdict (deny=1/0) for a command. */
static void cmd_filter_set(const char *name, int deny) {
    u32 slot = cmd_filter_hash(name);
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
static int cmd_filter_denied(const char *name) {
    if (!s_filter_ready)
        return 0; /* no policy loaded -> allow (capability still gates) */
    u32 slot = cmd_filter_hash(name);
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

static int cmd_is_rescue(const char *name) {
    for (u32 i = 0; i < sizeof(s_rescue_cmds) / sizeof(s_rescue_cmds[0]); i++)
        if (strcmp(s_rescue_cmds[i], name) == 0)
            return 1;
    return 0;
}

/* Query the policy service for the caller's role and mark the command
 * table.  Falls back to rescue-only mode on any failure. */
static void cmd_filter_load(void) {
    memset(s_cmd_filter, 0, sizeof(s_cmd_filter));
    s_filter_ready = 0;

    int port = port_get(POLICY_PORT_NAME);
    if (port < 0)
        return; /* service down: no policy -> all allowed (rescue implicit) */

    /* Resolve the caller's role via the user service (WHOAMI). */
    u32 role = 2; /* PERM_ROLE_STANDARD default */
    {
        int uport = port_get("user");
        if (uport >= 0) {
            user_req_login_t req;
            memset(&req, 0, sizeof(req));
            req.op = USER_OP_WHOAMI;
            user_resp_login_t resp;
            memset(&resp, 0, sizeof(resp));
            int rlen = (int)sizeof(resp);
            if (ipc_call(uport, &req, (int)sizeof(req), &resp, &rlen) == 0 && resp.ret == 0)
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
    if (ipc_call(port, &q, (int)sizeof(q), &resp, &rlen) < 0 || resp.ret < 0)
        return;

    /* Apply verdicts; rescue commands are force-allowed. */
    u32 n = 0;
    for (cmd_node_t *node = s_cmd_head; node && n < resp.count; node = node->next, n++) {
        u8 v = resp.verdicts[n];
        if (v == POLICY_DENY && !cmd_is_rescue(node->name))
            cmd_filter_set(node->name, 1);
        else
            cmd_filter_set(node->name, 0);
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
int shell_register_command(const char *name, const char *help, cmd_func_t func) {
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

static int s_term_port = -1; /* resolved once in shell_main()  */
static int s_kbd_port  = -1; /* resolved once in shell_main()  */

/* Send one byte to the terminal service (WRITE op). */
static void shell_putc(char c) {
    if (s_term_port < 0)
        return;

    u32 req[2 + 1]; /* { op; len; data[1] } */
    u32 resp[1];    /* { ret }             */
    req[0]         = TERM_OP_WRITE;
    req[1]         = 1;
    ((u8 *)req)[8] = (u8)c;
    int resp_len   = (int)sizeof(resp);
    ipc_call(s_term_port, (const void *)req, 9, (void *)resp, &resp_len);
}

/* Send a NUL-terminated string to the terminal service (WRITE op),
 * chunked so the request buffer stays small and bounded. */
static void shell_write(const char *s) {
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
        ipc_call(s_term_port, (const void *)req, 8 + n, (void *)resp, &resp_len);
        s += n;
    }
}

/* Local int -> decimal string (the libc has no itoa). */
static void shell_itoa(int v, char *buf) {
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
static void shell_utoa_hex(u32 v, char *buf) {
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
static void shell_printf(const char *fmt, ...) {
    va_list ap;
    char    num[12];

    va_start(ap, fmt);
    for (const char *p = fmt; *p != '\0'; p++) {
        if (*p != '%') {
            shell_putc(*p);
            continue;
        }
        p++;
        if (*p == '\0')
            break;
        switch (*p) {
        case '%':
            shell_putc('%');
            break;
        case 'c':
            shell_putc((char)va_arg(ap, int));
            break;
        case 's':
            shell_write(va_arg(ap, const char *));
            break;
        case 'd':
            shell_itoa(va_arg(ap, int), num);
            shell_write(num);
            break;
        case 'x':
            shell_utoa_hex(va_arg(ap, u32), num);
            shell_write(num);
            break;
        default:
            shell_putc('%');
            shell_putc(*p);
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
static void shell_prompt(char *out, size_t outsz) {
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
static void shell_redraw_line(const char *line, int pos) {
    char prompt[LINE_BUF_SIZE + 16];
    shell_prompt(prompt, sizeof(prompt));
    int plen = (int)strlen(prompt);

    /* Current row: query the term cursor, then set back to it after
     * re-printing (the shell's output cursor is on the prompt row). */
    u32 row = 0;
    if (s_term_port >= 0) {
        u32 req[2];
        u8  resp[16];
        req[0] = 7; /* TERM_OP_GET_CURSOR */
        req[1] = 0;
        int rlen = (int)sizeof(resp);
        if (ipc_call(s_term_port, req, 8, resp, &rlen) == 0 && rlen >= 12)
            row = ((u32 *)resp)[2];
    }

    /* Erase the current row and re-print prompt + line.  The extra
     * ERASE_MARGIN cells cover the row's right edge past the text
     * (leftovers from a longer previous line). */
    const int erase_margin = 4;
    shell_write("\r");
    for (int i = 0; i < plen + (int)strlen(line) + erase_margin; i++)
        shell_write(" ");
    shell_write("\r");
    shell_write(prompt);
    shell_write(line);

    /* Place the cursor at (plen + pos, row). */
    if (s_term_port >= 0) {
        u32 req[4];
        req[0] = 6; /* TERM_OP_SET_CURSOR */
        req[1] = 8;
        req[2] = (u32)(plen + pos);
        req[3] = row;
        int resp_len = 8;
        u8  resp[8];
        (void)ipc_call(s_term_port, req, 8 + 8, resp, &resp_len);
    } else {
        int llen = (int)strlen(line);
        for (int i = llen; i > pos; i--)
            shell_write("\b");
    }
}

/* Tab completion: first token -> command names; otherwise a path
 * fragment -> directory entries (absolute or cwd-relative).  Completes
 * to the longest common prefix; when no unique prefix, lists matches.
 * Returns 1 if the buffer changed. */
static int shell_complete(char *buf, int *pos, int maxlen) {
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
                for (int i = 0; i < common; i++)
                    buf[tok_start + i] = matches[0][i];
                buf[tok_start + common] = ' ';
                *pos = tok_start + common + 1;
                buf[*pos] = '\0';
                shell_redraw_line(buf, *pos);
                return 1;
            }
        } else if (nm > 1 && common > toklen) {
            for (int i = 0; i < common; i++)
                buf[tok_start + i] = matches[0][i];
            *pos = tok_start + common;
            buf[*pos] = '\0';
            shell_redraw_line(buf, *pos);
            return 1;
        } else if (nm > 1) {
            shell_write("\n");
            for (int i = 0; i < nm; i++) {
                shell_write("  ");
                shell_write(matches[i]);
                shell_write("\n");
            }
            shell_redraw_line(buf, *pos);
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
            if (shell_resolve_path(tmp, dir, sizeof(dir)) < 0)
                return 0;
        }
        strncpy(frag, slash + 1, sizeof(frag) - 1);
        frag[sizeof(frag) - 1] = '\0';
    } else {
        if (shell_resolve_path("", dir, sizeof(dir)) < 0)
            return 0;
        strncpy(frag, tok, sizeof(frag) - 1);
        frag[sizeof(frag) - 1] = '\0';
    }

    static vfs_enum_batch_t batch;
    static char matches[COMPLETE_MAX_MATCHES][64];
    int                    nm = 0, common = -1;
    vfs_handle_t           e;
    int                    r = fs_enum_begin(dir, &e);
    if (r < 0)
        return 0;
    for (;;) {
        r = fs_enum_next(e, &batch);
        if (r < 0 || batch.batch_count == 0)
            break;
        for (u32 i = 0; i < batch.batch_count && nm < COMPLETE_MAX_MATCHES; i++) {
            if (strncmp(batch.batch[i], frag, strlen(frag)) == 0) {
                strncpy(matches[nm], batch.batch[i], 63);
                matches[nm][63] = '\0';
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
    fs_enum_end(e);

    if (nm == 1 || (nm > 1 && common > (int)strlen(frag))) {
        if (tok_start + common < maxlen) {
            for (int i = 0; i < common; i++)
                buf[tok_start + i] = matches[0][i];
            *pos = tok_start + common;
            buf[*pos] = '\0';
            shell_redraw_line(buf, *pos);
        }
        return 1;
    }
    if (nm > 1) {
        shell_write("\n");
        for (int i = 0; i < nm; i++) {
            shell_write("  ");
            shell_write(matches[i]);
            shell_write("\n");
        }
        shell_redraw_line(buf, *pos);
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
static int read_line_impl(char *buf, int maxlen, int mask);

static int read_line(char *buf, int maxlen) {
    return read_line_impl(buf, maxlen, 0);
}

/* Password entry: echo '*' instead of the typed characters. */
static int read_line_masked(char *buf, int maxlen) {
    return read_line_impl(buf, maxlen, 1);
}

/* Shared line reader: mask=1 echoes '*' (password entry). */
static int read_line_impl(char *buf, int maxlen, int mask) {
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
        int ret      = ipc_call(s_kbd_port, (const void *)req, 8, (void *)resp, &resp_len);
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
            thread_yield();
            continue;
        }

        for (int i = 0; i < n; i++) {
            unsigned char ch = (unsigned char)((u8 *)resp)[4 + i];

            switch (ch) {
            case '\r':
            case '\n':
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
                shell_write("\r\n");
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
                    pos--;
                    if (pos < (int)strlen(buf)) {
                        /* Deleting in the middle: shift the tail left
                         * and redraw (a bare "\b \b" would leave the
                         * rest of the line out of place). */
                        for (int k = pos; buf[k] != '\0'; k++)
                            buf[k] = buf[k + 1];
                        shell_redraw_line(buf, pos);
                    } else {
                        shell_write("\b \b"); /* erase at end of line */
                        buf[pos] = '\0';
                    }
                }
                break;

            case '\t':
                /* Tab — command / path completion (v1.3). */
                (void)shell_complete(buf, &pos, maxlen);
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
                    shell_redraw_line(buf, pos);
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
                    shell_redraw_line(buf, pos);
                }
                break;

            case 0x01: /* Home */
                pos = 0;
                shell_redraw_line(buf, pos);
                break;

            case 0x05: /* End */
                pos = (int)strlen(buf);
                shell_redraw_line(buf, pos);
                break;

            case 0x02: /* PgUp — first history entry */
                if (s_hist_count > 0) {
                    s_hist_view = (s_hist_next + HIST_MAX - s_hist_count) % HIST_MAX;
                    strncpy(buf, s_history[s_hist_view], (size_t)maxlen - 1);
                    buf[maxlen - 1] = '\0';
                    pos = (int)strlen(buf);
                    shell_redraw_line(buf, pos);
                }
                break;

            case 0x06: /* PgDn — newest history entry */
                if (s_hist_count > 0) {
                    s_hist_view = (s_hist_next + HIST_MAX - 1) % HIST_MAX;
                    strncpy(buf, s_history[s_hist_view], (size_t)maxlen - 1);
                    buf[maxlen - 1] = '\0';
                    pos = (int)strlen(buf);
                    shell_redraw_line(buf, pos);
                }
                break;

            case 0x10: /* Left arrow (DLE) — move cursor left */
                if (pos > 0) {
                    pos--;
                    shell_redraw_line(buf, pos);
                }
                break;

            case 0x14: /* Right arrow (DC4) — move cursor right */
                if (pos < (int)strlen(buf)) {
                    pos++;
                    shell_redraw_line(buf, pos);
                }
                break;

            default:
                if (ch >= ' ' && ch < 0x7F) {
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
                            shell_redraw_line(buf, pos);
                        } else {
                            buf[pos++] = (char)ch;
                            /* The function name is the mask flag: echo '*'
                             * for passwords, the char itself otherwise. */
                            shell_putc(mask ? '*' : (char)ch);
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

static int parse_line(char *line, char *argv[], int max_args) {
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
static void worker_func(void *arg);

/* ====================================================================
 * Command execution
 * ==================================================================== */

static int execute(char *line) {
    char *argv[MAX_ARGS];
    int   argc = parse_line(line, argv, MAX_ARGS);

    if (argc == 0)
        return 0;

    /* Policy gate (v0.5): a DENY verdict blocks execution. */
    if (cmd_filter_denied(argv[0])) {
        shell_printf("shell: command '%s' denied by policy\n", argv[0]);
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
                    if (shell_resolve_path(argv[i], pbuf[n], sizeof(pbuf[n])) == 0) {
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

    shell_printf("shell: unknown command '%s' (try 'help')\n", argv[0]);
    return -1;
}

/* ====================================================================
 * Main loop
 * ==================================================================== */

static void shell_loop(void) {
    /* Banner (ASCII only - UTF-8 box chars break VGA/vc terminals) */
    shell_write("\n");
    shell_write("Welcome to OpSys \n");
    shell_write("  Copyright (c) 2026 OpSys Project \n");
    shell_write("  shell.c - Simple terminal shell (TTY-like) \n");
    shell_write("  Type 'help' for a command list. \n");
    shell_write("\n");

    char line[LINE_BUF_SIZE];

    for (;;) {
        /* Prompt: PS1 override, else "opsys:<cwd>$ " (bash-style).
         * Environment is user preference only; it never carries
         * security policy. */
        char prompt[LINE_BUF_SIZE + 16];
        shell_prompt(prompt, sizeof(prompt));
        shell_write(prompt);
        int len = read_line(line, LINE_BUF_SIZE);
        if (len < 0) {
            thread_yield(); /* serial service unavailable */
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
            execute(line);
        }
    }
}

/* ====================================================================
 * Built-in commands
 * ==================================================================== */

static int cmd_help(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    shell_write("Available commands:\n");
    for (cmd_node_t *n = s_cmd_head; n != NULL; n = n->next) {
        shell_write("  ");
        shell_write(n->name);
        int pad = 12 - (int)strlen(n->name); /* %-12s */
        while (pad > 0) {
            shell_putc(' ');
            pad--;
        }
        shell_write("  ");
        shell_write(n->help);
        shell_write("\n");
    }
    return 0;
}

static int cmd_echo(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (i > 1)
            shell_write(" ");
        shell_write(argv[i]);
    }
    shell_write("\n");
    return 0;
}

/* export NAME=value | export NAME — set an environment variable (or
 * print its current value).  The environment carries per-process user
 * preferences (PS1/EDITOR/LANG); it never carries security policy. */
static int cmd_export(int argc, char *argv[]) {
    if (argc < 2) {
        shell_write("Usage: export NAME=value | export NAME\n");
        return -1;
    }
    const char *arg = argv[1];
    char       *eq  = strchr(arg, '=');
    if (eq) {
        char saved = *eq;
        *eq        = '\0';
        int r      = setenv(arg, eq + 1, 1);
        *eq        = saved;
        if (r < 0) {
            shell_printf("export: setenv(%s) failed\n", arg);
            return -1;
        }
        shell_printf("export: %s\n", arg);
        return 0;
    }
    const char *v = getenv(arg);
    if (v)
        shell_printf("%s=%s\n", arg, v);
    else
        shell_printf("%s: not set\n", arg);
    return 0;
}

/* unset NAME — remove an environment variable. */
static int cmd_unset(int argc, char *argv[]) {
    if (argc < 2) {
        shell_write("Usage: unset NAME\n");
        return -1;
    }
    int r = unsetenv(argv[1]);
    if (r < 0) {
        shell_printf("unset: invalid name '%s'\n", argv[1]);
        return -1;
    }
    shell_printf("unset: %s\n", argv[1]);
    return 0;
}

/* env — print the whole environment, one "NAME=value" per line. */
static int cmd_env(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    if (!environ) {
        shell_write("env: (empty)\n");
        return 0;
    }
    for (char **e = environ; *e; e++) {
        shell_write(*e);
        shell_write("\n");
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
static void path_normalize(const char *in, char *out, size_t outsz) {
    enum { PN_MAX_DEPTH = 24, PN_SEG_MAX = 64 };
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
        if (o + sl + 2 > outsz) /* '/' + seg + NUL won't fit: truncate */
            break;
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
static int shell_resolve_path(const char *path, char *out, size_t outsz) {
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
            shell_printf("path: too long\n");
            return -1;
        }
        if (strcmp(s_cwd, "/") == 0) {
            snprintf(raw, sizeof(raw), "/%s", path);
        } else {
            snprintf(raw, sizeof(raw), "%s/%s", s_cwd, path);
        }
    }
    path_normalize(raw, out, outsz);
    return 0;
}

/* cd [dir] — change the working directory.  No argument -> "/".  The
 * target must exist and be a directory (fs_get_item + type check). */
static int cmd_cd(int argc, char *argv[]) {
    const char *arg = (argc >= 2) ? argv[1] : "/";

    char url[LINE_BUF_SIZE];
    if (shell_resolve_path(arg, url, sizeof(url)) < 0)
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
    int             r = fs_get_item(url, &info);
    if (r < 0) {
        shell_printf("cd: %s FAILED (%d)\n", url, r);
        return -1;
    }
    if (info.type != VFS_ITEM_DIR) {
        shell_printf("cd: %s: not a directory\n", url);
        return -1;
    }
    strncpy(s_cwd, url, sizeof(s_cwd) - 1);
    s_cwd[sizeof(s_cwd) - 1] = '\0';
    return 0;
}

/* pwd — print the working directory. */
static int cmd_pwd(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    shell_write(s_cwd);
    shell_write("\n");
    return 0;
}

/* scroll [lines|end] — page the terminal view through the term's
 * scrollback buffer (v0.7 Track 4).  Positive delta pages back (older
 * lines), negative forward; "end" or 0 returns to the live screen.
 * The next shell output (a write to the term) resets the view. */
static int cmd_scroll(int argc, char *argv[]) {
    if (s_term_port < 0) {
        shell_write("scroll: terminal unavailable\n");
        return -1;
    }
    /* Default (no arg): page back one screen (~20 lines). */
    i32 delta = 20;
    if (argc >= 2) {
        if (strcmp(argv[1], "end") == 0)
            delta = 0;
        else {
            char *end = NULL;
            long  v   = strtol(argv[1], &end, 10);
            if (!end || *end != '\0') {
                shell_write("scroll: invalid line count (use a number or 'end')\n");
                return -1;
            }
            delta = (i32)v;
        }
    }
    u32 req[3];
    req[0] = 10; /* TERM_OP_SCROLLVIEW */
    req[1] = 4;
    req[2] = (u32)delta;
    int resp_len = 8;
    u8  resp[8];
    int r = ipc_call(s_term_port, req, 12, resp, &resp_len);
    if (r < 0 || (i32)((u32 *)resp)[0] < 0) {
        shell_write("scroll: term op failed\n");
        return -1;
    }
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
static int cmd_disk(int argc, char *argv[]) {
    if (argc < 2) {
        shell_write("Usage: disk list | mount <vol> | unmount <vol> | "
                    "format <vol> | fill <vol> [bytes]\n");
        return -1;
    }

    if (strcmp(argv[1], "list") == 0) {
        static vfs_vol_info_t vols[VFS_MAX_VOLS];
        u32                   count = 0;
        int                   r     = fs_list_volumes(vols, &count);
        if (r < 0) {
            shell_printf("disk: list FAILED (%d)\n", r);
            return -1;
        }
        if (count == 0) {
            shell_write("disk: no volumes mounted\n");
            return 0;
        }
        for (u32 i = 0; i < count; i++) {
            char url[80];
            snprintf(url, sizeof(url), "/%s", vols[i].mount_name);
            u64 total = 0, used = 0;
            u32 ro   = 0;
            int  sr  = fs_stat_volume(url, &total, &used, &ro);
            shell_write(vols[i].mount_name);
            shell_write("  ");
            if (sr == 0)
                shell_printf("%d KiB used / %d KiB total%s\n", (int)(used / 1024u),
                             (int)(total / 1024u), ro ? " (ro)" : "");
            else
                shell_write("(stat unavailable)\n");
        }
        shell_printf("disk: %d volume(s)\n", (int)count);
        return 0;
    }

    if (argc < 3) {
        shell_write("Usage: disk list | mount <vol> | unmount <vol> | "
                    "format <vol> | fill <vol> [bytes]\n");
        return -1;
    }
    if (strlen(argv[2]) >= 64) {
        shell_printf("disk: volume name too long\n");
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
        shell_printf("disk: unknown subcommand '%s'\n", argv[1]);
        return -1;
    }

    /* Format is destructive: require an explicit confirmation word. */
    if (req.op == USER_OP_DISK_FORMAT) {
        shell_printf("disk: formatting '%s' destroys ALL data on it.\n", req.volume);
        shell_write("Type YES to continue: ");
        char conf[8];
        if (read_line(conf, sizeof(conf)) < 0)
            return -1;
        if (strcmp(conf, "YES") != 0) {
            shell_write("disk: format cancelled\n");
            return 0;
        }
    }

    user_resp_disk_t resp;
    memset(&resp, 0, sizeof(resp));
    int r = user_call(&req, (int)sizeof(req), &resp, (int)sizeof(resp));
    if (r < 0) {
        shell_printf("disk: ipc FAILED (%d)\n", r);
        return -1;
    }
    if (resp.ret < 0) {
        shell_printf("disk: %s '%s' FAILED (%d)", argv[1], req.volume, resp.ret);
        if (resp.detail[0])
            shell_printf(" - %s", resp.detail);
        shell_write("\n");
        return -1;
    }
    if (req.op == USER_OP_DISK_FILL)
        shell_printf("disk: %d KiB written to %s/fill.bin\n", (int)(resp.bytes / 1024u),
                     req.volume);
    else
        shell_printf("disk: %s '%s' ok\n", argv[1], req.volume);
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

static int cmd_bm(int argc, char *argv[]) {
    if (argc < 2) {
        shell_write("Usage: bm <create|resolve|revoke> ...\n");
        return -1;
    }
    if (strcmp(argv[1], "create") == 0)
        return cmd_bm_create(argc - 1, argv + 1);
    if (strcmp(argv[1], "resolve") == 0)
        return cmd_bm_resolve(argc - 1, argv + 1);
    if (strcmp(argv[1], "revoke") == 0)
        return cmd_bm_revoke(argc - 1, argv + 1);
    shell_printf("bm: unknown subcommand '%s'\n", argv[1]);
    return -1;
}

static int cmd_perm(int argc, char *argv[]) {
    if (argc < 2) {
        shell_write("Usage: perm <answer|query|revoke> ...\n");
        return -1;
    }
    if (strcmp(argv[1], "answer") == 0)
        return cmd_perm_answer(argc - 1, argv + 1);
    if (strcmp(argv[1], "query") == 0)
        return cmd_perm_query(argc - 1, argv + 1);
    if (strcmp(argv[1], "revoke") == 0)
        return cmd_perm_revoke(argc - 1, argv + 1);
    shell_printf("perm: unknown subcommand '%s'\n", argv[1]);
    return -1;
}

static int cmd_policy(int argc, char *argv[]) {
    if (argc < 2) {
        shell_write("Usage: policy <set|dump> ...\n");
        return -1;
    }
    if (strcmp(argv[1], "set") == 0)
        return cmd_policy_set(argc - 1, argv + 1);
    if (strcmp(argv[1], "dump") == 0)
        return cmd_policy_dump(argc - 1, argv + 1);
    shell_printf("policy: unknown subcommand '%s'\n", argv[1]);
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

/* Enumerate `dir` (an absolute URL) into items[]; returns count. */
static int fm_enum(const char *dir, char items[][64], int cap) {
    static vfs_enum_batch_t batch; /* ~16.5 KB — keep off the stack */
    vfs_handle_t            e;
    int                     r = fs_enum_begin(dir, &e);
    if (r < 0)
        return r;
    int total = 0;
    for (;;) {
        r = fs_enum_next(e, &batch);
        if (r < 0) {
            fs_enum_end(e);
            return r;
        }
        if (batch.batch_count == 0)
            break;
        for (u32 i = 0; i < batch.batch_count && total < cap; i++) {
            strncpy(items[total], batch.batch[i], 63);
            items[total][63] = '\0';
            total++;
        }
    }
    fs_enum_end(e);
    return total;
}

/* Compose dir + "/" + name into out. */
static void fm_join(const char *dir, const char *name, char *out, size_t outsz) {
    if (strcmp(dir, "/") == 0)
        snprintf(out, outsz, "/%s", name);
    else
        snprintf(out, outsz, "%s/%s", dir, name);
}

static int cmd_fm(int argc, char *argv[]) {
    char dir[LINE_BUF_SIZE];
    if (argc >= 2) {
        if (shell_resolve_path(argv[1], dir, sizeof(dir)) < 0)
            return -1;
    } else {
        strncpy(dir, s_cwd, sizeof(dir) - 1);
        dir[sizeof(dir) - 1] = '\0';
    }
    /* "/" shows the volume list — enumerate the root view. */
    if (strcmp(dir, "/") == 0) {
        static vfs_vol_info_t vols[VFS_MAX_VOLS];
        u32                   vcount = 0;
        int                   r      = fs_list_volumes(vols, &vcount);
        if (r < 0) {
            shell_printf("fm: volume list FAILED (%d)\n", r);
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
        int sel = tui_menu(30, 8, 50, (int)vcount + 2, "Volumes (j/k, Enter, q)",
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
        static char items[FM_MAX_ITEMS][64];
        static const char *ptrs[FM_MAX_ITEMS];
        int n = fm_enum(dir, items, FM_MAX_ITEMS);
        if (n < 0) {
            shell_printf("fm: %s FAILED (%d)\n", dir, n);
            return -1;
        }
        for (int i = 0; i < n; i++)
            ptrs[i] = items[i];

        char title[128];
        snprintf(title, sizeof(title), "fm: %s (j/k Enter v d q)", dir);
        int rows = (n + 2 < 20) ? n + 2 : 20;
        int sel  = tui_menu(4, 4, 80, rows, title, ptrs, n, NULL);
        if (sel < 0)
            break; /* q = quit */

        char full[LINE_BUF_SIZE];
        fm_join(dir, items[sel], full, sizeof(full));

        vfs_item_info_t info;
        int             r = fs_get_item(full, &info);
        if (r < 0) {
            shell_printf("fm: %s FAILED (%d)\n", full, r);
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
        shell_printf("fm: %s (%d bytes) - v=view d=delete r=rename c=copy q=back\n",
                     items[sel], (int)info.size);
        /* Read one key directly (READ_BLOCK on the keyboard port). */
        u32 req[2];
        u8  resp[8];
        req[0]       = KBD_OP_READ_BLOCK;
        req[1]       = 1;
        int resp_len = (int)sizeof(resp);
        u8  key      = 0;
        if (ipc_call(s_kbd_port, req, 8, resp, &resp_len) == 0 && resp_len >= 4)
            key = resp[4];

        if (key == 'v' || key == 'V') {
            /* View: reuse cmd_cat logic via a fresh argv. */
            char *vargv[2] = {(char *)"cat", full};
            (void)cmd_cat(2, vargv);
        } else if (key == 'd' || key == 'D') {
            char msg[128];
            snprintf(msg, sizeof(msg), "Delete '%s'?", items[sel]);
            int yes = tui_confirm(20, 14, 60, "Delete", msg, "y = delete, n = cancel");
            if (yes > 0) {
                int dr = fs_delete_item(full, 0);
                shell_printf("fm: delete %s (%d)\n", items[sel], dr);
            }
        } else if (key == 'r' || key == 'R') {
            /* Rename in place: TUI input line for the new name. */
            char newname[64];
            if (tui_input_line(5, 30, "New name: ", newname, sizeof(newname), 0) >= 0 &&
                newname[0] != '\0') {
                vfs_item_info_t ri;
                int mr = fs_move_item(full, dir, newname, &ri);
                shell_printf("fm: rename %s -> %s (%d)\n", items[sel], newname, mr);
            }
        } else if (key == 'c' || key == 'C') {
            /* Copy: read the file and write a "copy" sibling. */
            vfs_handle_t h;
            int          or = fs_open_item(full, VFS_OPEN_READONLY, VFS_ACCESS_READ, &h);
            if (or < 0) {
                shell_printf("fm: copy open FAILED (%d)\n", or);
            } else {
                char dst[LINE_BUF_SIZE];
                char copy_name[96];
                snprintf(copy_name, sizeof(copy_name), "%s.copy", items[sel]);
                fm_join(dir, copy_name, dst, sizeof(dst));
                vfs_handle_t dh;
                int          wr = fs_open_item(dst, VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE,
                                               VFS_ACCESS_WRITE, &dh);
                if (wr < 0) {
                    shell_printf("fm: copy create FAILED (%d)\n", wr);
                } else {
                    static u8 cbuf[1024];
                    u64       off = 0;
                    int       cr  = 0;
                    for (;;) {
                        u32 got = 0;
                        cr = fs_read(h, off, cbuf, sizeof(cbuf), &got);
                        if (cr < 0 || got == 0)
                            break;
                        if (fs_write(dh, off, cbuf, got) < 0)
                            break;
                        off += (u64)got;
                    }
                    fs_close(dh);
                    fs_close(h);
                    shell_printf("fm: copied to %s (%d)\n", copy_name, cr < 0 ? cr : (int)off);
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
static int cmd_policy_set(int argc, char *argv[]) {
    if (argc < 4) {
        shell_write("Usage: policy_set <role> <cmd> <allow|deny|unset>\n");
        return -1;
    }
    int port = port_get("user");
    if (port < 0) {
        shell_printf("policy_set: user service unavailable (%d)\n", port);
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
        shell_printf("policy_set: invalid role '%s'\n", argv[1]);
        return -2;
    }

    uint32_t verdict;
    if (strcmp(argv[3], "allow") == 0) verdict = POLICY_ALLOW;
    else if (strcmp(argv[3], "deny") == 0) verdict = POLICY_DENY;
    else if (strcmp(argv[3], "unset") == 0) verdict = POLICY_UNSET;
    else {
        shell_printf("policy_set: invalid verdict '%s' (allow|deny|unset)\n", argv[3]);
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
    int r    = ipc_call(port, &req, (int)sizeof(req), &resp, &rlen);
    if (r < 0) {
        shell_printf("policy_set: ipc FAILED (%d)\n", r);
        return -1;
    }
    if (resp.ret < 0) {
        shell_printf("policy_set: FAILED (%d) (admin only)\n", resp.ret);
        return -1;
    }
    shell_printf("policy_set: role=%s cmd=%s -> %s\n",
                 argv[1], argv[2], argv[3]);
    return 0;
}

/* policy_dump — print the full command-policy table (admin, proxied). */
static int cmd_policy_dump(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int port = port_get("user");
    if (port < 0) {
        shell_printf("policy_dump: user service unavailable (%d)\n", port);
        return -1;
    }
    user_req_policy_t req;
    memset(&req, 0, sizeof(req));
    req.op = USER_OP_POLICY_DUMP;
    user_resp_policy_t resp;
    memset(&resp, 0, sizeof(resp));
    int rlen = (int)sizeof(resp);
    int r    = ipc_call(port, &req, (int)sizeof(req), &resp, &rlen);
    if (r < 0) {
        shell_printf("policy_dump: ipc FAILED (%d)\n", r);
        return -1;
    }
    if (resp.ret < 0) {
        shell_printf("policy_dump: FAILED (%d) (admin only)\n", resp.ret);
        return -1;
    }
    shell_printf("policy_dump: %u rule(s)\n", (unsigned)resp.count);
    for (uint32_t i = 0; i < resp.count && i < 64; i++) {
        shell_write("  ");
        shell_write(resp.lines[i]);
        shell_write("\n");
    }
    return 0;
}

static int cmd_pid(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    shell_printf("PID=%d\n", get_pid());
    return 0;
}

/*
 * Spawn an embedded demo/service ELF.  Blob name comes from argv[1]
 * (defaults to "hello" for backward compatibility); every registered
 * image (hello, runtime_demo, sbox_demo, ...) is fetchable via
 * SYS_BLOB_GET and spawnable via SYS_PROCESS_CREATE.
 */
static int cmd_exec(int argc, char *argv[]) {
    const char *name = (argc > 1) ? argv[1] : "hello";
    static char blob_buf[262144]; /* must hold the largest ELF */
    int         size;

    if (strchr(name, '/') != NULL || name[0] == '.') {
        /* Path argument: run an ELF FILE from the VFS (e.g. exec /Disk/
         * app.elf or exec ./app.elf in the cwd).  Read it in full, then
         * spawn it like an embedded blob. */
        char url[LINE_BUF_SIZE];
        if (shell_resolve_path(name, url, sizeof(url)) < 0)
            return 1;
        vfs_handle_t h;
        int          r = fs_open_item(url, VFS_OPEN_READONLY, VFS_ACCESS_READ, &h);
        if (r < 0) {
            shell_printf("exec: open %s FAILED (%d)\n", url, r);
            return 1;
        }
        size = 0;
        for (;;) {
            u32 got = 0;
            r       = fs_read(h, (u64)size, blob_buf + size, (u32)(sizeof(blob_buf) - (u64)size), &got);
            if (r < 0 || got == 0)
                break;
            size += (int)got;
        }
        fs_close(h);
        if (size <= 0) {
            shell_printf("exec: %s is empty or unreadable\n", url);
            return 1;
        }
        /* Process name = basename of the path. */
        const char *base = strrchr(url, '/');
        name             = base ? base + 1 : url;
        shell_printf("exec: read %s (%d bytes)\n", url, size);
    } else {
        size = blob_get(name, blob_buf, sizeof(blob_buf));
        if (size < 0) {
            shell_printf("exec: blob_get(%s) FAILED (%d)\n", name, size);
            return 1;
        }
        shell_printf("exec: fetched %s.elf blob from kernel (%d bytes)\n", name, size);
    }
    int pid = process_create(name, blob_buf, size);
    if (pid < 0) {
        shell_printf("exec: FAILED (%d)\n", pid);
        return 1;
    }
    shell_printf("exec: created PID %d\n", pid);
    return 0;
}

static int cmd_free(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int free_pages = get_free_pages();
    shell_printf(
        "Free memory: %d pages (%d KB, %d MB)\n", free_pages, free_pages * 4, free_pages / 256);
    return 0;
}

static int cmd_clear(int argc, char *argv[]) {
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
        ipc_call(s_term_port, (const void *)req, 8, (void *)resp, &resp_len);
    }
    return 0;
}

static int cmd_cap(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    /* Create a dummy MEM capability as a test */
    int cap = cap_create(CAP_TYPE_MEM, RIGHT_WRITE);
    if (cap > 0) {
        shell_printf("Created MEM cap: handle=%d\n", cap);
        cap_revoke(cap);
        shell_write("Revoked\n");
    } else {
        shell_printf("cap_create failed: %d\n", cap);
    }
    return 0;
}

/* mouse — read the PS/2 mouse state once ({dx, dy, buttons} deltas
 * since the last read; buttons bit0=left bit1=right bit2=middle). */
static int cmd_mouse(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    if (s_kbd_port < 0) {
        shell_write("mouse: keyboard service unavailable\n");
        return -1;
    }
    u32 req[2];
    u8  resp[16];
    req[0] = 5; /* KBD_OP_MOUSE_READ */
    req[1] = 12;
    int resp_len = (int)sizeof(resp);
    int r        = ipc_call(s_kbd_port, req, 8, resp, &resp_len);
    if (r < 0 || resp_len < 4 + 12) {
        shell_printf("mouse: FAILED (%d)\n", r);
        return -1;
    }
    i32 *d = (i32 *)(resp + 4);
    shell_printf("mouse: dx=%d dy=%d buttons=%d\n", d[0], d[1], d[2]);
    return 0;
}

static int cmd_ports(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    /* Try to look up some well-known ports */
    int port = port_get("init");
    if (port > 0)
        shell_printf("'init' port: %d\n", port);
    else
        shell_write("'init' port not found\n");

    port = port_get("shell");
    if (port > 0)
        shell_printf("'shell' port: %d\n", port);
    else
        shell_write("'shell' port not registered\n");

    /* Create a test port + register it */
    port = ipc_port_create();
    if (port > 0) {
        shell_printf("Created test port: %d\n", port);
        int ret = port_register("shell_test", port);
        shell_printf("register 'shell_test' -> %d\n", ret);
    }
    return 0;
}

static int cmd_sleep(int argc, char *argv[]) {
    if (argc < 2) {
        shell_write("Usage: sleep <ticks>\n");
        return -1;
    }
    int ticks = atoi(argv[1]);
    if (ticks <= 0) {
        shell_write("sleep: tick count must be positive\n");
        return -1;
    }
    shell_printf("Sleeping for %d ticks...\n", ticks);
    int ret = sleep(ticks);
    if (ret < 0) {
        shell_printf("sleep: syscall failed (%d)\n", ret);
        return -1;
    }
    shell_write("Done\n");
    return 0;
}

static int cmd_threads(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    shell_write("Spawning worker thread...\n");
    int tid = thread_create(worker_func, NULL, 10);
    if (tid > 0) {
        shell_printf("Worker TID=%d, joining...\n", tid);
        int exit_code;
        thread_join(tid, &exit_code);
        shell_printf("Worker joined, exit_code=%d\n", exit_code);
    } else {
        shell_printf("thread_create failed: %d\n", tid);
    }
    return 0;
}

/* Worker thread function for cmd_threads */
static void worker_func(void *arg) {
    (void)arg;
    shell_write("  [worker] hello from thread!\n");
    thread_exit(42);
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
static void mutex_worker_func(void *arg) {
    (void)arg;
    for (int i = 0; i < MUTEX_DEMO_ITERS; i++) {
        int r = mutex_lock(s_demo_mutex);
        if (r < 0) {
            thread_exit(r); /* lock failed — propagate error */
        }
        s_demo_counter++;
        mutex_unlock(s_demo_mutex);
    }
    thread_exit(0);
}

static int cmd_mutex(int argc, char *argv[]) {
    int n = 4;
    if (argc >= 2) {
        n = atoi(argv[1]);
        if (n < 1)
            n = 1;
        if (n > 16)
            n = 16;
    }

    shell_write("mutex demo: creating mutex...\n");
    s_demo_mutex = mutex_create();
    if (s_demo_mutex < 0) {
        shell_printf("mutex_create failed: %d\n", s_demo_mutex);
        return -1;
    }
    shell_printf("mutex handle=%d\n", s_demo_mutex);

    /* Round-trip: uncontended lock/unlock */
    int ret = mutex_lock(s_demo_mutex);
    shell_printf("lock -> %d\n", ret);
    ret = mutex_unlock(s_demo_mutex);
    shell_printf("unlock -> %d\n", ret);

    /* Contended: N workers racing on the shared counter */
    s_demo_counter = 0;
    shell_printf("spawning %d workers x %d iters\n", n, MUTEX_DEMO_ITERS);

    int tids[16];
    for (int i = 0; i < n; i++) {
        tids[i] = thread_create(mutex_worker_func, NULL, 10);
        if (tids[i] < 0) {
            shell_printf("thread_create failed: %d\n", tids[i]);
            mutex_destroy(s_demo_mutex);
            return -1;
        }
    }
    for (int i = 0; i < n; i++) {
        int code;
        thread_join(tids[i], &code);
        if (code != 0)
            shell_printf("worker %d exited with %d\n", i, code);
    }

    int expect = n * MUTEX_DEMO_ITERS;
    if (s_demo_counter == expect)
        shell_printf("MUTEX PASS: counter=%d expected=%d\n", s_demo_counter, expect);
    else
        shell_printf("MUTEX FAIL: counter=%d expected=%d\n", s_demo_counter, expect);

    ret = mutex_destroy(s_demo_mutex);
    shell_printf("destroy -> %d\n", ret);
    return 0;
}

static int cmd_uptime(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int ticks = get_time();
    shell_printf("System ticks: %d\n", ticks);
    return 0;
}

static int cmd_exit(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    shell_write("shell: exiting thread\n");
    thread_exit(0);
    /* unreachable */
    return 0;
}

static int cmd_reboot(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    shell_write("Halting system...\n");
    (void)sys_reboot();
    /* Only reached if the reboot syscall failed. */
    shell_write("reboot: syscall failed, system still running\n");
    return 0;
}

/* shutdown — power off the machine (ACPI S5 via the kernel; falls back
 * to reboot if the platform has no ACPI power button). */
static int cmd_shutdown(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    shell_write("Shutting down...\n");
    (void)sys_shutdown();
    /* Only reached if the shutdown syscall failed (e.g. ERR_NOCAP). */
    shell_write("shutdown: syscall failed, system still running\n");
    return 0;
}


/*
 * Column helpers for cmd_ps.  The file-local shell_printf() has no
 * width/precision specifiers, so columns are padded manually exactly
 * like cmd_help does (write, then pad with trailing spaces).
 */
static void shell_pad_int(int v, int width) {
    char num[12];
    shell_itoa(v, num);
    shell_write(num);
    int pad = width - (int)strlen(num);
    while (pad > 0) {
        shell_putc(' ');
        pad--;
    }
}

static void shell_pad_str(const char *s, int width) {
    shell_write(s);
    int pad = width - (int)strlen(s);
    while (pad > 0) {
        shell_putc(' ');
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
static int cmd_ps(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    static proc_info_t s_ps_info[PROC_MAX_ITEMS];

    int n = process_list(s_ps_info, PROC_MAX_ITEMS);
    if (n < 0) {
        shell_printf("ps: process_list failed (%d)\n", n);
        return -1;
    }

    shell_write("PID  STATE    THR EXIT  NAME\n");
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
        shell_pad_int(p->pid, 4);
        shell_write(" ");
        shell_pad_str(state_name, 8);
        shell_write(" ");
        shell_pad_int((int)p->thread_count, 3);
        shell_write(" ");
        shell_pad_int(p->exit_code, 4);
        shell_write("  ");
        shell_write(p->name);
        shell_write("\n");
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
static int cmd_kill(int argc, char *argv[]) {
    int pid = 0;
    if (argc < 2) {
        /* v1.3: no PID -> TUI process picker. */
        static proc_info_t plist[PROC_MAX_ITEMS];
        static char        lines[PROC_MAX_ITEMS][32];
        static const char *ptrs[PROC_MAX_ITEMS];
        int                n = process_list(plist, PROC_MAX_ITEMS);
        if (n <= 0) {
            shell_write("kill: no processes\n");
            return -1;
        }
        int shown = 0;
        for (int i = 0; i < n && shown < 64; i++) {
            snprintf(lines[shown], 32, "%d  %s", (int)plist[i].pid, plist[i].name);
            ptrs[shown] = lines[shown];
            shown++;
        }
        int sel = tui_menu(30, 6, 44, (shown + 2 < 20) ? shown + 2 : 20,
                           "Processes (j/k Enter q)", ptrs, shown, NULL);
        if (sel < 0)
            return 0; /* cancelled */
        pid = (int)plist[sel].pid;
    } else {
        pid = atoi(argv[1]);
        if (pid <= 0) {
            shell_printf("kill: invalid pid '%s'\n", argv[1]);
            return -1;
        }
    }
    int signum = SIGKILL;
    if (argc >= 3) {
        signum = atoi(argv[2]);
        if (signum <= 0 || signum >= NSIG) {
            shell_printf("kill: invalid signum '%s' (1..%d)\n", argv[2], NSIG - 1);
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
        int r = user_call(&wq, (int)sizeof(wq), &who, (int)sizeof(who));
        if (r == 0 && who.ret == 0 &&
            (who.role == PERM_ROLE_OWNER || who.role == PERM_ROLE_ADMIN)) {
            user_req_kill_t kq;
            memset(&kq, 0, sizeof(kq));
            kq.op  = USER_OP_KILL;
            kq.pid = pid;
            user_resp_kill_t kr;
            memset(&kr, 0, sizeof(kr));
            r = user_call(&kq, (int)sizeof(kq), &kr, (int)sizeof(kr));
            if (r < 0 || kr.ret < 0) {
                shell_printf("kill: PID %d SIG %d FAILED (%d)%s%s\n",
                             pid, signum, r < 0 ? r : kr.ret,
                             kr.detail[0] ? " - " : "", kr.detail);
                return -1;
            }
            shell_printf("kill: %s\n", kr.detail);
            return 0;
        }
    }

    int ret = kill(pid, signum);
    if (ret < 0) {
        shell_printf("kill: PID %d SIG %d FAILED (%d) "
                     "(foreign PIDs need OWNER/ADMIN login)\n",
                     pid, signum, ret);
        return -1;
    }
    shell_printf("kill: sent SIG %d to PID %d\n", signum, pid);
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
static int cmd_ls(int argc, char *argv[]) {
    if (argc < 2) {
        shell_write("Usage: ls <dir-url>\n");
        return -1;
    }
    if (strcmp(argv[1], "/") == 0 || strcmp(argv[1], "/Volumes") == 0 ||
        strcmp(argv[1], "/Volumes/") == 0) {
        static vfs_vol_info_t vols[VFS_MAX_VOLS]; /* small, static */
        u32                   count = 0;
        int                   r     = fs_list_volumes(vols, &count);
        if (r < 0) {
            shell_printf("ls: %s FAILED (%d)\n", argv[1], r);
            return -1;
        }
        for (u32 i = 0; i < count; i++) {
            shell_write(vols[i].mount_name);
            shell_write(vols[i].read_only ? " (ro)\n" : "\n");
        }
        shell_printf("ls: %d volumes\n", count);
        return 0;
    }
    static vfs_enum_batch_t batch; /* ~16.5 KB — keep off the stack */
    vfs_handle_t            e;
    int                     r = fs_enum_begin(argv[1], &e);
    if (r < 0) {
        shell_printf("ls: %s FAILED (%d)\n", argv[1], r);
        return -1;
    }
    int total = 0;
    for (;;) {
        r = fs_enum_next(e, &batch);
        if (r < 0) {
            shell_printf("ls: enum FAILED (%d)\n", r);
            fs_enum_end(e);
            return -1;
        }
        if (batch.batch_count == 0)
            break;
        for (u32 i = 0; i < batch.batch_count; i++) {
            shell_write(batch.batch[i]);
            shell_write("\n");
            total++;
        }
    }
    fs_enum_end(e);
    shell_printf("ls: %d entries\n", total);
    return 0;
}

/*
 * cat <url> — read and display a file.  Non-printable bytes are
 * shown as '.' so binary blobs do not corrupt the terminal.  Ends with
 * a byte-count line so the content length can be verified against the
 * source blob.
 */
static int cmd_cat(int argc, char *argv[]) {
    if (argc < 2) {
        shell_write("Usage: cat <file-url>\n");
        return -1;
    }
    vfs_item_info_t info;
    int             r = fs_get_item(argv[1], &info);
    if (r < 0) {
        shell_printf("cat: %s FAILED (%d)\n", argv[1], r);
        return -1;
    }
    shell_printf("== %s (%d bytes) ==\n", argv[1], (int)info.size);

    vfs_handle_t h;
    r = fs_open_item(argv[1], VFS_OPEN_READONLY, VFS_ACCESS_READ, &h);
    if (r < 0) {
        shell_printf("cat: open FAILED (%d)\n", r);
        return -1;
    }
    static u8 buf[1024];
    u32       total = 0;
    for (;;) {
        u32 got = 0;
        r       = fs_read(h, total, buf, sizeof(buf), &got);
        if (r < 0) {
            shell_printf("cat: read FAILED (%d)\n", r);
            fs_close(h);
            return -1;
        }
        if (got == 0)
            break;
        for (u32 i = 0; i < got; i++) {
            u8 c = buf[i];
            if (c >= 32 && c < 127)
                shell_putc((char)c);
            else
                shell_putc('.');
        }
        total += got;
        if (got < sizeof(buf))
            break; /* EOF */
    }
    fs_close(h);
    shell_printf("\n== read %d bytes ==\n", (int)total);
    return 0;
}

/*
 * stat [url] — volume capacity/usage.  With no argument, stats
 * both configured volumes (System read-only, Users 32 MiB RAM).
 */
static int cmd_stat(int argc, char *argv[]) {
    static const char *vols[2] = {"/Volumes/System", "/Volumes/Users"};
    int                count   = (argc >= 2) ? 1 : 2;
    for (int v = 0; v < count; v++) {
        const char *url   = (argc >= 2) ? argv[1] : vols[v];
        u64         total = 0, used = 0;
        u32         ro = 0;
        int         r  = fs_stat_volume(url, &total, &used, &ro);
        if (r < 0) {
            shell_printf("stat: %s FAILED (%d)\n", url, r);
            continue;
        }
        shell_printf("%s: %d KB total, %d KB used, %s\n",
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
static int cmd_tee(int argc, char *argv[]) {
    if (argc < 3) {
        shell_write("Usage: tee <file-url> <text>\n");
        return -1;
    }
    const char *url  = argv[1];
    const char *text = argv[2];
    u32         len  = (u32)strlen(text);

    vfs_handle_t h;
    int          r = fs_open_item(url, VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE, VFS_ACCESS_WRITE, &h);
    if (r < 0) {
        shell_printf("tee: open %s FAILED (%d)\n", url, r);
        return -1;
    }
    r = fs_write(h, 0, text, len);
    if (r < 0) {
        shell_printf("tee: write FAILED (%d)\n", r);
        fs_close(h);
        return -1;
    }
    fs_close(h);
    shell_printf("tee: %d bytes written to %s\n", (int)len, url);
    return 0;
}

/*
 * fallocate <url> — grow a file in 4 KiB chunks until the volume is
 * full.  Exercises the ENOSPC path: the fs_mem_driver capacity check
 * must reject the write that would exceed the 32 MiB Users volume with
 * VFS_ERR_NOSPC (-101).  Prints progress every 8 MiB and the failing
 * offset + error code.
 */
static int cmd_fallocate(int argc, char *argv[]) {
    if (argc < 2) {
        shell_write("Usage: fallocate <file-url>\n");
        return -1;
    }
    const char *url = argv[1];

    vfs_handle_t h;
    int          r = fs_open_item(url, VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE, VFS_ACCESS_WRITE, &h);
    if (r < 0) {
        shell_printf("fallocate: open %s FAILED (%d)\n", url, r);
        return -1;
    }

    static u8 s_fill_buf[4096];
    memset(s_fill_buf, 0xAB, sizeof(s_fill_buf));

    u64 off = 0;
    for (;;) {
        r = fs_write(h, off, s_fill_buf, sizeof(s_fill_buf));
        if (r < 0) {
            /* Only a full volume is NOSPC; other failures (e.g. a dead
             * driver returning ERR_NOENT) are labelled accurately. */
            if (r == VFS_ERR_NOSPC)
                shell_printf("fallocate: NOSPC at %d MiB (err %d)\n", (int)(off >> 20), r);
            else
                shell_printf("fallocate: write FAILED at %d MiB (err %d)\n",
                             (int)(off >> 20), r);
            fs_close(h);
            return 0;
        }
        off += sizeof(s_fill_buf);
        if ((off & 0x7FFFFF) == 0) /* every 8 MiB */
            shell_printf("fallocate: %d MiB\n", (int)(off >> 20));
    }
}

/*
 * mkdir <url> — create an empty directory (fs_create_dir).
 * Fails with VFS_ERR_EXISTS (-104) if the directory already exists.
 */
static int cmd_mkdir(int argc, char *argv[]) {
    if (argc < 2) {
        shell_write("Usage: mkdir <dir-url>\n");
        return -1;
    }
    int r = fs_create_dir(argv[1]);
    if (r < 0) {
        shell_printf("mkdir: %s FAILED (%d)\n", argv[1], r);
        return -1;
    }
    shell_printf("mkdir: created %s\n", argv[1]);
    return 0;
}

/*
 * rm <url> — delete an item (fs_delete_item, non-recursive).  A
 * non-empty directory fails with ERR_BUSY.
 */
static int cmd_rm(int argc, char *argv[]) {
    if (argc < 2) {
        shell_write("Usage: rm <url>\n");
        return -1;
    }
    /* v1.3: destructive command -> TUI confirm dialog. */
    char msg[160];
    snprintf(msg, sizeof(msg), "Delete '%s'?", argv[1]);
    int yes = tui_confirm(20, 14, 60, "Confirm Delete", msg,
                          "Type y to confirm, n to cancel");
    if (yes < 0)
        return -1;
    if (!yes) {
        shell_write("rm: cancelled\n");
        return 0;
    }
    int r = fs_delete_item(argv[1], 0);
    if (r < 0) {
        shell_printf("rm: %s FAILED (%d)\n", argv[1], r);
        return -1;
    }
    shell_printf("rm: removed %s\n", argv[1]);
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
        s_perm_port = port_get(PERM_PORT_NAME);
    return s_perm_port;
}

/* Parse "r"/"w"/"rw" → VFS_ACCESS_* mask (default "r"). */
static u32 bm_access(const char *s) {
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
static int cmd_bm_create(int argc, char *argv[]) {
    if (argc < 2) {
        shell_write("Usage: bm_create <url> [r|w|rw]\n");
        return -1;
    }
    const char *url    = argv[1];
    u32         access = bm_access((argc >= 3) ? argv[2] : "r");

    u32 len = 0;
    int r   = fs_create_bookmark(url, access, 0, s_bm_blob, &len);
    if (r < 0) {
        shell_printf("bm_create: FAILED (%d)", r);
        if (r == VFS_ERR_ACCESS)
            shell_write(" (denied - see term, then perm_answer <id> y, "
                        "and retry)\n");
        else
            shell_write("\n");
        return -1;
    }
    s_bm_len = len;
    shell_printf("bm_create: ok, %d-byte bookmark cached\n", (int)len);
    return 0;
}

/* bm_resolve — blob → FileHandle (authorization re-checked server-side
 * on every resolve).  Prints handle/item/access, then closes. */
static int cmd_bm_resolve(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    if (s_bm_len == 0) {
        shell_write("bm_resolve: no cached bookmark (bm_create first)\n");
        return -1;
    }
    vfs_handle_t    h = 0;
    vfs_item_info_t item;
    u32             access = 0;
    int             r      = fs_resolve_bookmark(s_bm_blob, s_bm_len, &h, &item, &access);
    if (r < 0) {
        shell_printf("bm_resolve: FAILED (%d)", r);
        if (r == VFS_ERR_ACCESS)
            shell_write(" (EACCES)\n");
        else
            shell_write("\n");
        return -1;
    }
    shell_printf("bm_resolve: handle %d, item '%s' (id %d), access %d\n",
                 (int)h,
                 item.name,
                 (int)item.item_id,
                 (int)access);
    fs_close(h);
    return 0;
}

/* bm_revoke — drop the bookmark record server-side (idempotent). */
static int cmd_bm_revoke(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    if (s_bm_len == 0) {
        shell_write("bm_revoke: no cached bookmark (bm_create first)\n");
        return -1;
    }
    int r = fs_revoke_bookmark(s_bm_blob, s_bm_len);
    shell_printf("bm_revoke: %s (%d)\n", r < 0 ? "FAILED" : "ok", r);
    if (r == 0)
        s_bm_len = 0; /* record gone */
    return r < 0 ? -1 : 0;
}

/* perm_answer <query_id> <y|n> — user verdict on a pending Powerbox
 * query.  y → grant upserted; n → denied (default deny). */
static int cmd_perm_answer(int argc, char *argv[]) {
    if (argc < 3) {
        shell_write("Usage: perm_answer <query-id> <y|n>\n");
        return -1;
    }
    int port = perm_port();
    if (port < 0) {
        shell_printf("perm_answer: perm port unavailable (%d)\n", port);
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
    int r        = ipc_call(port, &req, (int)sizeof(req), &resp, &resp_len);
    if (r < 0) {
        shell_printf("perm_answer: ipc FAILED (%d)\n", r);
        return -1;
    }
    shell_printf("perm_answer: query %d -> %s (%d)\n",
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
static int cmd_perm_query(int argc, char *argv[]) {
    int port = perm_port();
    if (port < 0) {
        shell_printf("perm_query: perm port unavailable (%d)\n", port);
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
    int r        = ipc_call(port, &req, (int)sizeof(req), &resp, &resp_len);
    if (r < 0) {
        shell_printf("perm_query: ipc FAILED (%d)\n", r);
        return -1;
    }
    if (resp.ret < 0) {
        shell_printf("perm_query: no pending query (%d)\n", resp.ret);
        return -1;
    }
    shell_printf("perm_query: query %d: %s (PID %d) requests %s (%s) - %s "
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
static int cmd_perm_revoke(int argc, char *argv[]) {
    int port = perm_port();
    if (port < 0) {
        shell_printf("perm_revoke: perm port unavailable (%d)\n", port);
        return -1;
    }
    u64 subject = (argc >= 2) ? (u64)strtoull(argv[1], NULL, 10) : get_subject();

    perm_req_revoke_t req;
    memset(&req, 0, sizeof(req)); /* zero resource = all resources */
    req.op         = PERM_OP_REVOKE;
    req.subject_id = subject;

    perm_resp_revoke_t resp;
    memset(&resp, 0, sizeof(resp));
    int resp_len = (int)sizeof(resp);
    int r        = ipc_call(port, &req, (int)sizeof(req), &resp, &resp_len);
    if (r < 0) {
        shell_printf("perm_revoke: ipc FAILED (%d)\n", r);
        return -1;
    }
    shell_printf("perm_revoke: %d grant(s) dropped\n", (int)resp.revoked);
    return 0;
}

/* move <src-url> <dst-dir-url> [new-name] — move/rename an item.  The
 * fs_mem_driver keeps itemID stable, so cached bookmarks survive. */
static int cmd_mv(int argc, char *argv[]) {
    if (argc < 3) {
        shell_write("Usage: mv <src-url> <dst-dir-url|?> [new-name]\n");
        return -1;
    }
    char dst[LINE_BUF_SIZE];
    if (strcmp(argv[2], "?") == 0) {
        /* v1.3: TUI directory picker for the destination. */
        static char items[FM_MAX_ITEMS][64];
        static const char *ptrs[FM_MAX_ITEMS];
        int n = fm_enum(s_cwd, items, FM_MAX_ITEMS);
        if (n <= 0) {
            shell_printf("mv: no directories in %s\n", s_cwd);
            return -1;
        }
        int shown = 0;
        for (int i = 0; i < n && shown < 64; i++) {
            /* Only directories are valid move targets. */
            char full[LINE_BUF_SIZE];
            fm_join(s_cwd, items[i], full, sizeof(full));
            vfs_item_info_t it;
            if (fs_get_item(full, &it) == 0 && it.type == VFS_ITEM_DIR) {
                ptrs[shown] = items[i];
                shown++;
            }
        }
        if (shown == 0) {
            shell_printf("mv: no directories in %s\n", s_cwd);
            return -1;
        }
        int sel = tui_menu(30, 8, 50, (shown + 2 < 16) ? shown + 2 : 16,
                           "Move to dir (j/k Enter q)", ptrs, shown, NULL);
        if (sel < 0)
            return 0; /* cancelled */
        fm_join(s_cwd, ptrs[sel], dst, sizeof(dst));
    } else {
        strncpy(dst, argv[2], sizeof(dst) - 1);
        dst[sizeof(dst) - 1] = '\0';
    }
    /* v1.3: mutating command -> TUI confirm dialog. */
    char msg[200];
    snprintf(msg, sizeof(msg), "Move '%s' to '%s'?", argv[1], dst);
    int yes = tui_confirm(20, 14, 60, "Confirm Move", msg,
                          "Type y to confirm, n to cancel");
    if (yes < 0)
        return -1;
    if (!yes) {
        shell_write("mv: cancelled\n");
        return 0;
    }
    vfs_item_info_t item;
    int             r = fs_move_item(argv[1], dst, (argc >= 4) ? argv[3] : "", &item);
    if (r < 0) {
        shell_printf("mv: FAILED (%d)\n", r);
        return -1;
    }
    shell_printf("mv: '%s' -> item %d (size %d)\n", item.name, (int)item.item_id, (int)item.size);
    return 0;
}

/*
 * pkg <install|list|run|remove> — .ops application container
 * (docs/ops_format.md).  Talks to the pkg-manager service through the
 * libpkg client (user/lib/libpkg).  Every operation returns 0 on
 * success or a negative error code, printed on failure.
 */
static int cmd_pkg(int argc, char *argv[]) {
    if (argc < 2) {
        shell_write("Usage: pkg <install <name> [--perms=a,b,c] | list | "
                    "run <app_id> | remove <app_id>>\n");
        return -1;
    }
    const char *sub = argv[1];

    if (strcmp(sub, "install") == 0) {
        if (argc < 3) {
            shell_write("Usage: pkg install <name> [--perms=a,b,c]\n");
            return -1;
        }
        const char *perms = "";
        if (argc >= 4 && strncmp(argv[3], "--perms=", 8) == 0)
            perms = argv[3] + 8;
        int ret = pkg_install(argv[2], perms);
        if (ret < 0) {
            shell_printf("pkg install: FAILED (%d)\n", ret);
            return -1;
        }
        shell_printf("pkg install: '%s' installed\n", argv[2]);
        return 0;
    }

    if (strcmp(sub, "list") == 0) {
        char     apps[PKG_MAX_APPS][PKG_NAME_MAX];
        uint32_t count = 0;
        int      ret   = pkg_list(apps, &count);
        if (ret < 0) {
            shell_printf("pkg list: FAILED (%d)\n", ret);
            return -1;
        }
        shell_printf("pkg list: %d app(s) installed\n", (int)count);
        for (uint32_t i = 0; i < count; i++)
            shell_printf("  %s\n", apps[i]);
        return 0;
    }

    if (strcmp(sub, "run") == 0) {
        if (argc < 3) {
            shell_write("Usage: pkg run <app_id>\n");
            return -1;
        }
        int32_t pid = 0;
        int     ret = pkg_run(argv[2], &pid);
        if (ret < 0) {
            shell_printf("pkg run: FAILED (%d)\n", ret);
            return -1;
        }
        shell_printf("pkg run: '%s' spawned (PID=%d)\n", argv[2], (int)pid);
        return 0;
    }

    if (strcmp(sub, "remove") == 0) {
        if (argc < 3) {
            shell_write("Usage: pkg remove <app_id>\n");
            return -1;
        }
        int ret = pkg_remove(argv[2]);
        if (ret < 0) {
            shell_printf("pkg remove: FAILED (%d)\n", ret);
            return -1;
        }
        shell_printf("pkg remove: '%s' removed\n", argv[2]);
        return 0;
    }

    shell_printf("pkg: unknown subcommand '%s'\n", sub);
    return -1;
}

/* ====================================================================
 * Entry point (shell process main)
 * ==================================================================== */

static void shell_main(void *arg) {
    (void)arg;
    s_term_port = port_get("term");
    s_kbd_port  = port_get("keyboard");

    /* Register the built-in commands BEFORE the loop reads any input.
     * Registration order defines `help` output order.  Return values
     * are ignored: these are static entries, so the only failure mode
     * is heap exhaustion, which leaves the shell with fewer commands
     * but still running. */
    shell_register_command("help", "Show this help", cmd_help);
    shell_register_command("echo", "Print text: echo <message>", cmd_echo);
    shell_register_command("pid", "Show current process PID", cmd_pid);
    shell_register_command("free", "Show free physical memory", cmd_free);
    shell_register_command("clear", "Clear the terminal", cmd_clear);
    shell_register_command("cap", "Create a capability (test)", cmd_cap);
    shell_register_command("mouse", "Read the PS/2 mouse state (dx dy buttons)", cmd_mouse);
    shell_register_command("ports", "List registered IPC ports", cmd_ports);
    shell_register_command("sleep", "Sleep for N ticks: sleep <ticks>", cmd_sleep);
    shell_register_command("threads", "Spawn a test worker thread", cmd_threads);
    shell_register_command("mutex", "Mutex demo: N threads on a counter", cmd_mutex);
    shell_register_command("exec", "Spawn an embedded demo (exec [blob_name])", cmd_exec);
    shell_register_command("uptime", "Show system tick count", cmd_uptime);
    shell_register_command("exit", "Exit the shell", cmd_exit);
    shell_register_command("reboot", "Halt the system", cmd_reboot);
    shell_register_command("shutdown", "Power off the machine", cmd_shutdown);
    shell_register_command("kill", "Send a signal: kill <pid> [signum]", cmd_kill);
    shell_register_command("ps", "List running processes", cmd_ps);
    shell_register_command(
        "ls", "List a VFS dir: ls <url> (/ or /Volumes = volumes)", cmd_ls);
    shell_register_command("cat", "Show a VFS file: cat <url>", cmd_cat);
    shell_register_command("stat", "VFS volume stats: stat [url]", cmd_stat);
    shell_register_command("tee", "Write a VFS file: tee <url> <text>", cmd_tee);
    shell_register_command("fallocate", "Fill a volume until ENOSPC: fallocate <url>", cmd_fallocate);
    shell_register_command("mkdir", "Create a VFS dir: mkdir <url>", cmd_mkdir);
    shell_register_command("rm", "Delete a VFS item: rm <url>", cmd_rm);
    shell_register_command(
        "bm_create", "Powerbox-gated bookmark: bm_create <url> [r|w|rw]", cmd_bm_create);
    shell_register_command("bm_resolve", "Resolve cached bookmark to a handle", cmd_bm_resolve);
    shell_register_command("bm_revoke", "Drop the cached bookmark server-side", cmd_bm_revoke);
    shell_register_command(
        "perm_answer", "Answer a Powerbox query: perm_answer <id> y|n", cmd_perm_answer);
    shell_register_command(
        "perm_query", "Show pending Powerbox query: perm_query [id]", cmd_perm_query);
    shell_register_command("perm_revoke", "Drop grants: perm_revoke [subject_id]", cmd_perm_revoke);
    /* UNIX-style aliases: bm <create|resolve|revoke>, perm <answer|query|revoke>. */
    shell_register_command("bm", "Bookmarks: bm <create|resolve|revoke> ...", cmd_bm);
    shell_register_command("perm", "Powerbox: perm <answer|query|revoke> ...", cmd_perm);
    shell_register_command("mv", "Move/rename: mv <src> <dst-dir> [new-name]", cmd_mv);
    shell_register_command("pkg", "pkg-manager: pkg <install|list|run|remove>", cmd_pkg);

    /* User accounts + exit guard */
    shell_register_command("login", "Log in: login [name] [password]", cmd_login);
    shell_register_command("logout", "Log out the current account", cmd_logout);
    shell_register_command("whoami", "Show the logged-in account", cmd_whoami);
    shell_register_command("passwd", "Change password: passwd [name]", cmd_passwd);
    shell_register_command("useradd", "Create account (admin): useradd <name> <role> [pw]", cmd_useradd);
    shell_register_command("userdel", "Delete account (admin): userdel <name>", cmd_userdel);
    shell_register_command("user_lock", "Disable account (admin): user_lock <name>", cmd_userlock);
    shell_register_command("user_unlock", "Enable account (admin): user_unlock <name>", cmd_userunlock);
    shell_register_command("userlock", "Disable account (admin): userlock <name>", cmd_userlock);
    shell_register_command("userunlock", "Enable account (admin): userunlock <name>", cmd_userunlock);
    shell_register_command("users", "List accounts (admin)", cmd_users);
    shell_register_command("stop", "Stop a system program (admin, confirmed): stop <svc>", cmd_stop);
    shell_register_command("export", "Set env var: export NAME=value (user prefs only)", cmd_export);
    shell_register_command("unset", "Remove env var: unset NAME", cmd_unset);
    shell_register_command("env", "Print the environment", cmd_env);
    shell_register_command("policy_set", "Hot-update cmd policy (admin): policy_set <role> <cmd> <allow|deny|unset>", cmd_policy_set);
    shell_register_command("policy_dump", "Show cmd policy table (admin)", cmd_policy_dump);
    shell_register_command("policy", "Cmd policy: policy <set|dump> ...", cmd_policy);
    shell_register_command("cd", "Change directory: cd [dir]", cmd_cd);
    shell_register_command("pwd", "Print working directory", cmd_pwd);
    shell_register_command("scroll",
                           "Page through terminal scrollback: scroll [lines] | scroll end",
                           cmd_scroll);
    shell_register_command("disk",
                           "Disk mgmt: disk list|mount|unmount|format|fill <vol> [bytes]",
                           cmd_disk);
    shell_register_command("fm", "TUI file manager (j/k Enter v d q)", cmd_fm);

    /* v0.5: load the command policy filter (rescue list on failure). */
    cmd_filter_load();

    shell_loop();
}

/* ====================================================================
 * Process entry point (crt0 calls main())
 * ==================================================================== */

/*
 * The shell process's main thread.  Defers to shell_main(), which runs
 * the REPL forever; this function never returns.
 */
int main(void) {
    shell_main(NULL);
    return 0; /* unreachable */
}

/* ====================================================================
 * User account commands (user service, user.h)
 * ==================================================================== */

static int user_port(void) {
    static int s_port = -2;
    if (s_port >= -1)
        return s_port;
    s_port = port_get(USER_PORT_NAME);
    return s_port;
}

/* user_call: simple request/reply over the "user" port. */
static int user_call(const void *req, int req_len, void *resp, int resp_len) {
    int port = user_port();
    if (port < 0)
        return port;
    int rlen = resp_len;
    int r    = ipc_call(port, req, req_len, resp, &rlen);
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
static int cmd_login(int argc, char *argv[]) {
    char name[USER_NAME_MAX];
    char pw[USER_PW_MAX];

    if (argc >= 2) {
        strncpy(name, argv[1], sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
    } else {
        /* Prompt at the CURRENT cursor (no absolute-coordinate overlay):
         * the line editor echoes as usual. */
        shell_write("User: ");
        if (read_line(name, sizeof(name)) < 0)
            return -1;
    }
    if (argc >= 3) {
        strncpy(pw, argv[2], sizeof(pw) - 1);
        pw[sizeof(pw) - 1] = '\0';
    } else {
        /* Masked entry: echo '*' (password). */
        shell_write("Password: ");
        if (read_line_masked(pw, sizeof(pw)) < 0)
            return -1;
    }

    user_req_login_t req;
    memset(&req, 0, sizeof(req));
    req.op = USER_OP_LOGIN;
    strncpy(req.name, name, sizeof(req.name) - 1);
    strncpy(req.password, pw, sizeof(req.password) - 1);
    user_resp_login_t resp;
    memset(&resp, 0, sizeof(resp));
    int r = user_call(&req, (int)sizeof(req), &resp, (int)sizeof(resp));
    if (r < 0) {
        shell_printf("login: ipc FAILED (%d)\n", r);
        return -1;
    }
    if (resp.ret < 0) {
        shell_printf("login: FAILED (%d) - bad name or password\n", resp.ret);
        return -1;
    }
    shell_printf("login: ok - '%s' (%s)\n", resp.name, role_name(resp.role));

    /* The command policy is role-dependent: reload it so the new
     * role's allow/deny verdicts apply immediately. */
    cmd_filter_load();
    return 0;
}

static int cmd_logout(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    user_req_login_t req;
    memset(&req, 0, sizeof(req));
    req.op = USER_OP_LOGOUT;
    user_resp_login_t resp;
    memset(&resp, 0, sizeof(resp));
    int r = user_call(&req, (int)sizeof(req), &resp, (int)sizeof(resp));
    if (r < 0 || resp.ret < 0) {
        shell_printf("logout: FAILED (%d)\n", r < 0 ? r : resp.ret);
        return -1;
    }
    shell_printf("logout: ok\n");
    /* Role reverts on logout: reload the command policy. */
    cmd_filter_load();
    return 0;
}

static int cmd_whoami(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    user_req_login_t req;
    memset(&req, 0, sizeof(req));
    req.op = USER_OP_WHOAMI;
    user_resp_login_t resp;
    memset(&resp, 0, sizeof(resp));
    int r = user_call(&req, (int)sizeof(req), &resp, (int)sizeof(resp));
    if (r < 0) {
        shell_printf("whoami: ipc FAILED (%d)\n", r);
        return -1;
    }
    if (resp.ret < 0) {
        /* UNIX-style: not logged in -> "nobody" (exit 0). */
        shell_write("nobody\n");
        return 0;
    }
    shell_printf("%s (%s)\n", resp.name, role_name(resp.role));
    return 0;
}

/* passwd [name] — change own password (TUI masked input), or another
 * user's when given a name (requires admin re-auth). */
static int cmd_passwd(int argc, char *argv[]) {
    char oldpw[USER_PW_MAX];
    char newpw[USER_PW_MAX];

    if (tui_input_line(5, 30, "Current password: ", oldpw, sizeof(oldpw), 1) < 0)
        return -1;
    if (tui_input_line(5, 31, "New password: ", newpw, sizeof(newpw), 1) < 0)
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
    int r = user_call(&req, (int)sizeof(req), &resp, (int)sizeof(resp));
    if (r < 0 || resp.ret < 0) {
        shell_printf("passwd: FAILED (%d)\n", r < 0 ? r : resp.ret);
        return -1;
    }
    shell_printf("passwd: ok\n");
    return 0;
}

/* useradd <name> <role> [password] — create an account (admin only).
 * Role: owner|admin|standard|child|guest|auditor (or numeric).  An
 * unrecognized role name is an ERROR (never silently 0/OWNER). */
static int cmd_useradd(int argc, char *argv[]) {
    if (argc < 3) {
        shell_write("Usage: useradd <name> <role> [password]\n");
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
        shell_printf("useradd: invalid role '%s' "
                     "(owner|admin|standard|child|guest|auditor)\n",
                     argv[2]);
        return -2; /* ERR_INVAL */
    }

    char pw[USER_PW_MAX];
    if (argc >= 4) {
        strncpy(pw, argv[3], sizeof(pw) - 1);
        pw[sizeof(pw) - 1] = '\0';
    } else {
        if (tui_input_line(5, 30, "Password for new user: ", pw, sizeof(pw), 1) < 0)
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
    int r = user_call(&req, (int)sizeof(req), &resp, (int)sizeof(resp));
    if (r < 0 || resp.ret < 0) {
        shell_printf("useradd: FAILED (%d)\n", r < 0 ? r : resp.ret);
        return -1;
    }
    shell_printf("useradd: ok - '%s' (%s)\n", argv[1], role_name(role));
    return 0;
}

static int cmd_userdel(int argc, char *argv[]) {
    if (argc < 2) {
        shell_write("Usage: userdel <name>\n");
        return -1;
    }
    user_req_login_t req;
    memset(&req, 0, sizeof(req));
    req.op = USER_OP_USERDEL;
    strncpy(req.name, argv[1], sizeof(req.name) - 1);
    user_resp_login_t resp;
    memset(&resp, 0, sizeof(resp));
    int r = user_call(&req, (int)sizeof(req), &resp, (int)sizeof(resp));
    if (r < 0 || resp.ret < 0) {
        shell_printf("userdel: FAILED (%d)\n", r < 0 ? r : resp.ret);
        return -1;
    }
    shell_printf("userdel: ok\n");
    return 0;
}

/* user_lock <name> / user_unlock <name> — admin disables or re-enables
 * an account (also resets its lockout counter). */
static int cmd_userlock(int argc, char *argv[]) {
    if (argc < 2) {
        shell_write("Usage: user_lock <name>\n");
        return -1;
    }
    user_req_login_t req;
    memset(&req, 0, sizeof(req));
    req.op = USER_OP_LOCK;
    strncpy(req.name, argv[1], sizeof(req.name) - 1);
    user_resp_login_t resp;
    memset(&resp, 0, sizeof(resp));
    int r = user_call(&req, (int)sizeof(req), &resp, (int)sizeof(resp));
    if (r < 0 || resp.ret < 0) {
        shell_printf("user_lock: FAILED (%d)\n", r < 0 ? r : resp.ret);
        return -1;
    }
    shell_printf("user_lock: '%s' locked\n", argv[1]);
    return 0;
}

static int cmd_userunlock(int argc, char *argv[]) {
    if (argc < 2) {
        shell_write("Usage: user_unlock <name>\n");
        return -1;
    }
    user_req_login_t req;
    memset(&req, 0, sizeof(req));
    req.op = USER_OP_UNLOCK;
    strncpy(req.name, argv[1], sizeof(req.name) - 1);
    user_resp_login_t resp;
    memset(&resp, 0, sizeof(resp));
    int r = user_call(&req, (int)sizeof(req), &resp, (int)sizeof(resp));
    if (r < 0 || resp.ret < 0) {
        shell_printf("user_unlock: FAILED (%d)\n", r < 0 ? r : resp.ret);
        return -1;
    }
    shell_printf("user_unlock: '%s' unlocked\n", argv[1]);
    return 0;
}

static int cmd_users(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    user_req_login_t req;
    memset(&req, 0, sizeof(req));
    req.op = USER_OP_USERS;
    user_resp_login_t resp;
    memset(&resp, 0, sizeof(resp));
    int r = user_call(&req, (int)sizeof(req), &resp, (int)sizeof(resp));
    if (r < 0 || resp.ret < 0) {
        if (resp.ret == -9) /* ERR_DENIED */
            shell_write("users: permission denied - OWNER/ADMIN login required\n");
        else
            shell_printf("users: FAILED (%d) - run 'login' first\n", r < 0 ? r : resp.ret);
        return -1;
    }
    /* reason holds "name role;name role;..." lines. */
    char *p = resp.reason;
    if (resp.count == 0) {
        shell_write("Accounts: (none)\n");
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
        ipc_call(s_term_port, (const void *)req, 8, (void *)resp, &resp_len);
    }
    (void)tui_menu(38, 6, 40, shown + 2, "Accounts (Enter/q)", ptrs, shown, NULL);
    return 0;
}

/* ====================================================================
 * Exit guard: stop <svc>
 * 1. TUI confirm dialog 2. admin password (masked) 3. USER_OP_STOP.
 * The user service re-checks OWNER/ADMIN and refuses system-critical
 * services; the kill is executed there (shell is not management-plane).
 * ==================================================================== */

static int cmd_stop(int argc, char *argv[]) {
    if (argc < 2) {
        shell_write("Usage: stop <svc-name>\n");
        return -1;
    }

    /* 1. Confirm dialog. */
    char msg[96];
    snprintf(msg, sizeof(msg), "Stop system program '%s'?", argv[1]);
    int yes = tui_confirm(20, 14, 60, "Confirm Stop", msg,
                          "Type y to confirm, n to cancel");
    if (yes < 0) {
        shell_printf("stop: dialog error (%d)\n", yes);
        return -1;
    }
    if (!yes) {
        shell_printf("stop: cancelled\n");
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
        int r = user_call(&wq, (int)sizeof(wq), &who, (int)sizeof(who));
        if (r < 0 || who.ret < 0) {
            shell_printf("stop: not logged in - run 'login' first (%d)\n", r < 0 ? r : who.ret);
            return -1;
        }
    }

    char pw[USER_PW_MAX];
    if (tui_input_line(5, 31, "Admin password: ", pw, sizeof(pw), 1) < 0)
        return -1;

    user_req_login_t vq;
    memset(&vq, 0, sizeof(vq));
    vq.op = USER_OP_VERIFY;
    strncpy(vq.name, who.name, sizeof(vq.name) - 1);
    strncpy(vq.password, pw, sizeof(vq.password) - 1);
    user_resp_login_t vr;
    memset(&vr, 0, sizeof(vr));
    int r = user_call(&vq, (int)sizeof(vq), &vr, (int)sizeof(vr));
    if (r < 0 || vr.ret < 0) {
        shell_printf("stop: wrong password or verification failed (%d)\n", r < 0 ? r : vr.ret);
        return -1;
    }

    /* 3. Execute the stop (service re-checks OWNER/ADMIN + critical). */
    user_req_stop_t sq;
    memset(&sq, 0, sizeof(sq));
    sq.op = USER_OP_STOP;
    strncpy(sq.svc, argv[1], sizeof(sq.svc) - 1);
    user_resp_stop_t sr;
    memset(&sr, 0, sizeof(sr));
    r = user_call(&sq, (int)sizeof(sq), &sr, (int)sizeof(sr));
    if (r < 0 || sr.ret < 0) {
        shell_printf("stop: FAILED (%d)%s%s\n",
                     r < 0 ? r : sr.ret,
                     sr.detail[0] ? " - " : "",
                     sr.detail);
        return -1;
    }
    shell_printf("stop: %s\n", sr.detail);
    return 0;
}
