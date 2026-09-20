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
 * cmd_net.c - networking commands
 * Copyright (c) 2026 OpSys Project
 *
 *   ip [show|mac]                 interface state (address, gateway, MAC)
 *   ip set <a.b.c.d> [gw]         static address configuration
 *   netstat                       counters + interface summary
 *   udp bind|send|recv ...        raw UDP datagrams
 *   dns <name> [server]           A-record lookup (UDP/53)
 *   http <ip> <port> [path]       HTTP/1.0 GET over TCP
 *
 * The low-level `net` diagnostics (mac / arp / ping / tcp / recv /
 * stats) stay in shell.c; these are the higher-level tools built on the
 * same service.  Everything goes through the `net` port: the shell has
 * no PCI capability and never touches the NIC.
 *
 * ------------------------------------------------------------------
 * Structure (net service client + two protocol clients):
 *   NetCall(op, req, req_len, resp)   one request/reply over "net"
 *   NetPort()                         lazy port resolution
 *   CmdIp()   -> NET_OP_GET_IP / NET_OP_SET_IP
 *   CmdUdp()  -> NET_OP_UDP_BIND / UDP_SENDTO / UDP_RECV / UDP_UNBIND
 *   CmdDns()  -> builds a DNS query, sends it over UDP, parses the answer
 *   CmdHttp() -> NET_OP_TCP_CONNECT + TCP_SEND + TCP_RECV + TCP_CLOSE
 * How it works:
 *   All calls share one static request/response pair (net_req_t /
 *   net_resp_t are ~1.5 KB each, which is why they are static: a user
 *   thread only has 16 KiB of stack).  dns and http implement their
 *   protocol on top of the raw UDP/TCP primitives the service exposes,
 *   so no network logic lives in the shell beyond framing and parsing.
 * Purpose:
 *   Make the stack usable: see the interface, resolve a name, fetch an
 *   HTTP object, exchange a datagram — the things a "network tool" is
 *   expected to do.
 * Caveats:
 *   The TCP session is single-connection and server-side timeouts are
 *   short (6 s), so one slow fetch fails wholesale.  DNS needs a real
 *   resolver at the far end (QEMU's slirp answers on 10.0.2.3 and
 *   forwards to the host).  `http` prints the raw response, headers
 *   included; there is no TLS and no redirect handling.
 * ------------------------------------------------------------------
 */

#include "shell.h"

#include <stdarg.h>

#include "../lib/libc/stdio.h"
#include "../lib/libc/stdlib.h"
#include "../lib/libc/string.h"
#include "../lib/libos/syscalls.h"
#include "../net/net.h"

/* ====================================================================
 * Aligned output
 *
 * The shell's own PrintfLine() understands only %d / %x / %s / %c (plus
 * zero-padding) — it has no field widths, so "%-10s" would be emitted
 * literally and the arguments would desynchronize (which is exactly what
 * happened to the first column-aligned tables).  Column layouts here go
 * through libc's full-featured vsnprintf and are then handed to
 * ShellWrite(), which keeps the terminal as the single output path.
 * ==================================================================== */
static void PrintfLine(const char *fmt, ...) {
    char    buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ShellWrite(buf);
}

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t  i32;

/* ====================================================================
 * net service plumbing
 * ==================================================================== */

static int NetPort(void) {
    static int s_port = -2;
    if (s_port >= 0)
        return s_port;
    s_port = PortGet(NET_PORT_NAME);
    return s_port;
}

/* One request/reply.  The buffers are static on purpose (see the file
 * header): net_req_t/net_resp_t are ~1.5 KB each and the shell runs on
 * a 16 KiB stack. */
