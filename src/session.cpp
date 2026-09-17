#include "session.h"

#include <cstdio>
#include <new>

#include <rte_errno.h>
#include <rte_hash_crc.h>
#include <rte_lcore.h>

namespace fwd {

bool SessionTable::init(uint32_t max_sessions) {
    rte_hash_parameters params{};
    params.name = "session_table";
    params.entries = max_sessions;
    params.key_len = sizeof(FiveTuple);
    params.hash_func = rte_hash_crc;
    params.hash_func_init_val = 0;
    params.socket_id = static_cast<int>(rte_socket_id());

    hash_ = rte_hash_create(&params);
    if (hash_ == nullptr) {
        std::fprintf(stderr, "创建会话表失败: %s\n", rte_strerror(rte_errno));
        return false;
    }
    return true;
}

void SessionTable::destroy() {
    if (hash_ == nullptr) {
        return;
    }

    // 逐个释放条目内存，再销毁哈希表本身
    const void* key = nullptr;
    void* data = nullptr;
    uint32_t iter = 0;
    while (rte_hash_iterate(hash_, &key, &data, &iter) >= 0) {
        delete static_cast<SessionEntry*>(data);
    }

    rte_hash_free(hash_);
    hash_ = nullptr;
}

SessionEntry* SessionTable::find_and_touch(const FiveTuple& tuple, uint64_t now,
                                           uint32_t pkt_bytes) {
    if (hash_ == nullptr) {
        return nullptr;
    }

    const FiveTuple key = tuple.canonical();
    void* data = nullptr;
    if (rte_hash_lookup_data(hash_, &key, &data) < 0) {
        return nullptr;
    }

    auto* entry = static_cast<SessionEntry*>(data);
    entry->last_seen = now;
    entry->pkts += 1;
    entry->bytes += pkt_bytes;
    return entry;
}

SessionEntry* SessionTable::create(const FiveTuple& tuple, AclAction action, uint64_t now) {
    if (hash_ == nullptr) {
        return nullptr;
    }

    const FiveTuple key = tuple.canonical();

    void* existing = nullptr;
    if (rte_hash_lookup_data(hash_, &key, &existing) == 0) {
        auto* entry = static_cast<SessionEntry*>(existing);
        entry->last_seen = now;
        entry->pkts += 1;
        return entry;
    }

    auto* entry = new (std::nothrow) SessionEntry();
    if (entry == nullptr) {
        return nullptr;
    }
    entry->key = key;
    entry->action = action;
    entry->last_seen = now;
    entry->pkts = 1;

    const int ret = rte_hash_add_key_data(hash_, &key, entry);
    if (ret < 0) {
        delete entry;
        return nullptr;
    }
    return entry;
}

uint32_t SessionTable::age(uint64_t now, uint32_t timeout_sec) {
    if (hash_ == nullptr) {
        return 0;
    }

    // rte_hash_iterate 不允许在遍历中删除，所以先收集再统一删。
    // 单轮最多收集 kMaxRemovePerRound 条，剩下的下一轮老化继续处理。
    FiveTuple expired[kMaxRemovePerRound];
    uint32_t n_expired = 0;

    const void* key = nullptr;
    void* data = nullptr;
    uint32_t iter = 0;
    while (n_expired < kMaxRemovePerRound &&
           rte_hash_iterate(hash_, &key, &data, &iter) >= 0) {
        const auto* entry = static_cast<const SessionEntry*>(data);
        if (now > entry->last_seen && now - entry->last_seen > timeout_sec) {
            expired[n_expired++] = entry->key;
        }
    }

    uint32_t removed = 0;
    for (uint32_t i = 0; i < n_expired; ++i) {
        void* entry_data = nullptr;
        if (rte_hash_lookup_data(hash_, &expired[i], &entry_data) == 0) {
            delete static_cast<SessionEntry*>(entry_data);
        }
        if (rte_hash_del_key(hash_, &expired[i]) >= 0) {
            ++removed;
        }
    }
    return removed;
}

uint32_t SessionTable::count() const {
    return hash_ == nullptr ? 0 : rte_hash_count(hash_);
}

}  // namespace fwd
