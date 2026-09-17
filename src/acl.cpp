#include "acl.h"

#include <cstdio>

#include <rte_errno.h>
#include <rte_hash_crc.h>
#include <rte_lcore.h>

namespace fwd {

FiveTuple FiveTuple::canonical() const {
    if (src_ip < dst_ip) {
        return *this;
    }
    if (src_ip > dst_ip) {
        return reversed();
    }
    // IP 相同，再比端口
    if (src_port <= dst_port) {
        return *this;
    }
    return reversed();
}

const char* action_name(AclAction action) {
    return action == AclAction::kPass ? "PASS" : "DROP";
}

bool AclTable::init(uint32_t max_rules) {
    rte_hash_parameters params{};
    params.name = "acl_table";
    params.entries = max_rules;
    params.key_len = sizeof(FiveTuple);
    params.hash_func = rte_hash_crc;
    params.hash_func_init_val = 0;
    params.socket_id = static_cast<int>(rte_socket_id());

    hash_ = rte_hash_create(&params);
    if (hash_ == nullptr) {
        std::fprintf(stderr, "创建 ACL 表失败: %s\n", rte_strerror(rte_errno));
        return false;
    }
    return true;
}

void AclTable::destroy() {
    if (hash_ != nullptr) {
        rte_hash_free(hash_);
        hash_ = nullptr;
    }
}

bool AclTable::add(const FiveTuple& tuple, AclAction action) {
    if (hash_ == nullptr) {
        return false;
    }

    // 动作本身很小，直接编码进 data 指针，省一次内存分配
    void* data = reinterpret_cast<void*>(static_cast<uintptr_t>(action));
    const int ret = rte_hash_add_key_data(hash_, &tuple, data);
    if (ret < 0) {
        std::fprintf(stderr, "添加 ACL 规则失败: %s\n", rte_strerror(-ret));
        return false;
    }
    return true;
}

bool AclTable::lookup(const FiveTuple& tuple, AclAction* action) const {
    if (hash_ == nullptr) {
        return false;
    }

    void* data = nullptr;
    if (rte_hash_lookup_data(hash_, &tuple, &data) < 0) {
        return false;
    }

    if (action != nullptr) {
        *action = static_cast<AclAction>(reinterpret_cast<uintptr_t>(data));
    }
    return true;
}

uint32_t AclTable::count() const {
    return hash_ == nullptr ? 0 : rte_hash_count(hash_);
}

}  // namespace fwd
