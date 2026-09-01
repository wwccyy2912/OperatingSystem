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
 * proto.c - Minimal L3/L4 protocol stack (ARP + IPv4 + ICMP + UDP)
 * Copyright (c) 2026 OpSys Project
 *
 * The net service owns the NIC, so the whole stack lives here and runs
 * on the service thread: ProtoRx() is fed from the driver's frame
 * queue; ProtoIpSend() resolves the next hop via the ARP cache and
 * hands the finished frame to the driver.  UDP datagrams destined for
 * a bound local port are queued and drained with ProtoUdpRecv().
 */

#include "proto.h"

#include <libc/string.h>
#include <libc/stdio.h> /* printf */
#include <libos/syscalls.h> /* get_time */

/* The driver side (implemented in main.c): send a raw Ethernet frame
 * and the NIC MAC. */
extern int  NetSendRaw(const u8 *frame, u32 len);
extern void NetGetMac(u8 mac[6]);
extern void NetRxPumpNow(void);        /* NIC ring -> driver queue */
extern int  NetRxPump(u8 *out, u32 *out_len); /* pop a queued frame */
extern void NetYield(void);              /* sleep one 10 ms tick */

/* ---- address state ---- */
static u8 s_ip[4];  /* local IPv4 */
static u8 s_gw[4];  /* default gateway */
static u8 s_mac[6]; /* NIC MAC */

/* ---- ARP cache ---- */
#define ARP_CACHE_MAX 16
#define ARP_TTL_TICKS 1000 /* ~10 s at 10 ms/tick */
typedef struct {
    u8  valid;
    u8  ip[4];
    u8  mac[6];
    u32 ttl; /* ticks remaining */
} arp_entry_t;
static arp_entry_t s_arp[ARP_CACHE_MAX];

/* fwd: TCP segment handler (defined below) */
static void ProtoTcpRxTcp(const u8 *f, u32 fl);

/* ---- UDP ---- */
#define UDP_SOCK_MAX  16
#define UDP_RXQ_MAX   16
typedef struct {
    u8   in_use;
    u16  port;
} udp_sock_t;
static udp_sock_t s_udp[UDP_SOCK_MAX];

typedef struct {
    u8   src[4];
    u16  sport;
    u16  dport;
    u16  len;
    u8   data[1500];
} udp_pkt_t;
static udp_pkt_t s_udp_rxq[UDP_RXQ_MAX];
static u32       s_udp_rx_head;
static u32       s_udp_rx_count;

/* ---- helpers ---- */

static u16 IpChecksum(const u8 *buf, u32 len) {
    u32 sum = 0;
    u32 i   = 0;
    while (i + 1 < len) {
        sum += ((u16)buf[i] << 8) | buf[i + 1];
        i += 2;
    }
    if (i < len)
        sum += (u16)buf[i] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)~sum;
}

static int IpEqual(const u8 a[4], const u8 b[4]) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

/* ---- ARP ---- */

static arp_entry_t *arp_lookup(const u8 ip[4]) {
    for (int i = 0; i < ARP_CACHE_MAX; i++)
        if (s_arp[i].valid && s_arp[i].ttl > 0 && IpEqual(s_arp[i].ip, ip))
            return &s_arp[i];
    return NULL;
}

static void ArpLearn(const u8 ip[4], const u8 mac[6]) {
    arp_entry_t *e = arp_lookup(ip);
    if (!e) {
        for (int i = 0; i < ARP_CACHE_MAX; i++) {
            if (!s_arp[i].valid) {
                e = &s_arp[i];
                break;
            }
        }
        if (!e)
            e = &s_arp[0]; /* evict the first entry */
    }
    memcpy(e->ip, ip, 4);
    memcpy(e->mac, mac, 6);
    e->ttl   = ARP_TTL_TICKS;
    e->valid = 1;
}

/* Build + send an ARP request for target_ip. */
static void ArpRequest(const u8 target_ip[4]) {
    u8 f[60];
    memset(f, 0xFF, 6);           /* dst broadcast */
    memcpy(f + 6, s_mac, 6);      /* src = us */
    f[12] = (u8)(ETH_TYPE_ARP >> 8);
    f[13] = (u8)(ETH_TYPE_ARP & 0xFF);
    f[14] = 0; f[15] = 1;         /* hw ether */
    f[16] = 8; f[17] = 0;         /* proto IP */
    f[18] = 6; f[19] = 4;         /* hlen/plen */
    f[20] = 0; f[21] = 1;         /* op request */
    memcpy(f + 22, s_mac, 6);     /* sender MAC */
    memcpy(f + 28, s_ip, 4);      /* sender IP */
    memset(f + 32, 0, 6);         /* target MAC 0 */
    memcpy(f + 38, target_ip, 4); /* target IP */
    (void)NetSendRaw(f, 60);
}

