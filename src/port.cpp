#include "port.h"

#include <cstdio>

#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>

namespace fwd {

namespace {

constexpr uint16_t kRxRingSize = 1024;   // 网卡硬件接收环
constexpr uint16_t kTxRingSize = 1024;   // 网卡硬件发送环
constexpr uint16_t kEthOverhead = RTE_ETHER_HDR_LEN + RTE_ETHER_CRC_LEN;

// 端口默认能力集合：够跑通转发，不做额外 offload，避免不同网卡差异。
uint64_t default_rx_offload() { return RTE_ETH_RX_OFFLOAD_CHECKSUM; }
uint64_t default_tx_offload() { return 0; }

}  // namespace

int Port::init(rte_mempool* pool) {
    const uint16_t pid = cfg_.port_id;

    rte_eth_dev_info dev_info{};
    if (rte_eth_dev_info_get(pid, &dev_info) != 0) {
        std::fprintf(stderr, "port %u: 读取设备信息失败\n", pid);
        return -1;
    }

    rte_eth_conf port_conf{};
    port_conf.rxmode.mtu = RTE_ETHER_MTU;
    port_conf.rxmode.offloads = default_rx_offload();
    port_conf.txmode.offloads = default_tx_offload();

    // 队列数不能超过硬件能力
    if (cfg_.nb_rx_queue > dev_info.max_rx_queues) {
        cfg_.nb_rx_queue = dev_info.max_rx_queues;
    }
    if (cfg_.nb_tx_queue > dev_info.max_tx_queues) {
        cfg_.nb_tx_queue = dev_info.max_tx_queues;
    }

    if (rte_eth_dev_configure(pid, cfg_.nb_rx_queue, cfg_.nb_tx_queue, &port_conf) != 0) {
        std::fprintf(stderr, "port %u: 配置失败\n", pid);
        return -1;
    }

    rte_eth_dev_adjust_nb_rx_tx_desc(pid, &cfg_.nb_rx_desc, &cfg_.nb_tx_desc);

    const uint16_t socket_id = rte_eth_dev_socket_id(pid);

    // 每个队列一套收发环
    for (uint16_t q = 0; q < cfg_.nb_rx_queue; ++q) {
        const int ret = rte_eth_rx_queue_setup(
            pid, q, cfg_.nb_rx_desc, socket_id, nullptr, pool);
        if (ret < 0) {
            std::fprintf(stderr, "port %u: 接收队列 %u 建立失败 (%d)\n", pid, q, ret);
            return ret;
        }
    }

    for (uint16_t q = 0; q < cfg_.nb_tx_queue; ++q) {
        const int ret = rte_eth_tx_queue_setup(
            pid, q, cfg_.nb_tx_desc, socket_id, nullptr);
        if (ret < 0) {
            std::fprintf(stderr, "port %u: 发送队列 %u 建立失败 (%d)\n", pid, q, ret);
            return ret;
        }
    }

    // 混杂模式：转发器通常要处理目的 MAC 不是本机的包
    if (rte_eth_promiscuous_enable(pid) != 0) {
        std::fprintf(stderr, "port %u: 开启混杂模式失败\n", pid);
        return -1;
    }

    return 0;
}

void Port::start() {
    if (rte_eth_dev_start(cfg_.port_id) != 0) {
        std::fprintf(stderr, "port %u: 启动失败\n", cfg_.port_id);
        return;
    }
    started_ = true;
}

void Port::stop() {
    if (!started_) {
        return;
    }
    rte_eth_dev_stop(cfg_.port_id);
    rte_eth_dev_close(cfg_.port_id);
    started_ = false;
}

uint16_t Port::rx_burst(rte_mbuf** pkts, uint16_t max_pkts) {
    return rte_eth_rx_burst(cfg_.port_id, 0, pkts, max_pkts);
}

uint16_t Port::tx_burst(rte_mbuf** pkts, uint16_t nb_pkts) {
    return rte_eth_tx_burst(cfg_.port_id, 0, pkts, nb_pkts);
}

void Port::dump_info() const {
    const uint16_t pid = cfg_.port_id;

    rte_eth_dev_info dev_info{};
    if (rte_eth_dev_info_get(pid, &dev_info) != 0) {
        return;
    }

    rte_ether_addr mac{};
    rte_eth_macaddr_get(pid, &mac);

    rte_eth_link link{};
    if (rte_eth_link_get_nowait(pid, &link) < 0) {
        link.link_status = RTE_ETH_LINK_DOWN;
    }

    std::printf("  port %u\n", pid);
    std::printf("    driver    : %s\n", dev_info.driver_name);
    std::printf("    mac       : %02X:%02X:%02X:%02X:%02X:%02X\n",
                mac.addr_bytes[0], mac.addr_bytes[1], mac.addr_bytes[2],
                mac.addr_bytes[3], mac.addr_bytes[4], mac.addr_bytes[5]);
    std::printf("    rx queues : %u (desc %u)\n", cfg_.nb_rx_queue, cfg_.nb_rx_desc);
    std::printf("    tx queues : %u (desc %u)\n", cfg_.nb_tx_queue, cfg_.nb_tx_desc);
    if (link.link_status == RTE_ETH_LINK_UP) {
        std::printf("    link      : UP %u Mbps (duplex %s)\n",
                    link.link_speed,
                    link.link_duplex == RTE_ETH_LINK_FULL_DUPLEX ? "full" : "half");
    } else {
        std::printf("    link      : DOWN\n");
    }
}

}  // namespace fwd
