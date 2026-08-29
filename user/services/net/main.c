/*
 * main.c - PCnet-Fast III (AMD AM79C973) Ethernet driver + net service
 * Copyright (c) 2026 OpSys Project
 *
 * The driver owns the PCnet adapter (PCI vendor 0x1022 / device
 * 0x2000), programmed over its IO BAR (RAP/RDP registers) with DMA
 * rings + buffers allocated from a contiguous physical-page pool
 * (shm_create, gated on ATOM_SERVICE_MANAGE — the net service is in
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
 */

#include "net.h"

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
#define NET_RING_LOG   3 /* log2(8) -> encoded in InitBlock rlen/tlen */
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
 *   +0  mode (u16)         +2  rlen (u8, log2(ring) << 4)
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

static void csr_write(u16 reg, u16 val) {
    int r1 = io_write16((u16)(s_io_base + PCNET_RAP), reg);
    int r2 = io_write16((u16)(s_io_base + PCNET_RDP), val);
    if (r1 < 0 || r2 < 0)
        printf("net: csr_write(%u,%04x) io err %d/%d\n", reg, val, r1, r2);
}

static u16 csr_read(u16 reg) {
    io_write16((u16)(s_io_base + PCNET_RAP), reg);
    return (u16)io_read16((u16)(s_io_base + PCNET_RDP));
}

static void bcr_write(u16 reg, u16 val) {
    /* QEMU: IO+0x16 writes bcr[RAP]; BCR20 (SWSTYLE) values are
     * auto-encoded: 2 -> 0x0302 => SWSTYLE=2 AND SSIZE32=1 (16-byte
     * descriptors + flat 32-bit DMA addressing). */
    io_write16((u16)(s_io_base + PCNET_RAP), reg);
    io_write16((u16)(s_io_base + PCNET_BCR), val);
}

/* ---- init ---- */

static void pcnet_reset(void) {
    csr_write(0, CSR0_STOP); /* stop (QEMU: bit2) */
    bcr_write(BCR20, 0x0002); /* SWSTYLE 2: QEMU walks 16-byte descriptors */
    csr_write(CSR4, 0x0915);  /* QEMU-compatible default config */
}

/* Read the 6-byte MAC from the APROM (IO + 0x00..0x0B). */
static void pcnet_read_mac(void) {
    for (int i = 0; i < 6; i++)
        s_mac[i] = (u8)io_read8((u16)(s_io_base + i));
}