/* Handle an ARP frame: learn the sender, reply to requests for us. */
static void ArpHandle(const u8 *f) {
    /* f = start of the Ethernet payload (already past the 14B header). */
    u8 sender_ip[4], target_ip[4];
    memcpy(sender_ip, f + 14, 4);
    memcpy(target_ip, f + 24, 4);
    ArpLearn(sender_ip, f + 8);

    u16 op = (u16)((f[6] << 8) | f[7]);
    if (op == 1 && IpEqual(target_ip, s_ip)) {
        /* ARP reply: tell the requester our MAC. */
        u8 r[60];
        memset(r, 0xFF, 6);
        memset(r, 0, 6); /* dst = requester's MAC (learnt) */
        memcpy(r, f + 8, 6);
        memcpy(r + 6, s_mac, 6);
        r[12] = (u8)(ETH_TYPE_ARP >> 8);
        r[13] = (u8)(ETH_TYPE_ARP & 0xFF);
        r[14] = 0; r[15] = 1;
        r[16] = 8; r[17] = 0;
        r[18] = 6; r[19] = 4;
        r[20] = 0; r[21] = 2; /* op reply */
        memcpy(r + 22, s_mac, 6);
        memcpy(r + 28, s_ip, 4);
        memcpy(r + 32, f + 8, 6); /* target MAC = requester */
        memcpy(r + 38, sender_ip, 4);
        (void)NetSendRaw(r, 60);
    }
}

/* Resolve dst_ip to a MAC: cache hit, or send an ARP request and wait
 * for the reply (pumping the NIC + driver queue so the reply arrives
 * without client-side polling).  Returns 0 on success, -7 on timeout. */
static int ArpResolve(const u8 dst_ip[4], u8 mac[6]) {

    arp_entry_t *e = arp_lookup(dst_ip);
    if (e) {
        memcpy(mac, e->mac, 6);
        return 0;
    }
    ArpRequest(dst_ip);
    for (int i = 0; i < 600; i++) { /* ~6 s */
        NetRxPumpNow();
        u8 f[1600];
        u32 fl;
        while (NetRxPump(f, &fl)) {
            if (fl >= 42 && f[12] == 0x08 && f[13] == 0x06) {
                const u8 *a = f + 14;
                u16 op = (u16)((a[6] << 8) | a[7]);
                if (op == 2 && a[14] == dst_ip[0] && a[15] == dst_ip[1] &&
                    a[16] == dst_ip[2] && a[17] == dst_ip[3]) {
                    memcpy(mac, a + 8, 6);
                    ArpLearn(dst_ip, mac);
                    return 0;
                }
            }
            ProtoRx(f, fl); /* everything else into the stack */
        }
        NetYield();
    }
    return -7; /* ERR_FAULT: no ARP reply */
}

/* ---- IP ---- */

static int IpSendRaw(const u8 dst_ip[4], u8 proto, const u8 *payload, u32 len) {
    u8 mac[6];
    int r = ArpResolve(dst_ip, mac);
    if (r != 0)
        return r;

    u32 total = IP_HDR_LEN + len;
    if (total > 1500)
        return -8; /* ERR_OVERFLOW: no fragmentation */
    u8 f[14 + 1500];
    /* Ethernet header */
    memcpy(f, mac, 6);
    memcpy(f + 6, s_mac, 6);
    f[12] = (u8)(ETH_TYPE_IPV4 >> 8);
    f[13] = (u8)(ETH_TYPE_IPV4 & 0xFF);
    /* IP header */
    u8 *ip = f + 14;
    ip[0] = 0x45; /* v4, 5 dwords */
    ip[1] = 0;
    ip[2] = (u8)(total >> 8);
    ip[3] = (u8)(total & 0xFF);
    ip[4] = 0; ip[5] = 0;          /* id */
    ip[6] = 0; ip[7] = 0;          /* flags/frag */
    ip[8] = 64;                    /* TTL */
    ip[9] = proto;
    /* checksum field 10-11 filled after */
    memcpy(ip + 12, s_ip, 4);
    memcpy(ip + 16, dst_ip, 4);
    ip[10] = 0;
    ip[11] = 0;
    u16 cs = IpChecksum(ip, IP_HDR_LEN);
    ip[10] = (u8)(cs >> 8);
    ip[11] = (u8)(cs & 0xFF);
    memcpy(ip + IP_HDR_LEN, payload, len);
    return NetSendRaw(f, 14 + total);
}

