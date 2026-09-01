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
 * main.c - PCnet-Fast III (AMD AM79C973) Ethernet driver + net service
 * Copyright (c) 2026 OpSys Project
 *
 * The driver owns the PCnet adapter (PCI vendor 0x1022 / device
 * 0x2000), programmed over its IO BAR (RAP/RDP registers) with DMA
 * rings + buffers allocated from a contiguous physical-page pool
 * (ShmCreate, gated on ATOM_SERVICE_MANAGE — the net service is in
 * the kernel blob-identity seed list).  Received frames are drained
 * by polling the Rx ring (QEMU's pcnet INTx path is left disabled:
 * the 8259 is level-sensitive and an unmasked shared line storms the
 * CPU); the "net" port serves GET_MAC / SEND / RECV / STATS.
 *
 * SWSTYLE 2 (16-byte descriptors, SSIZE32 implied), 8 Rx + 8 Tx
 * descriptors, 2 KiB per-buffer frames (fits any 1514-byte frame).
 *
 * Pool layout (physical = s_pool_phys + offset):
 *   0x0000  InitBlock      (4 KiB-aligned by construction)
 *   0x0100  Rx ring        (8 x 16 B)
 *   0x0200  Tx ring        (8 x 16 B)
 *   0x0400  Rx buffers     (8 x 2048 B)
 *   0x4400  Tx buffers     (8 x 2048 B)
 *   total   0x8400 -> 10 pages
 *
 * ------------------------------------------------------------------
 * Structure (driver + stack): one process owns the PCnet adapter
 *   (RAP/RDP over the IO BAR) and a 10-page contiguous DMA pool
 *   (InitBlock, 8 Rx + 8 Tx rings, 2 KiB buffers); the protocol stack
 *   lives in proto.c (ARP/IP/ICMP/UDP/TCP) fed by the frame queue;
 *   the "net" port serves GET_MAC / SEND / RECV / STATS.
 * How it works:
 *   NetServerLoop() programs the adapter, polls the Rx ring (INTx is
 *   disabled), feeds every frame to ProtoRx() (ARP cache learning,
 *   ICMP echo, UDP/TCP demux) and sends via NetSendRaw(); client
 *   requests are served on the "net" port in the same loop.
 * Purpose:
 *   Network protocol stack service — raw Ethernet plus ARP/IP/ICMP/
 *   UDP/TCP endpoints for user clients over IPC.
 * Caveats:
 *   Rx is polled, not interrupt-driven (QEMU pcnet INTx storms the
 *   level-sensitive 8259); single-service-thread model; the ShmCreate
 *   pool is gated on ATOM_SERVICE_MANAGE.
 * ------------------------------------------------------------------
 */

#include "net.h"
#include "proto.h"

#include <libc/stdio.h>
#include <libc/stdlib.h> /* getenv */
#include <libc/string.h>
#include <libos/syscalls.h>

/* ---- PCI identities ---- */
#define PCNET_VENDOR 0x1022
#define PCNET_DEVICE 0x2000

/* ---- IO registers (BAR0, QEMU default base 0x10800) ----
 * QEMU's pcnet uses "AMD" offsets but dispatches on (addr & 0x0f):
 *   +0x10 -> RDP (addr&0xf == 0), +0x12 -> RAP (== 2),
 *   +0x14 -> RESET (== 4, read resets the chip!), +0x16 -> BCR (== 6).
 * Registers 0x00..0x0f of the BAR are the APROM (MAC).
 * (AMD hardware uses RDP=0x10/RAP=0x14, but QEMU is the target.) */
#define PCNET_RDP  0x10 /* register data port */
#define PCNET_RAP  0x12 /* register address port */
#define PCNET_BCR  0x16 /* bus control register port */

/* ---- CSR register numbers ---- */
#define CSR0  0
#define CSR4  4

/* CSR0 bits — QEMU's PCnet model (NOT the AMD bit layout):
 *   bit0 INIT, bit1 STRT, bit2 STOP, bit3 TDMD, bit6 INEA,
 *   bit8 IDON, bit9 TINT, bit10 RINT, bit11 MERR, bit12 INTR.
 * Writes clear interrupt flags (bits 8-14); INIT/STRT/STOP/TDMD
 * writes trigger the corresponding action. */
