#include "packet.h"

#include <cstring>

#include <rte_tcp.h>
#include <rte_udp.h>

namespace fwd {

bool is_broadcast_or_multicast(const rte_ether_addr& mac) {
    // 第一个字节的最低位为 1 表示组播；全 F 是广播
    if ((mac.addr_bytes[0] & 0x01) != 0) {
        return true;
    }
    static constexpr uint8_t kBroadcast[RTE_ETHER_ADDR_LEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    return std::memcmp(mac.addr_bytes, kBroadcast, RTE_ETHER_ADDR_LEN) == 0;
}

bool parse_packet(const rte_mbuf* m, PacketInfo& info) {
    info = PacketInfo{};

    // 用线性指针访问连续数据区；分包链的情况留到后续里程碑处理
    const auto* data = rte_pktmbuf_mtod(m, const uint8_t*);
    const uint32_t len = rte_pktmbuf_data_len(m);

    if (len < sizeof(rte_ether_hdr)) {
        return false;
    }

    const auto* eth = reinterpret_cast<const rte_ether_hdr*>(data);
    info.eth = eth;
    info.ether_type = rte_be_to_cpu_16(eth->ether_type);

    if (info.ether_type != RTE_ETHER_TYPE_IPV4) {
        return true;  // 非 IPv4（比如 ARP / IPv6），解析到此为止
    }

    if (len < sizeof(rte_ether_hdr) + sizeof(rte_ipv4_hdr)) {
        return true;
    }

    const auto* ip = reinterpret_cast<const rte_ipv4_hdr*>(data + sizeof(rte_ether_hdr));
    if ((ip->version_ihl >> 4) != 4) {
        return true;
    }

    info.has_ipv4 = true;
    info.ipv4 = ip;
    info.ip_proto = ip->next_proto_id;
    info.src_ip = rte_be_to_cpu_32(ip->src_addr);
    info.dst_ip = rte_be_to_cpu_32(ip->dst_addr);

    const uint32_t ip_hdr_len = static_cast<uint32_t>(ip->version_ihl & 0x0F) * 4U;
    if (ip_hdr_len < sizeof(rte_ipv4_hdr)) {
        return true;
    }

    const uint8_t* l4 = reinterpret_cast<const uint8_t*>(ip) + ip_hdr_len;
    const uint32_t l4_offset = static_cast<uint32_t>(l4 - data);

    if (info.ip_proto == IPPROTO_TCP) {
        if (len < l4_offset + sizeof(rte_tcp_hdr)) {
            return true;
        }
        const auto* tcp = reinterpret_cast<const rte_tcp_hdr*>(l4);
        info.has_l4 = true;
        info.src_port = rte_be_to_cpu_16(tcp->src_port);
        info.dst_port = rte_be_to_cpu_16(tcp->dst_port);
    } else if (info.ip_proto == IPPROTO_UDP) {
        if (len < l4_offset + sizeof(rte_udp_hdr)) {
            return true;
        }
        const auto* udp = reinterpret_cast<const rte_udp_hdr*>(l4);
        info.has_l4 = true;
        info.src_port = rte_be_to_cpu_16(udp->src_port);
        info.dst_port = rte_be_to_cpu_16(udp->dst_port);
    }

    return true;
}

}  // namespace fwd
