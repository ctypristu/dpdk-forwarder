#include "mac_table.h"

#include <cstring>

namespace fwd {

uint32_t MacTable::hash(const rte_ether_addr& mac) {
    // FNV-1a，对 6 字节 MAC 足够用
    uint32_t h = 2166136261u;
    for (int i = 0; i < RTE_ETHER_ADDR_LEN; ++i) {
        h ^= mac.addr_bytes[i];
        h *= 16777619u;
    }
    return h & (kCapacity - 1);
}

int MacTable::find_slot(const Entry* entries, const rte_ether_addr& mac) {
    const uint32_t start = hash(mac);
    for (uint32_t i = 0; i < kCapacity; ++i) {
        const uint32_t idx = (start + i) & (kCapacity - 1);
        if (!entries[idx].valid) {
            return static_cast<int>(idx);  // 空槽，未命中
        }
        if (std::memcmp(entries[idx].mac.addr_bytes, mac.addr_bytes,
                        RTE_ETHER_ADDR_LEN) == 0) {
            return static_cast<int>(idx);  // 命中
        }
    }
    return -1;
}

void MacTable::learn(const rte_ether_addr& mac, uint16_t port_id, uint64_t now) {
    const int slot = find_slot(entries_, mac);
    if (slot < 0) {
        return;  // 表满：不插入，等老化腾空间
    }

    Entry& e = entries_[slot];
    if (!e.valid) {
        e.mac = mac;
        e.valid = true;
        ++count_;
    }
    e.port_id = port_id;
    e.updated_at = now;
}

bool MacTable::lookup(const rte_ether_addr& mac, uint16_t* port_id, uint64_t now) const {
    const int slot = find_slot(entries_, mac);
    if (slot < 0) {
        return false;
    }

    const Entry& e = entries_[slot];
    if (!e.valid) {
        return false;
    }
    if (now > e.updated_at && now - e.updated_at > kAgeSeconds) {
        return false;  // 已超时，等老化清掉
    }

    if (port_id != nullptr) {
        *port_id = e.port_id;
    }
    return true;
}

uint32_t MacTable::age(uint64_t now) {
    uint32_t removed = 0;
    for (uint32_t i = 0; i < kCapacity; ++i) {
        Entry& e = entries_[i];
        if (!e.valid) {
            continue;
        }
        if (now > e.updated_at && now - e.updated_at > kAgeSeconds) {
            e.valid = false;
            e.updated_at = 0;
            --count_;
            ++removed;
        }
    }
    return removed;
}

}  // namespace fwd
