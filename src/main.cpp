// dpdk-forwarder —— 基于 DPDK 的用户态报文转发与 ACL 过滤系统
//
// 当前为最小骨架：初始化 EAL、枚举端口、打印基本信息。
// 后续按 docs/项目规划书.md 的里程碑逐步填入解析、ACL、会话与转发逻辑。

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_mbuf.h>

namespace {

void print_port_info(uint16_t port_id) {
    rte_eth_dev_info dev_info{};
    if (rte_eth_dev_info_get(port_id, &dev_info) != 0) {
        std::printf("  port %u: 获取设备信息失败\n", port_id);
        return;
    }

    rte_ether_addr mac{};
    rte_eth_macaddr_get(port_id, &mac);

    std::printf("  port %u\n", port_id);
    std::printf("    driver   : %s\n", dev_info.driver_name);
    std::printf("    mac      : %02X:%02X:%02X:%02X:%02X:%02X\n",
                mac.addr_bytes[0], mac.addr_bytes[1], mac.addr_bytes[2],
                mac.addr_bytes[3], mac.addr_bytes[4], mac.addr_bytes[5]);
    std::printf("    rx queue : %u\n", dev_info.max_rx_queues);
    std::printf("    tx queue : %u\n", dev_info.max_tx_queues);
    std::printf("    rx offload: 0x%lx\n",
                static_cast<unsigned long>(dev_info.rx_offload_capa));
    std::printf("    tx offload: 0x%lx\n",
                static_cast<unsigned long>(dev_info.tx_offload_capa));
}

}  // namespace

int main(int argc, char** argv) {
    int ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "EAL 初始化失败\n");
    }
    argc -= ret;
    argv += ret;

    const uint16_t port_count = rte_eth_dev_count_avail();
    std::printf("=== dpdk-forwarder ===\n");
    std::printf("已探测到 %u 个 DPDK 端口\n", port_count);

    for (uint16_t port = 0; port < port_count; ++port) {
        print_port_info(port);
    }

    if (port_count == 0) {
        std::printf("没有可用端口。请先用 scripts/bind_ports.sh 绑定网卡。\n");
    }

    rte_eal_cleanup();
    return 0;
}
