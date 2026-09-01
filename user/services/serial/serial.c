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
 * serial.c - Userspace serial (COM1) driver service
 * Copyright (c) 2026 OpSys Project
 *
 * Ring-3 driver for the 16550 UART on COM1.  The kernel provides
 * IRQ binding, async notification and I/O-port syscalls (P0-A);
 * this service owns the hardware and serves COM1 reads/writes to
 * clients over a registered IPC port.  Architecture:
 *
 *   manager process (spawned by init via SYS_PROCESS_CREATE)
 *     └─ SYS_PROCESS_CREATE("serial")                 serial process
 *          └─ main() = SerialServiceMain()          server thread
 *               ├─ CapCreate(CAP_TYPE_IO_PORT, RIGHT_ALL)  private to
 *               │                                           this process
 *               ├─ IpcPortCreate() + PortRegister("serial")
 *               ├─ ThreadCreate(serial_irq_main)     IRQ thread
 *               │    ├─ CapCreate(CAP_TYPE_IRQ, RIGHT_READ)
 *               │    ├─ BindIrq(irq_cap, 4, mask)    binds the calling
 *               │    │                                thread
 *               │    └─ loop: WaitNotification(SERIAL_IRQ_MASK) blocking,
 *               │         on notification: drain the 16550 RX FIFO into the
 *               │         ring buffer (io_read8) — promptly: the FIFO is
 *               │         only 16 bytes deep (14-byte trigger).
 *               └─ loop: IpcRecv(port, req, &len) -> WRITE (io_write8) /
 *                             READ (copy ring) -> IpcReply(port, resp, len)
 *
 * serial runs as its OWN process (spawned by the manager via
 * SYS_PROCESS_CREATE), so it has a private address space and a private
 * capability table.  process_current() therefore resolves to the
 * serial process — the capability-gated syscalls this driver needs
 * (CapCreate, BindIrq, io_read8/io_write8) gate on the serial
 * process's own caps, created here in main().  The IRQ thread is
 * spawned via sys_thread_create() inside the same process, so it
 * shares that address space and capability table.  BindIrq()
 * attaches IRQ4 to the calling thread and irq_enable(4) also sets the
 * 16550 IER RX bit, so received bytes assert IRQ4 and deliver
 * notification bit SERIAL_IRQ_MASK to the IRQ thread.
 *
 * RX ring buffer: a plain head/tail circular buffer.  v0.1 single-CPU
 * model — NO locks, NO atomics, matching kernel conventions.  The IRQ
 * thread is the only writer and the server thread the only reader
 * (strict SPSC), so the head/tail discipline keeps the bounds exact:
 * the data store always precedes the tail update in program order, so
 * the reader can never observe an unwritten byte.
 *
 * IPC protocol (flat structs, raw copy, native little-endian):
 *   Request:  { u32 op; u32 len; u8 data[]; }
 *     op 1 = WRITE    (data = bytes to transmit, len = byte count)
 *     op 2 = READ     (len  = max bytes to read,   data unused)
 *   Response: { i32 ret; u8 data[]; }
 *     ret >= 0 : bytes processed (WRITE: transmitted, READ: copied)
 *     ret <  0 : negative error code (ERR_INVAL / ERR_NOCAP / ERR_FAULT)
 * READ (op 2) never blocks: it copies whatever is in the ring (0 if
 * empty) — kept for bounded-poll clients (e.g. the manager's serial
 * self-test).
 * READ_BLOCK (op 3) blocks until at least one byte is available: when
 * the ring is empty the server parks the call in a single pending slot
 * and the IRQ thread completes it (ipc_reply) as soon as its FIFO drain
 * pushes bytes.  The caller (e.g. the shell) therefore parks inside
 * ipc_call until RX data arrives — no polling.
 *
 * ------------------------------------------------------------------
 * Structure (driver service): one process, two threads —
 *   SerialServiceMain() serves the "serial" port (WRITE / READ /
 *   READ_BLOCK) while the IRQ thread owns IRQ4 and drains the 16550
 *   RX FIFO; a 256-byte SPSC ring plus one READ_BLOCK park slot link
 *   the two threads.
 * How it works:
 *   The IRQ thread BindIrq()s IRQ4, waits on SERIAL_IRQ_MASK and on
 *   notification pushes FIFO bytes into the ring (also completing a
 *   parked read); SerialServiceMain writes via io_write8 (bounded LSR
 *   poll), copies ring bytes for READ and parks empty READ_BLOCK calls.
 * Purpose:
 *   Userspace COM1 (16550 UART) driver — serial console I/O for
 *   clients such as the shell and the manager's self-test.
 * Caveats:
 *   v0.1 single-CPU model: no locks, no atomics — strict SPSC; TX
 *   spins at most SERIAL_TX_SPIN_LIMIT so a stuck UART never hangs
 *   the service; READ (op 2) never blocks.
 * ------------------------------------------------------------------
 */

