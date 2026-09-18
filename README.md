# dpdk-forwarder

基于 DPDK 的用户态高性能报文转发与 ACL 过滤系统。

在 Linux 上用 C++ 实现一个跑在用户态的转发器：从网卡收包，解析后做五元组
ACL 过滤与会话跟踪，再转发出去。全程零拷贝、无系统调用、无锁。

> 📌 **后续演进**：本仓库是前置项目。下一步方向见 [**ne6100-sim**](https://github.com/ctypristu/ne6100-sim) —— NE6100 仿真：把 DPU 网卡的控制面与数据面语义用纯软件实现（DPDK），无硬件即可跑通策略下发与转发验证。

## 目标

- 功能：L2/L3 转发 + 五元组 ACL + 会话表与老化 + 流量统计
- 性能：在测试平台上达到线速或接近线速（具体指标见规划书）
- 对比：相对内核协议栈转发，吞吐与 CPU 占用有可量化的改善

## 处理模型

系统采用快慢路径分流，这也是智能网卡最核心的设计思路：

```
收包 → 解析五元组
        │
        ├─ 命中会话表 ────────────► 快路径：直接按会话动作处理
        │
        └─ 未命中 ──► 查 ACL 规则表 ──► 建会话 ──► 按规则动作处理
                        （慢路径，只在首包发生）
```

一条连接通常有成千上万个包，但只有首包需要查 ACL 规则表。实测中
5000 个包只有 1 次 ACL 查询，其余 4999 次都命中会话表。

## ACL 规则

规则从文本文件加载，每行格式：

```
<源IP> <目的IP> <源端口> <目的端口> <协议号> <pass|drop>
```

示例见 `config/acl.conf`。协议号 `6`=TCP、`17`=UDP、`1`=ICMP。

启动时用 `--acl <文件>` 指定；不加则所有首包走默认动作（默认放行，
可用 `--default-drop` 改成拒绝）。

## 快速开始

### 在 WSL 里开发（推荐，不需要真实网卡）

```bash
# 1) 编译
./scripts/build.sh

# 2) 运行（用 TAP 虚拟端口代替真实网卡）
sudo ./scripts/run_wsl.sh
```

WSL 不支持把网卡绑给 DPDK（没有 IOMMU 透传），所以功能开发阶段用
`--vdev=net_tap0` 创建一个虚拟端口。**这条路只能验证程序逻辑，不能测性能。**

### 在真实服务器上运行

```bash
# 1) 编译
./scripts/build.sh

# 2) 绑定网卡到 vfio-pci / igb_uio
sudo ./scripts/bind_ports.sh

# 3) 运行
sudo ./build/dpdk-forwarder -l 1,2 -n 4 -- -c config/forwarder.conf
```

### 手动编译

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

构建依赖：`libdpdk`（通过 pkg-config 查找）、CMake ≥ 3.16、支持 C++17 的编译器。

## 环境说明

| 环境 | 用途 | 限制 |
|---|---|---|
| WSL2（Ubuntu） | 功能开发、编译验证 | 用 TAP 虚拟端口，测不了性能 |
| 真实服务器 | 性能测试 | 需要网卡绑定与大页配置 |

WSL 里的 DPDK 用 `--no-huge` 启动（WSL 不支持 2MB 大页）。

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
