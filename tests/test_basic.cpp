// 基础单元测试：不依赖真实网卡，也不需要 EAL 初始化。
//
// 覆盖的是纯逻辑部分——五元组规范化、MAC 表的哈希与老化、
// 地址类型判断。这些是转发正确性的地基，出问题最难排查。

#include <cstdio>
#include <cstring>

#include "acl.h"
#include "mac_table.h"
#include "packet.h"

namespace {

int g_failed = 0;
int g_total = 0;

void check(bool ok, const char* name) {
    ++g_total;
    if (ok) {
        std::printf("  ok    %s\n", name);
    } else {
        std::printf("  FAIL  %s\n", name);
        ++g_failed;
    }
}

rte_ether_addr make_mac(const uint8_t (&bytes)[6]) {
    rte_ether_addr mac{};
    std::memcpy(mac.addr_bytes, bytes, 6);
    return mac;
}

void test_five_tuple() {
    std::printf("\n[五元组]\n");

    fwd::FiveTuple t;
    t.src_ip = 0x0A000001;   // 10.0.0.1
    t.dst_ip = 0x0A000002;   // 10.0.0.2
    t.src_port = 12345;
    t.dst_port = 80;
    t.proto = 6;

    const fwd::FiveTuple r = t.reversed();
    check(r.src_ip == t.dst_ip && r.dst_ip == t.src_ip, "reversed 交换了 IP");
    check(r.src_port == t.dst_port && r.dst_port == t.src_port, "reversed 交换了端口");
    check(r.proto == t.proto, "reversed 保留协议号");

    check(t.reversed().reversed() == t, "反转两次回到原值");

    // 双向规范化是会话表的基础：一条连接的两个方向必须落到同一个 key
    check(t.canonical() == t.reversed().canonical(), "正反方向的 canonical 一致");
    check(t.canonical().src_ip == 0x0A000001, "canonical 把较小的 IP 放在源");

    fwd::FiveTuple reverse_order;
    reverse_order.src_ip = 0x0A000002;
    reverse_order.dst_ip = 0x0A000001;
    reverse_order.src_port = 80;
    reverse_order.dst_port = 12345;
    reverse_order.proto = 6;
    check(reverse_order.canonical() == t.canonical(), "顺序不同也规范化到同一个 key");

    // IP 相同的情况靠端口区分顺序
    fwd::FiveTuple same_ip;
    same_ip.src_ip = 0x0A000001;
    same_ip.dst_ip = 0x0A000001;
    same_ip.src_port = 5000;
    same_ip.dst_port = 6000;
    same_ip.proto = 17;
    check(same_ip.canonical().src_port == 5000, "IP 相同时用端口决定顺序");
    check(same_ip.canonical() == same_ip.reversed().canonical(), "IP 相同时双向仍一致");
}

void test_mac_table() {
    std::printf("\n[MAC 表]\n");

    fwd::MacTable table;
    check(table.size() == 0, "初始为空");

    const rte_ether_addr mac_a = make_mac({0x00, 0x11, 0x22, 0x33, 0x44, 0x55});
    const rte_ether_addr mac_b = make_mac({0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF});

    table.learn(mac_a, 3, 1000);
    check(table.size() == 1, "学习一条后 size 为 1");

    uint16_t port = 0;
    check(table.lookup(mac_a, &port, 1000), "能查到刚学的 MAC");
    check(port == 3, "端口号正确");

    check(!table.lookup(mac_b, &port, 1000), "没学过的 MAC 查不到");

    // 同一个 MAC 换端口，应该更新而不是新增
    table.learn(mac_a, 5, 1001);
    check(table.size() == 1, "同一 MAC 换端口不增加条目数");
    table.lookup(mac_a, &port, 1001);
    check(port == 5, "端口号已更新");

    // 老化：超过 kAgeSeconds 未更新的条目应被清除
    table.learn(mac_b, 1, 1000);
    check(table.size() == 2, "学习第二条");
    // 注意时间基准：mac_a 最后一次更新是 1001，mac_b 是 1000。
    // 取 1001 + 超时 + 1，两条都刚好越界。
    const uint32_t removed = table.age(1001 + fwd::MacTable::kAgeSeconds + 1);
    check(removed == 2, "超时条目全部被老化");
    check(table.size() == 0, "老化后表为空");

    // 边界：恰好等于超时时间不应被清除（条件是"大于"而非"大于等于"）
    table.learn(mac_a, 1, 2000);
    check(table.age(2000 + fwd::MacTable::kAgeSeconds) == 0, "刚好达到超时时间不清除");
    check(table.age(2000 + fwd::MacTable::kAgeSeconds + 1) == 1, "超过一秒钟后被清除");

    // 未超时的条目不应被清除
    table.learn(mac_a, 2, 5000);
    const uint32_t removed2 = table.age(5000 + 10);
    check(removed2 == 0, "未超时的不被清除");
    check(table.size() == 1, "条目保留");
}

void test_address_type() {
    std::printf("\n[地址类型判断]\n");

    const rte_ether_addr bcast = make_mac({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
    check(fwd::is_broadcast_or_multicast(bcast), "全 F 识别为广播");

    const rte_ether_addr mcast = make_mac({0x01, 0x00, 0x5E, 0x00, 0x00, 0x01});
    check(fwd::is_broadcast_or_multicast(mcast), "组播地址被识别");

    const rte_ether_addr ipv6_mcast = make_mac({0x33, 0x33, 0x00, 0x00, 0x00, 0x01});
    check(fwd::is_broadcast_or_multicast(ipv6_mcast), "IPv6 组播被识别");

    const rte_ether_addr unicast = make_mac({0x00, 0x11, 0x22, 0x33, 0x44, 0x55});
    check(!fwd::is_broadcast_or_multicast(unicast), "普通单播不被误判");
}

}  // namespace

int main() {
    std::printf("=== dpdk-forwarder 单元测试 ===\n");

    test_five_tuple();
    test_mac_table();
    test_address_type();

    std::printf("\n--------------------------------\n");
    std::printf("共 %d 项，失败 %d 项\n", g_total, g_failed);
    return g_failed == 0 ? 0 : 1;
}