#include "../lib/libc/stdio.h"
#include "../lib/libos/syscalls.h"
#include "../lib/libc/string.h"
#include <stdint.h>

/* Fixed-width types.  kernel/types.h is not includable from user space:
 * its error_t enum collides with the OK/ERR_* macros in syscalls.h. */
typedef uint8_t  u8;
typedef uint32_t u32;
typedef int32_t  i32;

/* ====================================================================
 * Constants
 * ==================================================================== */

/* 16550 COM1 register map */
#define SERIAL_COM1_BASE 0x3F8
#define SERIAL_THR       (SERIAL_COM1_BASE + 0) /* Transmit holding register */
#define SERIAL_RBR       (SERIAL_COM1_BASE + 0) /* Receive buffer register  */
#define SERIAL_IER       (SERIAL_COM1_BASE + 1) /* Interrupt enable register */
#define SERIAL_LSR       (SERIAL_COM1_BASE + 5) /* Line status register      */

/* Line status register bits */
#define LSR_RX_READY (1 << 0)
#define LSR_TX_EMPTY (1 << 5)

/* IRQ4 = COM1 (PIC line, bound by the IRQ thread) */
#define SERIAL_IRQ      4
#define SERIAL_IRQ_MASK 1u

/* Protocol limits */
#define SERIAL_OP_WRITE      1
#define SERIAL_OP_READ       2
#define SERIAL_OP_READ_BLOCK 3
#define SERIAL_MAX_DATA      256    /* max payload bytes per request   */
#define SERIAL_RX_RING_SIZE  256    /* ring buffer (holds 255 bytes)   */
#define SERIAL_TX_SPIN_LIMIT 100000 /* bounded LSR poll before giving  */
                                    /* up — a stuck UART must never    */
                                    /* hang the service forever       */

/* ====================================================================
 * Protocol structures (flat, raw copy — see header comment)
 * ==================================================================== */

typedef struct {
    u32 op;
    u32 len;
    u8  data[]; /* payload (WRITE / LOOPBACK) */
} serial_req_t;

typedef struct {
    i32 ret;
    u8  data[]; /* payload (READ) */
} serial_resp_t;

#define SERIAL_REQ_HDR  ((u32)sizeof(serial_req_t))
#define SERIAL_RESP_HDR ((u32)sizeof(serial_resp_t))

/* ====================================================================
 * Service state
 *
 * All threads of this process share one address space and one capability
 * table, so plain static globals are the natural shared state here.
 * The ring buffer is SPSC: the IRQ thread writes (pushes), the server
 * thread reads (pops) — see the header comment.
 * ==================================================================== */

static u8  s_rx_buf[SERIAL_RX_RING_SIZE];
static u32 s_rx_head; /* next byte to read  (server thread) */
static u32 s_rx_tail; /* next free slot     (IRQ thread)    */
static u8  s_req_buf[SERIAL_REQ_HDR + SERIAL_MAX_DATA];
static u8  s_resp_buf[SERIAL_RESP_HDR + SERIAL_MAX_DATA];
static u8  s_parked_buf[SERIAL_RESP_HDR + SERIAL_MAX_DATA];

