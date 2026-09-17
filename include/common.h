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

// ACL 规则表容量与会话表容量。
//
// 这两个值直接决定内存占用：会话表每条大约 48 字节（结构体）+ 哈希表开销。
// 这里的默认值偏保守，是为了能在 WSL / 内存受限的环境里跑起来。
// 真机测试时（配好大页、内存充足）可以调大，比如会话表用 1<<20。
constexpr uint32_t kDefaultAclEntries = 1u << 16;       // 65536 条规则
constexpr uint32_t kDefaultSessionEntries = 1u << 16;   // 65536 条会话

}  // namespace fwd