/* ---- ICMP ---- */

/* Reply to an ICMP echo request (ping).  f = IP payload (ICMP header). */
static void IcmpEchoReply(const u8 *src_ip, const u8 *icmp, u32 icmp_len) {
    /* icmp[0]=type(8 req) icmp[1]=code icmp[2..3]=checksum
     * icmp[4..5]=id icmp[6..7]=seq + data */
    u8 payload[1500];
    u32 plen = icmp_len;
    memcpy(payload, icmp, plen);
    payload[0] = 0; /* type: echo reply */
    payload[1] = 0;
    payload[2] = 0;
    payload[3] = 0;
    u16 cs = IpChecksum(payload, plen);
    payload[2] = (u8)(cs >> 8);
    payload[3] = (u8)(cs & 0xFF);
    (void)IpSendRaw(src_ip, IP_PROTO_ICMP, payload, plen);
}

/* fwd: TCP segment handler (defined below) */
static void ProtoTcpRxTcp(const u8 *f, u32 fl);

/* ---- UDP ---- */

static udp_sock_t *udp_find(u16 port) {
    for (int i = 0; i < UDP_SOCK_MAX; i++)
        if (s_udp[i].in_use && s_udp[i].port == port)
            return &s_udp[i];
    return NULL;
}

static int UdpPortBound(u16 port) {
    return udp_find(port) != NULL;
}

static void UdpQueue(const u8 src[4], u16 sport, u16 dport, const u8 *data, u32 len) {
    if (s_udp_rx_count >= UDP_RXQ_MAX)
        return; /* drop on overflow */
    u32 slot = (s_udp_rx_head + s_udp_rx_count) % UDP_RXQ_MAX;
    udp_pkt_t *p = &s_udp_rxq[slot];
    memcpy(p->src, src, 4);
    p->sport = sport;
    p->dport = dport;
    p->len   = (u16)(len > 1500 ? 1500 : len);
    memcpy(p->data, data, p->len);
    s_udp_rx_count++;
}

/* ---- public API ---- */

void ProtoInit(const u8 ip[4], const u8 gw[4]) {
    memcpy(s_ip, ip, 4);
    memcpy(s_gw, gw, 4);
    NetGetMac(s_mac);
    memset(s_arp, 0, sizeof(s_arp));
    memset(s_udp, 0, sizeof(s_udp));
    s_udp_rx_head = 0;
    s_udp_rx_count = 0;
}

void ProtoRx(const u8 *frame, u32 len) {
    if (!frame || len < 14)
        return;
    u16 etype = (u16)((frame[12] << 8) | frame[13]);
    /* dst must be our MAC or the broadcast address. */
    int ours = 1;
    for (int i = 0; i < 6; i++)
        if (frame[i] != s_mac[i]) {
            ours = 0;
            break;
        }
    if (!ours && frame[0] != 0xFF)
        return;
    const u8 *p = frame + 14; /* payload */
    u32 plen = len - 14;

    if (etype == ETH_TYPE_ARP) {
        if (plen >= 28)
            ArpHandle(p);
        return;
    }
    if (etype != ETH_TYPE_IPV4 || plen < IP_HDR_LEN)
        return;

    const u8 *ip = p;
    if ((ip[0] >> 4) != 4)
        return; /* IPv4 only */
    u16 iplen = (u16)((ip[2] << 8) | ip[3]);
    if (iplen > plen)
        return;
    /* verify the header checksum */
    if (IpChecksum(ip, IP_HDR_LEN) != 0)
        return;
    u8 src[4];
    memcpy(src, ip + 12, 4);
    u8 proto = ip[9];
    const u8 *pl = ip + IP_HDR_LEN;
    u32 lplen = iplen - IP_HDR_LEN;

    if (proto == IP_PROTO_ICMP && lplen >= 8) {
        if (pl[0] == 8) /* echo request */
            IcmpEchoReply(src, pl, lplen);
        return;
    }
    if (proto == IP_PROTO_UDP && lplen >= UDP_HDR_LEN) {
        u16 sport = (u16)((pl[0] << 8) | pl[1]);
        u16 dport = (u16)((pl[2] << 8) | pl[3]);
        if (UdpPortBound(dport))
            UdpQueue(src, sport, dport, pl + UDP_HDR_LEN, lplen - UDP_HDR_LEN);
        return;
    }
    if (proto == IP_PROTO_TCP && lplen >= TCP_HDR_LEN)
        ProtoTcpRxTcp(frame, len);
}