/*
 * Pending blocking-READ slot.  The server thread parks a READ_BLOCK
 * call here when the ring is empty; the IRQ thread completes it via
 * IpcReply() as soon as its FIFO drain receives bytes.  Single slot:
 * if a READ_BLOCK arrives while one is already parked, the new one is
 * served non-blocking (whatever the ring holds, possibly 0) — the
 * shell is the only blocking client, so the slot is never contended
 * in practice.  Written by the server, read+cleared by the IRQ thread:
 * s_pending_token == -1 means no call parked.
 *
 * s_parked_buf / s_pending_resp_len: the parked reply is assembled in
 * a DEDICATED buffer — the server thread keeps using s_resp_buf for
 * every other reply, and the IRQ thread routes drained RX bytes into
 * the parked buffer, so sharing one buffer would clobber concurrent
 * WRITE replies.  The ring stays strictly SPSC (IRQ thread the only
 * writer, server the only reader); parked bytes bypass the ring.
 */
static i32 s_pending_token    = -1;
static u32 s_pending_max      = 0;
static u32 s_pending_resp_len = 0; /* bytes routed to s_parked_buf */

/* ====================================================================
 * RX ring buffer
 * ==================================================================== */

/*
 * Push one received byte into the RX ring buffer (writer: IRQ thread).
 * Drops the byte when the ring is full — COM1 has no flow control in
 * v0.1 and the IRQ thread must never block.  The data store always
 * happens before the tail update in program order on the single CPU,
 * so the reader can never observe an unwritten byte.
 */
static void SerialRxPush(u8 c) {
    u32 next = (s_rx_tail + 1) % SERIAL_RX_RING_SIZE;
    if (next == s_rx_head)
        return; /* ring full: drop the byte */
    s_rx_buf[s_rx_tail] = c;
    s_rx_tail           = next;
}

/*
 * Copy up to max bytes out of the RX ring buffer (reader: server thread).
 * Returns the number of bytes copied (0 when the buffer is empty).
 * Head is advanced only after each byte is consumed, so the writer can
 * only reuse a slot whose data was already handed to the caller.
 */
static u32 SerialRxRead(u8 *dst, u32 max) {
    u32 n = 0;
    while (n < max && s_rx_head != s_rx_tail) {
        dst[n]    = s_rx_buf[s_rx_head];
        s_rx_head = (s_rx_head + 1) % SERIAL_RX_RING_SIZE;
        n++;
    }
    return n;
}

/*
 * Drain the 16550 RX FIFO.
 * Called by the IRQ thread in response to IRQ4 notifications.
 *
 * When a blocking READ is parked (s_pending_token >= 0), drained bytes
 * are routed straight into s_parked_buf instead of the ring, so the
 * ring remains strictly SPSC (server thread the only reader) and the
 * IRQ thread never reads the ring.  Returns the number of bytes
 * routed to the parked reply (0 when no blocking read is parked or
 * its max was reached; overflow bytes fall back to the ring).
 */
static u32 SerialRxDrain(void) {
    u32 parked = 0;
    for (;;) {
        int lsr = IoRead8(SERIAL_LSR);
        if (lsr < 0)
            break; /* I/O error — cannot proceed */
        if (!(lsr & LSR_RX_READY))
            break; /* FIFO empty */
        int c = IoRead8(SERIAL_RBR);
        if (c < 0)
            break;

        if (s_pending_token >= 0 && parked < s_pending_max) {
            ((serial_resp_t *)s_parked_buf)->data[parked] = (u8)c;
            parked++;
        } else {
            SerialRxPush((u8)c);
        }
    }
    if (parked > 0)
        s_pending_resp_len = parked;
    return parked;
}

/* ====================================================================
 * COM1 TX path
 * ==================================================================== */