#define CSR0_INIT  0x0001
#define CSR0_STRT  0x0002
#define CSR0_STOP  0x0004
#define CSR0_TDMD  0x0008
#define CSR0_INEA  0x0040
#define CSR0_IDON  0x0100
#define CSR0_TINT  0x0200
#define CSR0_RINT  0x0400
#define CSR0_MERR  0x0800
#define CSR0_INTR  0x1000

#define BCR20 20 /* SWSTYLE: 2 = 16-byte descriptors (QEMU non-zero path) */

/* Descriptor flags (QEMU pcnet_TMD/pcnet_RMD status) */
#define DESC_OWN 0x8000
#define DESC_STP 0x0200
#define DESC_ENP 0x0100

/* ---- DMA ring geometry ---- */
#define NET_RINGS      8
#define NET_RING_LOG   3 /* Log2(8) -> encoded in InitBlock rlen/tlen */
#define NET_BUF_SIZE   2048
#define NET_OFF_INIT   0x0000
#define NET_OFF_RXRING 0x0100
#define NET_OFF_TXRING 0x0200
#define NET_OFF_RXBUF  0x0400
#define NET_OFF_TXBUF  0x4400
#define NET_POOL_SIZE  0x8400 /* 33 KiB -> 10 pages */

#define NET_RXQ_DEPTH 32

/* 16-byte PCnet descriptor (SWSTYLE 0, layout matches QEMU):
 *   +0  rbadr  buffer address
 *   +4  bcnt   buffer count (Tx: frame length; Rx: buffer size)
 *   +6  status bit15 OWN (1 = NIC owns)
 *   +8  mcnt   message count (Rx: received frame length)
 *   +12 reserved */
typedef struct {
    volatile u32 addr;
    volatile u16 len;    /* bcnt */
    volatile u16 status; /* OWN + flags */
    volatile u32 mcnt;
    volatile u32 reserved;
} __attribute__((packed)) pcnet_desc_t;

_Static_assert(sizeof(pcnet_desc_t) == 16, "pcnet desc must be 16 bytes");

/* PCnet InitBlock — SSIZE32 layout (QEMU pcnet_initblk32, 28 bytes):
 *   +0  mode (u16)         +2  rlen (u8, Log2(ring) << 4)
 *   +3  tlen (u8, same)    +4  phys_addr[6] (MAC, byte order)
 *   +10 reserved (u16)     +12 logical_addr[8] (LADRF, zero)
 *   +20 rdra (u32 phys)    +24 tdra (u32 phys)
 * SSIZE32 is implied by BCR20=2 (QEMU encodes 0x0302), so the
 * 28-byte layout is mandatory (the 16-byte AMD layout is only used
 * when SSIZE32 is clear). */
typedef struct {
    u16 mode;
    u8  rlen; /* ring length code << 4 */
    u8  tlen;
    u8  phys_addr[6];
    u16 reserved;
    u8  logical_addr[8];
    u32 rdra;
    u32 tdra;
} __attribute__((packed)) pcnet_init_t;

_Static_assert(sizeof(pcnet_init_t) == 28, "initblk32 must be 28 bytes");

static int  s_io_base; /* BAR0 IO base */
static int  s_irq;     /* PCI IRQ line */
static u8   s_mac[6];

static u64  s_pool_phys;
static u8  *s_pool_va; /* shm mapping */

static pcnet_init_t *s_init;   /* = s_pool_va + NET_OFF_INIT */
static pcnet_desc_t *s_rx_ring; /* Rx descriptors */
static pcnet_desc_t *s_tx_ring; /* Tx descriptors */

static int s_lock = -1; /* mutex: rx queue + tx ring */

/* Received-frame queue (IRQ thread enqueues, RECV drains). */
static u8  s_rxq[NET_RXQ_DEPTH][NET_MTU];
static u32 s_rxq_len[NET_RXQ_DEPTH];
static u32 s_rxq_head;
static u32 s_rxq_count;

