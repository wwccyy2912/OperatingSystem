/*
 * keyboard.c - Userspace PS/2 keyboard driver service
 * Copyright (c) 2026 OpSys Project
 *
 * Ring-3 driver for the PS/2 keyboard controller.  The kernel provides
 * IRQ binding, async notification and I/O-port syscalls (P0-A); this
 * service owns the hardware (IRQ1, ports 0x60/0x64) and serves decoded
 * ASCII keys to clients over a registered IPC port.  Architecture:
 *
 *   manager process (spawned by init via SYS_PROCESS_CREATE)
 *     └─ SYS_PROCESS_CREATE("keyboard")            keyboard process
 *          └─ main() = kbd_service_main()          server thread
 *               ├─ cap_create(CAP_TYPE_IO_PORT, RIGHT_ALL)
 *               │           obj_id = (5 << 16) | 0x60   → 0x60..0x64
 *               ├─ ipc_port_create() + port_register("keyboard")
 *               ├─ thread_create(kbd_irq_main)     IRQ thread
 *               │    ├─ cap_create(CAP_TYPE_IRQ, RIGHT_READ, 1)
 *               │    ├─ bind_irq(irq_cap, 1, 1)    IRQ1 → notification bit 0
 *               │    └─ loop: wait_notification(1) → drain 0x60 → decode
 *               │         scancode set-1 → ASCII (shift/caps state machine)
 *               │         → SPSC ring buffer
 *               └─ loop: ipc_recv_from(port, req, &len, &subj)
 *                          -> READ / READ_BLOCK / TAKE / RELEASE_FOCUS
 *                          (copy ring / park / focus) -> ipc_reply(port, resp, len)
 *
 * keyboard runs as its OWN process, so it has a private address space
 * and a private capability table — exactly like the serial driver.
 * The IRQ thread shares that space/caps (created via sys_thread_create
 * inside the same process).
 *
 * PS/2 notes (QEMU and real hardware):
 *   - Data port 0x60, status port 0x64 (bit 0 = output buffer full).
 *   - IRQ1 fires per byte delivered to 0x60.
 *   - Scancode set 1: make code = key press, break = make | 0x80.
 *     0xE0 prefixes extended keys (arrows etc.) — skipped in v0.1.
 *   - No controller init commands are sent: QEMU (and a PC BIOS) leave
 *     the controller scanning; sending 0xAE/0xF4 would require ACK
 *     (0xFA) handling for zero benefit here.  A startup flush drains
 *     any bytes the BIOS left in the output buffer.
 *
 * RX ring buffer: plain head/tail circular buffer, SPSC like serial.c —
 * the IRQ thread is the only writer, the server thread the only reader.
 *
 * IPC protocol (flat structs, raw copy, native little-endian):
 *   Request:  { u32 op; u32 len; }
 *     op 1 = READ          (len = max bytes to read, non-blocking)
 *     op 2 = READ_BLOCK    (len = max bytes to read, blocks until RX data)
 *     op 3 = TAKE_FOCUS    (len ignored; caller becomes the keyboard owner)
 *     op 4 = RELEASE_FOCUS (len ignored; owner gives the keyboard back)
 *   Response: { i32 ret; u8 data[]; }
 *     ret >= 0 : bytes copied (0 when READ finds an empty ring)
 *     ret <  0 : negative error code (ERR_INVAL / ERR_NOCAP / ERR_FAULT)
 * READ_BLOCK parks the call in one of KBD_PARK_MAX slots (mirror of
 * serial.c); the IRQ thread completes it via ipc_reply as soon as its
 * drain decodes keys.  Keyboard focus: TAKE_FOCUS makes a client the
 * sole owner (s_focus_owner); decoded keys are then routed to the
 * owner's parked read, and every other parked client — typically the
 * shell's read_line — stays parked untouched until focus is released,
 * so the TUI panel can own the keyboard without preempting anyone.
 * With focus free the FIRST parked slot is served, which preserves the
 * original single-parking-client behavior exactly.
 */

#include "../lib/libc/stdio.h"
#include "../lib/libos/syscalls.h"
#include "../lib/libc/string.h"
#include <stdint.h>

/* Fixed-width types (kernel/types.h is not includable from user space) */
typedef uint8_t  u8;
typedef uint32_t u32;
typedef int32_t  i32;
typedef uint64_t u64;

