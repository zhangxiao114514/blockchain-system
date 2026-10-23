"""integration.py — 端到端集成测试

自包含：启动 3 个节点 → 验证 P2P 同步 → 领币 → 转账 → 分叉收敛 → 全链校验 → 清理。

    python tests/integration.py
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PY = sys.executable
CLI = os.path.join(ROOT, "node", "cli.py")
NODES = [(3001, 6001, [], True), (3002, 6002, ["127.0.0.1:6001"], False), (3003, 6003, ["127.0.0.1:6001"], False)]

results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print(f"  {'✓' if ok else '✗'} {name}{('  — ' + detail) if detail else ''}")
    return ok


def api(port, path, method="GET", body=None, timeout=15):
    url = f"http://127.0.0.1:{port}{path}"
    data = json.dumps(body).encode() if body is not None else None
    headers = {"Content-Type": "application/json"} if data else {}
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode())


def wait_ready(port, timeout=25):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            api(port, "/api/status", timeout=3)
            return True
        except Exception:
            time.sleep(0.4)
    return False


def wait_until(fn, timeout=60, interval=0.5):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            if fn():
                return True
        except Exception:
            pass
        time.sleep(interval)
    return False


def status(port):
    return api(port, "/api/status")


def main():
    # ---------- 清理 ----------
    for http_port, _, _, _ in NODES:
        d = os.path.join(ROOT, "data", f"n{http_port}")
        shutil.rmtree(d, ignore_errors=True)
    os.makedirs(os.path.join(ROOT, "data"), exist_ok=True)

    engine = os.path.join(ROOT, "bin", "chain-engine.exe" if os.name == "nt" else "chain-engine")
    if not os.path.exists(engine):
        print("未找到 C++ 引擎，请先运行 build.sh")
        return 1

    procs = []
    print("\n[1] 启动 3 个节点")
    for http_port, p2p_port, seeds, auto_mine in NODES:
        datadir = os.path.join(ROOT, "data", f"n{http_port}")
        cmd = [PY, "-u", CLI, "run", "--datadir", datadir, "--port", str(http_port), "--p2p", str(p2p_port)]
        for s in seeds:
            cmd += ["--seed", s]
        if auto_mine:
            cmd += ["--auto-mine"]
        log = open(os.path.join(ROOT, "data", f"itest-{http_port}.log"), "w", encoding="utf-8")
        procs.append((subprocess.Popen(cmd, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT), log))
        time.sleep(0.3)

    try:
        ok_all = all(wait_ready(p) for p, _ in [(n[0], None) for n in NODES])
        check("三个节点 HTTP 服务就绪", ok_all)

        # ---------- P2P 同步 ----------
        print("\n[2] P2P 握手与区块同步")
        ok = wait_until(lambda: all(status(p)["peer_count"] >= 2 for p, _, _, _ in NODES), timeout=30)
        check("每个节点都发现 2 个对等节点", ok,
              " ".join(f"{p}:{status(p)['peer_count']}" for p, _, _, _ in NODES))

        ok = wait_until(lambda: all(status(p)["chain"]["height"] >= 3 for p, _, _, _ in NODES), timeout=60)
        heights = [status(p)["chain"]["height"] for p, _, _, _ in NODES]
        check("区块广播到全部节点", ok, f"高度 {heights}")

        ok = wait_until(lambda: len({status(p)["chain"]["tip_hash"] for p, _, _, _ in NODES}) == 1, timeout=30)
        tips = {status(p)["chain"]["tip_hash"][:12] for p, _, _, _ in NODES}
        check("三节点链尖一致（最长链共识）", ok, f"{tips}")

        # ---------- 钱包 / 水龙头 ----------
        print("\n[3] 钱包与测试币")
        w = api(3001, "/api/wallets", "POST", {"label": "集成测试钱包"})
        addr = w["wallet"]["address"]
        check("创建钱包", bool(addr), addr)

        tx = api(3001, "/api/faucet", "POST", {"address": addr, "amount": "100"})
        check("水龙头提交交易", bool(tx.get("txid")), tx.get("txid", "")[:16] + "…")

        ok = wait_until(lambda: int(api(3001, f"/api/account?address={addr}")["balance"]) == 100 * 10**8,
                        timeout=45)
        bal = api(3001, f"/api/account?address={addr}")["balance"]
        check("100 币到账（含出块确认）", ok, f"余额 {bal / 10**8:.2f}")

        ok = wait_until(lambda: int(api(3003, f"/api/account?address={addr}")["balance"]) == 100 * 10**8,
                        timeout=30)
        check("余额在其它节点同步可见", ok)

        # ---------- 转账 ----------
        print("\n[4] 转账与确认")
        w2 = api(3001, "/api/wallets", "POST", {"label": "收款钱包"})
        to = w2["wallet"]["address"]
        api(3001, "/api/tx", "POST", {"from": addr, "to": to, "amount": "25.5", "fee": "0.001"})
        ok = wait_until(lambda: int(api(3001, f"/api/account?address={to}")["balance"]) == int(25.5 * 10**8),
                        timeout=45)
        check("转账 25.5 币成功确认", ok)

        src = api(3002, f"/api/account?address={addr}")
        check("付款方余额扣减正确（100 - 25.5 - 0.001）",
              int(src["balance"]) == int(100 * 10**8 - 25.5 * 10**8 - 10**5),
              f"{int(src['balance']) / 10**8:.3f}")
        check("付款方 nonce 递增", int(src["nonce"]) == 1, f"nonce={src['nonce']}")

        # ---------- 分叉与收敛 ----------
        print("\n[5] 多矿工竞争与分叉收敛")
        api(3002, "/api/automine", "POST", {"enabled": True})
        api(3003, "/api/automine", "POST", {"enabled": True})
        time.sleep(18)
        ok = wait_until(lambda: len({status(p)["chain"]["tip_hash"] for p, _, _, _ in NODES}) == 1, timeout=40)
        hs = [status(p)["chain"]["height"] for p, _, _, _ in NODES]
        check("三节点挖矿后仍收敛到同一链", ok, f"高度 {hs}")
        check("高度已增长（确有新块产生）", max(hs) >= 8, f"最大高度 {max(hs)}")
        api(3002, "/api/automine", "POST", {"enabled": False})
        api(3003, "/api/automine", "POST", {"enabled": False})

        # ---------- 校验 ----------
        print("\n[6] 全链完整性校验")
        for p, _, _, _ in NODES:
            v = api(p, "/api/validate", timeout=60)
            check(f"节点 {p} 链校验通过", bool(v.get("valid")), v.get("error") or f"高度 {v.get('height')}")

        # ---------- 持久化 ----------
        print("\n[7] 数据持久化")
        files = []
        for p, _, _, _ in NODES:
            f = os.path.join(ROOT, "data", f"n{p}", "blocks.ndjson")
            files.append(os.path.getsize(f) if os.path.exists(f) else 0)
        check("区块文件已落盘", all(s > 0 for s in files), f"字节数 {files}")
        ks = os.path.join(ROOT, "data", "n3001", "keystore.json")
        with open(ks, "r", encoding="utf-8") as f:
            blob = json.load(f)
        check("钱包库为密文（无明文私钥）", "ct" in blob and "wallets" not in blob, f"字段 {sorted(blob)}")

    finally:
        print("\n[8] 关闭节点")
        for p, log in procs:
            try:
                p.terminate()
            except Exception:
                pass
        time.sleep(1.0)
        for p, log in procs:
            try:
                p.kill()
            except Exception:
                pass
            log.close()

    passed = sum(1 for _, ok, _ in results if ok)
    total = len(results)
    print("\n" + "=" * 56)
    print(f"集成测试结果：{passed}/{total} 通过")
    if passed != total:
        for name, ok, detail in results:
            if not ok:
                print(f"  失败: {name} {detail}")
    print("=" * 56)
    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
