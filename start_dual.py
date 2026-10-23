"""start_dual.py — 一键启动双链跨链调试环境

    python start_dual.py --fresh          # 链 A(1001):3001 / 链 B(1002):3002
    python start_dual.py --fresh --detach # 后台常驻

流程：
  1. 生成一个中继钱包（两条链共用同一地址）
  2. 分别启动两条链的节点，并把中继地址配置为各自的 relayer
  3. 把中继私钥导入两个节点的加密钱包库
  4. 给中继地址发放测试币（用于支付 commitRoot / claim 的手续费）
  5. 打印后续调试命令
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.abspath(__file__))
PY = sys.executable
CLI = os.path.join(ROOT, "node", "cli.py")
CHAINCTL = os.path.join(ROOT, "bin", "chainctl.exe" if os.name == "nt" else "bin/chainctl")

PLAN = [
    # (name, chain_id, http_port, p2p_port)
    ("A", 1001, 3001, 6001),
    ("B", 1002, 3002, 6002),
]


def api(port, path, method="GET", body=None, timeout=20):
    url = f"http://127.0.0.1:{port}{path}"
    data = json.dumps(body).encode() if body is not None else None
    headers = {"Content-Type": "application/json"} if data else {}
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode())


def wait_ready(port, timeout=30):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            api(port, "/api/status", timeout=3)
            return True
        except Exception:
            time.sleep(0.4)
    return False


def gen_relayer_key():
    """用 C++ 命令行生成中继密钥对（不依赖任何节点的钱包库）。"""
    out = subprocess.run([CHAINCTL, "--datadir", os.path.join(ROOT, "data", "_relayer_tmp"), "wallet-new"],
                         capture_output=True)
    if out.returncode != 0:
        raise RuntimeError("生成中继密钥失败: " + out.stderr.decode("utf-8", "replace"))
    return json.loads(out.stdout.decode("utf-8", "replace"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fresh", action="store_true")
    ap.add_argument("--detach", action="store_true")
    ap.add_argument("--no-mine", action="store_true")
    ap.add_argument("--faucet", default="60")
    args = ap.parse_args()

    engine = os.path.join(ROOT, "bin", "chain-engine.exe" if os.name == "nt" else "bin/chain-engine")
    if not os.path.exists(engine):
        print("未找到引擎，正在编译…")
        if subprocess.call(["bash", os.path.join(ROOT, "build.sh")], cwd=ROOT) != 0:
            return 1

    if args.fresh:
        for _, _, port, _ in PLAN:
            d = os.path.join(ROOT, "data", f"chain{port}")
            shutil.rmtree(d, ignore_errors=True)
        shutil.rmtree(os.path.join(ROOT, "data", "_relayer_tmp"), ignore_errors=True)
        print("已清空双链数据")

    relayer = gen_relayer_key()
    print(f"中继地址 {relayer['address']}")

    procs = []
    for name, chain_id, port, p2p in PLAN:
        datadir = os.path.join(ROOT, "data", f"chain{port}")
        log_path = os.path.join(ROOT, "data", f"chain-{chain_id}.log")
        os.makedirs(os.path.join(ROOT, "data"), exist_ok=True)
        cmd = [PY, "-u", CLI, "run", "--datadir", datadir, "--port", str(port), "--p2p", str(p2p),
               "--chain-id", str(chain_id), "--relayer", relayer["address"]]
        if not args.no_mine:
            cmd += ["--auto-mine"]
        if args.detach:
            logf = open(log_path, "w", encoding="utf-8")
            p = subprocess.Popen(cmd, cwd=ROOT, stdout=logf, stderr=subprocess.STDOUT)
            procs.append((p, logf, port))
        else:
            logf = open(log_path, "w", encoding="utf-8")
            p = subprocess.Popen(cmd, cwd=ROOT, stdout=logf, stderr=subprocess.STDOUT)
            procs.append((p, logf, port))
        time.sleep(0.5)

    print("等待节点就绪…")
    for _, _, port, _ in PLAN:
        if not wait_ready(port):
            print(f"节点 {port} 启动失败，请查看 data/chain-*.log")
            return 1

    # 导入中继私钥并发放手续费
    for name, chain_id, port, _ in PLAN:
        try:
            api(port, "/api/wallets/import", "POST",
                {"private_key": relayer["private_key"], "label": "跨链中继"})
        except urllib.error.HTTPError as e:
            print(f"导入中继钱包失败({port}): {e.read().decode()[:100]}")
        # 等矿工钱包攒够余额再发测试币
        for _ in range(60):
            try:
                ws = api(port, "/api/wallets")["wallets"]
                miner = [w for w in ws if w["is_miner"]]
                if miner and int(miner[0]["balance"]) >= int(args.faucet) * 100_000_000:
                    break
            except Exception:
                pass
            time.sleep(0.5)
        try:
            api(port, "/api/faucet", "POST", {"address": relayer["address"], "amount": args.faucet})
        except urllib.error.HTTPError as e:
            print(f"发放测试币失败({port}): {e.read().decode()[:100]}")

    time.sleep(3)
    print()
    print("=" * 68)
    print("  双链跨链调试环境已就绪")
    print("=" * 68)
    for name, chain_id, port, p2p in PLAN:
        print(f"  链 {name}  chain_id={chain_id}  控制台 http://127.0.0.1:{port}  p2p={p2p}")
    print(f"  中继地址 {relayer['address']}")
    print()
    print("  启动中继：")
    print(f"    python bridge/relayer.py --chains {PLAN[0][2]}:{PLAN[0][1]},{PLAN[1][2]}:{PLAN[1][1]}")
    print()
    print("  发起跨链转账（A→B）：")
    print(f"    python node/cli.py xfer --port {PLAN[0][2]} --from <钱包> --to <B链地址> "
          f"--target-chain {PLAN[1][1]} --amount 10")
    print("=" * 68)

    if args.detach:
        print("  节点已在后台运行，停止：python stop.py")
        return 0

    try:
        while True:
            time.sleep(2)
            for p, _, port in procs:
                if p.poll() is not None:
                    print(f"[警告] 节点 {port} 已退出（{p.returncode}）")
    except KeyboardInterrupt:
        print("\n正在停止…")
    finally:
        for p, logf, _ in procs:
            try:
                p.terminate()
            except Exception:
                pass
        time.sleep(0.8)
        for p, logf, _ in procs:
            try:
                p.kill()
            except Exception:
                pass
            logf.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
