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
 *               └─ loop: ipc_recv(port, req, &len) -> READ / READ_BLOCK
 *                          (copy ring / park) -> ipc_reply(port, resp, len)
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
 *     op 1 = READ       (len = max bytes to read,   non-blocking)
 *     op 2 = READ_BLOCK (len = max bytes to read,   blocks until RX data)
 *   Response: { i32 ret; u8 data[]; }
 *     ret >= 0 : bytes copied (0 when READ finds an empty ring)
 *     ret <  0 : negative error code (ERR_INVAL / ERR_NOCAP / ERR_FAULT)
 * READ_BLOCK parks the call in a single pending slot (mirror of
 * serial.c); the IRQ thread completes it via ipc_reply as soon as its
 * drain decodes keys.  The shell is the only blocking client.
 */

#include "../lib/libc/stdio.h"
#include "../lib/libos/syscalls.h"
#include "../lib/libc/string.h"
#include <stdint.h>

/* Fixed-width types (kernel/types.h is not includable from user space) */
typedef uint8_t     u8;
typedef uint32_t    u32;
typedef int32_t     i32;

/* ====================================================================
 * Constants
 * ==================================================================== */

#define KBD_DATA_PORT   0x60    /* PS/2 data port */
#define KBD_STATUS_PORT 0x64    /* PS/2 status port (bit 0 = OBF) */
#define KBD_STATUS_OBF  (1 << 0)

/* IRQ1 = keyboard (PIC line) */
#define KBD_IRQ         1
#define KBD_IRQ_MASK    1u

/* Protocol ops */
#define KBD_OP_READ      1
#define KBD_OP_READ_BLOCK 2
#define KBD_MAX_DATA     256         /* max payload bytes per request */
#define KBD_RX_RING_SIZE 256         /* ring buffer (holds 255 bytes) */

/* Scancode set-1: make codes for modifier keys */
#define SC_LSHIFT_MAKE   0x2A
#define SC_LSHIFT_BREAK  0xAA
#define SC_RSHIFT_MAKE   0x36
#define SC_RSHIFT_BREAK  0xB6
#define SC_CAPS          0x3A
#define SC_EXT_PREFIX    0xE0

/* ====================================================================
 * Protocol structures (flat, raw copy — see header comment)
 * ==================================================================== */

typedef struct {
    u32 op;
    u32 len;
} kbd_req_t;

typedef struct {
    i32 ret;
    u8  data[];             /* payload (READ) */
} kbd_resp_t;

#define KBD_REQ_HDR      ((u32)sizeof(kbd_req_t))
#define KBD_RESP_HDR     ((u32)sizeof(kbd_resp_t))

/* ====================================================================
 * Service state (SPSC: IRQ thread writes, server thread reads)
 * ==================================================================== */

static u8   s_rx_buf[KBD_RX_RING_SIZE];
static u32  s_rx_head;              /* next byte to read  (server thread) */
static u32  s_rx_tail;              /* next free slot     (IRQ thread)    */
static u8   s_req_buf[KBD_REQ_HDR];
static u8   s_resp_buf[KBD_RESP_HDR + KBD_MAX_DATA];
static u8   s_parked_buf[KBD_RESP_HDR + KBD_MAX_DATA];

/* Pending blocking-READ slot (mirror of serial.c) */
static i32  s_pending_token = -1;
static u32  s_pending_max = 0;
static u32  s_pending_resp_len = 0;

/* Scancode decode state — touched ONLY by the IRQ thread */
static u8   s_extended;             /* last byte was the 0xE0 prefix */
static u8   s_shift;                /* left or right shift held */
static u8   s_caps;                 /* caps lock toggled on */

/* ====================================================================
 * Scancode set-1 → ASCII (US layout)
 *
 * Indexed by make code; 0 = key produces no text (modifier, F-keys,
 * keypad, unused).  Enter maps to '\n'; Backspace to '\b'.
 * ==================================================================== */

