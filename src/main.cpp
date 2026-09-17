// dpdk-forwarder —— 基于 DPDK 的用户态报文转发与 ACL 过滤系统
//
// 里程碑进度：
//   第 2 周  端口初始化、mempool、收包与速率统计
//   第 3 周  报文解析、MAC 表学习与二层转发   ← 当前
//
// 下一步：ACL 规则匹配与会话跟踪（第 4-5 周）

#include <csignal>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_timer.h>

#include "common.h"
#include "mac_table.h"
#include "packet.h"
#include "port.h"

namespace {

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int /*signo*/) { g_stop = 1; }

rte_mempool* create_mempool() {
    rte_mempool* pool = rte_pktmbuf_pool_create(
        "MBUF_POOL",
        fwd::kDefaultNbMbufs,
        fwd::kDefaultMbufCacheSize,
        0,
        RTE_MBUF_DEFAULT_BUF_SIZE,
        static_cast<int>(rte_socket_id()));
    if (pool == nullptr) {
        std::fprintf(stderr, "创建 mbuf 内存池失败: %s\n", rte_strerror(rte_errno));
    }
    return pool;
}

void format_rate(double bytes_per_sec, char* out, size_t out_len) {
    const double bits = bytes_per_sec * 8.0;
    if (bits >= 1e9) {
        std::snprintf(out, out_len, "%.2f Gbps", bits / 1e9);
    } else if (bits >= 1e6) {
        std::snprintf(out, out_len, "%.2f Mbps", bits / 1e6);
    } else if (bits >= 1e3) {
        std::snprintf(out, out_len, "%.2f Kbps", bits / 1e3);
    } else {
        std::snprintf(out, out_len, "%.2f bps", bits);
    }
}

struct Counters {
    uint64_t rx_pkts = 0;
    uint64_t rx_bytes = 0;
    uint64_t tx_pkts = 0;
    uint64_t tx_bytes = 0;
    uint64_t flooded = 0;   // 目的 MAC 未知，泛洪的次数
    uint64_t dropped = 0;   // 发送失败或无出口
};

// 通道号到端口号的映射：只使用配置里给定的一组端口
struct Forwarder {
    fwd::Port* ports = nullptr;
    uint16_t port_count = 0;
    fwd::MacTable mac_table;
    Counters stats;
};

// 把一个包从指定端口发出去。失败则释放。
void send_on_port(Forwarder& fw, uint16_t out_port, rte_mbuf* m) {
    fwd::Port& port = fw.ports[out_port];
    if (port.tx_burst(&m, 1) == 1) {
        fw.stats.tx_pkts += 1;
        fw.stats.tx_bytes += rte_pktmbuf_pkt_len(m);
    } else {
        fw.stats.dropped += 1;
        rte_pktmbuf_free(m);
    }
}

// 泛洪：从除入端口以外的所有端口发出去
void flood(Forwarder& fw, uint16_t in_port, rte_mbuf* m) {
    fw.stats.flooded += 1;

    for (uint16_t p = 0; p < fw.port_count; ++p) {
        if (p == in_port) {
            continue;
        }
        // 最后一个出口直接用原 mbuf，前面的出口都要拷贝
        const bool is_last = (p == fw.port_count - 1) ||
                             (in_port == fw.port_count - 1 && p == fw.port_count - 2);
        if (is_last) {
            send_on_port(fw, p, m);
            return;
        }
        rte_mbuf* copy = rte_pktmbuf_clone(m, m->pool);
        if (copy != nullptr) {
            send_on_port(fw, p, copy);
        }
    }
    // 所有出口都发过了还没 return，说明没找到"最后一个"，直接释放
    rte_pktmbuf_free(m);
}

// 处理一个收到的包：学习源地址 → 查目的地址 → 转发
void handle_packet(Forwarder& fw, uint16_t in_port, rte_mbuf* m, uint64_t now_sec) {
    fwd::PacketInfo info;
    if (!fwd::parse_packet(m, info)) {
        fw.stats.dropped += 1;
        rte_pktmbuf_free(m);
        return;
    }

    // 1) 学习源 MAC 与入端口的关系
    fw.mac_table.learn(info.eth->src_addr, in_port, now_sec);

    // 2) 组播/广播直接泛洪（交换机的基本行为）
    if (fwd::is_broadcast_or_multicast(info.eth->dst_addr)) {
        flood(fw, in_port, m);
        return;
    }

    // 3) 查目的 MAC
    uint16_t out_port = 0;
    if (!fw.mac_table.lookup(info.eth->dst_addr, &out_port, now_sec)) {
        flood(fw, in_port, m);  // 未知单播：泛洪
        return;
    }

    if (out_port == in_port) {
        // 出口就是入端口，说明目的地在本端口后面，丢弃避免回环
        fw.stats.dropped += 1;
        rte_pktmbuf_free(m);
        return;
    }

    send_on_port(fw, out_port, m);
}

}  // namespace