static int NetCall(u32 op, const void *payload, u32 payload_len, i32 *out_ret, void *out, u32 out_max) {
    static net_req_t  req;
    static net_resp_t resp;

    int port = NetPort();
    if (port < 0) {
        PrintfLine("net: 'net' port unavailable (%d) - is a NIC attached?\n", port);
        return port;
    }

    memset(&req, 0, sizeof(req));
    req.op  = op;
    req.len = payload_len;
    if (payload_len && payload && payload_len <= sizeof(req.data))
        memcpy(req.data, payload, payload_len);

    memset(&resp, 0, sizeof(resp));
    int rlen = (int)sizeof(resp);
    int r    = IpcCall(port, &req, (int)(8 + payload_len), &resp, &rlen);
    if (r < 0) {
        PrintfLine("net: ipc FAILED (%d)\n", r);
        return r;
    }
    if (out_ret)
        *out_ret = resp.ret;
    /* Copy the reply payload.  Services that report a length (RECV,
     * UDP_RECV) get exactly that many bytes; the fixed-layout replies
     * (GET_IP, GET_MAC) leave len at 0 and the caller's out_max decides
     * how much of the data[] union is meaningful. */
    if (out && out_max) {
        u32 n = (resp.len && resp.len < out_max) ? resp.len : out_max;
        memcpy(out, resp.data, n);
    }
    return 0;
}

static int NetGetMac(u8 mac[6]) {
    i32 ret = 0;
    if (NetCall(NET_OP_GET_MAC, NULL, 0, &ret, mac, 6) < 0)
        return -1;
    if (ret < 0) {
        PrintfLine("net: GET_MAC FAILED (%d)\n", ret);
        return -1;
    }
    return 0;
}

/* Parse "a.b.c.d" into 4 bytes.  Returns 0 on success. */
static int ParseIp(const char *s, u8 ip[4]) {
    int part = 0, val = 0, digits = 0;
    for (const char *p = s;; p++) {
        if (*p >= '0' && *p <= '9') {
            val = val * 10 + (*p - '0');
            digits++;
            if (val > 255 || digits > 3)
                return -1;
            continue;
        }
        if (*p == '.' || *p == '\0') {
            if (!digits || part > 3)
                return -1;
            ip[part++] = (u8)val;
            val = 0;
            digits = 0;
            if (*p == '\0')
                break;
            continue;
        }
        return -1;
    }
    return part == 4 ? 0 : -1;
}