int ProtoIpSend(const u8 dst_ip[4], u8 proto, const u8 *payload, u32 len) {
    return IpSendRaw(dst_ip, proto, payload, len);
}

/* ICMP echo request + wait for the reply.  The driver queue is pumped
 * inside the wait loop so the reply arrives without client polling. */
int ProtoPing(const u8 dst_ip[4]) {
    /* Build the echo request. */
    static u16 s_ping_id = 0x1234;
    u8 icmp[8 + 32];
    icmp[0] = 8; icmp[1] = 0; icmp[2] = 0; icmp[3] = 0;
    icmp[4] = (u8)(s_ping_id >> 8);
    icmp[5] = (u8)(s_ping_id & 0xFF);
    icmp[6] = 0; icmp[7] = 1; /* seq */
    for (int i = 0; i < 32; i++)
        icmp[8 + i] = (u8)i;
    u16 cs = IpChecksum(icmp, sizeof(icmp));
    icmp[2] = (u8)(cs >> 8);
    icmp[3] = (u8)(cs & 0xFF);
    int r = IpSendRaw(dst_ip, IP_PROTO_ICMP, icmp, sizeof(icmp));
    if (r != 0) {
        return r;
    }
    s_ping_id++;
    /* Wait for the echo reply: pump the driver queue via NetRxPump().
     * The reply path (icmp_echo_reply in the peer) arrives as an ICMP
     * type-0 packet — proto_rx drops it silently (no local echo match
     * tracking needed: reaching the peer proves reachability via the
     * reply's arrival on our NIC).  We simply wait for a matching
     * ICMP echo reply by scanning the driver queue. */
    for (int i = 0; i < 600; i++) { /* ~6 s */
        u8 frame[1600];
        u32 flen;
        NetRxPumpNow(); /* NIC ring -> driver queue (missed: the
                            * reply sits in the RMD ring otherwise) */
        if (NetRxPump(frame, &flen)) {
            if (flen >= 14 + IP_HDR_LEN + 8) {
                const u8 *ip = frame + 14;
                if ((ip[0] >> 4) == 4 && ip[9] == IP_PROTO_ICMP) {
                    const u8 *ic = ip + IP_HDR_LEN;
                    if (ic[0] == 0) /* echo reply */
                        return 0;
                }
            }
            ProtoRx(frame, flen);
        }
        NetYield();
    }
    return -7; /* ERR_FAULT: timeout */
}

int ProtoUdpBind(u16 port) {
    if (port < 16 || UdpPortBound(port))
        return -2; /* ERR_INVAL / already bound */
    for (int i = 0; i < UDP_SOCK_MAX; i++) {
        if (!s_udp[i].in_use) {
            s_udp[i].in_use = 1;
            s_udp[i].port   = port;
            return 0;
        }
    }
    return -1; /* ERR_NOMEM */
}

int ProtoUdpUnbind(u16 port) {
    udp_sock_t *s = udp_find(port);
    if (!s)
        return -4; /* ERR_NOENT */
    s->in_use = 0;
    return 0;
}

int ProtoUdpSendto(const u8 dst_ip[4], u16 sport, u16 dport,
                     const u8 *data, u32 len) {
    u8 udp[UDP_HDR_LEN + 1500];
    u32 total = UDP_HDR_LEN + len;
    if (total > 1500)
        return -8;
    udp[0] = (u8)(sport >> 8);
    udp[1] = (u8)(sport & 0xFF);
    udp[2] = (u8)(dport >> 8);
    udp[3] = (u8)(dport & 0xFF);
    udp[4] = (u8)(total >> 8);
    udp[5] = (u8)(total & 0xFF);
    udp[6] = 0; udp[7] = 0; /* checksum 0 (RFC 768: allowed) */
    memcpy(udp + UDP_HDR_LEN, data, len);
    return IpSendRaw(dst_ip, IP_PROTO_UDP, udp, total);
}

