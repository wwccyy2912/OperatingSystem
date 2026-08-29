/*
 * net.h - PCnet-Fast III (AMD AM79C973) NIC driver protocol
 * Copyright (c) 2026 OpSys Project
 *
 * The net service owns the PCnet adapter (PCI vendor 0x1022 / device
 * 0x2000), maps its IO BAR + DMA rings (shm pool), and exposes a
 * minimal Ethernet API over IPC:
 *   NET_OP_GET_MAC  -> { mac[6] }
 *   NET_OP_SEND     -> { len; data[] }         -> { ret }
 *   NET_OP_RECV     -> { max }                 -> { ret; len; data[] }
 *                      (non-blocking: ERR_AGAIN when no packet)
 *   NET_OP_STATS    -> { rx; tx; err }
 * Frames are raw Ethernet (14-byte header + payload, no FCS).
 */

#ifndef USER_SERVICES_NET_NET_H
#define USER_SERVICES_NET_NET_H

#include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t  i32;
typedef uint64_t u64;

#define NET_PORT_NAME "net"
#define NET_MTU        1514 /* 14-byte Ethernet header + 1500 payload */

/* Ops */
enum {
    NET_OP_GET_MAC = 1,
    NET_OP_SEND    = 2,
    NET_OP_RECV    = 3,
    NET_OP_STATS   = 4,
    /* ---- L3/L4 protocol ops (phase 4) ---- */
    NET_OP_SET_IP     = 5,  /* { ip[4]; gw[4] }  static address config */
    NET_OP_IP_SEND    = 6,  /* { ip[4]; proto; payload }  raw IP send */
    NET_OP_PING       = 7,  /* { ip[4] }  ICMP echo + wait reply */
    NET_OP_UDP_BIND   = 8,  /* { port }  bind a local UDP port */
    NET_OP_UDP_SENDTO = 9,  /* { ip[4]; sport; dport; data } */
    NET_OP_UDP_RECV   = 10, /* -> { srcip[4]; sport; dport; data } */
    NET_OP_UDP_UNBIND = 11, /* { port } */
};

/* ---- request/response envelopes (fit the 4 KiB IPC limit) ---- */

typedef struct {
    u32 op;
    u32 len; /* payload bytes */
    u8  data[NET_MTU + 16];
} net_req_t;

typedef struct {
    i32 ret;
    u32 len; /* RECV: bytes copied */
    u8  data[NET_MTU + 16];
} net_resp_t;

_Static_assert(sizeof(net_req_t) <= 4096, "net_req_t too big");
_Static_assert(sizeof(net_resp_t) <= 4096, "net_resp_t too big");

/* ---- Ethernet / IP constants ---- */
#define ETH_TYPE_IPV4 0x0800
#define ETH_TYPE_ARP  0x0806
#define IP_PROTO_ICMP 1
#define IP_PROTO_UDP  17
#define IP_HDR_LEN    20
#define UDP_HDR_LEN   8

#endif /* USER_SERVICES_NET_NET_H */
