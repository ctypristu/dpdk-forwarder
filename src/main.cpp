// dpdk-forwarder —— 基于 DPDK 的用户态报文转发与 ACL 过滤系统
//
// 里程碑进度：
//   第 2 周  端口初始化、mempool、收包与速率统计
//   第 3 周  报文解析、MAC 表学习与二层转发
//   第 4-5 周  ACL 规则匹配与会话跟踪          ← 当前
//
// 处理模型（快慢路径分流）：
//   首包   → 查 ACL 规则表（慢路径），命中则建会话
//   后续包 → 查会话表（快路径），直接按会话动作处理

#include <arpa/inet.h>

#include <csignal>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_timer.h>

#include "acl.h"
#include "common.h"
#include "mac_table.h"
#include "packet.h"
#include "port.h"
#include "session.h"

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
    uint64_t flooded = 0;
    uint64_t dropped = 0;
    uint64_t acl_hit = 0;       // 首包在 ACL 表里查到规则
    uint64_t acl_default = 0;   // 首包没查到规则，走默认动作
    uint64_t session_hit = 0;   // 后续包命中会话表
    uint64_t denied = 0;        // 被策略拒绝（含会话判定的）
};

struct Forwarder {
    fwd::Port* ports = nullptr;
    uint16_t port_count = 0;
    fwd::MacTable mac_table;
    fwd::AclTable acl;
    fwd::SessionTable sessions;
    Counters stats;
    fwd::AclAction default_action = fwd::AclAction::kPass;
};

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

void flood(Forwarder& fw, uint16_t in_port, rte_mbuf* m) {
    fw.stats.flooded += 1;

    for (uint16_t p = 0; p < fw.port_count; ++p) {
        if (p == in_port) {
            continue;
        }
        // 最后一个出口直接用原 mbuf，避免多余的拷贝
        const bool is_last = (p == fw.port_count - 1) ||
                             (in_port == fw.port_count - 1 && p + 1 == fw.port_count - 1);
        if (is_last) {
            send_on_port(fw, p, m);
            return;
        }
        rte_mbuf* copy = rte_pktmbuf_clone(m, m->pool);
        if (copy != nullptr) {
            send_on_port(fw, p, copy);
        }
    }
    rte_pktmbuf_free(m);
}

// 按会话动作决定这个包怎么处理
void apply_action(Forwarder& fw, uint16_t in_port, rte_mbuf* m, fwd::AclAction action,
                  const fwd::PacketInfo& info, uint64_t now_sec) {
    if (action == fwd::AclAction::kDrop) {
        fw.stats.denied += 1;
        rte_pktmbuf_free(m);
        return;
    }

    // 放行：按二层转发逻辑送出
    if (fwd::is_broadcast_or_multicast(info.eth->dst_addr)) {
        flood(fw, in_port, m);
        return;
    }

    uint16_t out_port = 0;
    if (!fw.mac_table.lookup(info.eth->dst_addr, &out_port, now_sec)) {
        flood(fw, in_port, m);
        return;
    }
    if (out_port == in_port) {
        fw.stats.dropped += 1;
        rte_pktmbuf_free(m);
        return;
    }
    send_on_port(fw, out_port, m);
}

void handle_packet(Forwarder& fw, uint16_t in_port, rte_mbuf* m, uint64_t now_sec) {
    fwd::PacketInfo info;
    if (!fwd::parse_packet(m, info)) {
        fw.stats.dropped += 1;
        rte_pktmbuf_free(m);
        return;
    }

    // 学习源 MAC（不论后续是否放行，先学习，与交换机行为一致）
    fw.mac_table.learn(info.eth->src_addr, in_port, now_sec);

    const uint32_t pkt_len = rte_pktmbuf_pkt_len(m);

    // 非 IPv4 报文（ARP / IPv6 等）不做 ACL，直接按二层转发。
    // 注意这里只判断 has_ipv4：ICMP 这类协议没有端口，但同样应该受 ACL 管，
    // 它的 src_port / dst_port 保持 0 参与匹配。
    if (!info.has_ipv4) {
        if (fwd::is_broadcast_or_multicast(info.eth->dst_addr)) {
            flood(fw, in_port, m);
            return;
        }
        uint16_t out_port = 0;
        if (fw.mac_table.lookup(info.eth->dst_addr, &out_port, now_sec) && out_port != in_port) {
            send_on_port(fw, out_port, m);
        } else {
            flood(fw, in_port, m);
        }
        return;
    }

    fwd::FiveTuple tuple;
    tuple.src_ip = info.src_ip;
    tuple.dst_ip = info.dst_ip;
    tuple.src_port = info.src_port;
    tuple.dst_port = info.dst_port;
    tuple.proto = info.ip_proto;

    // ---- 快路径：先查会话表 ----
    if (auto* session = fw.sessions.find_and_touch(tuple, now_sec, pkt_len)) {
        fw.stats.session_hit += 1;
        apply_action(fw, in_port, m, session->action, info, now_sec);
        return;
    }

    // ---- 慢路径：首包查 ACL，并建立会话 ----
    fwd::AclAction action = fw.default_action;
    if (fw.acl.lookup(tuple, &action)) {
        fw.stats.acl_hit += 1;
    } else {
        fw.stats.acl_default += 1;
    }

    fw.sessions.create(tuple, action, now_sec);
    apply_action(fw, in_port, m, action, info, now_sec);
}