int main(int argc, char** argv) {
    const int ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "EAL 初始化失败\n");
    }
    argc -= ret;
    argv += ret;

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    const uint16_t avail = rte_eth_dev_count_avail();
    std::printf("=== dpdk-forwarder ===\n");
    std::printf("可用端口数: %u\n", avail);
    if (avail < 2) {
        rte_exit(EXIT_FAILURE,
                 "二层转发至少需要 2 个端口。\n"
                 "WSL 里可以这样起两个虚拟端口：\n"
                 "  --vdev=net_tap0,iface=dtap0 --vdev=net_tap1,iface=dtap1\n");
    }

    rte_mempool* pool = create_mempool();
    if (pool == nullptr) {
        rte_exit(EXIT_FAILURE, "内存池创建失败\n");
    }
    std::printf("mbuf 内存池: %u 个，每个 %u 字节，cache %u\n",
                fwd::kDefaultNbMbufs,
                static_cast<unsigned>(RTE_MBUF_DEFAULT_BUF_SIZE),
                fwd::kDefaultMbufCacheSize);

    // 使用前两个端口做转发
    const uint16_t use_ports = avail >= 2 ? 2 : avail;
    auto* port_objs = new fwd::Port[use_ports];
    for (uint16_t i = 0; i < use_ports; ++i) {
        fwd::PortConfig cfg;
        cfg.port_id = i;
        cfg.nb_rx_queue = fwd::kDefaultNbRxQueue;
        cfg.nb_tx_queue = fwd::kDefaultNbTxQueue;
        cfg.nb_rx_desc = fwd::kDefaultRxDesc;
        cfg.nb_tx_desc = fwd::kDefaultTxDesc;

        port_objs[i] = fwd::Port(cfg);
        if (port_objs[i].init(pool) < 0) {
            rte_exit(EXIT_FAILURE, "端口 %u 初始化失败\n", i);
        }
    }
    for (uint16_t i = 0; i < use_ports; ++i) {
        port_objs[i].start();
    }

    std::printf("\n转发端口:\n");
    for (uint16_t i = 0; i < use_ports; ++i) {
        port_objs[i].dump_info();
    }

    std::printf("\n开始转发（Ctrl+C 退出）...\n\n");

    Forwarder fw;
    fw.ports = port_objs;
    fw.port_count = use_ports;

    rte_mbuf* bufs[fwd::kDefaultBurstSize];
    const uint64_t ticks_per_sec = rte_get_timer_hz();
    uint64_t last_report = rte_get_timer_cycles();
    uint64_t last_age = last_report;
    Counters prev;

    while (g_stop == 0) {
        for (uint16_t pid = 0; pid < fw.port_count; ++pid) {
            const uint16_t nb_rx = fw.ports[pid].rx_burst(bufs, fwd::kDefaultBurstSize);
            if (nb_rx == 0) {
                continue;
            }

            const uint64_t now_sec = rte_get_timer_cycles() / ticks_per_sec;
            for (uint16_t i = 0; i < nb_rx; ++i) {
                fw.stats.rx_pkts += 1;
                fw.stats.rx_bytes += rte_pktmbuf_pkt_len(bufs[i]);
                handle_packet(fw, pid, bufs[i], now_sec);
            }
        }

        const uint64_t now = rte_get_timer_cycles();

        // MAC 表老化，每分钟一次
        if (now - last_age >= ticks_per_sec * 60) {
            last_age = now;
            const uint32_t removed = fw.mac_table.age(now / ticks_per_sec);
            if (removed > 0) {
                std::printf("[老化] 清除 %u 条 MAC 表项，当前 %u 条\n",
                            removed, fw.mac_table.size());
            }
        }

        if (now - last_report >= ticks_per_sec * fwd::kStatsIntervalSec) {
            const double elapsed =
                static_cast<double>(now - last_report) / static_cast<double>(ticks_per_sec);
            const uint64_t d_rx = fw.stats.rx_pkts - prev.rx_pkts;
            const uint64_t d_rx_bytes = fw.stats.rx_bytes - prev.rx_bytes;
            const uint64_t d_tx = fw.stats.tx_pkts - prev.tx_pkts;

            char rx_rate[32];
            format_rate(static_cast<double>(d_rx_bytes) / elapsed, rx_rate, sizeof(rx_rate));

            std::printf("RX %" PRIu64 " pkt/s (%s) | TX %" PRIu64 " pkt/s | "
                        "MAC 表 %u | 泛洪 %" PRIu64 " | 丢弃 %" PRIu64 "\n",
                        static_cast<uint64_t>(static_cast<double>(d_rx) / elapsed),
                        rx_rate,
                        static_cast<uint64_t>(static_cast<double>(d_tx) / elapsed),
                        fw.mac_table.size(),
                        fw.stats.flooded,
                        fw.stats.dropped);
            std::fflush(stdout);

            prev = fw.stats;
            last_report = now;
        }
    }

    std::printf("\n收到退出信号，正在停止...\n");
    std::printf("累计: RX %" PRIu64 " 包 / %" PRIu64 " 字节，TX %" PRIu64 " 包 / %" PRIu64 " 字节\n",
                fw.stats.rx_pkts, fw.stats.rx_bytes, fw.stats.tx_pkts, fw.stats.tx_bytes);
    std::printf("     泛洪 %" PRIu64 " 次，丢弃 %" PRIu64 " 个包，MAC 表 %u 条\n",
                fw.stats.flooded, fw.stats.dropped, fw.mac_table.size());

    for (uint16_t i = 0; i < use_ports; ++i) {
        port_objs[i].stop();
    }
    delete[] port_objs;

    rte_mempool_free(pool);
    rte_eal_cleanup();
    std::printf("已退出。\n");
    return 0;
}