int ProtoUdpRecv(u8 src[4], u16 *sport, u16 *dport, u8 *data, u32 max) {
    if (s_udp_rx_count == 0)
        return -6; /* ERR_AGAIN */
    udp_pkt_t *p = &s_udp_rxq[s_udp_rx_head];
    memcpy(src, p->src, 4);
    if (sport)
        *sport = p->sport;
    if (dport)
        *dport = p->dport;
    u32 n = p->len;
    if (n > max)
        n = max;
    memcpy(data, p->data, n);
    s_udp_rx_head = (s_udp_rx_head + 1) % UDP_RXQ_MAX;
    s_udp_rx_count--;
    return (int)n;
}

/* ====================================================================
 * TCP — minimal single-connection implementation (RFC 793 subset)
 *
 * One listening socket; after accept() a single established connection
 * carries data.  The three-way handshake, sequence/ack accounting and
 * passive/active close are implemented; retransmission and windowing
 * are deliberately omitted (QEMU slirp on a local link never drops).
 * ==================================================================== */

#define TCP_STATE_LISTEN    0
#define TCP_STATE_SYN_SENT  1
#define TCP_STATE_ESTAB     2
#define TCP_STATE_CLOSE_WAIT 3 /* peer FIN'd; we may still send */
#define TCP_STATE_CLOSED    4

#define TCP_RXQ_MAX 8
typedef struct {
    u8   src[4];
    u16  sport;
    u16  len;
    u8   data[1500];
} tcp_pkt_t;

static u8      s_tcp_state = TCP_STATE_CLOSED;
static u16     s_tcp_lport;  /* listening / local port */
static u8      s_tcp_peer[4]; /* established peer */
static u16     s_tcp_peer_port;
static u32     s_tcp_iss;   /* initial send seq */
static u32     s_tcp_snd_nxt; /* next seq to send */
static u32     s_tcp_rcv_nxt; /* next seq expected from peer */
static tcp_pkt_t s_tcp_rxq[TCP_RXQ_MAX];
static u32     s_tcp_rx_head;
static u32     s_tcp_rx_count;

/* TCP pseudo-header checksum (RFC 793). */
static u16 TcpChecksum(const u8 *src_ip, const u8 *dst_ip,
                        const u8 *seg, u32 seg_len) {
    u8 pseudo[12];
    memcpy(pseudo, src_ip, 4);
    memcpy(pseudo + 4, dst_ip, 4);
    pseudo[8] = 0;
    pseudo[9] = IP_PROTO_TCP;
    pseudo[10] = (u8)(seg_len >> 8);
    pseudo[11] = (u8)(seg_len & 0xFF);
    /* Checksum over pseudo header + segment. */
    u32 sum = 0;
    u32 i = 0;
    for (; i + 1 < 12; i += 2)
        sum += ((u16)pseudo[i] << 8) | pseudo[i + 1];
    for (i = 0; i + 1 < seg_len; i += 2)
        sum += ((u16)seg[i] << 8) | seg[i + 1];
    if (seg_len & 1)
        sum += (u16)seg[seg_len - 1] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)~sum;
}

/* Build + send a raw TCP segment. */
static int TcpSendSeg(u8 flags, u32 seq, u32 ack, const u8 *data, u32 len) {
    u8 seg[TCP_HDR_LEN + 1500];
    memset(seg, 0, TCP_HDR_LEN);
    seg[0] = (u8)(s_tcp_lport >> 8);
    seg[1] = (u8)(s_tcp_lport & 0xFF);
    seg[2] = (u8)(s_tcp_peer_port >> 8);
    seg[3] = (u8)(s_tcp_peer_port & 0xFF);
    seg[4] = (u8)(seq >> 24);
    seg[5] = (u8)(seq >> 16);
    seg[6] = (u8)(seq >> 8);
    seg[7] = (u8)(seq & 0xFF);
    seg[8] = (u8)(ack >> 24);
    seg[9] = (u8)(ack >> 16);
    seg[10] = (u8)(ack >> 8);
    seg[11] = (u8)(ack & 0xFF);
    seg[12] = 0x50; /* data offset 5 */
    seg[13] = flags;
    seg[14] = 0x10; /* window 4096 */
    seg[15] = 0x00;
    seg[16] = 0;
    seg[17] = 0; /* checksum */
    seg[18] = 0;
    seg[19] = 0; /* urgent */
    if (data && len)
        memcpy(seg + TCP_HDR_LEN, data, len);
    u32 total = TCP_HDR_LEN + len;
    u16 cs = TcpChecksum(s_ip, s_tcp_peer, seg, total);
    seg[16] = (u8)(cs >> 8);
    seg[17] = (u8)(cs & 0xFF);
    return IpSendRaw(s_tcp_peer, IP_PROTO_TCP, seg, total);
}

