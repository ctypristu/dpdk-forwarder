#!/usr/bin/env bash
# 在 WSL 里运行（不需要真实网卡）
#
# WSL 不支持网卡绑定给 DPDK，这里用 TAP 虚拟端口代替，
# 只用于验证程序逻辑，不做性能测试。
#
# 用法：sudo ./scripts/run_wsl.sh [额外参数...]

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT_DIR/build/dpdk-forwarder"

if [[ ! -x "$BIN" ]]; then
    echo "还没编译。先跑 ./scripts/build.sh" >&2
    exit 1
fi

if [[ "$(id -u)" -ne 0 ]]; then
    echo "需要 root（创建 TAP 接口）。请用 sudo 运行。" >&2
    exit 1
fi

exec "$BIN" \
    --no-huge \
    -l 0-1 \
    --log-level=info \
    --vdev=net_tap0 \
    "$@"