/* ====================================================================
 * Constants
 * ==================================================================== */

#define KBD_DATA_PORT   0x60 /* PS/2 data port */
#define KBD_STATUS_PORT 0x64 /* PS/2 status port (bit 0 = OBF) */
#define KBD_STATUS_OBF  (1 << 0)

/* IRQ1 = keyboard (PIC line) */
#define KBD_IRQ      1
#define KBD_IRQ_MASK 1u

/* Protocol ops */
#define KBD_OP_READ          1
#define KBD_OP_READ_BLOCK    2
#define KBD_OP_TAKE_FOCUS    3
#define KBD_OP_RELEASE_FOCUS 4
#define KBD_MAX_DATA         256 /* max payload bytes per request */
#define KBD_RX_RING_SIZE     256 /* ring buffer (holds 255 bytes) */
#define KBD_PARK_MAX         4   /* park-table slots for READ_BLOCK */

/* Scancode set-1: make codes for modifier keys */
#define SC_LSHIFT_MAKE  0x2A
#define SC_LSHIFT_BREAK 0xAA
#define SC_RSHIFT_MAKE  0x36
#define SC_RSHIFT_BREAK 0xB6
#define SC_CAPS         0x3A
#define SC_EXT_PREFIX   0xE0

/* ====================================================================
 * Protocol structures (flat, raw copy — see header comment)
 * ==================================================================== */

typedef struct {
    u32 op;
    u32 len;
} kbd_req_t;

typedef struct {
    i32 ret;
    u8  data[]; /* payload (READ) */
} kbd_resp_t;

#define KBD_REQ_HDR  ((u32)sizeof(kbd_req_t))
#define KBD_RESP_HDR ((u32)sizeof(kbd_resp_t))

/* ====================================================================
 * Service state (SPSC: IRQ thread writes, server thread reads)
 * ==================================================================== */

static u8  s_rx_buf[KBD_RX_RING_SIZE];
static u32 s_rx_head; /* next byte to read  (server thread) */
static u32 s_rx_tail; /* next free slot     (IRQ thread)    */
static u8  s_req_buf[KBD_REQ_HDR];
static u8  s_resp_buf[KBD_RESP_HDR + KBD_MAX_DATA];

/* Keyboard focus: 0 = free (today's behavior — the first parked read
 * gets every key); non-zero = subject id of the client that currently
 * owns the keyboard (the TUI panel, via TAKE_FOCUS).  Written by the
 * server thread on TAKE/RELEASE_FOCUS, read by the IRQ thread while
 * routing keys — single-word cross-thread state, like the park table. */
static u64 s_focus_owner = 0;

/* Park table for blocking READ_BLOCK calls (mirror of serial.c, but
 * multi-slot so the TUI panel can park alongside the shell).  An entry
 * is free when token < 0; owner records the parking caller's subject.
 * The server thread publishes owner/max/resp_len BEFORE token; the IRQ
 * thread completes the entry via ipc_reply once its drain fills buf. */
typedef struct {
    u64 owner;                            /* caller subject that parked here */
    i32 token;                            /* IPC token, < 0 = free slot */
    u32 max;                              /* max bytes the caller accepts */
    u32 resp_len;                         /* bytes decoded by the IRQ thread */
    u8  buf[KBD_RESP_HDR + KBD_MAX_DATA]; /* parked reply payload */
} kbd_park_t;

static kbd_park_t s_park[KBD_PARK_MAX] = {
    {0, -1, 0, 0, {0}},
    {0, -1, 0, 0, {0}},
    {0, -1, 0, 0, {0}},
    {0, -1, 0, 0, {0}},
};

/* Scancode decode state — touched ONLY by the IRQ thread */
static u8 s_extended; /* last byte was the 0xE0 prefix */
static u8 s_shift;    /* left or right shift held */
static u8 s_caps;     /* caps lock toggled on */

/* ====================================================================
 * Scancode set-1 → ASCII (US layout)
 *
 * Indexed by make code; 0 = key produces no text (modifier, F-keys,
 * keypad, unused).  Enter maps to '\n'; Backspace to '\b'.
 * ==================================================================== */