static u32 s_stat_rx;
static u32 s_stat_tx;
static u32 s_stat_err;

/* IPC buffers (single server thread) */
static u8 s_req[4096];
static u8 s_resp[4096];

/* ---- IO register access ---- */

static void CsrWrite(u16 reg, u16 val) {
    int r1 = IoWrite16((u16)(s_io_base + PCNET_RAP), reg);
    int r2 = IoWrite16((u16)(s_io_base + PCNET_RDP), val);
    if (r1 < 0 || r2 < 0)
        printf("net: CsrWrite(%u,%04x) io err %d/%d\n", reg, val, r1, r2);
}

static u16 CsrRead(u16 reg) {
    IoWrite16((u16)(s_io_base + PCNET_RAP), reg);
    return (u16)IoRead16((u16)(s_io_base + PCNET_RDP));
}

static void BcrWrite(u16 reg, u16 val) {
    /* QEMU: IO+0x16 writes bcr[RAP]; BCR20 (SWSTYLE) values are
     * auto-encoded: 2 -> 0x0302 => SWSTYLE=2 AND SSIZE32=1 (16-byte
     * descriptors + flat 32-bit DMA addressing). */
    IoWrite16((u16)(s_io_base + PCNET_RAP), reg);
    IoWrite16((u16)(s_io_base + PCNET_BCR), val);
}

/* ---- init ---- */

static void PcnetReset(void) {
    CsrWrite(0, CSR0_STOP); /* stop (QEMU: bit2) */
    BcrWrite(BCR20, 0x0002); /* SWSTYLE 2: QEMU walks 16-byte descriptors */
    CsrWrite(CSR4, 0x0915);  /* QEMU-compatible default config */
}

/* Read the 6-byte MAC from the APROM (IO + 0x00..0x0B). */
static void PcnetReadMac(void) {
    for (int i = 0; i < 6; i++)
        s_mac[i] = (u8)IoRead8((u16)(s_io_base + i));
}

static void PcnetInitRings(void) {
    memset(s_pool_va, 0, NET_POOL_SIZE);

    /* InitBlock: mode 0 (normal), MAC, ring pointers + lengths.
     * rlen/tlen hold Log2(ring) << 4 (QEMU: rlen = byte >> 4). */
    s_init->mode = 0x0000;
    s_init->rlen = (u8)(NET_RING_LOG << 4);
    s_init->tlen = (u8)(NET_RING_LOG << 4);
    memcpy(s_init->phys_addr, s_mac, 6);
    s_init->rdra = (u32)(s_pool_phys + NET_OFF_RXRING);
    s_init->tdra = (u32)(s_pool_phys + NET_OFF_TXRING);

    /* Rx descriptors: point at the Rx buffers, handed to the NIC.
     * AMD OWN bit semantics: OWN=1 means the NIC owns the descriptor
     * (may fill it); after a packet lands the NIC clears OWN so the
     * driver can read it.  Initializing to OWN=1 hands the rings over.
     * BCNT = 4096 - buf_size; bits 12-15 must read 0xF (QEMU's
     * CHECK_RMD validates the descriptor's ONES/ZEROS nibbles). */
    for (int i = 0; i < NET_RINGS; i++) {
        s_rx_ring[i].addr   = (u32)(s_pool_phys + NET_OFF_RXBUF + i * NET_BUF_SIZE);
        s_rx_ring[i].len    = (u16)((4096 - NET_BUF_SIZE) | 0xF000); /* BCNT + ONES */
        s_rx_ring[i].mcnt   = 0;
        s_rx_ring[i].status = DESC_OWN; /* OWN=1: NIC may fill */
    }
}