static const char s_key_normal[128] = {
    /* 0x00 */ 0, 0, '1', '2', '3', '4', '5', '6',
    /* 0x08 */ '7', '8', '9', '0', '-', '=', '\b', '\t',
    /* 0x10 */ 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i',
    /* 0x18 */ 'o', 'p', '[', ']', '\n', 0, 'a', 's',
    /* 0x20 */ 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
    /* 0x28 */ '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
    /* 0x30 */ 'b', 'n', 'm', ',', '.', '/', 0, '*',
    /* 0x38 */ 0, ' ', 0, 0, 0, 0, 0, 0,
    /* 0x40 */ 0, 0, 0, 0, 0, 0, 0, '7',
    /* 0x48 */ '8', '9', '-', '4', '5', '6', '+', '1',
    /* 0x50 */ '2', '3', '0', '.', 0, 0, 0, 0,
    /* 0x58 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x60 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x68 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x70 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x78 */ 0, 0, 0, 0, 0, 0, 0, 0,
};

static const char s_key_shift[128] = {
    /* 0x00 */ 0, 0, '!', '@', '#', '$', '%', '^',
    /* 0x08 */ '&', '*', '(', ')', '_', '+', '\b', '\t',
    /* 0x10 */ 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I',
    /* 0x18 */ 'O', 'P', '{', '}', '\n', 0, 'A', 'S',
    /* 0x20 */ 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',
    /* 0x28 */ '"', '~', 0, '|', 'Z', 'X', 'C', 'V',
    /* 0x30 */ 'B', 'N', 'M', '<', '>', '?', 0, '*',
    /* 0x38 */ 0, ' ', 0, 0, 0, 0, 0, 0,
    /* 0x40 */ 0, 0, 0, 0, 0, 0, 0, '7',
    /* 0x48 */ '8', '9', '-', '4', '5', '6', '+', '1',
    /* 0x50 */ '2', '3', '0', '.', 0, 0, 0, 0,
    /* 0x58 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x60 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x68 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x70 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x78 */ 0, 0, 0, 0, 0, 0, 0, 0,
};

/* ====================================================================
 * RX ring buffer
 * ==================================================================== */

static void kbd_rx_push(u8 c)
{
    u32 next = (s_rx_tail + 1) % KBD_RX_RING_SIZE;
    if (next == s_rx_head)
        return;                     /* ring full: drop the key */
    s_rx_buf[s_rx_tail] = c;
    s_rx_tail = next;
}

