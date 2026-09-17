#pragma once

#include <cstdint>

namespace fwd {

// 默认收发批量。DPDK 里一次 burst 处理多个包，可以摊薄访问网卡的固定开销。
// 32 是常见取值：再小收益不明显，再大会拉高单包延迟。
constexpr uint16_t kDefaultBurstSize = 32;

// 收发描述符环大小。深度越大越抗突发，但会占用更多内存、降低 cache 命中率。
constexpr uint16_t kDefaultRxDesc = 1024;
constexpr uint16_t kDefaultTxDesc = 1024;

// mempool 里 mbuf 的数量。要能覆盖所有队列的 in-flight 包，否则会丢包。
constexpr uint32_t kDefaultNbMbufs = 8191;

// 每个 lcore 的 mempool 本地缓存大小。必须是 2 的幂减 1，且能被核数整除。
constexpr uint32_t kDefaultMbufCacheSize = 250;

constexpr uint16_t kDefaultNbRxQueue = 1;
constexpr uint16_t kDefaultNbTxQueue = 1;

// 统计输出间隔（秒）
constexpr uint32_t kStatsIntervalSec = 1;

}  // namespace fwd
