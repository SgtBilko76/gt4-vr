// SPDX-License-Identifier: GPL-3.0-or-later
#include "dhcp.hpp"

#include <cstdio>
#include <cstring>

namespace ps2::net::dhcp {

namespace {

constexpr std::uint32_t kServer = (10u << 24) | (76u << 16) | 1u;  // 10.76.0.1
constexpr std::uint32_t kMask = 0xFFFF0000u;
constexpr std::uint32_t kLease = 86400;
constexpr std::size_t kEth = 14, kIp = 20, kUdp = 8, kBootp = 236;
constexpr std::uint8_t kDiscover = 1, kOffer = 2, kRequest = 3, kAck = 5;

std::uint16_t get16(const std::uint8_t *p) { return static_cast<std::uint16_t>((p[0] << 8) | p[1]); }
void put16(std::uint8_t *p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 8);
    p[1] = static_cast<std::uint8_t>(v);
}
void put32(std::uint8_t *p, std::uint32_t v) {
    put16(p, v >> 16);
    put16(p + 2, v);
}

std::uint16_t ip_checksum(const std::uint8_t *p, std::size_t n) {
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i + 1 < n; i += 2) sum += get16(p + i);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return static_cast<std::uint16_t>(~sum);
}

}

std::uint32_t address_for(const Mac &mac) {
    std::uint32_t host = (static_cast<std::uint32_t>(mac[4]) << 8) | mac[5];
    if (host < 2 || host == 0xFFFF) host = 2 + mac[3];
    return (kServer & kMask) | host;
}

bool handle(const std::uint8_t *f, std::size_t size, const Mac &mac, std::vector<std::uint8_t> &reply) {
    if (size < kEth + kIp + kUdp + kBootp + 4) return false;
    if (get16(f + 12) != 0x0800) return false;                          // IPv4
    const std::uint8_t *ip = f + kEth;
    const std::size_t ihl = (ip[0] & 0x0Fu) * 4u;
    if ((ip[0] >> 4) != 4 || ip[9] != 17 || ihl < kIp) return false;     // UDP
    const std::uint8_t *udp = ip + ihl;
    if (static_cast<std::size_t>(udp - f) + kUdp + kBootp + 4 > size) return false;
    if (get16(udp) != 68 || get16(udp + 2) != 67) return false;          // client -> server
    const std::uint8_t *bootp = udp + kUdp;
    if (bootp[0] != 1) return false;                                     // BOOTREQUEST
    const std::uint8_t *opt = bootp + kBootp;
    if (opt[0] != 99 || opt[1] != 130 || opt[2] != 83 || opt[3] != 99) return false;
    const std::uint8_t *end = f + size;
    std::uint8_t type = 0;
    for (const std::uint8_t *o = opt + 4; o < end && *o != 255;) {
        if (*o == 0) { ++o; continue; }
        if (o + 2 > end || o + 2 + o[1] > end) break;
        if (o[0] == 53 && o[1] >= 1) type = o[2];
        o += 2 + o[1];
    }
    if (type != kDiscover && type != kRequest) return false;
    const std::uint32_t yours = address_for(mac);

    reply.assign(kEth + kIp + kUdp + kBootp + 64, 0);
    std::uint8_t *r = reply.data();
    std::memset(r, 0xFF, 6);                                             // broadcast to the PS2
    r[6] = 0x02; r[7] = 0x00; r[8] = 0x4C; r[9] = 0x00; r[10] = 0x00; r[11] = 0x01;  // local MAC of the server
    put16(r + 12, 0x0800);
    std::uint8_t *rip = r + kEth;
    std::uint8_t *rudp = rip + kIp;
    std::uint8_t *rb = rudp + kUdp;
    rb[0] = 2;                     // BOOTREPLY
    rb[1] = 1;                     // Ethernet
    rb[2] = 6;
    std::memcpy(rb + 4, bootp + 4, 4);    // xid
    std::memcpy(rb + 10, bootp + 10, 2);  // flags
    put32(rb + 16, yours);                // yiaddr
    put32(rb + 20, kServer);              // siaddr
    std::memcpy(rb + 28, bootp + 28, 16); // chaddr
    std::uint8_t *o = rb + kBootp;
    *o++ = 99; *o++ = 130; *o++ = 83; *o++ = 99;
    *o++ = 53; *o++ = 1; *o++ = type == kDiscover ? kOffer : kAck;
    *o++ = 54; *o++ = 4; put32(o, kServer); o += 4;
    *o++ = 51; *o++ = 4; put32(o, kLease); o += 4;
    *o++ = 58; *o++ = 4; put32(o, kLease / 2); o += 4;
    *o++ = 59; *o++ = 4; put32(o, kLease * 7 / 8); o += 4;
    *o++ = 1; *o++ = 4; put32(o, kMask); o += 4;
    *o++ = 3; *o++ = 4; put32(o, kServer); o += 4;
    *o++ = 6; *o++ = 4; put32(o, kServer); o += 4;
    *o++ = 255;
    const std::size_t bootp_len = static_cast<std::size_t>(o - rb);
    reply.resize(kEth + kIp + kUdp + bootp_len);
    put16(rudp, 67);
    put16(rudp + 2, 68);
    put16(rudp + 4, kUdp + bootp_len);   // UDP checksum 0 = none
    rip[0] = 0x45;
    put16(rip + 2, kIp + kUdp + bootp_len);
    rip[8] = 64;
    rip[9] = 17;
    put32(rip + 12, kServer);
    put32(rip + 16, 0xFFFFFFFFu);
    put16(rip + 10, ip_checksum(rip, kIp));
    if (reply.size() < 60) reply.resize(60, 0);
    std::fprintf(stderr, "[dhcp] %s -> %u.%u.%u.%u\n", type == kDiscover ? "offer" : "ack", yours >> 24,
                 (yours >> 16) & 255, (yours >> 8) & 255, yours & 255);
    return true;
}

}