static const char s_key_normal[128] = {
    /* 0x00 */ 0,    0,   '1', '2',  '3',  '4', '5',  '6',
    /* 0x08 */ '7',  '8', '9', '0',  '-',  '=', '\b', '\t',
    /* 0x10 */ 'q',  'w', 'e', 'r',  't',  'y', 'u',  'i',
    /* 0x18 */ 'o',  'p', '[', ']',  '\n', 0,   'a',  's',
    /* 0x20 */ 'd',  'f', 'g', 'h',  'j',  'k', 'l',  ';',
    /* 0x28 */ '\'', '`', 0,   '\\', 'z',  'x', 'c',  'v',
    /* 0x30 */ 'b',  'n', 'm', ',',  '.',  '/', 0,    '*',
    /* 0x38 */ 0,    ' ', 0,   0,    0,    0,   0,    0,
    /* 0x40 */ 0,    0,   0,   0,    0,    0,   0,    '7',
    /* 0x48 */ '8',  '9', '-', '4',  '5',  '6', '+',  '1',
    /* 0x50 */ '2',  '3', '0', '.',  0,    0,   0,    0,
    /* 0x58 */ 0,    0,   0,   0,    0,    0,   0,    0,
    /* 0x60 */ 0,    0,   0,   0,    0,    0,   0,    0,
    /* 0x68 */ 0,    0,   0,   0,    0,    0,   0,    0,
    /* 0x70 */ 0,    0,   0,   0,    0,    0,   0,    0,
    /* 0x78 */ 0,    0,   0,   0,    0,    0,   0,    0,
};

static const char s_key_shift[128] = {
    /* 0x00 */ 0,   0,   '!', '@', '#',  '$', '%',  '^',
    /* 0x08 */ '&', '*', '(', ')', '_',  '+', '\b', '\t',
    /* 0x10 */ 'Q', 'W', 'E', 'R', 'T',  'Y', 'U',  'I',
    /* 0x18 */ 'O', 'P', '{', '}', '\n', 0,   'A',  'S',
    /* 0x20 */ 'D', 'F', 'G', 'H', 'J',  'K', 'L',  ':',
    /* 0x28 */ '"', '~', 0,   '|', 'Z',  'X', 'C',  'V',
    /* 0x30 */ 'B', 'N', 'M', '<', '>',  '?', 0,    '*',
    /* 0x38 */ 0,   ' ', 0,   0,   0,    0,   0,    0,
    /* 0x40 */ 0,   0,   0,   0,   0,    0,   0,    '7',
    /* 0x48 */ '8', '9', '-', '4', '5',  '6', '+',  '1',
    /* 0x50 */ '2', '3', '0', '.', 0,    0,   0,    0,
    /* 0x58 */ 0,   0,   0,   0,   0,    0,   0,    0,
    /* 0x60 */ 0,   0,   0,   0,   0,    0,   0,    0,
    /* 0x68 */ 0,   0,   0,   0,   0,    0,   0,    0,
    /* 0x70 */ 0,   0,   0,   0,   0,    0,   0,    0,
    /* 0x78 */ 0,   0,   0,   0,   0,    0,   0,    0,
};

/* ====================================================================
 * RX ring buffer
 * ==================================================================== */

static void kbd_rx_push(u8 c) {
    u32 next = (s_rx_tail + 1) % KBD_RX_RING_SIZE;
    if (next == s_rx_head)
        return; /* ring full: drop the key */
    s_rx_buf[s_rx_tail] = c;
    s_rx_tail           = next;
}

static u32 kbd_rx_read(u8 *dst, u32 max) {
    u32 n = 0;
    while (n < max && s_rx_head != s_rx_tail) {
        dst[n]    = s_rx_buf[s_rx_head];
        s_rx_head = (s_rx_head + 1) % KBD_RX_RING_SIZE;
        n++;
    }
    return n;
}

/* ====================================================================
 * Park table — key routing
 * ==================================================================== */

/*
 * Return the parked entry a decoded key must be routed to, or NULL.
 * While focus is held (s_focus_owner != 0) only the focus owner's
 * parked entry is eligible; while focus is free (0, the default) the
 * FIRST parked entry is served — identical to the old single-slot
 * shell behavior.  If the target entry is full or none is parked, the
 * key falls through to the RX ring.
 */