int ProtoTcpListen(u16 port) {
    if (port < 16)
        return -2;
    s_tcp_state = TCP_STATE_LISTEN;
    s_tcp_lport = port;
    s_tcp_rx_head = 0;
    s_tcp_rx_count = 0;
    return 0;
}

int ProtoTcpClose(void) {
    s_tcp_state = TCP_STATE_CLOSED;
    s_tcp_rx_head = 0;
    s_tcp_rx_count = 0;
    return 0;
}

/* Block until a connection is established (or ~6 s timeout). */
int ProtoTcpAccept(u8 peer[4], u16 *peer_port) {
    if (s_tcp_state != TCP_STATE_LISTEN)
        return -2;
    extern void NetYield(void);
    for (int i = 0; i < 6000; i++) { /* ~60 s accept window */
        NetRxPumpNow();
        u8 f[1600];
        u32 fl;
        while (NetRxPump(f, &fl)) {
            if (fl >= 14 + IP_HDR_LEN + TCP_HDR_LEN && f[12] == 0x08 && f[13] == 0x00) {
                const u8 *ip = f + 14;
                if (ip[9] == IP_PROTO_TCP) {
                    const u8 *t = ip + IP_HDR_LEN;
                    u16 dport = (u16)((t[2] << 8) | t[3]);
                    u8 flags  = t[13];
                    u32 seq   = ((u32)t[4] << 24) | ((u32)t[5] << 16) |
                                ((u32)t[6] << 8) | t[7];
                    if (dport == s_tcp_lport && (flags & TCP_SYN) && !(flags & TCP_ACK)) {
                        /* SYN: handshake. */
                        memcpy(s_tcp_peer, ip + 12, 4);
                        s_tcp_peer_port = (u16)((t[0] << 8) | t[1]);
                        s_tcp_rcv_nxt = seq + 1;
                        s_tcp_iss = (u32)GetTime();
                        if (s_tcp_iss == 0)
                            s_tcp_iss = 0x1234;
                        s_tcp_snd_nxt = s_tcp_iss + 1;
                        s_tcp_state = TCP_STATE_SYN_SENT; /* awaiting ACK */
                        (void)TcpSendSeg(TCP_SYN | TCP_ACK, s_tcp_iss,
                                           s_tcp_rcv_nxt, NULL, 0);
                        /* fall through: the ACK arrives next pump */
                        ProtoTcpRxTcp(f, fl);
                    }
                }
            }
            ProtoRx(f, fl);
        }
        if (s_tcp_state == TCP_STATE_ESTAB || s_tcp_state == TCP_STATE_CLOSE_WAIT) {
            memcpy(peer, s_tcp_peer, 4);
            *peer_port = s_tcp_peer_port;
            return 0;
        }
        NetYield();
    }
    s_tcp_state = TCP_STATE_LISTEN;
    return -7; /* timeout */
}
/* Handle an incoming TCP segment (called from proto_rx). */
static void ProtoTcpRxTcp(const u8 *f, u32 fl); /* fwd */