// 从文本文件加载 ACL 规则。
// 每行格式：<源IP> <目的IP> <源端口> <目的端口> <协议号> <pass|drop>
// 以 # 开头的行和空行忽略。端口或协议写 0 表示精确匹配 0（暂不支持通配）。
bool load_acl_file(fwd::AclTable& acl, const char* path) {
    FILE* fp = std::fopen(path, "r");
    if (fp == nullptr) {
        std::fprintf(stderr, "打开 ACL 文件失败: %s\n", path);
        return false;
    }

    char line[256];
    uint32_t added = 0;
    uint32_t lineno = 0;
    while (std::fgets(line, sizeof(line), fp) != nullptr) {
        ++lineno;
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') {
            continue;
        }

        char src[64] = {0};
        char dst[64] = {0};
        char act[16] = {0};
        unsigned sport = 0;
        unsigned dport = 0;
        unsigned proto = 0;

        if (std::sscanf(line, "%63s %63s %u %u %u %15s",
                        src, dst, &sport, &dport, &proto, act) != 6) {
            std::fprintf(stderr, "第 %u 行格式不对，已跳过\n", lineno);
            continue;
        }

        fwd::FiveTuple tuple;
        in_addr a{};
        if (inet_pton(AF_INET, src, &a) != 1) {
            std::fprintf(stderr, "第 %u 行源 IP 非法: %s\n", lineno, src);
            continue;
        }
        tuple.src_ip = ntohl(a.s_addr);
        if (inet_pton(AF_INET, dst, &a) != 1) {
            std::fprintf(stderr, "第 %u 行目的 IP 非法: %s\n", lineno, dst);
            continue;
        }
        tuple.dst_ip = ntohl(a.s_addr);
        tuple.src_port = static_cast<uint16_t>(sport);
        tuple.dst_port = static_cast<uint16_t>(dport);
        tuple.proto = static_cast<uint8_t>(proto);

        const fwd::AclAction action =
            (std::strcmp(act, "pass") == 0) ? fwd::AclAction::kPass : fwd::AclAction::kDrop;

        if (acl.add(tuple, action)) {
            ++added;
        }
    }
    std::fclose(fp);

    std::printf("已从 %s 加载 %u 条 ACL 规则\n", path, added);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const int ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "EAL 初始化失败\n");
    }
    argc -= ret;
    argv += ret;

    // 解析应用自己的参数（EAL 已经把它认识的参数摘走了）
    const char* acl_file = nullptr;
    bool default_allow = true;
    for (int i = 0; i < argc; ++i) {
        if (std::strcmp(argv[i], "--acl") == 0 && i + 1 < argc) {
            acl_file = argv[++i];
        } else if (std::strcmp(argv[i], "--default-drop") == 0) {
            default_allow = false;
        }
    }

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

    Forwarder fw;
    fw.default_action = default_allow ? fwd::AclAction::kPass : fwd::AclAction::kDrop;

    if (!fw.acl.init(fwd::kDefaultAclEntries)) {
        rte_exit(EXIT_FAILURE, "ACL 表初始化失败\n");
    }
    if (!fw.sessions.init(fwd::kDefaultSessionEntries)) {
        rte_exit(EXIT_FAILURE, "会话表初始化失败\n");
    }

    if (acl_file != nullptr) {
        load_acl_file(fw.acl, acl_file);
    } else {
        std::printf("未指定 --acl 文件，所有首包走默认动作: %s\n",
                    fwd::action_name(fw.default_action));
    }
    std::printf("ACL 规则 %u 条，会话表容量 %u\n",
                fw.acl.count(), fwd::kDefaultSessionEntries);

    const uint16_t use_ports = 2;
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

        // 每分钟做一次老化：MAC 表 + 会话表
        if (now - last_age >= ticks_per_sec * 60) {
            last_age = now;
            const uint32_t now_sec = static_cast<uint32_t>(now / ticks_per_sec);
            const uint32_t mac_removed = fw.mac_table.age(now_sec);
            const uint32_t sess_removed = fw.sessions.age(now_sec);
            if (mac_removed > 0 || sess_removed > 0) {
                std::printf("[老化] MAC 表清除 %u 条（余 %u），会话清除 %u 条（余 %u）\n",
                            mac_removed, fw.mac_table.size(),
                            sess_removed, fw.sessions.count());
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
                        "会话 %u | ACL命中 %" PRIu64 " | 会话命中 %" PRIu64 " | 拒绝 %" PRIu64 "\n",
                        static_cast<uint64_t>(static_cast<double>(d_rx) / elapsed),
                        rx_rate,
                        static_cast<uint64_t>(static_cast<double>(d_tx) / elapsed),
                        fw.sessions.count(),
                        fw.stats.acl_hit,
                        fw.stats.session_hit,
                        fw.stats.denied);
            std::fflush(stdout);

            prev = fw.stats;
            last_report = now;
        }
    }

    std::printf("\n收到退出信号，正在停止...\n");
    std::printf("累计: RX %" PRIu64 " 包 / %" PRIu64 " 字节，TX %" PRIu64 " 包 / %" PRIu64 " 字节\n",
                fw.stats.rx_pkts, fw.stats.rx_bytes, fw.stats.tx_pkts, fw.stats.tx_bytes);
    std::printf("     泛洪 %" PRIu64 "，丢弃 %" PRIu64 "，拒绝 %" PRIu64 "\n",
                fw.stats.flooded, fw.stats.dropped, fw.stats.denied);
    std::printf("     ACL 命中 %" PRIu64 "，默认动作 %" PRIu64 "，会话命中 %" PRIu64 "\n",
                fw.stats.acl_hit, fw.stats.acl_default, fw.stats.session_hit);
    std::printf("     剩余会话 %u 条\n", fw.sessions.count());

    for (uint16_t i = 0; i < use_ports; ++i) {
        port_objs[i].stop();
    }
    delete[] port_objs;

    fw.sessions.destroy();
    fw.acl.destroy();
    rte_mempool_free(pool);
    rte_eal_cleanup();
    std::printf("已退出。\n");
    return 0;
}