static kbd_park_t *kbd_park_target(void) {
    if (s_focus_owner != 0) {
        /* Only the focus owner's parked entry is served while focus is
         * held.  If the focus holder is NOT parked (the perm UI reads
         * the RX ring non-blockingly), keys fall through to the RX
         * ring — that is exactly how 'y'/'n' reach the panel.  No
         * fallback to other parked readers: the shell parks in
         * READ_BLOCK and would swallow the panel's keys. */
        for (u32 i = 0; i < KBD_PARK_MAX; i++)
            if (s_park[i].token >= 0 && s_park[i].owner == s_focus_owner)
                return &s_park[i];
        return NULL;
    }
    for (u32 i = 0; i < KBD_PARK_MAX; i++)
        if (s_park[i].token >= 0)
            return &s_park[i];
    return NULL;
}

/* ====================================================================
 * Scancode decoding (IRQ thread)
 * ==================================================================== */

/*
 * Decode one scancode byte from set 1 into a text character and push it
 * into the RX ring (or the parked reply buffer).  Modifier keys only
 * update state; breaks of non-modifier keys are ignored (v0.1: no
 * key-repeat / release tracking).  Extended (0xE0-prefixed) keys are
 * skipped.
 */
static void kbd_decode_byte(u8 sc) {
    if (sc == SC_EXT_PREFIX) {
        s_extended = 1;
        return;
    }
    if (s_extended) {
        s_extended = 0;
        /* Extended (0xE0-prefixed) keys: map navigation keys to single
         * ASCII control codes so the shell/TUI can handle them like any
         * other byte (no multi-byte escape sequences).  Set-1 extended
         * make codes: Up=0x48 Down=0x50 Left=0x4B Right=0x4D,
         * Home=0x47 End=0x4F PgUp=0x49 PgDn=0x51.  Break codes (|0x80)
         * are ignored. */
        if (sc & 0x80)
            return;
        char ch = 0;
        switch (sc) {
        case 0x48: ch = 0x0B; break; /* Up    -> VT   */
        case 0x50: ch = 0x0C; break; /* Down  -> FF   */
        case 0x4B: ch = 0x08; break; /* Left  -> BS   */
        case 0x4D: ch = 0x14; break; /* Right -> DC4  */
        case 0x47: ch = 0x01; break; /* Home  -> SOH  */
        case 0x4F: ch = 0x05; break; /* End   -> ENQ  */
        case 0x49: ch = 0x02; break; /* PgUp  -> STX  */
        case 0x51: ch = 0x06; break; /* PgDn  -> ACK  */
        default:   return;
        }
        if (ch == 0)
            return;
        kbd_park_t *p = kbd_park_target();
        if (p && p->resp_len < p->max) {
            ((kbd_resp_t *)p->buf)->data[p->resp_len] = (u8)ch;
            p->resp_len++;
        } else {
            kbd_rx_push((u8)ch);
        }
        return;
    }

    /* Modifier make/break codes — update state, produce no text */
    if (sc == SC_LSHIFT_MAKE || sc == SC_RSHIFT_MAKE) {
        s_shift = 1;
        return;
    }
    if (sc == SC_LSHIFT_BREAK || sc == SC_RSHIFT_BREAK) {
        s_shift = 0;
        return;
    }
    if (sc == SC_CAPS) {
        s_caps = !s_caps;
        return;
    }

    if (sc & 0x80)
        return; /* non-modifier break code: ignore */

    char ch = s_shift ? s_key_shift[sc] : s_key_normal[sc];
    if (s_caps && ch >= 'a' && ch <= 'z')
        ch = (char)(ch - 'a' + 'A');
    if (ch == 0)
        return;

    /* Route to the focus owner's parked blocking READ (or the first
     * parked entry while focus is free), else the RX ring. */
    kbd_park_t *p = kbd_park_target();
    if (p && p->resp_len < p->max) {
        ((kbd_resp_t *)p->buf)->data[p->resp_len] = (u8)ch;
        p->resp_len++;
    } else {
        kbd_rx_push((u8)ch);
    }
}

/*
 * Drain the PS/2 output buffer after an IRQ1 notification.
 * Reads status port 0x64; while bit 0 (output buffer full) is set,
 * read one byte from data port 0x60 and decode it.  Never blocks.
 */