static int PcnetStart(void) {
    /* Load the InitBlock address into CSR1/2 and issue INIT. */
    u32 ip = (u32)(s_pool_phys + NET_OFF_INIT);
    CsrWrite(1, (u16)(ip & 0xFFFF));
    CsrWrite(2, (u16)(ip >> 16));
    CsrWrite(0, CSR0_INIT);

    /* Wait for IDON (init done); fail loudly if the chip never
     * acknowledges (bad InitBlock address / layout / port map). */
    int idon = 0;
    for (int i = 0; i < 1000000; i++) {
        u16 c = CsrRead(0);
        if (c & CSR0_IDON) {
            CsrWrite(0, CSR0_IDON); /* clear */
            idon = 1;
            break;
        }
        if (c & CSR0_MERR) {
            printf("net: init error csr0=%04x\n", c);
            return -7; /* ERR_FAULT */
        }
    }
    if (!idon) {
        printf("net: INIT timeout (csr0=%04x) — aborting\n", CsrRead(0));
        return -7; /* ERR_FAULT */
    }

    CsrWrite(0, CSR0_STRT); /* start Rx + Tx */
    /* NOTE: no INEA / IRQ binding — Rx is delivered by polling the
     * ring (QEMU's pcnet INTx path is unreliable and a latched RINT
     * storms the level-sensitive 8259).  The IRQ line stays masked by
     * the kernel's boot-time PIC mask, so nothing asserts here. */
    return 0;
}

/* ---- Tx ---- */

/* ---- protocol-stack glue (implementations for proto.c) ---- */

static int NetSend(const u8 *data, u32 len); /* fwd */
static void NetServiceRx(void);             /* fwd */

/* Send a raw Ethernet frame (the protocol stack's egress path). */
int NetSendRaw(const u8 *frame, u32 len) {
    return NetSend(frame, len);
}

void NetGetMac(u8 mac[6]) {
    memcpy(mac, s_mac, 6);
}

/* Pull one received frame out of the driver queue (the protocol stack
 * consumes these).  Returns 1 and fills *out when a frame is pending. */
int NetRxPump(u8 *out, u32 *out_len) {
    if (s_lock >= 0)
        (void)MutexLock(s_lock);
    int have = 0;
    if (s_rxq_count > 0) {
        u32 slot = s_rxq_head;
        u32 n    = s_rxq_len[slot];
        if (n > NET_MTU)
            n = NET_MTU;
        memcpy(out, s_rxq[slot], n);
        *out_len = n;
        s_rxq_head = (s_rxq_head + 1) % NET_RXQ_DEPTH;
        s_rxq_count--;
        have = 1;
    }
    if (s_lock >= 0)
        (void)MutexUnlock(s_lock);
    return have;
}

/* Drain the NIC ring into the driver queue (used by the protocol
 * stack while it waits for ARP/ICMP replies). */
void NetRxPumpNow(void) {
    NetServiceRx();
}

/* Yield one scheduling tick while the stack waits for replies. */
void NetYield(void) {
    (void)Sleep(1); /* 10 ms */
}

