#pragma once

#include <cstdint>

#include <rte_ether.h>

namespace fwd {

// 二层转发表。
//
// 用开放寻址 + 线性探查实现：结构简单、无动态分配，
// 适合单核独占的场景（每个核一份自己的表，天然无锁）。
// 条目满了以后不再插入新条目，等老化腾出空间。
class MacTable {
public:
    static constexpr uint32_t kCapacity = 4096;    // 必须为 2 的幂
    static constexpr uint32_t kAgeSeconds = 300;   // 5 分钟无流量则老化

    // 学到一条 (MAC → 端口) 映射。已存在则刷新时间戳和端口。
    void learn(const rte_ether_addr& mac, uint16_t port_id, uint64_t now);

    // 查表。命中返回 true 并填出端口号。
    bool lookup(const rte_ether_addr& mac, uint16_t* port_id, uint64_t now) const;

    // 老化：清掉超过 kAgeSeconds 没更新的条目。返回清掉的条数。
    uint32_t age(uint64_t now);

    uint32_t size() const { return count_; }
    uint32_t capacity() const { return kCapacity; }

private:
    struct Entry {
        rte_ether_addr mac{};
        uint16_t port_id = 0;
        uint64_t updated_at = 0;
        bool valid = false;
    };

    static uint32_t hash(const rte_ether_addr& mac);
    static int find_slot(const Entry* entries, const rte_ether_addr& mac);

    Entry entries_[kCapacity]{};
    uint32_t count_ = 0;
};

}  // namespace fwd
