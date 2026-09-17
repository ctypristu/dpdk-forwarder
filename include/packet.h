#pragma once

#include <cstdint>

#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mbuf.h>

namespace fwd {

// 一个报文里我们关心的字段。只在解析成功时有效。
struct PacketInfo {
    const rte_ether_hdr* eth = nullptr;
    uint16_t ether_type = 0;

    bool has_ipv4 = false;
    const rte_ipv4_hdr* ipv4 = nullptr;
    uint8_t ip_proto = 0;
    uint32_t src_ip = 0;      // 主机字节序
    uint32_t dst_ip = 0;      // 主机字节序

    bool has_l4 = false;
    uint16_t src_port = 0;    // 主机字节序
    uint16_t dst_port = 0;    // 主机字节序
};

// 解析以太网头与（可选的）IPv4 / TCP / UDP 头。
// 返回 false 表示报文太短，连以太网头都不完整。
bool parse_packet(const rte_mbuf* m, PacketInfo& info);

// 判断目的 MAC 是不是广播或多播
bool is_broadcast_or_multicast(const rte_ether_addr& mac);

}  // namespace fwd