static void FmtIp(const u8 ip[4], char *buf, size_t n) {
    snprintf(buf, n, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
}

/* ====================================================================
 * ip - interface configuration
 * ==================================================================== */

static int IpShow(void) {
    u8  mac[6] = {0};
    u8  ip[4]  = {0};
    u8  gw[4]  = {0};
    i32 ret    = 0;

    /* NET_OP_GET_IP answers with { ip[4]; gw[4] } in data[]. */
    u8  both[8] = {0};
    if (NetCall(NET_OP_GET_IP, NULL, 0, &ret, both, sizeof(both)) == 0 && ret >= 0) {
        memcpy(ip, both, 4);
        memcpy(gw, both + 4, 4);
    }

    char ips[16], gws[16], macs[32];
    FmtIp(ip, ips, sizeof(ips));
    FmtIp(gw, gws, sizeof(gws));
    if (NetGetMac(mac) == 0)
        snprintf(macs, sizeof(macs), "%02x:%02x:%02x:%02x:%02x:%02x",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    else
        snprintf(macs, sizeof(macs), "(unavailable)");

    ShellWrite("pcnet0: OpSys Ethernet interface\n");
    PrintfLine("        inet %s  gateway %s\n", ips, gws);
    PrintfLine("        ether %s\n", macs);
    ShellWrite("        status UP (polling driver, no IRQ binding)\n");
    return 0;
}

static int CmdIp(int argc, char *argv[]) {
    if (argc >= 2 && strcmp(argv[1], "mac") == 0) {
        u8 mac[6];
        if (NetGetMac(mac) < 0)
            return -1;
        PrintfLine("hwaddr %02x:%02x:%02x:%02x:%02x:%02x\n",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return 0;
    }

    if (argc < 3 || strcmp(argv[1], "set") != 0)
        return IpShow();

    u8 ip[4], gw[4];
    if (ParseIp(argv[2], ip) < 0) {
        PrintfLine("ip: bad address '%s' (expected a.b.c.d)\n", argv[2]);
        return -1;
    }
    u8 payload[8];
    memcpy(payload, ip, 4);
    if (argc >= 4) {
        if (ParseIp(argv[3], gw) < 0) {
            PrintfLine("ip: bad gateway '%s'\n", argv[3]);
            return -1;
        }
    } else {
        memset(gw, 0, 4);
    }
    memcpy(payload + 4, gw, 4);

    i32 ret = 0;
    if (NetCall(NET_OP_SET_IP, payload, 8, &ret, NULL, 0) < 0)
        return -1;
    if (ret < 0) {
        PrintfLine("ip: SET_IP FAILED (%d)\n", ret);
        return -1;
    }
    char ips[16], gws[16];
    FmtIp(ip, ips, sizeof(ips));
    FmtIp(gw, gws, sizeof(gws));
    PrintfLine("ip: address set to %s (gateway %s)\n", ips, gws);
    ShellWrite("ip: note - the stack still routes to 10.0.2.2 (slirp gateway)\n");
    return 0;
}

/* ====================================================================
 * netstat - interface summary + counters
 * ==================================================================== */

static int CmdNetstat(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    u8  mac[6] = {0};
    u8  both[8] = {0};
    i32 ret = 0;
    (void)NetCall(NET_OP_GET_IP, NULL, 0, &ret, both, sizeof(both));

    char ips[16], gws[16], macs[32];
    FmtIp(both, ips, sizeof(ips));
    FmtIp(both + 4, gws, sizeof(gws));
    if (NetGetMac(mac) == 0)
        snprintf(macs, sizeof(macs), "%02x:%02x:%02x:%02x:%02x:%02x",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    else
        snprintf(macs, sizeof(macs), "(unavailable)");

    ShellWrite("Interface  Address          Gateway          Hardware\n");
    ShellWrite("---------  ---------------  ---------------  -------------------\n");
    PrintfLine("pcnet0     %-15s  %-15s  %s\n", ips, gws, macs);

    /* STATS returns three u32 counters as a text summary from the
     * service (see NET_OP_STATS in net.h). */
    static u8 data[128];
    if (NetCall(NET_OP_STATS, NULL, 0, &ret, data, sizeof(data) - 1) == 0 && ret >= 0) {
        data[sizeof(data) - 1] = '\0';
        ShellWrite("\nCounters\n");
        PrintfLine("  %s\n", (const char *)data);
    } else {
        ShellWrite("\nCounters: unavailable\n");
    }
    return 0;
}

/* ====================================================================
 * udp - datagram send/receive
 * ==================================================================== */

static int UdpSend(int argc, char *argv[]) {
    if (argc < 6) {
        ShellWrite("Usage: udp send <ip> <sport> <dport> <text...>\n");
        return -1;
    }
    u8 ip[4];
    if (ParseIp(argv[2], ip) < 0) {
        PrintfLine("udp: bad address '%s'\n", argv[2]);
        return -1;
    }
    u16 sport = (u16)atoi(argv[3]);
    u16 dport = (u16)atoi(argv[4]);

    /* Join the remaining argv into one payload. */
    static char payload[512];
    int         n = 0;
    for (int i = 5; i < argc && n < (int)sizeof(payload) - 2; i++) {
        if (i > 5)
            payload[n++] = ' ';
        for (const char *p = argv[i]; *p && n < (int)sizeof(payload) - 2; p++)
            payload[n++] = *p;
    }
    payload[n++] = '\n';
    payload[n]   = '\0';

    /* The service expects { ip[4]; sport; dport; data } in one payload;
     * the ports travel in host order inside the first 8 bytes. */
    static u8 req[8 + sizeof(payload)];
    memcpy(req, ip, 4);
    req[4] = (u8)(sport & 0xff);
    req[5] = (u8)(sport >> 8);
    req[6] = (u8)(dport & 0xff);
    req[7] = (u8)(dport >> 8);
    memcpy(req + 8, payload, (size_t)n);

    i32 ret = 0;
    if (NetCall(NET_OP_UDP_SENDTO, req, (u32)(8 + n), &ret, NULL, 0) < 0)
        return -1;
    if (ret < 0) {
        PrintfLine("udp: send FAILED (%d)\n", ret);
        return -1;
    }
    PrintfLine("udp: sent %d byte(s) to %s:%d from port %d\n", n, argv[2], dport, sport);
    return 0;
}

static int UdpRecv(int argc, char *argv[]) {
    if (argc < 3) {
        ShellWrite("Usage: udp recv <port> [tries]\n");
        return -1;
    }
    u16 port  = (u16)atoi(argv[2]);
    int tries = (argc >= 4) ? atoi(argv[3]) : 50;
    if (tries <= 0)
        tries = 50;

    i32 ret = 0;
    u16 pa[2] = {port, 0};
    if (NetCall(NET_OP_UDP_BIND, pa, 2, &ret, NULL, 0) < 0)
        return -1;
    if (ret < 0) {
        PrintfLine("udp: bind %d FAILED (%d)\n", port, ret);
        return -1;
    }

    static u8 data[1024];
    for (int i = 0; i < tries; i++) {
        ret = 0;
        if (NetCall(NET_OP_UDP_RECV, NULL, 0, &ret, data, sizeof(data) - 1) == 0 && ret > 0) {
            data[sizeof(data) - 1] = '\0';
            PrintfLine("udp: received %d byte(s):\n", ret);
            ShellWrite((const char *)data);
            ShellWrite("\n");
            (void)NetCall(NET_OP_UDP_UNBIND, pa, 2, &ret, NULL, 0);
            return 0;
        }
        Sleep(1);
    }
    PrintfLine("udp: no datagram on port %d after %d tries\n", port, tries);
    (void)NetCall(NET_OP_UDP_UNBIND, pa, 2, &ret, NULL, 0);
    return -1;
}

static int CmdUdp(int argc, char *argv[]) {
    if (argc < 2) {
        ShellWrite("Usage: udp <bind <port> | send <ip> <sport> <dport> <text> | recv <port>>\n");
        return -1;
    }
    if (strcmp(argv[1], "bind") == 0) {
        if (argc < 3) {
            ShellWrite("Usage: udp bind <port>\n");
            return -1;
        }
        u16 port  = (u16)atoi(argv[2]);
        u16 pa[1] = {port};
        i32 ret   = 0;
        if (NetCall(NET_OP_UDP_BIND, pa, 2, &ret, NULL, 0) < 0)
            return -1;
        if (ret < 0) {
            PrintfLine("udp: bind %d FAILED (%d)\n", port, ret);
            return -1;
        }
        PrintfLine("udp: bound port %d\n", port);
        return 0;
    }
    if (strcmp(argv[1], "send") == 0)
        return UdpSend(argc, argv);
    if (strcmp(argv[1], "recv") == 0)
        return UdpRecv(argc, argv);
    PrintfLine("udp: unknown subcommand '%s'\n", argv[1]);
    return -1;
}

/* ====================================================================
 * dns - minimal A-record resolver (UDP/53)
 * ==================================================================== */

#define DNS_SERVER_DEFAULT "10.0.2.3" /* slirp's resolver */

static int DnsBuildQuery(const char *name, u8 *buf, u32 max, u16 id) {
    if (strlen(name) + 18 > max)
        return -1;
    memset(buf, 0, 12);
    buf[0] = (u8)(id >> 8);
    buf[1] = (u8)(id & 0xff);
    buf[2] = 0x01; /* standard query, recursion desired */
    buf[5] = 0x01; /* QDCOUNT = 1 */

    u32 n = 12;
    const char *p = name;
    while (*p) {
        const char *dot = p;
        while (*dot && *dot != '.')
            dot++;
        u32 len = (u32)(dot - p);
        if (len == 0 || len > 63 || n + len + 1 >= max)
            return -1;
        buf[n++] = (u8)len;
        memcpy(buf + n, p, len);
        n += len;
        p = (*dot == '.') ? dot + 1 : dot;
    }
    buf[n++] = 0;    /* root label */
    buf[n++] = 0;    /* QTYPE  = A */
    buf[n++] = 1;
    buf[n++] = 0;    /* QCLASS = IN */
    buf[n++] = 1;
    return (int)n;
}

/* Skip a (possibly compressed) name; returns the offset past it. */
static int DnsSkipName(const u8 *buf, int len, int off) {
    while (off < len) {
        u8 l = buf[off];
        if (l == 0)
            return off + 1;
        if ((l & 0xc0) == 0xc0)
            return off + 2; /* compression pointer */
        off += 1 + l;
    }
    return -1;
}

static int CmdDns(int argc, char *argv[]) {
    if (argc < 2) {
        ShellWrite("Usage: dns <name> [server-ip]\n");
        return -1;
    }
    const char *name   = argv[1];
    const char *server = (argc >= 3) ? argv[2] : DNS_SERVER_DEFAULT;

    u8 sip[4];
    if (ParseIp(server, sip) < 0) {
        PrintfLine("dns: bad server address '%s'\n", server);
        return -1;
    }

    static u8 query[256];
    u16       id  = (u16)(GetTime() & 0xffff);
    int       qlen = DnsBuildQuery(name, query, sizeof(query), id);
    if (qlen < 0) {
        PrintfLine("dns: cannot encode '%s'\n", name);
        return -1;
    }

    /* Bind an ephemeral source port, send, then poll for the answer. */
    u16 sport = (u16)(49152 + (id & 0x3fff));
    u16 bindp[1] = {sport};
    i32 ret = 0;
    if (NetCall(NET_OP_UDP_BIND, bindp, 2, &ret, NULL, 0) < 0 || ret < 0)
        return -1;

    static u8 req[8 + sizeof(query)];
    memcpy(req, sip, 4);
    req[4] = (u8)(sport & 0xff);
    req[5] = (u8)(sport >> 8);
    req[6] = 0;    /* dport 53 */
    req[7] = 53;
    memcpy(req + 8, query, (size_t)qlen);

    if (NetCall(NET_OP_UDP_SENDTO, req, (u32)(8 + qlen), &ret, NULL, 0) < 0)
        return -1;
    if (ret < 0) {
        PrintfLine("dns: query send FAILED (%d)\n", ret);
        (void)NetCall(NET_OP_UDP_UNBIND, bindp, 2, &ret, NULL, 0);
        return -1;
    }
    PrintfLine("dns: query for %s sent to %s:53 (id %d)\n", name, server, id);

    static u8 resp[768];
    for (int i = 0; i < 300; i++) {
        ret = 0;
        if (NetCall(NET_OP_UDP_RECV, NULL, 0, &ret, resp, sizeof(resp)) == 0 && ret > 12) {
            int len = ret;
            if (resp[0] != (u8)(id >> 8) || resp[1] != (u8)(id & 0xff)) {
                Sleep(1);
                continue; /* not our transaction */
            }
            u16 ancount = (u16)((resp[6] << 8) | resp[7]);
            if (ancount == 0) {
                PrintfLine("dns: %s - no answer records (NXDOMAIN/empty)\n", name);
                break;
            }
            int off = DnsSkipName(resp, len, 12);
            if (off < 0 || off + 4 > len)
                break;
            off += 4; /* skip QTYPE + QCLASS */
            for (u16 a = 0; a < ancount; a++) {
                off = DnsSkipName(resp, len, off);
                if (off < 0 || off + 10 > len)
                    break;
                u16 type = (u16)((resp[off] << 8) | resp[off + 1]);
                u16 rdlen = (u16)((resp[off + 8] << 8) | resp[off + 9]);
                off += 10;
                if (type == 1 && rdlen == 4 && off + 4 <= len) {
                    char ips[16];
                    FmtIp(resp + off, ips, sizeof(ips));
                    PrintfLine("dns: %s -> %s\n", name, ips);
                    (void)NetCall(NET_OP_UDP_UNBIND, bindp, 2, &ret, NULL, 0);
                    return 0;
                }
                off += rdlen;
            }
            PrintfLine("dns: %s - no A record in the reply\n", name);
            break;
        }
        Sleep(1);
    }
    (void)NetCall(NET_OP_UDP_UNBIND, bindp, 2, &ret, NULL, 0);
    PrintfLine("dns: lookup of %s failed\n", name);
    return -1;
}

/* ====================================================================
 * http - HTTP/1.0 GET over the single TCP connection
 * ==================================================================== */

static int CmdHttp(int argc, char *argv[]) {
    if (argc < 3) {
        ShellWrite("Usage: http <ip> <port> [path]   (default path: /)\n");
        return -1;
    }
    u8 sip[4];
    if (ParseIp(argv[1], sip) < 0) {
        PrintfLine("http: bad address '%s'\n", argv[1]);
        return -1;
    }
    u16         port = (u16)atoi(argv[2]);
    const char *path = (argc >= 4) ? argv[3] : "/";
    if (path[0] != '/') {
        PrintfLine("http: path must start with '/' (got '%s')\n", path);
        return -1;
    }

    /* CONNECT: payload is { ip[4]; port } with the port in host order
     * inside the 8-byte payload. */
    u8  conn[8];
    memcpy(conn, sip, 4);
    conn[4] = (u8)(port & 0xff);
    conn[5] = (u8)(port >> 8);
    conn[6] = 0;
    conn[7] = 0;

    i32 ret = 0;
    PrintfLine("http: connecting to %s:%d ...\n", argv[1], port);
    if (NetCall(NET_OP_TCP_CONNECT, conn, 8, &ret, NULL, 0) < 0)
        return -1;
    if (ret < 0) {
        PrintfLine("http: connect FAILED (%d)%s\n", ret,
                     (ret == -6) ? " - timeout (is a server listening?)" : "");
        return -1;
    }

    static char request[512];
    int         n = snprintf(request, sizeof(request),
                              "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: OpSys/0.9\r\n\r\n",
                              path, argv[1]);
    if (NetCall(NET_OP_TCP_SEND, request, (u32)n, &ret, NULL, 0) < 0 || ret < 0) {
        PrintfLine("http: send FAILED (%d)\n", ret);
        (void)NetCall(NET_OP_TCP_CLOSE, NULL, 0, &ret, NULL, 0);
        return -1;
    }
    PrintfLine("http: GET %s (%d bytes sent), waiting for the reply...\n", path, n);

    static u8 body[2048];
    int       total = 0;
    for (int i = 0; i < 20; i++) {
        ret = 0;
        u32 room = (u32)(sizeof(body) - 1 - (size_t)total);
        if (room == 0)
            break;
        if (NetCall(NET_OP_TCP_RECV, NULL, 0, &ret, body + total, room) == 0 && ret > 0) {
            total += ret;
            if (total >= (int)sizeof(body) - 1)
                break;
        }
    }
    (void)NetCall(NET_OP_TCP_CLOSE, NULL, 0, &ret, NULL, 0);

    if (total == 0) {
        ShellWrite("http: no data received\n");
        return -1;
    }
    body[total] = '\0';
    PrintfLine("http: %d byte(s) received\n", total);
    ShellWrite("---- response ----\n");
    ShellWrite((const char *)body);
    if (total && body[total - 1] != '\n')
        ShellWrite("\n");
    ShellWrite("---- end ----\n");
    return 0;
}

/* ====================================================================
 * Registration
 * ==================================================================== */

void ShellRegisterNetCommands(void) {
    ShellRegisterCommand("ip", "Interface: ip [show|mac] | ip set <a.b.c.d> [gw]", CmdIp);
    ShellRegisterCommand("netstat", "Interface summary + packet counters", CmdNetstat);
    ShellRegisterCommand("udp", "UDP: udp <bind|send|recv> ...", CmdUdp);
    ShellRegisterCommand("dns", "Resolve an A record: dns <name> [server]", CmdDns);
    ShellRegisterCommand("http", "HTTP GET over TCP: http <ip> <port> [path]", CmdHttp);
}