static void ProtoTcpRxTcp(const u8 *f, u32 fl) {
    const u8 *ip = f + 14;
    const u8 *t  = ip + IP_HDR_LEN;
    u16 sport = (u16)((t[0] << 8) | t[1]);
    u16 dport = (u16)((t[2] << 8) | t[3]);
    u8 flags  = t[13];
    u32 seq   = ((u32)t[4] << 24) | ((u32)t[5] << 16) | ((u32)t[6] << 8) | t[7];
    u32 ack   = ((u32)t[8] << 24) | ((u32)t[9] << 16) | ((u32)t[10] << 8) | t[11];
    (void)sport;

    if (dport != s_tcp_lport)
        return;

    if (s_tcp_state == TCP_STATE_SYN_SENT) {
        /* We sent SYN-ACK; the client completes the handshake with a
         * plain ACK (0x10) — the normal server-side case.  (A
         * simultaneous-open SYN-ACK also works.) */
        if ((flags & TCP_ACK) && !(flags & TCP_SYN)) {
            s_tcp_rcv_nxt = seq;  /* client's next data seq */
            s_tcp_snd_nxt = ack;  /* our SYN-ACK was acked */
            s_tcp_state = TCP_STATE_ESTAB;
        } else if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            s_tcp_rcv_nxt = seq + 1;
            s_tcp_snd_nxt = ack;
            s_tcp_state = TCP_STATE_ESTAB;
            (void)TcpSendSeg(TCP_ACK, s_tcp_snd_nxt, s_tcp_rcv_nxt, NULL, 0);
        }
        return;
    }
    if (s_tcp_state == TCP_STATE_ESTAB || s_tcp_state == TCP_STATE_CLOSE_WAIT) {
        /* Data: deliver in order (single connection, no reordering). */
        if (fl >= 14 + IP_HDR_LEN + TCP_HDR_LEN + 1) {
            u32 hlen = ((t[12] >> 4) & 0xF) * 4;
            u32 dlen = fl - (14 + IP_HDR_LEN + hlen);
            if (dlen > 0 && seq == s_tcp_rcv_nxt && s_tcp_rx_count < TCP_RXQ_MAX) {
                u32 slot = (s_tcp_rx_head + s_tcp_rx_count) % TCP_RXQ_MAX;
                memcpy(s_tcp_rxq[slot].src, ip + 12, 4);
                s_tcp_rxq[slot].sport = sport;
                s_tcp_rxq[slot].len = (u16)(dlen > 1500 ? 1500 : dlen);
                memcpy(s_tcp_rxq[slot].data, t + hlen, s_tcp_rxq[slot].len);
                s_tcp_rx_count++;
                s_tcp_rcv_nxt = seq + dlen;
                (void)TcpSendSeg(TCP_ACK, s_tcp_snd_nxt, s_tcp_rcv_nxt, NULL, 0);
            }
        }
        if (flags & TCP_FIN) {
            s_tcp_rcv_nxt = seq + 1;
            s_tcp_state = TCP_STATE_CLOSE_WAIT;
            (void)TcpSendSeg(TCP_ACK, s_tcp_snd_nxt, s_tcp_rcv_nxt, NULL, 0);
        }
        return;
    }
}

int ProtoTcpSend(const u8 *data, u32 len) {
    if (s_tcp_state != TCP_STATE_ESTAB && s_tcp_state != TCP_STATE_CLOSE_WAIT)
        return -2;
    if (len > 1400)
        len = 1400;
    int r = TcpSendSeg(TCP_ACK | TCP_PSH, s_tcp_snd_nxt, s_tcp_rcv_nxt,
                         data, len);
    if (r == 0)
        s_tcp_snd_nxt += len;
    return r;
}

/* Block for one received segment (~6 s). */
int ProtoTcpRecv(u8 *data, u32 max, u16 *peer_port) {
    extern void NetYield(void);
    for (int i = 0; i < 600; i++) {
        NetRxPumpNow();
        u8 f[1600];
        u32 fl;
        while (NetRxPump(f, &fl)) {
            if (fl >= 14 + IP_HDR_LEN + TCP_HDR_LEN && f[12] == 0x08 && f[13] == 0x00) {
                const u8 *ip = f + 14;
                if (ip[9] == IP_PROTO_TCP) {
                    const u8 *t = ip + IP_HDR_LEN;
                    u16 dport = (u16)((t[2] << 8) | t[3]);
                    if (dport == s_tcp_lport)
                        ProtoTcpRxTcp(f, fl);
                }
            }
            ProtoRx(f, fl);
        }
        if (s_tcp_rx_count > 0) {
            tcp_pkt_t *p = &s_tcp_rxq[s_tcp_rx_head];
            u32 n = p->len;
            if (n > max)
                n = max;
            memcpy(data, p->data, n);
            if (peer_port)
                *peer_port = p->sport;
            s_tcp_rx_head = (s_tcp_rx_head + 1) % TCP_RXQ_MAX;
            s_tcp_rx_count--;
            return (int)n;
        }
        if (s_tcp_state == TCP_STATE_CLOSE_WAIT && s_tcp_rx_count == 0) {
            /* Peer closed and drained: report EOF. */
            return 0;
        }
        NetYield();
    }
    return -7;
}