static int NetSend(const u8 *data, u32 len) {
    if (!data || len == 0 || len > NET_MTU)
        return -2; /* ERR_INVAL */
    if (s_lock >= 0)
        (void)MutexLock(s_lock);

    /* QEMU's transmitter consumes descriptors strictly in XMTRC order
     * (csr[74]): the next frame must go into slot (XMTRL - XMTRC).
     * Picking "the first free OWN=0 slot" desynchronises from that
     * pointer on the second packet (the NIC reads a different slot),
     * so the transmit demand never fires.  Read XMTRC, target its
     * slot, and wait for that slot to drain before refilling it. */
    u16 xmtrc = CsrRead(74); /* CSR_XMTRC: 1..XMTRL */
    int slot  = (NET_RINGS - (int)xmtrc) % NET_RINGS;
    if (slot < 0)
        slot += NET_RINGS;
    pcnet_desc_t *t = &s_tx_ring[slot];

    /* Wait for the NIC to consume any previous frame in this slot. */
    {
        int spins = 0;
        while ((t->status & DESC_OWN) && spins < 1000000)
            spins++;
        if (spins >= 1000000) {
            t->status = 0;
            t->len    = (u16)((4096 - NET_BUF_SIZE) | 0xF000);
            t->mcnt   = 0;
            s_stat_err++;
            if (s_lock >= 0)
                (void)MutexUnlock(s_lock);
            return -7; /* ERR_FAULT: ring stuck */
        }
    }

    u8 *dst = s_pool_va + NET_OFF_TXBUF + slot * NET_BUF_SIZE;
    memcpy(dst, data, len);
    /* IEEE 802.3 minimum frame: pad short frames (e.g. a 42-byte
     * ARP request) to 60 bytes, or real hardware/switches drop
     * them as runts. */
    if (len < 60) {
        memset(dst + len, 0, 60 - len);
        len = 60;
    }
    t->addr   = (u32)(s_pool_phys + NET_OFF_TXBUF + slot * NET_BUF_SIZE);
    t->len    = (u16)((4096 - len) | 0xF000); /* BCNT + ONES nibble */
    t->mcnt   = 0;
    t->status = DESC_OWN | DESC_STP | DESC_ENP;
    CsrWrite(0, CSR0_TDMD); /* transmit demand */
    /* Wait for the NIC to consume the descriptor (OWN cleared).
     * Confirms the frame was actually transmitted. */
    {
        int spins = 0;
        while ((t->status & DESC_OWN) && spins < 1000000)
            spins++;
        if (spins >= 1000000) {
            /* Timeout: the NIC never consumed the descriptor.  Reset
             * the slot so it does not leak (an OWN=1 TMD would make the
             * ring permanently short), then report the fault. */
            printf("net: TX timeout len=%d c0=%04x tstat=%04x\n", len,
                   CsrRead(0), (u16)t->status);
            t->status = 0;
            t->len    = (u16)((4096 - NET_BUF_SIZE) | 0xF000);
            t->mcnt   = 0;
            s_stat_err++;
            if (s_lock >= 0)
                (void)MutexUnlock(s_lock);
            return -7; /* ERR_FAULT: NIC never transmitted */
        }
    }
    s_stat_tx++;
    if (s_lock >= 0)
        (void)MutexUnlock(s_lock);
    return 0;
}

/* ---- Rx (IRQ thread) ---- */

static void NetServiceRx(void) {
    for (int i = 0; i < NET_RINGS; i++) {
        pcnet_desc_t *r = &s_rx_ring[i];
        if (r->status & DESC_OWN)
            continue; /* NIC still owns it: no packet yet */
        /* Deliver only complete single-descriptor frames: STP+ENP set
         * and no error bits (ERR|FRAM|OFLO|CRC|BUFF = 0x7C00).
         * Anything else is a dropped/oversized frame — count + recycle. */
        u16 st = r->status;
        if ((st & (DESC_STP | DESC_ENP)) != (DESC_STP | DESC_ENP) ||
            (st & 0x7C00)) {
            s_stat_err++;
            r->status = DESC_OWN; /* recycle */
            continue;
        }
        u32 mcnt = r->mcnt; /* u32: check before truncating */
        if (mcnt == 0 || mcnt > NET_MTU) {
            s_stat_err++;
            r->status = DESC_OWN; /* recycle */
            continue;
        }
        u16 len = (u16)mcnt;
        const u8 *src = s_pool_va + NET_OFF_RXBUF + i * NET_BUF_SIZE;
        if (s_lock >= 0)
            (void)MutexLock(s_lock);
        if (s_rxq_count < NET_RXQ_DEPTH) {
            u32 slot = (s_rxq_head + s_rxq_count) % NET_RXQ_DEPTH;
            memcpy(s_rxq[slot], src, len);
            s_rxq_len[slot] = len;
            s_rxq_count++;
            s_stat_rx++;
        } else {
            s_stat_err++;
        }
        if (s_lock >= 0)
            (void)MutexUnlock(s_lock);
        r->status = DESC_OWN; /* hand the descriptor back to the NIC */

    }

    /* Clear RINT: QEMU's 8259 is level-sensitive, so a latched RINT
     * keeps the INTx line high and storms the CPU (the line re-asserts
     * on every EOI).  Write-1-to-clear the flag now that the ring has
     * been drained. */
    CsrWrite(0, CSR0_RINT);
}

/* ---- IPC server ---- */

