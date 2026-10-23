"""start.py — 一键启动多节点演示网络

    python start.py              # 启动 3 个节点（3001/3002/3003）
    python start.py --fresh      # 清空数据后重新开始
    python start.py --nodes 2    # 只启动 2 个节点

节点 1 默认开启自动挖矿并作为种子节点，其余节点通过 P2P 同步。
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.abspath(__file__))
PY = sys.executable
CLI = os.path.join(ROOT, "node", "cli.py")

PLAN = [
    # (http_port, p2p_port, seeds, auto_mine)
    (3001, 6001, [], True),
    (3002, 6002, ["127.0.0.1:6001"], False),
    (3003, 6003, ["127.0.0.1:6001"], False),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fresh", action="store_true", help="清空 data/ 下的链数据")
    ap.add_argument("--nodes", type=int, default=3, help="启动节点数量（1-3）")
    ap.add_argument("--difficulty", type=int, default=4)
    ap.add_argument("--no-mine", action="store_true", help="不自动挖矿")
    ap.add_argument("--detach", action="store_true", help="后台常驻运行（关闭终端后节点继续运行）")
    args = ap.parse_args()

    engine = os.path.join(ROOT, "bin", "chain-engine.exe" if os.name == "nt" else "chain-engine")
    if not os.path.exists(engine):
        print("未找到 C++ 引擎，正在编译…")
        rc = subprocess.call(["bash", os.path.join(ROOT, "build.sh")], cwd=ROOT)
        if rc != 0:
            print("编译失败，请检查 g++ 环境")
            return 1

    plan = PLAN[: max(1, min(args.nodes, len(PLAN)))]
    if args.fresh:
        for http_port, _, _, _ in plan:
            d = os.path.join(ROOT, "data", f"n{http_port}")
            if os.path.isdir(d):
                shutil.rmtree(d, ignore_errors=True)
        print("已清空链数据")

    procs = []
    print("=" * 64)
    print("  启动区块链节点网络")
    print("=" * 64)
    for http_port, p2p_port, seeds, auto_mine in plan:
        datadir = os.path.join(ROOT, "data", f"n{http_port}")
        cmd = [PY, CLI, "run", "--datadir", datadir, "--port", str(http_port), "--p2p", str(p2p_port),
               "--difficulty", str(args.difficulty)]
        for s in seeds:
            cmd += ["--seed", s]
        if auto_mine and not args.no_mine:
            cmd += ["--auto-mine"]
        log_path = os.path.join(ROOT, "data", f"node-{http_port}.log")
        os.makedirs(os.path.join(ROOT, "data"), exist_ok=True)
        if args.detach:
            # 通过 cmd 的 start /B 启动，进程脱离当前 shell 的作业对象，可长期驻留
            full = ["cmd", "/c", "start", "", "/B"] + cmd + ["--log", log_path]
            p = subprocess.Popen(full, cwd=ROOT, close_fds=True)
            procs.append((p, None, http_port))
            time.sleep(1.0)
            continue
        logf = open(log_path, "w", encoding="utf-8")
        p = subprocess.Popen(cmd, cwd=ROOT, stdout=logf, stderr=subprocess.STDOUT)
        procs.append((p, logf, http_port))
        time.sleep(0.4)

    time.sleep(1.5)
    print()
    for _, _, http_port in procs:
        print(f"  节点 http://127.0.0.1:{http_port}   (控制台 http://127.0.0.1:{http_port}/)")
    print()
    print("  提示：控制台右上角可切换节点；节点 1 正在自动挖矿，")
    print("       在其它节点打开自动挖矿即可观察分叉与最长链重组。")
    print("  日志：data/node-<port>.log")
    if args.detach:
        print("  节点已在后台常驻运行（本窗口可直接关闭）")
        print("  停止：python stop.py")
        print("=" * 64)
        return 0
    print("  按 Ctrl+C 停止全部节点")
    print("=" * 64)

    try:
        while True:
            for p, _, port in procs:
                if p.poll() is not None:
                    print(f"[警告] 节点 {port} 已退出（返回码 {p.returncode}），详见 data/node-{port}.log")
            time.sleep(1.5)
    except KeyboardInterrupt:
        print("\n正在停止节点…")
    finally:
        for p, logf, _ in procs:
            if args.detach:
                continue
            try:
                p.terminate()
            except Exception:
                pass
        time.sleep(0.6)
        for p, logf, _ in procs:
            try:
                p.kill()
            except Exception:
                pass
            logf.close()
        print("已全部停止")
    return 0


if __name__ == "__main__":
    sys.exit(main())
