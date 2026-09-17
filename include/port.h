#pragma once

#include <cstdint>

#include <rte_ethdev.h>
#include <rte_mbuf.h>

namespace fwd {

struct PortConfig {
    uint16_t port_id = 0;
    uint16_t nb_rx_queue = 1;
    uint16_t nb_tx_queue = 1;
    uint16_t nb_rx_desc = 1024;
    uint16_t nb_tx_desc = 1024;
};

// 封装一个 DPDK 端口的生命周期：配置 → 启动 → 收发 → 停止
class Port {
public:
    Port() = default;
    explicit Port(const PortConfig& cfg) : cfg_(cfg) {}

    // 配置收发队列并启动端口。mempool 用于接收侧分配 mbuf。
    int init(rte_mempool* pool);
    void start();
    void stop();

    uint16_t rx_burst(rte_mbuf** pkts, uint16_t max_pkts);
    uint16_t tx_burst(rte_mbuf** pkts, uint16_t nb_pkts);

    uint16_t id() const { return cfg_.port_id; }
    bool is_started() const { return started_; }

    // 打印端口的驱动、MAC、队列能力、链路状态
    void dump_info() const;

private:
    PortConfig cfg_;
    bool started_ = false;
};

}  // namespace fwd