/* Transmit one byte to COM1 with LSR polling / bounded spin. */
static i32 SerialTxByte(u8 b) {
    int lsr = IoRead8(SERIAL_LSR);
    if (lsr < 0)
        return (i32)lsr;
    if (!(lsr & LSR_TX_EMPTY)) {
        u32 spins;
        for (spins = 0; spins < SERIAL_TX_SPIN_LIMIT; spins++) {
            lsr = IoRead8(SERIAL_LSR);
            if (lsr < 0)
                return (i32)lsr;
            if (lsr & LSR_TX_EMPTY)
                break;
        }
        if (!(lsr & LSR_TX_EMPTY))
            return (i32)ERR_BUSY;
    }
    return IoWrite8(SERIAL_THR, b);
}

/*
 * Transmit len bytes to COM1.  Polls LSR bit 5 (THR empty) before
 * writing each byte to THR.  Returns the number of bytes transmitted,
 * or a negative error code on failure (io syscall error, or ERR_BUSY
 * when the UART stays busy past the spin limit).
 *
 * LF is translated to CRLF: QEMU's vc terminal (and a real 16550
 * attached to a terminal emulator) does not auto-carriage-return on
 * LF alone, which causes staircase output.  CRLF is emitted at the
 * byte level so chunk boundaries in the client never split it.
 */
static i32 SerialTx(const u8 *data, u32 len) {
    u32 i;
    for (i = 0; i < len; i++) {
        if (data[i] == '\n') {
            i32 r = SerialTxByte('\r');
            if (r < 0)
                return r;
        }
        i32 r = SerialTxByte(data[i]);
        if (r < 0)
            return r;
    }
    return (i32)len;
}

/* ====================================================================
 * Server side
 * ==================================================================== */

/*
 * Reply to the client whose call token we are answering.
 * Sends the flat response { ret } (header only — READ appends data).
 */
static void SerialReply(int token, i32 ret) {
    serial_resp_t *resp = (serial_resp_t *)s_resp_buf;
    resp->ret           = ret;
    int r               = IpcReply(token, s_resp_buf, (int)SERIAL_RESP_HDR);
    if (r < 0)
        printf("serial: ipc_reply failed (%d)\n", r);
}

/*
 * Reply to a READ request.  When blocking is false this never blocks:
 * an empty ring just yields ret = 0 (bounded-poll clients).
 *
 * When blocking is true (READ_BLOCK) and the ring is empty, the call
 * is PARKED instead of answered: its token is recorded in
 * s_pending_token and the IRQ thread completes it with IpcReply()
 * as soon as its FIFO drain routes bytes into s_parked_buf.  The
 * caller therefore blocks inside ipc_call until RX data arrives —
 * no polling.  Only one blocking read can be parked; a second one is
 * answered immediately with whatever the ring holds (possibly 0).
 */
static void SerialReplyRead(int token, u32 max, int blocking) {
    serial_resp_t *resp = (serial_resp_t *)s_resp_buf;

    u32 n = SerialRxRead(resp->data, max);
    if (n > 0 || !blocking || s_pending_token >= 0) {
        /* Bytes available, non-blocking request, or the single pending
         * slot is already taken: serve what we have (possibly 0). */
        resp->ret = (i32)n;
        int r     = IpcReply(token, s_resp_buf, (int)(SERIAL_RESP_HDR + n));
        if (r < 0)
            printf("serial: ipc_reply failed (%d)\n", r);
        return;
    }

    /* Blocking read with an empty ring: park the call for the IRQ
     * thread.  Publish s_pending_max BEFORE s_pending_token so the IRQ
     * thread, which keys on token >= 0, can never observe a torn max. */
    s_pending_max      = max;
    s_pending_resp_len = 0;
    s_pending_token    = token;

    /* Re-check once: a FIFO drain may have pushed bytes into the ring
     * between the first read above and the publish.  If so, serve them
     * directly and clear the slot — the IRQ thread then finds nothing
     * parked and leaves the reply alone. */
    n = SerialRxRead(resp->data, max);
    if (n > 0) {
        s_pending_token = -1;
        resp->ret       = (i32)n;
        int r           = IpcReply(token, s_resp_buf, (int)(SERIAL_RESP_HDR + n));
        if (r < 0)
            printf("serial: ipc_reply failed (%d)\n", r);
    }
    /* else: the IRQ thread completes this call when RX bytes arrive. */
}