static u32 kbd_rx_read(u8 *dst, u32 max)
{
    u32 n = 0;
    while (n < max && s_rx_head != s_rx_tail) {
        dst[n] = s_rx_buf[s_rx_head];
        s_rx_head = (s_rx_head + 1) % KBD_RX_RING_SIZE;
        n++;
    }
    return n;
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
static void kbd_decode_byte(u8 sc)
{
    if (sc == SC_EXT_PREFIX) {
        s_extended = 1;
        return;
    }
    if (s_extended) {
        s_extended = 0;             /* skip the extended key body */
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
        return;                     /* non-modifier break code: ignore */

    char ch = s_shift ? s_key_shift[sc] : s_key_normal[sc];
    if (s_caps && ch >= 'a' && ch <= 'z')
        ch = (char)(ch - 'a' + 'A');
    if (ch == 0)
        return;

    /* Route to the parked blocking READ if one is pending, else ring */
    if (s_pending_token >= 0 && s_pending_resp_len < s_pending_max) {
        ((kbd_resp_t *)s_parked_buf)->data[s_pending_resp_len] = (u8)ch;
        s_pending_resp_len++;
    } else {
        kbd_rx_push((u8)ch);
    }
}

/*
 * Drain the PS/2 output buffer after an IRQ1 notification.
 * Reads status port 0x64; while bit 0 (output buffer full) is set,
 * read one byte from data port 0x60 and decode it.  Never blocks.
 */
static void kbd_rx_drain(void)
{
    for (;;) {
        int st = io_read8(KBD_STATUS_PORT);
        if (st < 0)
            break;                  /* I/O error — cannot proceed */
        if (!(st & KBD_STATUS_OBF))
            break;                  /* output buffer empty */
        int c = io_read8(KBD_DATA_PORT);
        if (c < 0)
            break;
        kbd_decode_byte((u8)c);
    }
}

/* ====================================================================
 * Server side
 * ==================================================================== */

static void kbd_reply(int token, i32 ret, const u8 *data, u32 len)
{
    kbd_resp_t *resp = (kbd_resp_t *)s_resp_buf;
    resp->ret = ret;
    if (data && len > 0)
        memcpy(resp->data, data, len);
    int r = ipc_reply(token, s_resp_buf, (int)(KBD_RESP_HDR + len));
    if (r < 0)
        printf("keyboard: ipc_reply failed (%d)\n", r);
}

/*
 * Serve a READ / READ_BLOCK request.  Non-blocking READ never blocks:
 * an empty ring yields ret = 0.  READ_BLOCK with an empty ring parks
 * the call (single pending slot); the IRQ thread completes it via
 * ipc_reply as soon as its drain decodes keys into s_parked_buf.
 */
static void kbd_reply_read(int token, u32 max, int blocking)
{
    kbd_resp_t *resp = (kbd_resp_t *)s_resp_buf;

    u32 n = kbd_rx_read(resp->data, max);
    if (n > 0 || !blocking || s_pending_token >= 0) {
        /* Bytes available, non-blocking request, or the single pending
         * slot is already taken: serve what we have (possibly 0). */
        kbd_reply(token, (i32)n, resp->data, n);
        return;
    }

    /* Blocking read with an empty ring: park the call for the IRQ
     * thread.  Publish s_pending_max BEFORE s_pending_token. */
    s_pending_max = max;
    s_pending_resp_len = 0;
    s_pending_token = token;

    /* Re-check once: a drain may have pushed bytes between the first
     * read and the publish.  If so, serve them directly. */
    n = kbd_rx_read(resp->data, max);
    if (n > 0) {
        s_pending_token = -1;
        kbd_reply(token, (i32)n, resp->data, n);
    }
    /* else: the IRQ thread completes this call when keys arrive. */
}

static void kbd_handle_request(int token)
{
    kbd_req_t *req = (kbd_req_t *)s_req_buf;

    if (req->op == KBD_OP_READ || req->op == KBD_OP_READ_BLOCK) {
        if (req->len > KBD_MAX_DATA) {
            kbd_reply(token, ERR_INVAL, NULL, 0);
            return;
        }
        kbd_reply_read(token, req->len, req->op == KBD_OP_READ_BLOCK);
    } else {
        kbd_reply(token, ERR_INVAL, NULL, 0);
    }
}

static void kbd_server_loop(int port)
{
    for (;;) {
        int msg_len = (int)sizeof(s_req_buf);
        int token = 0;
        int ret = ipc_recv(port, s_req_buf, &msg_len, &token);
        if (ret < 0) {
            printf("keyboard: ipc_recv failed (%d)\n", ret);
            thread_exit(1);
        }
        if (msg_len < (int)KBD_REQ_HDR) {
            kbd_reply(token, ERR_INVAL, NULL, 0);
            continue;
        }
        kbd_handle_request(token);
    }
}

/* ====================================================================
 * IRQ thread (spawned by kbd_service_main)
 * ==================================================================== */

static void kbd_irq_main(void *arg)
{
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
         * its response buffer. */
        if (s_pending_token >= 0 && s_pending_resp_len > 0) {
            int tok = s_pending_token;
            u32 n = s_pending_resp_len;
            s_pending_token = -1;
            s_pending_resp_len = 0;
            kbd_resp_t *parked = (kbd_resp_t *)s_parked_buf;
            parked->ret = (i32)n;   /* header must carry the byte count */
            int r = ipc_reply(tok, s_parked_buf, (int)(KBD_RESP_HDR + n));
            if (r < 0)
                printf("keyboard: parked-read ipc_reply failed (%d)\n", r);
        }
    }
}

/* ====================================================================
 * Entry point (keyboard process main)
 * ==================================================================== */

static void kbd_service_main(void *arg)
{
    (void)arg;

    printf("keyboard: starting PS/2 driver service\n");

    /* 1. I/O-port capability: covers 0x60..0x64 (data + status). */
    int io_cap = cap_create_obj(CAP_TYPE_IO_PORT, RIGHT_ALL,
                                (5 << 16) | KBD_DATA_PORT);
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

int main(void)
{
    kbd_service_main(NULL);
    return 0;   /* unreachable */
}
