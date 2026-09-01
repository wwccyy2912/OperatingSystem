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
 * proto.h - Minimal L3/L4 protocol stack (ARP + IPv4 + ICMP + UDP)
 * Copyright (c) 2026 OpSys Project
 *
 * Built on top of the PCnet raw-frame driver: eth frames in -> parsed
 * and dispatched (ARP / ICMP / UDP); IP datagrams out -> ARP-resolved
 * and wrapped.  All functions are called from the net service thread
 * (single-threaded; no locks inside).
 */

#ifndef USER_SERVICES_NET_PROTO_H
#define USER_SERVICES_NET_PROTO_H

#include "net.h"

/* Configure the static address (call once at startup). */
void ProtoInit(const u8 ip[4], const u8 gw[4]);

/* Feed one received Ethernet frame into the stack (from the driver
 * queue).  Handles ARP requests/replies and IP (ICMP echo reply,
 * UDP datagrams queued for the bound ports). */
void ProtoRx(const u8 *frame, u32 len);

/* Send a raw IP payload to dst_ip with the given protocol.
 * Returns 0 / negative error. */
int ProtoIpSend(const u8 dst_ip[4], u8 proto, const u8 *payload, u32 len);

/* ICMP echo request to dst_ip; returns 0 when the reply arrived
 * (within ~1.5 s), -7 on timeout, negative error otherwise. */
int ProtoPing(const u8 dst_ip[4]);

/* Bind a local UDP port (16-65535).  Returns 0 / negative error. */
int ProtoUdpBind(u16 port);
int ProtoUdpUnbind(u16 port);

/* Send a UDP datagram from sport to dst_ip:dport. */
int ProtoUdpSendto(const u8 dst_ip[4], u16 sport, u16 dport,
                     const u8 *data, u32 len);

/* Receive a queued UDP datagram destined for any bound port.
 * Fills src[4], *sport, *dport, data; returns payload length or
 * negative error (ERR_AGAIN when nothing queued). */
int ProtoUdpRecv(u8 src[4], u16 *sport, u16 *dport, u8 *data, u32 max);

#endif /* USER_SERVICES_NET_PROTO_H */

/* ---- TCP (phase 5, single-connection) ---- */
int ProtoTcpListen(u16 port);
int ProtoTcpAccept(u8 peer[4], u16 *peer_port); /* blocking ~6 s */
int ProtoTcpSend(const u8 *data, u32 len);
int ProtoTcpRecv(u8 *data, u32 max, u16 *peer_port); /* blocking */
int ProtoTcpClose(void);
