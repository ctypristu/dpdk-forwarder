#pragma once

#include <cstdint>

#include <rte_hash.h>

namespace fwd {

enum class AclAction : uint8_t {
    kDrop = 0,
    kPass = 1,
};

// 报文五元组。所有字段都是主机字节序，方便直接比较与哈希。
struct FiveTuple {
    uint32_t src_ip = 0;
    uint32_t dst_ip = 0;
    uint16_t src_port = 0;
    uint16_t dst_port = 0;
    uint8_t proto = 0;

    bool operator==(const FiveTuple& other) const {
        return src_ip == other.src_ip && dst_ip == other.dst_ip &&
               src_port == other.src_port && dst_port == other.dst_port &&
               proto == other.proto;
    }

    // 反向五元组：源和目的对调
    FiveTuple reversed() const {
        FiveTuple r;
        r.src_ip = dst_ip;
        r.dst_ip = src_ip;
        r.src_port = dst_port;
        r.dst_port = src_port;
        r.proto = proto;
        return r;
    }

    // 双向规范化：按 (IP, 端口) 排序，使一条连接的两个方向得到同一个 key。
    // 这样会话表只需要存一份，正反向包都能查到。
    FiveTuple canonical() const;
};

// ACL 规则表。
//
// 用 DPDK 的 rte_hash 做精确五元组匹配：查找 O(1)，支持多线程并发读。
// 带掩码的通配规则（比如整个网段）留到后续里程碑，当前先做精确匹配。
class AclTable {
public:
    AclTable() = default;
    ~AclTable() { destroy(); }

    AclTable(const AclTable&) = delete;
    AclTable& operator=(const AclTable&) = delete;

    bool init(uint32_t max_rules);
    void destroy();

    // 添加规则。同一五元组重复添加会覆盖动作。
    bool add(const FiveTuple& tuple, AclAction action);

    // 查找规则。命中返回 true 并填出动作。
    bool lookup(const FiveTuple& tuple, AclAction* action) const;

    uint32_t count() const;

private:
    rte_hash* hash_ = nullptr;
};

// 动作转成可读文本，用于日志
const char* action_name(AclAction action);

}  // namespace fwd
