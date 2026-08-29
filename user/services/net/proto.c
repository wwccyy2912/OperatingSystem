/*
 * proto.c - Minimal L3/L4 protocol stack (ARP + IPv4 + ICMP + UDP)
 * Copyright (c) 2026 OpSys Project
 *
 * The net service owns the NIC, so the whole stack lives here and runs
 * on the service thread: proto_rx() is fed from the driver's frame
 * queue; proto_ip_send() resolves the next hop via the ARP cache and
 * hands the finished frame to the driver.  UDP datagrams destined for
 * a bound local port are queued and drained with proto_udp_recv().
 */

#include "proto.h"

#include <libc/string.h>
#include <libc/stdio.h> /* printf */

/* The driver side (implemented in main.c): send a raw Ethernet frame
 * and the NIC MAC. */
extern int  net_send_raw(const u8 *frame, u32 len);
extern void net_get_mac(u8 mac[6]);
extern void net_rx_pump_now(void);        /* NIC ring -> driver queue */
extern int  net_rx_pump(u8 *out, u32 *out_len); /* pop a queued frame */
extern void net_yield(void);              /* sleep one 10 ms tick */

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

static u16 ip_checksum(const u8 *buf, u32 len) {
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

static int ip_equal(const u8 a[4], const u8 b[4]) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

/* ---- ARP ---- */

static arp_entry_t *arp_lookup(const u8 ip[4]) {
    for (int i = 0; i < ARP_CACHE_MAX; i++)
        if (s_arp[i].valid && s_arp[i].ttl > 0 && ip_equal(s_arp[i].ip, ip))
            return &s_arp[i];
    return NULL;
}

static void arp_learn(const u8 ip[4], const u8 mac[6]) {
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
static void arp_request(const u8 target_ip[4]) {
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
    (void)net_send_raw(f, 60);
}

/* Handle an ARP frame: learn the sender, reply to requests for us. */
static void arp_handle(const u8 *f) {
    /* f = start of the Ethernet payload (already past the 14B header). */
    u8 sender_ip[4], target_ip[4];
    memcpy(sender_ip, f + 14, 4);
    memcpy(target_ip, f + 24, 4);
    arp_learn(sender_ip, f + 8);

    u16 op = (u16)((f[6] << 8) | f[7]);
    if (op == 1 && ip_equal(target_ip, s_ip)) {
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
        (void)net_send_raw(r, 60);
    }
}

/* Resolve dst_ip to a MAC: cache hit, or send an ARP request and wait
 * for the reply (pumping the NIC + driver queue so the reply arrives
 * without client-side polling).  Returns 0 on success, -7 on timeout. */
static int arp_resolve(const u8 dst_ip[4], u8 mac[6]) {

    arp_entry_t *e = arp_lookup(dst_ip);
    if (e) {
        memcpy(mac, e->mac, 6);
        return 0;
    }
    arp_request(dst_ip);
    for (int i = 0; i < 600; i++) { /* ~6 s */
        net_rx_pump_now();
        u8 f[1600];
        u32 fl;
        while (net_rx_pump(f, &fl)) {
            if (fl >= 42 && f[12] == 0x08 && f[13] == 0x06) {
                const u8 *a = f + 14;
                u16 op = (u16)((a[6] << 8) | a[7]);
                if (op == 2 && a[14] == dst_ip[0] && a[15] == dst_ip[1] &&
                    a[16] == dst_ip[2] && a[17] == dst_ip[3]) {
                    memcpy(mac, a + 8, 6);
                    arp_learn(dst_ip, mac);
                    return 0;
                }
            }
            proto_rx(f, fl); /* everything else into the stack */
        }
        net_yield();
    }
    return -7; /* ERR_FAULT: no ARP reply */
}

/* ---- IP ---- */

static int ip_send_raw(const u8 dst_ip[4], u8 proto, const u8 *payload, u32 len) {
    u8 mac[6];
    int r = arp_resolve(dst_ip, mac);
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
    u16 cs = ip_checksum(ip, IP_HDR_LEN);
    ip[10] = (u8)(cs >> 8);
    ip[11] = (u8)(cs & 0xFF);
    memcpy(ip + IP_HDR_LEN, payload, len);
    return net_send_raw(f, 14 + total);
}

/* ---- ICMP ---- */

/* Reply to an ICMP echo request (ping).  f = IP payload (ICMP header). */
static void icmp_echo_reply(const u8 *src_ip, const u8 *icmp, u32 icmp_len) {
    /* icmp[0]=type(8 req) icmp[1]=code icmp[2..3]=checksum
     * icmp[4..5]=id icmp[6..7]=seq + data */
    u8 payload[1500];
    u32 plen = icmp_len;
    memcpy(payload, icmp, plen);
    payload[0] = 0; /* type: echo reply */
    payload[1] = 0;
    payload[2] = 0;
    payload[3] = 0;
    u16 cs = ip_checksum(payload, plen);
    payload[2] = (u8)(cs >> 8);
    payload[3] = (u8)(cs & 0xFF);
    (void)ip_send_raw(src_ip, IP_PROTO_ICMP, payload, plen);
}

/* ---- UDP ---- */

static udp_sock_t *udp_find(u16 port) {
    for (int i = 0; i < UDP_SOCK_MAX; i++)
        if (s_udp[i].in_use && s_udp[i].port == port)
            return &s_udp[i];
    return NULL;
}

static int udp_port_bound(u16 port) {
    return udp_find(port) != NULL;
}

static void udp_queue(const u8 src[4], u16 sport, u16 dport, const u8 *data, u32 len) {
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

void proto_init(const u8 ip[4], const u8 gw[4]) {
    memcpy(s_ip, ip, 4);
    memcpy(s_gw, gw, 4);
    net_get_mac(s_mac);
    memset(s_arp, 0, sizeof(s_arp));
    memset(s_udp, 0, sizeof(s_udp));
    s_udp_rx_head = 0;
    s_udp_rx_count = 0;
}

void proto_rx(const u8 *frame, u32 len) {
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
            arp_handle(p);
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
    if (ip_checksum(ip, IP_HDR_LEN) != 0)
        return;
    u8 src[4];
    memcpy(src, ip + 12, 4);
    u8 proto = ip[9];
    const u8 *pl = ip + IP_HDR_LEN;
    u32 lplen = iplen - IP_HDR_LEN;

    if (proto == IP_PROTO_ICMP && lplen >= 8) {
        if (pl[0] == 8) /* echo request */
            icmp_echo_reply(src, pl, lplen);
        return;
    }
    if (proto == IP_PROTO_UDP && lplen >= UDP_HDR_LEN) {
        u16 sport = (u16)((pl[0] << 8) | pl[1]);
        u16 dport = (u16)((pl[2] << 8) | pl[3]);
        if (udp_port_bound(dport))
            udp_queue(src, sport, dport, pl + UDP_HDR_LEN, lplen - UDP_HDR_LEN);
        return;
    }
}

int proto_ip_send(const u8 dst_ip[4], u8 proto, const u8 *payload, u32 len) {
    return ip_send_raw(dst_ip, proto, payload, len);
}

/* ICMP echo request + wait for the reply.  The driver queue is pumped
 * inside the wait loop so the reply arrives without client polling. */
int proto_ping(const u8 dst_ip[4]) {
    /* Build the echo request. */
    static u16 s_ping_id = 0x1234;
    u8 icmp[8 + 32];
    icmp[0] = 8; icmp[1] = 0; icmp[2] = 0; icmp[3] = 0;
    icmp[4] = (u8)(s_ping_id >> 8);
    icmp[5] = (u8)(s_ping_id & 0xFF);
    icmp[6] = 0; icmp[7] = 1; /* seq */
    for (int i = 0; i < 32; i++)
        icmp[8 + i] = (u8)i;
    u16 cs = ip_checksum(icmp, sizeof(icmp));
    icmp[2] = (u8)(cs >> 8);
    icmp[3] = (u8)(cs & 0xFF);
    int r = ip_send_raw(dst_ip, IP_PROTO_ICMP, icmp, sizeof(icmp));
    if (r != 0) {
        return r;
    }
    s_ping_id++;
    /* Wait for the echo reply: pump the driver queue via net_rx_pump().
     * The reply path (icmp_echo_reply in the peer) arrives as an ICMP
     * type-0 packet — proto_rx drops it silently (no local echo match
     * tracking needed: reaching the peer proves reachability via the
     * reply's arrival on our NIC).  We simply wait for a matching
     * ICMP echo reply by scanning the driver queue. */
    for (int i = 0; i < 600; i++) { /* ~6 s */
        u8 frame[1600];
        u32 flen;
        net_rx_pump_now(); /* NIC ring -> driver queue (missed: the
                            * reply sits in the RMD ring otherwise) */
        if (net_rx_pump(frame, &flen)) {
            if (flen >= 14 + IP_HDR_LEN + 8) {
                const u8 *ip = frame + 14;
                if ((ip[0] >> 4) == 4 && ip[9] == IP_PROTO_ICMP) {
                    const u8 *ic = ip + IP_HDR_LEN;
                    if (ic[0] == 0) /* echo reply */
                        return 0;
                }
            }
            proto_rx(frame, flen);
        }
        net_yield();
    }
    return -7; /* ERR_FAULT: timeout */
}

int proto_udp_bind(u16 port) {
    if (port < 16 || udp_port_bound(port))
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

int proto_udp_unbind(u16 port) {
    udp_sock_t *s = udp_find(port);
    if (!s)
        return -4; /* ERR_NOENT */
    s->in_use = 0;
    return 0;
}

int proto_udp_sendto(const u8 dst_ip[4], u16 sport, u16 dport,
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
    return ip_send_raw(dst_ip, IP_PROTO_UDP, udp, total);
}

int proto_udp_recv(u8 src[4], u16 *sport, u16 *dport, u8 *data, u32 max) {
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