/*
 * Complete a parked blocking READ from the IRQ thread.
 * s_parked_buf holds up to s_pending_resp_len fresh RX bytes routed by
 * the FIFO drain; reply with them and clear the slot.  If the server
 * already answered the call directly (re-check raced ahead), ipc_reply
 * fails with ERR_BUSY/ERR_NOENT and is safely ignored — the kernel's
 * per-call replied flag guarantees at most one reply is delivered.
 */
static void SerialCompleteParkedRead(void) {
    serial_resp_t *parked = (serial_resp_t *)s_parked_buf;
    int            tok    = s_pending_token;
    u32            n      = s_pending_resp_len;
    s_pending_token       = -1;
    s_pending_resp_len    = 0;

    parked->ret = (i32)n;
    int r       = IpcReply(tok, s_parked_buf, (int)(SERIAL_RESP_HDR + n));
    if (r < 0)
        printf("serial: parked-read ipc_reply failed (%d)\n", r);
}

/*
 * Interpret one client request and reply.  Never crashes on malformed
 * input: the opcode is validated, the payload length is capped at
 * SERIAL_MAX_DATA, and every buffer access is bounded by the length
 * ipc_recv actually reported.
 */
static void SerialHandleRequest(int token, int msg_len) {
    if (msg_len > (int)sizeof(s_req_buf))
        msg_len = (int)sizeof(s_req_buf);

    /* Every request carries at least the 8-byte header */
    if (msg_len < (int)SERIAL_REQ_HDR) {
        SerialReply(token, ERR_INVAL);
        return;
    }

    serial_req_t *req = (serial_req_t *)s_req_buf;

    if (req->op == SERIAL_OP_WRITE) {
        if (req->len > SERIAL_MAX_DATA || msg_len < (int)(SERIAL_REQ_HDR + req->len)) {
            SerialReply(token, ERR_INVAL);
            return;
        }
        SerialReply(token, SerialTx(req->data, req->len));
    } else if (req->op == SERIAL_OP_READ || req->op == SERIAL_OP_READ_BLOCK) {
        if (req->len > SERIAL_MAX_DATA) {
            SerialReply(token, ERR_INVAL);
            return;
        }
        SerialReplyRead(token, req->len, req->op == SERIAL_OP_READ_BLOCK);
    } else {
        SerialReply(token, ERR_INVAL);
    }
}

/*
 * Main service loop: receive a request, serve it, reply, repeat.
 * Runs on the server thread (the port owner).  IRQ4 notifications and
 * RX draining belong to the IRQ thread, so no poll happens here.
 */
static void SerialServerLoop(int port) {
    for (;;) {
        int msg_len = (int)sizeof(s_req_buf);
        int token   = 0;
        int ret     = IpcRecv(port, s_req_buf, &msg_len, &token);
        if (ret < 0) {
            printf("serial: ipc_recv failed (%d)\n", ret);
            ThreadExit(1);
        }

        SerialHandleRequest(token, msg_len);
    }
}

/* ====================================================================
 * IRQ thread (spawned by serial_service_main)
 * ==================================================================== */

/*
 * COM1 RX owner.  Binds IRQ4 to THIS thread — BindIrq() attaches the
 * calling thread, unmasks the PIC line and sets the 16550 IER RX bit,
 * so every received byte asserts IRQ4 and delivers notification bit
 * SERIAL_IRQ_MASK to us.  While the binding is active the kernel's
 * legacy unbound-IRQ4 FIFO drain is skipped, so this thread is the
 * sole reader of the COM1 RX path.
 *
 * The 16550 RX FIFO is only 16 bytes deep (14-byte trigger), so every
 * notification is drained promptly into the ring buffer.  The drain
 * never blocks; the ring is SPSC with the server thread as reader.
 */
