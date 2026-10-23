"""stop.py — 停止本机所有区块链节点（按监听端口定位进程）"""
import os
import re
import subprocess
import sys

PORTS = [3001, 3002, 3003, 6001, 6002, 6003, 3101, 3102, 3103]


def main():
    out = subprocess.run(["netstat", "-ano"], capture_output=True).stdout.decode("gbk", "replace")
    pids = set()
    for line in out.splitlines():
        if "LISTENING" not in line:
            continue
        m = re.search(r":(\d+)\s+\S+\s+LISTENING\s+(\d+)", line)
        if m and int(m.group(1)) in PORTS:
            pids.add(m.group(2))
    if not pids:
        print("没有发现运行中的节点")
        return 0
    for pid in pids:
        subprocess.run(["taskkill", "/F", "/PID", pid], capture_output=True)
        print(f"已停止 PID {pid}")
    print(f"共停止 {len(pids)} 个进程")
    return 0


if __name__ == "__main__":
    sys.exit(main())