static void NetServerLoop(int port) {
    for (;;) {
        /* Poll the Rx ring first (the QEMU 10.2 pcnet INTx path is
         * unreliable; the IRQ thread is a bonus, not the delivery
         * mechanism).  Draining here keeps RECV latency low. */
        NetServiceRx();
        /* Feed every received frame into the protocol stack (ARP
         * cache learning/replies, ICMP echo, UDP queueing). */
        {
            u8 frame[1600];
            u32 flen;
            while (NetRxPump(frame, &flen))
                ProtoRx(frame, flen);
        }

        int msg_len = (int)sizeof(s_req);
        int token   = 0;
        int ret     = IpcRecv(port, s_req, &msg_len, &token);
        if (ret < 0) {
            printf("net: ipc_recv failed (%d)\n", ret);
            ThreadExit(1);
        }
        net_req_t  *req  = (net_req_t *)s_req;
        net_resp_t *resp = (net_resp_t *)s_resp;
        memset(resp, 0, sizeof(*resp));
        resp->ret = -2; /* ERR_INVAL */

        switch (req->op) {
        case NET_OP_GET_MAC:
            memcpy(resp->data, s_mac, 6);
            resp->ret = 0;
            resp->len = 6;
            break;
        case NET_OP_SEND:
            /* Reject a length that exceeds the actual message payload:
             * the request's data[] is bounded by msg_len - 8, and a
             * lying len would make net_send copy stale bytes. */
            if (req->len > 0 && req->len <= NET_MTU &&
                req->len <= (u32)(msg_len - 8)) {
                resp->ret = NetSend(req->data, req->len);
                resp->len = 0;
            }
            break;
        case NET_OP_RECV:
            if (s_lock >= 0)
                (void)MutexLock(s_lock);
            if (s_rxq_count > 0) {
                u32 slot = s_rxq_head;
                u32 n    = s_rxq_len[slot];
                if (n > NET_MTU)
                    n = NET_MTU;
                memcpy(resp->data, s_rxq[slot], n);
                resp->len = n;
                resp->ret = 0;
                s_rxq_head = (s_rxq_head + 1) % NET_RXQ_DEPTH;
                s_rxq_count--;
            } else {
                resp->ret = -6; /* ERR_AGAIN: nothing pending */
            }
            if (s_lock >= 0)
                (void)MutexUnlock(s_lock);
            break;
        case NET_OP_STATS:
            if (s_lock >= 0)
                (void)MutexLock(s_lock);
            {
                u32 *st = (u32 *)resp->data;
                st[0] = s_stat_rx;
                st[1] = s_stat_tx;
                st[2] = s_stat_err;
            }
            resp->len = 12;
            resp->ret = 0;
            if (s_lock >= 0)
                (void)MutexUnlock(s_lock);
            break;

        /* ---- L3/L4 protocol ops ---- */
        case NET_OP_SET_IP: {
            /* { ip[4]; gw[4] } — reconfigure the static address. */
            if (req->len >= 8) {
                ProtoInit(req->data, req->data + 4);
                resp->ret = 0;
            }
            break;
        }
        case NET_OP_IP_SEND: {
            /* { ip[4]; proto; payload } */
            if (req->len >= 5) {
                u8 dst[4];
                memcpy(dst, req->data, 4);
                resp->ret = ProtoIpSend(dst, req->data[4],
                                          req->data + 5, req->len - 5);
            }
            break;
        }
        case NET_OP_PING: {
            if (req->len >= 4) {
                u8 dst[4];
                memcpy(dst, req->data, 4);
                resp->ret = ProtoPing(dst);
            }
            break;
        }
        case NET_OP_UDP_BIND: {
            if (req->len >= 2) {
                u16 p = (u16)((req->data[0] << 8) | req->data[1]);
                resp->ret = ProtoUdpBind(p);
            }
            break;
        }
        case NET_OP_UDP_UNBIND: {
            if (req->len >= 2) {
                u16 p = (u16)((req->data[0] << 8) | req->data[1]);
                resp->ret = ProtoUdpUnbind(p);
            }
            break;
        }
        case NET_OP_UDP_SENDTO: {
            /* { ip[4]; sport; dport; data } */
            if (req->len >= 8) {
                u8 dst[4];
                memcpy(dst, req->data, 4);
                u16 sport = (u16)((req->data[4] << 8) | req->data[5]);
                u16 dport = (u16)((req->data[6] << 8) | req->data[7]);
                resp->ret = ProtoUdpSendto(dst, sport, dport,
                                             req->data + 8, req->len - 8);
            }
            break;
        }
        case NET_OP_UDP_RECV: {
            u8 src[4];
            u16 sport, dport;
            int n = ProtoUdpRecv(src, &sport, &dport, resp->data,
                                   NET_MTU);
            if (n >= 0) {
                /* Reply: { srcip[4]; sport; dport; data } */
                u8 out[4 + 4 + 1500];
                memcpy(out, src, 4);
                out[4] = (u8)(sport >> 8);
                out[5] = (u8)(sport & 0xFF);
                out[6] = (u8)(dport >> 8);
                out[7] = (u8)(dport & 0xFF);
                memcpy(out + 8, resp->data, (u32)n);
                memcpy(resp->data, out, (u32)n + 8);
                resp->len = (u32)n + 8;
                resp->ret = 0;
            } else {
                resp->ret = n;
            }
            break;
        }
        case NET_OP_TCP_LISTEN: {
            if (req->len >= 2) {
                u16 p = (u16)((req->data[0] << 8) | req->data[1]);
                resp->ret = ProtoTcpListen(p);
            }
            break;
        }
        case NET_OP_TCP_ACCEPT: {
            u8 peer[4];
            u16 pport;
            int n = ProtoTcpAccept(peer, &pport);
            if (n == 0) {
                resp->data[0] = peer[0];
                resp->data[1] = peer[1];
                resp->data[2] = peer[2];
                resp->data[3] = peer[3];
                resp->data[4] = (u8)(pport >> 8);
                resp->data[5] = (u8)(pport & 0xFF);
                resp->len = 6;
                resp->ret = 0;
            } else {
                resp->ret = n;
            }
            break;
        }
        case NET_OP_TCP_SEND: {
            if (req->len > 0)
                resp->ret = ProtoTcpSend(req->data, req->len);
            break;
        }
        case NET_OP_TCP_RECV: {
            u16 pport;
            int n = ProtoTcpRecv(resp->data, NET_MTU, &pport);
            if (n >= 0) {
                resp->len = (u32)n;
                resp->ret = 0;
            } else {
                resp->ret = n;
            }
            break;
        }
        case NET_OP_TCP_CLOSE:
            resp->ret = ProtoTcpClose();
            break;
        default:
            break;
        }
        /* Reply envelope: net_resp_t header is ret(4) + len(4), so the
         * payload begins at offset 8 — 8 + resp->len bytes total. */
        (void)IpcReply(token, resp, (int)(8 + resp->len));
    }
}

