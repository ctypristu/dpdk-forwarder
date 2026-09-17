# dpdk-forwarder

基于 DPDK 的用户态高性能报文转发与 ACL 过滤系统。

在 Linux 上用 C++ 实现一个跑在用户态的转发器：从网卡收包，解析后做五元组
ACL 过滤与会话跟踪，再转发出去。全程零拷贝、无系统调用、无锁。

## 目标

- 功能：L2/L3 转发 + 五元组 ACL + 会话表与老化 + 流量统计
- 性能：在测试平台上达到线速或接近线速（具体指标见规划书）
- 对比：相对内核协议栈转发，吞吐与 CPU 占用有可量化的改善

## 快速开始

> 详细环境准备见 `docs/` 下的文档。

```bash
# 1) 编译
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# 2) 绑定网卡到 igb_uio / vfio-pci
sudo ./scripts/bind_ports.sh

# 3) 运行
sudo ./build/dpdk-forwarder -l 1,2 -n 4 -- -c config/forwarder.conf
```

## 文档

| 文档 | 内容 |
|---|---|
| `docs/项目规划书.md` | 目标、范围、里程碑、简历素材 |
| `docs/设计文档.md` | 模块划分与关键数据结构 |
| `docs/性能测试报告.md` | 测试方法与实测数据 |

## 目录结构

```
dpdk-forwarder/
├── src/            源码
├── include/        头文件
├── config/         配置示例
├── scripts/        环境准备与测试脚本
├── tests/          功能测试
└── docs/           文档
```

## 状态

项目初始化中。进度见 `docs/项目规划书.md` 的里程碑表。
