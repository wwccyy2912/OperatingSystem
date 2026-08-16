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
#include "../lib/libfs/fs.h"    /* libfs VFS client (vfs_ls/vfs_cat/vfs_stat) */
#include "../lib/libpkg/pkg.h"  /* libpkg pkg-manager client (pkg_*) */
#include "../lib/libos/syscalls.h"
#include "../perm/perm.h" /* Powerbox protocol (perm_answer/perm_revoke) */
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
#define MAX_ARGS      16
#define SHELL_PROMPT  "opsys$ "

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
static int cmd_meminfo(int argc, char *argv[]);
static int cmd_clear(int argc, char *argv[]);
static int cmd_cap(int argc, char *argv[]);
static int cmd_ports(int argc, char *argv[]);
static int cmd_sleep(int argc, char *argv[]);
static int cmd_threads(int argc, char *argv[]);
static int cmd_mutex(int argc, char *argv[]);
static int cmd_spawn(int argc, char *argv[]);
static int cmd_uptime(int argc, char *argv[]);
static int cmd_exit(int argc, char *argv[]);
static int cmd_reboot(int argc, char *argv[]);
static int cmd_panic(int argc, char *argv[]); /* TEMP test hook */
static int cmd_kill(int argc, char *argv[]);
static int cmd_ps(int argc, char *argv[]);
static int cmd_vfs_ls(int argc, char *argv[]);
static int cmd_vfs_cat(int argc, char *argv[]);
static int cmd_vfs_stat(int argc, char *argv[]);
static int cmd_vfs_write(int argc, char *argv[]);
static int cmd_vfs_fill(int argc, char *argv[]);
static int cmd_vfs_mkdir(int argc, char *argv[]);
static int cmd_vfs_rm(int argc, char *argv[]);
/* Phase 2: bookmarks + Powerbox + move (design §8) */
static int cmd_bm_create(int argc, char *argv[]);
static int cmd_bm_resolve(int argc, char *argv[]);
static int cmd_bm_revoke(int argc, char *argv[]);
static int cmd_perm_answer(int argc, char *argv[]);
static int cmd_perm_query(int argc, char *argv[]);
static int cmd_perm_revoke(int argc, char *argv[]);
static int cmd_move(int argc, char *argv[]);
static int cmd_pkg(int argc, char *argv[]);

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

/*
 * Read one line from keyboard input.
 * Uses a blocking READ_BLOCK: the calling thread parks inside ipc_call
 * until the keyboard service delivers key bytes (the service parks the
 * call and the IRQ thread completes it when PS/2 scancodes arrive), so
 * there is no poll/yield busy loop.  Echoes characters, handles
 * backspace and basic editing.  Returns the number of characters read
 * (excluding null terminator), or -1 on error.
 */