/* ---- entry ---- */

int main(void) {
    printf("net: starting PCnet-Fast III driver\n");

    /* 1. Find the adapter in the PCI enumeration. */
    int idx = -1;
    int n   = PciGetCount();
    for (int i = 0; i < n; i++) {
        pci_device_info_t dev;
        if (PciGetDevice(i, &dev) < 0)
            continue;
        if (dev.vendor_id == PCNET_VENDOR && dev.device_id == PCNET_DEVICE) {
            idx = i;
            printf("net: PCnet at PCI[%d] bus=%u dev=%u func=%u class=%04x irq=%u\n",
                   i, dev.bus, dev.dev, dev.func, dev.class_code, dev.irq_line);
            s_io_base = (int)(dev.bar[0] & 0xFFFFFFF0u);
            s_irq     = dev.irq_line;
            break;
        }
    }
    if (idx < 0) {
        printf("net: no PCnet adapter found\n");
        for (;;)
            Sleep(10); /* stay alive (no NIC in this configuration) */
    }
    printf("net: IO base 0x%x, IRQ %d\n", s_io_base, s_irq);

    /* 2. Capabilities: PCI device + IO ports (0x00..0x1F of the BAR). */
    int pci_cap = CapCreateObj(CAP_TYPE_PCI_DEV, RIGHT_READ | RIGHT_WRITE, (u32)idx);
    if (pci_cap < 0) {
        printf("net: CapCreate(PCI_DEV %d) failed (%d)\n", idx, pci_cap);
        ThreadExit(1);
    }
    int io_cap = CapCreateObj(CAP_TYPE_IO_PORT, RIGHT_ALL,
                                (0x20u << 16) | (u32)s_io_base);
    if (io_cap < 0) {
        printf("net: CapCreate(IO_PORT 0x%x) failed (%d)\n", s_io_base, io_cap);
        ThreadExit(1);
    }

    /* 3. MAC from the APROM. */
    PcnetReadMac();
    printf("net: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
           s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5]);

    /* 3b. Enable PCI bus mastering (command register bit 2).  QEMU
     * 10.2+ gates DMA behind a per-device "bus master" alias that is
     * DISABLED until the guest sets PCI_COMMAND_MASTER — without this
     * the NIC's DMA reads (InitBlock, descriptors, buffers) all come
     * back zero.  Keep the status register (upper 16 bits) untouched. */
    {
        u32 cmd = (u32)PciCfgRead32(idx, 0x04);
        u32 nv  = (cmd & 0xFFFF0000u) | ((cmd | 0x0007u) & 0xFFFFu); /* IO|MEM|MASTER */
        int rc  = PciCfgWrite32(idx, 0x04, nv);
        if (rc < 0) {
            printf("net: PciCfgWrite32(0x04) failed (%d)\n", rc);
            ThreadExit(1);
        }
        cmd = (u32)PciCfgRead32(idx, 0x04);
        printf("net: PCI command=0x%08x (bus master %s)\n", cmd,
               (cmd & 0x0004u) ? "ON" : "OFF");
    }

    /* 4. DMA pool: contiguous physical pages for rings + buffers. */
    u64 pool_va = 0x50000000ULL;
    s_pool_phys = ShmCreate(NET_POOL_SIZE / 4096 + 1, (void *)pool_va);
    if (s_pool_phys == 0) {
        printf("net: shm_create failed (no ATOM_SERVICE_MANAGE?)\n");
        ThreadExit(1);
    }
    s_pool_va   = (u8 *)pool_va;
    s_init      = (pcnet_init_t *)(s_pool_va + NET_OFF_INIT);
    s_rx_ring   = (pcnet_desc_t *)(s_pool_va + NET_OFF_RXRING);
    s_tx_ring   = (pcnet_desc_t *)(s_pool_va + NET_OFF_TXRING);
    printf("net: DMA pool phys=0x%llx va=%p\n", (unsigned long long)s_pool_phys,
           (void *)s_pool_va);

    /* 5. Reset + program the rings. */
    PcnetReset();
    PcnetInitRings();

    /* 6. Lock. */
    s_lock = MutexCreate();

    /* 7. Start the NIC FIRST (its INTR line is level-triggered; binding
     * the IRQ before INIT/STRT lets a stale assertion storm the CPU). */
    int r = PcnetStart();
    if (r < 0) {
        printf("net: NIC start FAILED (%d)\n", r);
        ThreadExit(1);
    }
    printf("net: NIC started (Rx/Tx live)\n");

    /* 7c. Protocol stack: static slirp-typical address. */
    {
        static const u8 ip[4] = {10, 0, 2, 15};
        static const u8 gw[4] = {10, 0, 2, 2};
        ProtoInit(ip, gw);
        printf("net: stack up 10.0.2.15 gw 10.0.2.2\n");
    }

    /* 8. IPC port. */
    int port = IpcPortCreate();
    if (port < 0) {
        printf("net: ipc_port_create failed (%d)\n", port);
        ThreadExit(1);
    }
    int ret = PortRegister(NET_PORT_NAME, port);
    if (ret < 0) {
        printf("net: PortRegister('%s') failed (%d)\n", NET_PORT_NAME, ret);
        ThreadExit(1);
    }
    printf("net: port %d registered as '%s'\n", port, NET_PORT_NAME);

    NetServerLoop(port);
    return 0;
}