static void kbd_rx_drain(void) {
    for (;;) {
        int st = io_read8(KBD_STATUS_PORT);
        if (st < 0)
            break; /* I/O error — cannot proceed */
        if (!(st & KBD_STATUS_OBF))
            break; /* output buffer empty */
        int c = io_read8(KBD_DATA_PORT);
        if (c < 0)
            break;
        kbd_decode_byte((u8)c);
    }
}

/* ====================================================================
 * Server side
 * ==================================================================== */

static void kbd_reply(int token, i32 ret, const u8 *data, u32 len) {
    kbd_resp_t *resp = (kbd_resp_t *)s_resp_buf;
    resp->ret        = ret;
    if (data && len > 0)
        memcpy(resp->data, data, len);
    int r = ipc_reply(token, s_resp_buf, (int)(KBD_RESP_HDR + len));
    if (r < 0)
        printf("keyboard: ipc_reply failed (%d)\n", r);
}

/*
 * Serve a READ / READ_BLOCK request.  Non-blocking READ never blocks:
 * an empty ring yields ret = 0.  READ_BLOCK with an empty ring parks
 * the call in the first free park-table slot (recording the caller's
 * subject as owner); the IRQ thread completes it via ipc_reply as soon
 * as its drain decodes keys into that entry's buffer.  Table full:
 * serve 0 (same fallback as the old single slot being taken).
 */
static void kbd_reply_read(int token, u32 max, int blocking, u64 owner) {
    kbd_resp_t *resp = (kbd_resp_t *)s_resp_buf;

    u32 n = kbd_rx_read(resp->data, max);
    if (n > 0 || !blocking) {
        /* Bytes available or non-blocking request: serve what we have
         * (possibly 0). */
        kbd_reply(token, (i32)n, resp->data, n);
        return;
    }

    /* Blocking read with an empty ring: claim the first free slot. */
    kbd_park_t *p = NULL;
    for (u32 i = 0; i < KBD_PARK_MAX; i++) {
        if (s_park[i].token < 0) {
            p = &s_park[i];
            break;
        }
    }
    if (p == NULL) {
        /* Park table full: serve 0 (existing fallback behavior). */
        kbd_reply(token, 0, NULL, 0);
        return;
    }

    /* Park the call for the IRQ thread.  Publish owner/max/resp_len
     * BEFORE token. */
    p->owner    = owner;
    p->max      = max;
    p->resp_len = 0;
    p->token    = token;

    /* Re-check once: a drain may have pushed bytes between the first
     * read and the publish.  If so, serve them directly. */
    n = kbd_rx_read(resp->data, max);
    if (n > 0) {
        p->token = -1;
        kbd_reply(token, (i32)n, resp->data, n);
    }
    /* else: the IRQ thread completes this call when keys arrive. */
}

static void kbd_handle_request(int token, u64 caller_subject) {
    kbd_req_t *req = (kbd_req_t *)s_req_buf;

    if (req->op == KBD_OP_READ || req->op == KBD_OP_READ_BLOCK) {
        if (req->len > KBD_MAX_DATA) {
            kbd_reply(token, ERR_INVAL, NULL, 0);
            return;
        }
        kbd_reply_read(token, req->len, req->op == KBD_OP_READ_BLOCK, caller_subject);
    } else if (req->op == KBD_OP_TAKE_FOCUS) {
        /* The caller becomes the keyboard owner.  Idempotent: the same
         * owner re-taking is a no-op success.  Never completes anyone's
         * parked read. */
        s_focus_owner = caller_subject;
        kbd_reply(token, 0, NULL, 0);
    } else if (req->op == KBD_OP_RELEASE_FOCUS) {
        /* Only the focus holder may release (the kernel-filled caller
         * subject is never 0, so focus free also means no holder). */
        if (s_focus_owner == 0 || caller_subject != s_focus_owner) {
            kbd_reply(token, ERR_NOCAP, NULL, 0);
            return;
        }
        s_focus_owner = 0;
        kbd_reply(token, 0, NULL, 0);
    } else {
        kbd_reply(token, ERR_INVAL, NULL, 0);
    }
}

