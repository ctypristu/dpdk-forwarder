// dpdk-forwarder —— 基于 DPDK 的用户态报文转发与 ACL 过滤系统
//
// 当前阶段（里程碑第 2 周）：端口初始化 + 收发主循环 + 实时速率统计。
// 转发逻辑、ACL、会话表按 docs/项目规划书.md 的里程碑逐步加入。

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cinttypes>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_timer.h>

#include "common.h"
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

// 把字节数换算成便于阅读的带宽文本
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
    uint64_t pkts = 0;
    uint64_t bytes = 0;
};

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

    const uint16_t port_count = rte_eth_dev_count_avail();
    std::printf("=== dpdk-forwarder ===\n");
    std::printf("可用端口数: %u\n", port_count);
    if (port_count == 0) {
        rte_exit(EXIT_FAILURE,
                 "没有可用端口。真实环境请先绑定网卡，WSL 里可用 "
                 "--vdev=net_tap0 创建虚拟端口。\n");
    }

    rte_mempool* pool = create_mempool();
    if (pool == nullptr) {
        rte_exit(EXIT_FAILURE, "内存池创建失败\n");
    }
    std::printf("mbuf 内存池: %u 个 mbuf，每个 %u 字节，cache %u\n",
                fwd::kDefaultNbMbufs,
                static_cast<unsigned>(RTE_MBUF_DEFAULT_BUF_SIZE),
                fwd::kDefaultMbufCacheSize);

    // 目前只使用第一个端口：先验证收包路径，后续再做端口间转发
    fwd::PortConfig cfg;
    cfg.port_id = 0;
    cfg.nb_rx_queue = fwd::kDefaultNbRxQueue;
    cfg.nb_tx_queue = fwd::kDefaultNbTxQueue;
    cfg.nb_rx_desc = fwd::kDefaultRxDesc;
    cfg.nb_tx_desc = fwd::kDefaultTxDesc;

    fwd::Port port(cfg);
    const int init_ret = port.init(pool);
    if (init_ret < 0) {
        port.stop();
        rte_exit(EXIT_FAILURE, "端口初始化失败\n");
    }
    port.start();
    port.dump_info();

    std::printf("\n开始收包（Ctrl+C 退出）...\n\n");

    rte_mbuf* bufs[fwd::kDefaultBurstSize];
    const uint64_t ticks_per_sec = rte_get_timer_hz();
    uint64_t last_report = rte_get_timer_cycles();

    Counters cur;
    Counters prev;

    while (g_stop == 0) {
        const uint16_t nb_rx = port.rx_burst(bufs, fwd::kDefaultBurstSize);
        if (nb_rx > 0) {
            for (uint16_t i = 0; i < nb_rx; ++i) {
                cur.bytes += rte_pktmbuf_pkt_len(bufs[i]);
            }
            cur.pkts += nb_rx;

            // 当前阶段先把包释放掉，只做收包与统计。
            // 下一步会替换成 ACK → ACL 匹配 → 转发的处理链。
            rte_pktmbuf_free_bulk(bufs, nb_rx);
        }

        const uint64_t now = rte_get_timer_cycles();
        if (now - last_report >= ticks_per_sec * fwd::kStatsIntervalSec) {
            const double elapsed =
                static_cast<double>(now - last_report) / static_cast<double>(ticks_per_sec);
            const uint64_t d_pkts = cur.pkts - prev.pkts;
            const uint64_t d_bytes = cur.bytes - prev.bytes;

            char rate[32];
            format_rate(static_cast<double>(d_bytes) / elapsed, rate, sizeof(rate));

            std::printf("RX: %" PRIu64 " pkt/s  %s  (累计 %" PRIu64 " 包 / %" PRIu64 " 字节)\n",
                        static_cast<uint64_t>(static_cast<double>(d_pkts) / elapsed),
                        rate, cur.pkts, cur.bytes);
            std::fflush(stdout);

            prev = cur;
            last_report = now;
        }
    }

    std::printf("\n收到退出信号，正在停止...\n");
    port.stop();

    rte_mempool_free(pool);
    rte_eal_cleanup();
    std::printf("已退出。\n");
    return 0;
}
