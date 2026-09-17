#pragma once

#include <cstdint>

#include <rte_hash.h>

#include "acl.h"

namespace fwd {

// 一条会话记录。键是双向规范化后的五元组，所以正反方向的包都指向同一条记录。
struct SessionEntry {
    FiveTuple key{};
    AclAction action = AclAction::kDrop;
    uint64_t last_seen = 0;   // 最后活动时间（秒）
    uint64_t pkts = 0;        // 命中的报文数
    uint64_t bytes = 0;       // 累计字节数
};

// 会话表。
//
// 作用是把"每个包都查一遍 ACL"变成"首包查 ACL、后续包查会话"。
// 这正是智能网卡里快慢路径分流的雏形：首包走慢路径建立状态，
// 后续包命中后直接按会话处理。
class SessionTable {
public:
    static constexpr uint32_t kDefaultTimeoutSec = 60;
    static constexpr uint32_t kMaxRemovePerRound = 4096;

    SessionTable() = default;
    ~SessionTable() { destroy(); }

    SessionTable(const SessionTable&) = delete;
    SessionTable& operator=(const SessionTable&) = delete;

    bool init(uint32_t max_sessions);
    void destroy();

    // 查找并刷新。命中返回条目指针，未命中返回 nullptr。
    SessionEntry* find_and_touch(const FiveTuple& tuple, uint64_t now, uint32_t pkt_bytes);

    // 新建会话。已存在则直接返回现有条目（不覆盖动作）。
    SessionEntry* create(const FiveTuple& tuple, AclAction action, uint64_t now);

    // 老化：清除闲置超过 timeout 秒的会话，返回清除条数。
    uint32_t age(uint64_t now, uint32_t timeout_sec = kDefaultTimeoutSec);

    uint32_t count() const;

private:
    rte_hash* hash_ = nullptr;
};

}  // namespace fwd