static void kbd_server_loop(int port) {
    for (;;) {
        int msg_len        = (int)sizeof(s_req_buf);
        int token          = 0;
        u64 caller_subject = 0;
        /* ipc_recv_from gives the kernel-filled unforgeable sender
         * subject — the basis for focus ownership and park routing. */
        int ret = ipc_recv_from(port, s_req_buf, &msg_len, &token, &caller_subject);
        if (ret < 0) {
            printf("keyboard: ipc_recv failed (%d)\n", ret);
            thread_exit(1);
        }
        if (msg_len < (int)KBD_REQ_HDR) {
            kbd_reply(token, ERR_INVAL, NULL, 0);
            continue;
        }
        kbd_handle_request(token, caller_subject);
    }
}

/* ====================================================================
 * IRQ thread (spawned by kbd_service_main)
 * ==================================================================== */

static void kbd_irq_main(void *arg) {
    (void)arg;

    printf("keyboard: IRQ thread started\n");

    int irq_cap = cap_create_obj(CAP_TYPE_IRQ, RIGHT_READ, KBD_IRQ);
    if (irq_cap < 0) {
        printf("keyboard: cap_create(IRQ) failed (%d)\n", irq_cap);
        thread_exit(1);
    }

    int ret = bind_irq(irq_cap, KBD_IRQ, KBD_IRQ_MASK);
    if (ret < 0) {
        printf("keyboard: bind_irq(%d) failed (%d)\n", KBD_IRQ, ret);
        thread_exit(1);
    }
    printf("keyboard: IRQ1 bound, PS/2 drain active\n");

    for (;;) {
        wait_notification(KBD_IRQ_MASK);
        kbd_rx_drain();

        /* Complete a parked blocking READ if the drain routed keys into
         * its response buffer — the focus owner's entry, or the first
         * parked entry while focus is free. */
        kbd_park_t *p = kbd_park_target();
        if (p && p->resp_len > 0) {
            int tok            = p->token;
            u32 n              = p->resp_len;
            p->token           = -1;
            p->resp_len        = 0;
            kbd_resp_t *parked = (kbd_resp_t *)p->buf;
            parked->ret        = (i32)n; /* header must carry the byte count */
            int r              = ipc_reply(tok, p->buf, (int)(KBD_RESP_HDR + n));
            if (r < 0)
                printf("keyboard: parked-read ipc_reply failed (%d)\n", r);
        }
    }
}

/* ====================================================================
 * Entry point (keyboard process main)
 * ==================================================================== */

static void kbd_service_main(void *arg) {
    (void)arg;

    printf("keyboard: starting PS/2 driver service\n");

    /* 1. I/O-port capability: covers 0x60..0x64 (data + status). */
    int io_cap = cap_create_obj(CAP_TYPE_IO_PORT, RIGHT_ALL, (5 << 16) | KBD_DATA_PORT);
    if (io_cap < 0) {
        printf("keyboard: cap_create(IO_PORT) failed (%d)\n", io_cap);
        thread_exit(1);
    }
    printf("keyboard: caps OK (io_port=%d)\n", io_cap);

    /* 2. Flush any scancodes the BIOS left in the PS/2 output buffer,
     *    so stale data never masquerades as a fresh keypress. */
    kbd_rx_drain();

    /* 3. IPC port, registered under the well-known name "keyboard". */
    int port = ipc_port_create();
    if (port < 0) {
        printf("keyboard: ipc_port_create failed (%d)\n", port);
        thread_exit(1);
    }
    int ret = port_register("keyboard", port);
    if (ret < 0) {
        printf("keyboard: port_register('keyboard') failed (%d)\n", ret);
        thread_exit(1);
    }
    printf("keyboard: port %d registered as 'keyboard'\n", port);

    /* 4. Spawn the IRQ thread (binds IRQ1, drains + decodes 0x60). */
    int irq_tid = thread_create(kbd_irq_main, NULL, 10);
    if (irq_tid < 0) {
        printf("keyboard: thread_create(IRQ thread) failed (%d)\n", irq_tid);
        thread_exit(1);
    }
    printf("keyboard: IRQ thread TID=%d\n", irq_tid);

    /* 5. Serve clients. */
    printf("keyboard: serving on port %d\n", port);
    kbd_server_loop(port);
}

int main(void) {
    kbd_service_main(NULL);
    return 0; /* unreachable */
}
