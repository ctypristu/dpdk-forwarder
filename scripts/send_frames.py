#!/usr/bin/env python3
"""向指定网卡发送原始以太网帧，用于在没有打流仪的场合做收包验证。

只在本地开发时使用。它构造的是固定内容的广播帧，不追求真实性，
目的是让 DPDK 程序有包可收，验证收包路径通不通。

用法：
    sudo python3 scripts/send_frames.py dtap0 20000
"""

import socket
import struct
import sys
import time


def build_frame(payload_len: int) -> bytes:
    """构造 以太网头 + IPv4 头 + ICMP 头 + 填充"""
    dst_mac = bytes.fromhex("ffffffffffff")
    src_mac = bytes.fromhex("001122334455")
    eth_type = bytes.fromhex("0800")

    payload = b"X" * payload_len
    icmp = struct.pack("!BBHHH", 8, 0, 0, 0, 1) + payload  # echo request
    total_len = 20 + len(icmp)
    ip = struct.pack(
        "!BBHHHBBH4s4s",
        0x45, 0, total_len, 0, 0, 64, 1, 0,
        socket.inet_aton("10.99.99.2"),
        socket.inet_aton("10.99.99.1"),
    )
    frame = dst_mac + src_mac + eth_type + ip + icmp
    # 以太网最小帧长 60 字节（不含 FCS）
    if len(frame) < 60:
        frame += b"\x00" * (60 - len(frame))
    return frame


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 1

    iface = sys.argv[1]
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 20000
    frame = build_frame(64)

    sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW)
    sock.bind((iface, 0))

    print(f"接口 {iface}，发送 {count} 个 {len(frame)} 字节的帧...")
    start = time.time()
    sent = 0
    for _ in range(count):
        try:
            sock.send(frame)
            sent += 1
        except OSError as exc:
            print(f"发送失败: {exc}")
            break
    elapsed = time.time() - start

    if elapsed > 0:
        print(f"已发送 {sent} 个包，用时 {elapsed:.2f} 秒，约 {sent / elapsed:.0f} pps")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
