"""crosschain.py — 双链跨链端到端测试

覆盖：
  1. 双链（1001/1002）独立启动与 chain_id 隔离
  2. 托管桥 cross_transfer：锁定、全局序列号、CrossOut 事件
  3. 中继 commitRoot + Merkle 证明认领
  4. 目标链到账金额正确
  5. 重放同一 (源链, 序列号) 必须被拒绝
  6. 篡改金额的证明必须被拒绝
  7. 资产守恒：源链锁定 == 目标链铸造
  8. 跨链签名不可跨链重放（tx id 绑定 chain_id）

    python tests/crosschain.py
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
sys.path.insert(0, os.path.join(ROOT, "bridge"))

from relayer import CROSS_OUT_TOPIC, Relayer, Chain  # noqa: E402

PY = sys.executable
CLI = os.path.join(ROOT, "node", "cli.py")
CHAINCTL = os.path.join(ROOT, "bin", "chainctl.exe" if os.name == "nt" else "chainctl")
COIN = 100_000_000

CHAINS = [
    # (chain_id, http_port, p2p_port)
    (1001, 3001, 6001),
    (1002, 3002, 6002),
]

results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print(f"  {'✓' if ok else '✗'} {name}{('  — ' + detail) if detail else ''}")
    return ok


def api(port, path, method="GET", body=None, timeout=20):
    url = f"http://127.0.0.1:{port}{path}"
    data = json.dumps(body).encode() if body is not None else None
    headers = {"Content-Type": "application/json"} if data else {}
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode())


def expect_error(port, path, body, keyword=""):
    """期望请求失败，返回错误信息。"""
    try:
        api(port, path, "POST", body)
        return None
    except urllib.error.HTTPError as e:
        msg = e.read().decode()
        try:
            msg = json.loads(msg).get("error", msg)
        except Exception:
            pass
        return msg if (not keyword or keyword in msg) else None


def wait_ready(port, timeout=30):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            api(port, "/api/status", timeout=3)
            return True
        except Exception:
            time.sleep(0.4)
    return False


def wait_until(fn, timeout=60, interval=0.3):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            if fn():
                return True
        except Exception:
            pass
        time.sleep(interval)
    return False


def wait_miner_funded(port, coins, timeout=90):
    """等矿工钱包攒够手续费（水龙头从矿工钱包出账）。"""
    def ok():
        ws = api(port, "/api/wallets")["wallets"]
        miner = [w for w in ws if w["is_miner"]]
        return bool(miner) and int(miner[0]["balance"]) >= coins * COIN
    return wait_until(ok, timeout)


def gen_relayer_key():
    out = subprocess.run([CHAINCTL, "--datadir", os.path.join(ROOT, "data", "_cc_relayer"), "wallet-new"],
                         capture_output=True)
    return json.loads(out.stdout.decode("utf-8", "replace"))


def main():
    engine = os.path.join(ROOT, "bin", "chain-engine.exe" if os.name == "nt" else "chain-engine")
    if not os.path.exists(engine):
        print("未找到引擎，请先运行 build.sh")
        return 1

    for _, port, _ in CHAINS:
        shutil.rmtree(os.path.join(ROOT, "data", f"chain{port}"), ignore_errors=True)
    shutil.rmtree(os.path.join(ROOT, "data", "_cc_relayer"), ignore_errors=True)

    relayer_kp = gen_relayer_key()
    relayer_addr = relayer_kp["address"]

    procs = []
    print("\n[1] 启动双链节点")
    for chain_id, port, p2p in CHAINS:
        cmd = [PY, "-u", CLI, "run", "--datadir", os.path.join(ROOT, "data", f"chain{port}"),
               "--port", str(port), "--p2p", str(p2p), "--chain-id", str(chain_id),
               "--relayer", relayer_addr, "--auto-mine"]
        log = open(os.path.join(ROOT, "data", f"cc-{chain_id}.log"), "w", encoding="utf-8")
        procs.append((subprocess.Popen(cmd, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT), log))
        time.sleep(0.4)

    try:
        ok = all(wait_ready(port) for _, port, _ in CHAINS)
        check("两条链节点就绪", ok)

        ids = [api(port, "/api/status")["chain_id"] for _, port, _ in CHAINS]
        check("chain_id 相互隔离", ids == [1001, 1002], f"{ids}")

        # 中继钱包导入 + 发测试币
        print("\n[2] 准备中继与钱包")
        for _, port, _ in CHAINS:
            api(port, "/api/wallets/import", "POST", {"private_key": relayer_kp["private_key"], "label": "中继"})
        for _, port, _ in CHAINS:
            wait_miner_funded(port, 110)
            api(port, "/api/faucet", "POST", {"address": relayer_addr, "amount": "100"})
        check("中继钱包已在两条链就绪", True, relayer_addr)

        user = api(3001, "/api/wallets", "POST", {"label": "跨链用户"})["wallet"]["address"]
        wait_miner_funded(3001, 110)
        api(3001, "/api/faucet", "POST", {"address": user, "amount": "100"})
        recipient = api(3002, "/api/wallets", "POST", {"label": "B链接收"})["wallet"]["address"]
        ok = wait_until(lambda: int(api(3001, f"/api/account?address={user}")["balance"]) == 100 * COIN, 40)
        check("源链用户已获测试币", ok, f"{user[:12]}…")

        # ---------- 跨链转账 ----------
        print("\n[3] 发起跨链转账 A(1001) → B(1002)")
        amount = 12 * COIN
        api(3001, "/api/bridge/transfer", "POST",
            {"from": user, "to": recipient, "target_chain_id": 1002, "amount": "12"})
        ok = wait_until(lambda: int(api(3001, "/api/bridge")["seq"]) >= 1, 40)
        br = api(3001, "/api/bridge")
        check("源链已锁定并分配全局序列号", ok, f"seq={br['seq']} locked={br['locked_str']}")

        bal_before = int(api(3002, f"/api/account?address={recipient}")["balance"])
        check("目标链尚未到账（等待中继）", bal_before == 0, f"余额 {bal_before}")

        logs = api(3001, f"/api/logs?topic={CROSS_OUT_TOPIC}")["logs"]
        check("CrossOut 事件已写入区块日志", len(logs) == 1, f"{len(logs)} 条")

        # ---------- 中继 ----------
        print("\n[4] 运行可信中继（commitRoot + Merkle 证明认领）")
        rl = Relayer([Chain(3001, 1001), Chain(3002, 1002)], verbose=False)
        rl.run_once()
        ok = wait_until(lambda: int(api(3002, f"/api/account?address={recipient}")["balance"]) == amount, 40)
        check("目标链到账 12 币", ok, f"余额 {int(api(3002, f'/api/account?address={recipient}')['balance']) / COIN}")

        brb = api(3002, "/api/bridge")
        check("目标链记录了源链的跨链根", "1001" in (brb.get("relay_roots") or {}),
              str(list((brb.get("relay_roots") or {}).keys())))

        # ---------- 资产守恒 ----------
        print("\n[5] 资产守恒与防重复")
        bra = api(3001, "/api/bridge")
        check("源链锁定 == 目标链铸造", int(bra["locked"]) == int(brb["minted"]),
              f"{bra['locked_str']} vs {brb['minted_str']}")

        # 重放：同一 (源链, 序列号) 再认领一次
        ev = rl.cross_out_events(Chain(3001, 1001), 1002)[0]
        mb = api(3001, "/api/_merkle", "POST", {"leaves": [rl.leaf_of(ev)]})
        err = expect_error(3002, "/api/bridge/claim", {
            "src_chain_id": 1001, "seq": ev["seq"], "index": 0, "to": recipient,
            "amount": ev["amount"], "root": mb["root"], "proof": mb["proofs"][0],
        }, "已被认领")
        check("重放同一序列号被拒绝", err is not None, err or "未被拒绝")

        # 篡改金额
        err2 = expect_error(3002, "/api/bridge/claim", {
            "src_chain_id": 1001, "seq": ev["seq"] + 1, "index": 0, "to": recipient,
            "amount": 999 * COIN, "root": mb["root"], "proof": mb["proofs"][0],
        }, "默克尔")
        check("篡改金额的证明被拒绝", err2 is not None, err2 or "未被拒绝")

        # chain_id 隔离：把 A 链上已确认的签名交易原样提交到 B 链
        print("\n[6] chain_id 隔离")
        tx_a = None
        for b in api(3001, "/api/blocks?limit=40")["blocks"]:
            for t in b.get("transactions", []):
                if t["type"] == "transfer":
                    tx_a = t
                    break
            if tx_a:
                break
        if tx_a:
            err3 = expect_error(3002, "/api/tx/raw", {"tx": tx_a}, "")
            check("A 链签名交易在 B 链被拒绝（chain_id 隔离）",
                  err3 is not None and "哈希" in (err3 or ""), err3 or "未被拒绝")
        else:
            check("A 链签名交易在 B 链被拒绝（chain_id 隔离）", False, "未找到可复用的交易")

        # ---------- 反向 ----------
        print("\n[7] 反向跨链 B(1002) → A(1001)")
        back_to = api(3001, "/api/wallets", "POST", {"label": "A链回程"})["wallet"]["address"]
        api(3002, "/api/bridge/transfer", "POST",
            {"from": recipient, "to": back_to, "target_chain_id": 1001, "amount": "5"})
        wait_until(lambda: int(api(3002, "/api/bridge")["seq"]) >= 1, 40)
        rl.run_once()
        ok = wait_until(lambda: int(api(3001, f"/api/account?address={back_to}")["balance"]) == 5 * COIN, 40)
        check("反向跨链 5 币到账", ok, f"余额 {int(api(3001, f'/api/account?address={back_to}')['balance']) / COIN}")

        # ---------- 校验 ----------
        print("\n[8] 两条链完整性校验")
        for chain_id, port, _ in CHAINS:
            v = api(port, "/api/validate", timeout=60)
            check(f"链 {chain_id} 全链校验通过", bool(v.get("valid")), v.get("error") or f"高度 {v.get('height')}")

    finally:
        print("\n[9] 关闭节点")
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
    print("\n" + "=" * 60)
    print(f"跨链测试结果：{passed}/{total} 通过")
    for name, ok, detail in results:
        if not ok:
            print(f"  失败: {name} {detail}")
    print("=" * 60)
    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