static int read_line(char *buf, int maxlen) {
    int pos = 0;
    u32 req[2 + 1];  /* { op; len }        */
    u32 resp[1 + 8]; /* { ret; data[32] }  */

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
                buf[pos] = '\0';
                return pos;

            case '\b':
            case 0x7F: /* DEL */
                if (pos > 0) {
                    pos--;
                    shell_write("\b \b"); /* erase on screen */
                }
                break;

            case '\t':
                /* Tab — ignore for now */
                break;

            default:
                if (ch >= ' ' && ch < 0x7F) {
                    if (pos < maxlen - 1) {
                        buf[pos++] = (char)ch;
                        shell_putc((char)ch);
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
    shell_write("\n");

    char line[LINE_BUF_SIZE];

    for (;;) {
        shell_write(SHELL_PROMPT);
        int len = read_line(line, LINE_BUF_SIZE);
        if (len < 0) {
            thread_yield(); /* serial service unavailable */
            continue;
        }
        if (len > 0) {
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
static int cmd_spawn(int argc, char *argv[]) {
    const char *name = (argc > 1) ? argv[1] : "hello";
    static char blob_buf[131072]; /* must hold the largest demo ELF */
    int         size = blob_get(name, blob_buf, sizeof(blob_buf));
    if (size < 0) {
        shell_printf("spawn: blob_get(%s) FAILED (%d)\n", name, size);
        return 1;
    }
    shell_printf("spawn: fetched %s.elf blob from kernel (%d bytes)\n", name, size);
    int pid = process_create(name, blob_buf, size);
    if (pid < 0) {
        shell_printf("spawn: FAILED (%d)\n", pid);
        return 1;
    }
    shell_printf("spawn: created PID %d\n", pid);
    printf("[shell] cmd_spawn: pid=%d tick=%d\n", pid, get_time());
    return 0;
}

static int cmd_meminfo(int argc, char *argv[]) {
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

/*
 * TEMP test hook: exercise the unified kernel panic path from the shell.
 * The kernel halts — this never returns on success.
 */
static int cmd_panic(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    shell_write("Triggering kernel panic...\n");
    (void)sys_panic();
    /* Only reached if the panic syscall failed. */
    shell_write("panic: syscall failed, system still running\n");
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
    static proc_info_t s_ps_info[64];

    int n = process_list(s_ps_info, 64);
    printf("[shell] cmd_ps: n=%d tick=%d\n", n, get_time());
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
    if (argc < 2) {
        shell_write("Usage: kill <pid> [signum]\n");
        return -1;
    }
    int pid = atoi(argv[1]);
    if (pid <= 0) {
        shell_printf("kill: invalid pid '%s'\n", argv[1]);
        return -1;
    }
    int signum = SIGKILL;
    if (argc >= 3) {
        signum = atoi(argv[2]);
        if (signum <= 0 || signum >= NSIG) {
            shell_printf("kill: invalid signum '%s' (1..%d)\n", argv[2], NSIG - 1);
            return -1;
        }
    }
    int ret = kill(pid, signum);
    printf("[shell] cmd_kill: pid=%d signum=%d ret=%d tick=%d\n", pid, signum, ret, get_time());
    if (ret < 0) {
        shell_printf("kill: PID %d SIG %d FAILED (%d)\n", pid, signum, ret);
        return -1;
    }
    shell_printf("kill: sent SIG %d to PID %d\n", signum, pid);
    return 0;
}

/* ====================================================================
 * VFS client commands (libfs over the "vfs" server)
 * ==================================================================== */

/*
 * vfs_ls <url> — enumerate a directory.  URL like "/Volumes/System/"
 * or "Users".  "/" and "/Volumes" enumerate the mounted volumes (the
 * root view: there is no root item, the server answers from the mount
 * table).  Prints one child name per line, then the entry count.
 */
static int cmd_vfs_ls(int argc, char *argv[]) {
    if (argc < 2) {
        shell_write("Usage: vfs_ls <dir-url>\n");
        return -1;
    }
    if (strcmp(argv[1], "/") == 0 || strcmp(argv[1], "/Volumes") == 0 ||
        strcmp(argv[1], "/Volumes/") == 0) {
        static vfs_vol_info_t vols[VFS_MAX_VOLS]; /* small, static */
        u32                   count = 0;
        int                   r     = fs_list_volumes(vols, &count);
        if (r < 0) {
            shell_printf("vfs_ls: %s FAILED (%d)\n", argv[1], r);
            return -1;
        }
        for (u32 i = 0; i < count; i++) {
            shell_write(vols[i].mount_name);
            shell_write(vols[i].read_only ? " (ro)\n" : "\n");
        }
        shell_printf("vfs_ls: %d volumes\n", count);
        return 0;
    }
    static vfs_enum_batch_t batch; /* ~16.5 KB — keep off the stack */
    vfs_handle_t            e;
    int                     r = fs_enum_begin(argv[1], &e);
    if (r < 0) {
        shell_printf("vfs_ls: %s FAILED (%d)\n", argv[1], r);
        return -1;
    }
    int total = 0;
    for (;;) {
        r = fs_enum_next(e, &batch);
        if (r < 0) {
            shell_printf("vfs_ls: enum FAILED (%d)\n", r);
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
    shell_printf("vfs_ls: %d entries\n", total);
    return 0;
}

/*
 * vfs_cat <url> — read and display a file.  Non-printable bytes are
 * shown as '.' so binary blobs do not corrupt the terminal.  Ends with
 * a byte-count line so the content length can be verified against the
 * source blob.
 */
static int cmd_vfs_cat(int argc, char *argv[]) {
    if (argc < 2) {
        shell_write("Usage: vfs_cat <file-url>\n");
        return -1;
    }
    vfs_item_info_t info;
    int             r = fs_get_item(argv[1], &info);
    if (r < 0) {
        shell_printf("vfs_cat: %s FAILED (%d)\n", argv[1], r);
        return -1;
    }
    shell_printf("== %s (%d bytes) ==\n", argv[1], (int)info.size);

    vfs_handle_t h;
    r = fs_open_item(argv[1], VFS_OPEN_READONLY, VFS_ACCESS_READ, &h);
    if (r < 0) {
        shell_printf("vfs_cat: open FAILED (%d)\n", r);
        return -1;
    }
    static u8 buf[1024];
    u32       total = 0;
    for (;;) {
        u32 got = 0;
        r       = fs_read(h, total, buf, sizeof(buf), &got);
        if (r < 0) {
            shell_printf("vfs_cat: read FAILED (%d)\n", r);
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
 * vfs_stat [url] — volume capacity/usage.  With no argument, stats
 * both configured volumes (System read-only, Users 32 MiB RAM).
 */
static int cmd_vfs_stat(int argc, char *argv[]) {
    static const char *vols[2] = {"/Volumes/System", "/Volumes/Users"};
    int                count   = (argc >= 2) ? 1 : 2;
    for (int v = 0; v < count; v++) {
        const char *url   = (argc >= 2) ? argv[1] : vols[v];
        u64         total = 0, used = 0;
        u32         ro = 0;
        int         r  = fs_stat_volume(url, &total, &used, &ro);
        if (r < 0) {
            shell_printf("vfs_stat: %s FAILED (%d)\n", url, r);
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
 * vfs_write <url> <text> — write text to a file (create/truncate,
 * VFS_ACCESS_WRITE).  Exercises the write path: on the read-only System
 * volume the server must reject the open with VFS_ERR_READONLY (-100);
 * on the RAM volume the bytes must land so vfs_cat can read them back.
 */
static int cmd_vfs_write(int argc, char *argv[]) {
    if (argc < 3) {
        shell_write("Usage: vfs_write <file-url> <text>\n");
        return -1;
    }
    const char *url  = argv[1];
    const char *text = argv[2];
    u32         len  = (u32)strlen(text);

    vfs_handle_t h;
    int          r = fs_open_item(url, VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE, VFS_ACCESS_WRITE, &h);
    if (r < 0) {
        shell_printf("vfs_write: open %s FAILED (%d)\n", url, r);
        return -1;
    }
    r = fs_write(h, 0, text, len);
    if (r < 0) {
        shell_printf("vfs_write: write FAILED (%d)\n", r);
        fs_close(h);
        return -1;
    }
    fs_close(h);
    shell_printf("vfs_write: %d bytes written to %s\n", (int)len, url);
    return 0;
}

/*
 * vfs_fill <url> — grow a file in 4 KiB chunks until the volume is
 * full.  Exercises the ENOSPC path: the fs_mem_driver capacity check
 * must reject the write that would exceed the 32 MiB Users volume with
 * VFS_ERR_NOSPC (-101).  Prints progress every 8 MiB and the failing
 * offset + error code.
 */
static int cmd_vfs_fill(int argc, char *argv[]) {
    if (argc < 2) {
        shell_write("Usage: vfs_fill <file-url>\n");
        return -1;
    }
    const char *url = argv[1];

    vfs_handle_t h;
    int          r = fs_open_item(url, VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE, VFS_ACCESS_WRITE, &h);
    if (r < 0) {
        shell_printf("vfs_fill: open %s FAILED (%d)\n", url, r);
        return -1;
    }

    static u8 s_fill_buf[4096];
    memset(s_fill_buf, 0xAB, sizeof(s_fill_buf));

    u64 off = 0;
    for (;;) {
        r = fs_write(h, off, s_fill_buf, sizeof(s_fill_buf));
        if (r < 0) {
            shell_printf("vfs_fill: NOSPC at %d MiB (err %d)\n", (int)(off >> 20), r);
            fs_close(h);
            return 0;
        }
        off += sizeof(s_fill_buf);
        if ((off & 0x7FFFFF) == 0) /* every 8 MiB */
            shell_printf("vfs_fill: %d MiB\n", (int)(off >> 20));
    }
}

/*
 * vfs_mkdir <url> — create an empty directory (fs_create_dir).
 * Fails with VFS_ERR_EXISTS (-104) if the directory already exists.
 */
static int cmd_vfs_mkdir(int argc, char *argv[]) {
    if (argc < 2) {
        shell_write("Usage: vfs_mkdir <dir-url>\n");
        return -1;
    }
    int r = fs_create_dir(argv[1]);
    if (r < 0) {
        shell_printf("vfs_mkdir: %s FAILED (%d)\n", argv[1], r);
        return -1;
    }
    shell_printf("vfs_mkdir: created %s\n", argv[1]);
    return 0;
}

/*
 * vfs_rm <url> — delete an item (fs_delete_item, non-recursive).  A
 * non-empty directory fails with ERR_BUSY.
 */
static int cmd_vfs_rm(int argc, char *argv[]) {
    if (argc < 2) {
        shell_write("Usage: vfs_rm <url>\n");
        return -1;
    }
    int r = fs_delete_item(argv[1], 0);
    if (r < 0) {
        shell_printf("vfs_rm: %s FAILED (%d)\n", argv[1], r);
        return -1;
    }
    shell_printf("vfs_rm: removed %s\n", argv[1]);
    return 0;
}

/* ====================================================================
 * Phase 2 test commands: security-scoped bookmarks + Powerbox + move
 * (design §8).  Acceptance flow:
 *   bm_create <url> r        → -105 (no grant; term shows the prompt)
 *   perm_answer <id> y       → grant upserted, UI_SHOW update pushed
 *   bm_create <url> r        → ok, blob cached
 *   bm_resolve               → handle (授权后 → 句柄)
 *   move <url> <dst> [name]  → itemID stable, bookmark still valid
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
            shell_write(" (denied — see term, then perm_answer <id> y, "
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
static int cmd_move(int argc, char *argv[]) {
    if (argc < 3) {
        shell_write("Usage: move <src-url> <dst-dir-url> [new-name]\n");
        return -1;
    }
    vfs_item_info_t item;
    int             r = fs_move_item(argv[1], argv[2], (argc >= 4) ? argv[3] : "", &item);
    if (r < 0) {
        shell_printf("move: FAILED (%d)\n", r);
        return -1;
    }
    shell_printf("move: '%s' -> item %d (size %d)\n", item.name, (int)item.item_id, (int)item.size);
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
    shell_register_command("meminfo", "Show free physical memory", cmd_meminfo);
    shell_register_command("clear", "Clear the terminal", cmd_clear);
    shell_register_command("cap", "Create a capability (test)", cmd_cap);
    shell_register_command("ports", "List registered IPC ports", cmd_ports);
    shell_register_command("sleep", "Sleep for N ticks: sleep <ticks>", cmd_sleep);
    shell_register_command("threads", "Spawn a test worker thread", cmd_threads);
    shell_register_command("mutex", "Mutex demo: N threads on a counter", cmd_mutex);
    shell_register_command("spawn", "Spawn an embedded demo (spawn [blob_name])", cmd_spawn);
    shell_register_command("uptime", "Show system tick count", cmd_uptime);
    shell_register_command("exit", "Exit the shell", cmd_exit);
    shell_register_command("reboot", "Halt the system", cmd_reboot);
    shell_register_command("panic", "Trigger a kernel panic (TEMP test)", cmd_panic);
    shell_register_command("kill", "Send a signal: kill <pid> [signum]", cmd_kill);
    shell_register_command("ps", "List running processes", cmd_ps);
    shell_register_command(
        "vfs_ls", "List a VFS dir: vfs_ls <url> (/ or /Volumes = volumes)", cmd_vfs_ls);
    shell_register_command("vfs_cat", "Show a VFS file: vfs_cat <url>", cmd_vfs_cat);
    shell_register_command("vfs_stat", "VFS volume stats: vfs_stat [url]", cmd_vfs_stat);
    shell_register_command("vfs_write", "Write a VFS file: vfs_write <url> <text>", cmd_vfs_write);
    shell_register_command("vfs_fill", "Fill a volume until ENOSPC: vfs_fill <url>", cmd_vfs_fill);
    shell_register_command("vfs_mkdir", "Create a VFS dir: vfs_mkdir <url>", cmd_vfs_mkdir);
    shell_register_command("vfs_rm", "Delete a VFS item: vfs_rm <url>", cmd_vfs_rm);
    shell_register_command(
        "bm_create", "Powerbox-gated bookmark: bm_create <url> [r|w|rw]", cmd_bm_create);
    shell_register_command("bm_resolve", "Resolve cached bookmark to a handle", cmd_bm_resolve);
    shell_register_command("bm_revoke", "Drop the cached bookmark server-side", cmd_bm_revoke);
    shell_register_command(
        "perm_answer", "Answer a Powerbox query: perm_answer <id> y|n", cmd_perm_answer);
    shell_register_command(
        "perm_query", "Show pending Powerbox query: perm_query [id]", cmd_perm_query);
    shell_register_command("perm_revoke", "Drop grants: perm_revoke [subject_id]", cmd_perm_revoke);
    shell_register_command("move", "Move/rename: move <src> <dst-dir> [new-name]", cmd_move);
    shell_register_command("pkg", "pkg-manager: pkg <install|list|run|remove>", cmd_pkg);

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