static void SerialIrqMain(void *arg) {
    (void)arg;

    printf("serial: IRQ thread started\n");

    /* IRQ capability names the exact line (obj_id == SERIAL_IRQ) so the
     * kernel's bind_irq obj_id check passes. */
    int irq_cap = CapCreateObj(CAP_TYPE_IRQ, RIGHT_READ, SERIAL_IRQ);
    if (irq_cap < 0) {
        printf("serial: CapCreate(IRQ) failed (%d)\n", irq_cap);
        ThreadExit(1);
    }

    int ret = BindIrq(irq_cap, SERIAL_IRQ, SERIAL_IRQ_MASK);
    if (ret < 0) {
        printf("serial: BindIrq(%d) failed (%d)\n", SERIAL_IRQ, ret);
        ThreadExit(1);
    }
    printf("serial: IRQ4 bound, RX FIFO drained on notification\n");

    for (;;) {
        /* Block until IRQ4 is pending, then drain the FIFO promptly. */
        WaitNotification(SERIAL_IRQ_MASK);
        SerialRxDrain();

        /* If a blocking READ is parked and the drain routed fresh bytes
         * into its response buffer, complete the call right here.  The
         * drain already copied the data into s_parked_buf, so the ring
         * is never read from this thread (it stays strictly SPSC). */
        if (s_pending_token >= 0 && s_pending_resp_len > 0)
            SerialCompleteParkedRead();
    }
}

/* ====================================================================
 * Entry point (serial process main)
 * ==================================================================== */

/*
 * Server thread entry point.  Creates the process's I/O-port capability
 * (gates io_read8/io_write8 for both service threads), registers the
 * "serial" IPC port, spawns the IRQ thread, then serves clients on the
 * port forever.  Never returns.
 */
static void SerialServiceMain(void *arg) {
    (void)arg;

    printf("serial: starting COM1 driver service\n");

    /* 1. I/O-port capability.  Process-level: gates io_read8/io_write8
     *    for BOTH service threads — created here, before the IRQ thread
     *    is spawned, so it can never race on it.
     *    obj_id encodes the COM1 port range: (count << 16) | base_port,
     *    covering 0x3F8..0x3FF (THR/RBR, IER, FCR/IIR, LCR, MCR, LSR). */
    int io_cap = CapCreateObj(CAP_TYPE_IO_PORT, RIGHT_ALL, (8 << 16) | SERIAL_COM1_BASE);
    if (io_cap < 0) {
        printf("serial: CapCreate(IO_PORT) failed (%d)\n", io_cap);
        ThreadExit(1);
    }
    printf("serial: caps OK (io_port=%d)\n", io_cap);

    /* 2. IPC port, registered under the well-known name "serial". */
    int port = IpcPortCreate();
    if (port < 0) {
        printf("serial: ipc_port_create failed (%d)\n", port);
        ThreadExit(1);
    }
    int ret = PortRegister("serial", port);
    if (ret < 0) {
        printf("serial: PortRegister('serial') failed (%d)\n", ret);
        ThreadExit(1);
    }
    printf("serial: port %d registered as 'serial'\n", port);

    /* 3. Spawn the IRQ thread.  It creates the IRQ capability, binds
     *    IRQ4 to itself and drains the RX FIFO on notification. */
    int irq_tid = ThreadCreate(SerialIrqMain, NULL, 10);
    if (irq_tid < 0) {
        printf("serial: ThreadCreate(IRQ thread) failed (%d)\n", irq_tid);
        ThreadExit(1);
    }
    printf("serial: IRQ thread TID=%d\n", irq_tid);

    /* 4. Serve clients. */
    printf("serial: serving on port %d\n", port);
    SerialServerLoop(port);
}

/* ====================================================================
 * Process entry point (crt0 calls main())
 * ==================================================================== */

/*
 * The serial process's main thread.  Defers to SerialServiceMain(),
 * which runs the server loop forever; this function never returns.
 */
int main(void) {
    SerialServiceMain(NULL);
    return 0; /* unreachable */
}