static void pcnet_init_rings(void) {
    memset(s_pool_va, 0, NET_POOL_SIZE);

    /* InitBlock: mode 0 (normal), MAC, ring pointers + lengths.
     * rlen/tlen hold log2(ring) << 4 (QEMU: rlen = byte >> 4). */
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

static int pcnet_start(void) {
    /* Load the InitBlock address into CSR1/2 and issue INIT. */
    u32 ip = (u32)(s_pool_phys + NET_OFF_INIT);
    csr_write(1, (u16)(ip & 0xFFFF));
    csr_write(2, (u16)(ip >> 16));
    csr_write(0, CSR0_INIT);

    /* Wait for IDON (init done); fail loudly if the chip never
     * acknowledges (bad InitBlock address / layout / port map). */
    int idon = 0;
    for (int i = 0; i < 1000000; i++) {
        u16 c = csr_read(0);
        if (c & CSR0_IDON) {
            csr_write(0, CSR0_IDON); /* clear */
            idon = 1;
            break;
        }
        if (c & CSR0_MERR) {
            printf("net: init error csr0=%04x\n", c);
            return -7; /* ERR_FAULT */
        }
    }
    if (!idon) {
        printf("net: INIT timeout (csr0=%04x) — aborting\n", csr_read(0));
        return -7; /* ERR_FAULT */
    }

    csr_write(0, CSR0_STRT); /* start Rx + Tx */
    /* NOTE: no INEA / IRQ binding — Rx is delivered by polling the
     * ring (QEMU's pcnet INTx path is unreliable and a latched RINT
     * storms the level-sensitive 8259).  The IRQ line stays masked by
     * the kernel's boot-time PIC mask, so nothing asserts here. */
    return 0;
}

/* ---- Tx ---- */

static int net_send(const u8 *data, u32 len) {
    if (!data || len == 0 || len > NET_MTU)
        return -2; /* ERR_INVAL */
    if (s_lock >= 0)
        (void)mutex_lock(s_lock);

    for (int i = 0; i < NET_RINGS; i++) {
        pcnet_desc_t *t = &s_tx_ring[i];
        if (t->status & 0x8000)
            continue; /* still owned by the NIC */
        u8 *dst = s_pool_va + NET_OFF_TXBUF + i * NET_BUF_SIZE;
        memcpy(dst, data, len);
        /* IEEE 802.3 minimum frame: pad short frames (e.g. a 42-byte
         * ARP request) to 60 bytes, or real hardware/switches drop
         * them as runts. */
        if (len < 60) {
            memset(dst + len, 0, 60 - len);
            len = 60;
        }
        t->addr   = (u32)(s_pool_phys + NET_OFF_TXBUF + i * NET_BUF_SIZE);
        t->len    = (u16)((4096 - len) | 0xF000); /* BCNT + ONES nibble */
        t->mcnt   = 0;
        t->status = DESC_OWN | DESC_STP | DESC_ENP;
        csr_write(0, CSR0_TDMD); /* transmit demand */
        /* Wait for the NIC to consume the descriptor (OWN cleared).
         * Confirms the frame was actually transmitted. */
        int spins = 0;
        while ((t->status & DESC_OWN) && spins < 1000000)
            spins++;
        if (spins >= 1000000) {
            /* Timeout: the NIC never consumed the descriptor.  Reset
             * the slot so it does not leak (an OWN=1 TMD would make the
             * ring permanently short), then report the fault. */
            t->status = 0;
            t->len    = (u16)((4096 - NET_BUF_SIZE) | 0xF000);
            t->mcnt   = 0;
            s_stat_err++;
            if (s_lock >= 0)
                (void)mutex_unlock(s_lock);
            return -7; /* ERR_FAULT: NIC never transmitted */
        }
        s_stat_tx++;
        if (s_lock >= 0)
            (void)mutex_unlock(s_lock);
        return 0;
    }
    if (s_lock >= 0)
        (void)mutex_unlock(s_lock);
    return -6; /* ERR_AGAIN: no free descriptor */
}

/* ---- Rx (IRQ thread) ---- */

static void net_service_rx(void) {
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
            (void)mutex_lock(s_lock);
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
            (void)mutex_unlock(s_lock);
        r->status = DESC_OWN; /* hand the descriptor back to the NIC */

    }

    /* Clear RINT: QEMU's 8259 is level-sensitive, so a latched RINT
     * keeps the INTx line high and storms the CPU (the line re-asserts
     * on every EOI).  Write-1-to-clear the flag now that the ring has
     * been drained. */
    csr_write(0, CSR0_RINT);
}

/* ---- IPC server ---- */

static void net_server_loop(int port) {
    for (;;) {
        /* Poll the Rx ring first (the QEMU 10.2 pcnet INTx path is
         * unreliable; the IRQ thread is a bonus, not the delivery
         * mechanism).  Draining here keeps RECV latency low. */
        net_service_rx();

        int msg_len = (int)sizeof(s_req);
        int token   = 0;
        int ret     = ipc_recv(port, s_req, &msg_len, &token);
        if (ret < 0) {
            printf("net: ipc_recv failed (%d)\n", ret);
            thread_exit(1);
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
                resp->ret = net_send(req->data, req->len);
                resp->len = 0;
            }
            break;
        case NET_OP_RECV:
            if (s_lock >= 0)
                (void)mutex_lock(s_lock);
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
                (void)mutex_unlock(s_lock);
            break;
        case NET_OP_STATS:
            if (s_lock >= 0)
                (void)mutex_lock(s_lock);
            {
                u32 *st = (u32 *)resp->data;
                st[0] = s_stat_rx;
                st[1] = s_stat_tx;
                st[2] = s_stat_err;
            }
            resp->len = 12;
            resp->ret = 0;
            if (s_lock >= 0)
                (void)mutex_unlock(s_lock);
            break;
        default:
            break;
        }
        /* Reply envelope: net_resp_t header is ret(4) + len(4), so the
         * payload begins at offset 8 — 8 + resp->len bytes total. */
        (void)ipc_reply(token, resp, (int)(8 + resp->len));
    }
}

/* ---- entry ---- */

