#!/usr/bin/env bash
# 编译 dpdk-forwarder
#
# 用法：./scripts/build.sh [Debug|Release]

set -euo pipefail

BUILD_TYPE="${1:-Release}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cd "$ROOT_DIR"

if ! pkg-config --exists libdpdk; then
    echo "找不到 libdpdk。请先安装 DPDK，或用 PKG_CONFIG_PATH 指定 libdpdk.pc 的位置。" >&2
    exit 1
fi

echo "DPDK 版本: $(pkg-config --modversion libdpdk)"
echo "构建类型: $BUILD_TYPE"

cmake -B build -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
cmake --build build -j "$(nproc)"

echo
echo "产物: $ROOT_DIR/build/dpdk-forwarder"

if [[ -x "$ROOT_DIR/build/test_basic" ]]; then
    echo
    echo "运行单元测试："
    "$ROOT_DIR/build/test_basic"
fi