int main(void) {
    printf("net: starting PCnet-Fast III driver\n");

    /* 1. Find the adapter in the PCI enumeration. */
    int idx = -1;
    int n   = pci_get_count();
    for (int i = 0; i < n; i++) {
        pci_device_info_t dev;
        if (pci_get_device(i, &dev) < 0)
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
            sleep(10); /* stay alive (no NIC in this configuration) */
    }
    printf("net: IO base 0x%x, IRQ %d\n", s_io_base, s_irq);

    /* 2. Capabilities: PCI device + IO ports (0x00..0x1F of the BAR). */
    int pci_cap = cap_create_obj(CAP_TYPE_PCI_DEV, RIGHT_READ | RIGHT_WRITE, (u32)idx);
    if (pci_cap < 0) {
        printf("net: cap_create(PCI_DEV %d) failed (%d)\n", idx, pci_cap);
        thread_exit(1);
    }
    int io_cap = cap_create_obj(CAP_TYPE_IO_PORT, RIGHT_ALL,
                                (0x20u << 16) | (u32)s_io_base);
    if (io_cap < 0) {
        printf("net: cap_create(IO_PORT 0x%x) failed (%d)\n", s_io_base, io_cap);
        thread_exit(1);
    }

    /* 3. MAC from the APROM. */
    pcnet_read_mac();
    printf("net: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
           s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5]);

    /* 3b. Enable PCI bus mastering (command register bit 2).  QEMU
     * 10.2+ gates DMA behind a per-device "bus master" alias that is
     * DISABLED until the guest sets PCI_COMMAND_MASTER — without this
     * the NIC's DMA reads (InitBlock, descriptors, buffers) all come
     * back zero.  Keep the status register (upper 16 bits) untouched. */
    {
        u32 cmd = (u32)pci_cfg_read32(idx, 0x04);
        u32 nv  = (cmd & 0xFFFF0000u) | ((cmd | 0x0007u) & 0xFFFFu); /* IO|MEM|MASTER */
        int rc  = pci_cfg_write32(idx, 0x04, nv);
        if (rc < 0) {
            printf("net: pci_cfg_write32(0x04) failed (%d)\n", rc);
            thread_exit(1);
        }
        cmd = (u32)pci_cfg_read32(idx, 0x04);
        printf("net: PCI command=0x%08x (bus master %s)\n", cmd,
               (cmd & 0x0004u) ? "ON" : "OFF");
    }

    /* 4. DMA pool: contiguous physical pages for rings + buffers. */
    u64 pool_va = 0x50000000ULL;
    s_pool_phys = shm_create(NET_POOL_SIZE / 4096 + 1, (void *)pool_va);
    if (s_pool_phys == 0) {
        printf("net: shm_create failed (no ATOM_SERVICE_MANAGE?)\n");
        thread_exit(1);
    }
    s_pool_va   = (u8 *)pool_va;
    s_init      = (pcnet_init_t *)(s_pool_va + NET_OFF_INIT);
    s_rx_ring   = (pcnet_desc_t *)(s_pool_va + NET_OFF_RXRING);
    s_tx_ring   = (pcnet_desc_t *)(s_pool_va + NET_OFF_TXRING);
    printf("net: DMA pool phys=0x%llx va=%p\n", (unsigned long long)s_pool_phys,
           (void *)s_pool_va);

    /* 5. Reset + program the rings. */
    pcnet_reset();
    pcnet_init_rings();

    /* 6. Lock. */
    s_lock = mutex_create();

    /* 7. Start the NIC FIRST (its INTR line is level-triggered; binding
     * the IRQ before INIT/STRT lets a stale assertion storm the CPU). */
    int r = pcnet_start();
    if (r < 0) {
        printf("net: NIC start FAILED (%d)\n", r);
        thread_exit(1);
    }
    printf("net: NIC started (Rx/Tx live)\n");

    /* 8. IPC port. */
    int port = ipc_port_create();
    if (port < 0) {
        printf("net: ipc_port_create failed (%d)\n", port);
        thread_exit(1);
    }
    int ret = port_register(NET_PORT_NAME, port);
    if (ret < 0) {
        printf("net: port_register('%s') failed (%d)\n", NET_PORT_NAME, ret);
        thread_exit(1);
    }
    printf("net: port %d registered as '%s'\n", port, NET_PORT_NAME);

    net_server_loop(port);
    return 0;
}
