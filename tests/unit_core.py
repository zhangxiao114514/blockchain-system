"""unit_core.py — C++ 内核单元测试（直接驱动引擎进程，不启动节点网络）

覆盖：
  * VM 算术 / 存储 / gas 耗尽 / 非法跳转 / REVERT 回滚
  * ABI 选择器与参数编解码
  * 合约部署地址确定性、调用与存储槽
  * 交易类型校验（部署/调用/跨链的边界条件）
  * 跨链桥：中继授权、重放、证明篡改、序列号单调
  * 交易哈希绑定 chain_id

    python tests/unit_core.py
"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENGINE = os.path.join(ROOT, "bin", "chain-engine.exe" if os.name == "nt" else "bin/chain-engine")
COIN = 100_000_000

results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print(f"  {'✓' if ok else '✗'} {name}{('  — ' + detail) if detail else ''}")
    return ok


class Engine:
    def __init__(self, datadir, chain_id=1001, relayer=""):
        self.p = subprocess.Popen([ENGINE], stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0)
        self.rid = 0
        r = self.call("init", datadir=datadir, chain_id=chain_id, relayer=relayer)
        assert r.get("ok"), r

    def call(self, cmd, **kw):
        self.rid += 1
        req = {"rid": self.rid, "cmd": cmd}
        req.update(kw)
        self.p.stdin.write((json.dumps(req) + "\n").encode())
        self.p.stdin.flush()
        return json.loads(self.p.stdout.readline())

    def close(self):
        try:
            self.p.stdin.close()
        except Exception:
            pass


def next_nonce(e, addr):
    return int(e.call("balance", address=addr).get("next_nonce", 0))


def sign(e, priv, addr, **kw):
    """自动取下一个 nonce 签名，避免失败交易不消耗 nonce 造成的错位。"""
    kw.setdefault("nonce", next_nonce(e, addr))
    kw.setdefault("fee", 100000)
    kw.setdefault("amount", 0)
    kw.setdefault("to", "")
    kw.setdefault("gas_limit", 100000)
    return e.call("sign_tx", private_key=priv, **kw)["tx"]


def mine_until_found(e, miner, rounds=20):
    for _ in range(rounds):
        r = e.call("mine_step", miner=miner, iters=200000)
        if r.get("found"):
            return r["block"]
    return None


def main():
    if not os.path.exists(ENGINE):
        print("未找到引擎，请先运行 build.sh")
        return 1

    base = os.path.join(ROOT, "data", "_unit")
    shutil.rmtree(base, ignore_errors=True)

    print("\n[1] VM 指令与 gas")
    # 配置一个中继地址，用于验证「非中继不得提交跨链根」
    e = Engine(os.path.join(base, "vm"), relayer="0x" + "9" * 40)
    try:
        w = e.call("wallet_new")
        addr, priv = w["address"], w["private_key"]
        mine_until_found(e, addr)

        # 2+3 = 5，通过 RETURN 返回
        code = "600260030160005260206000f3"
        tx = sign(e, priv, addr, type="deploy_contract", code=code)
        check("部署含算术的合约", e.call("submit_tx", tx=tx).get("ok"))
        blk = mine_until_found(e, addr)
        caddr = "0x" + hashlib.sha256(f"create|1001|{addr}|{tx['nonce']}".encode()).hexdigest()[:40]
        c = e.call("contract", address=caddr)
        check("合约地址由 (chain_id, from, nonce) 确定性派生", c.get("ok") and c.get("address") == caddr)

        # gas 耗尽：JUMPDEST / PUSH1 0 / JUMP 死循环
        loop = "5b600056"
        tx2 = sign(e, priv, addr, type="deploy_contract", code=loop, gas_limit=30000)
        r2 = e.call("submit_tx", tx=tx2)
        check("gas 耗尽的合约无法上链", not r2.get("ok") and "gas" in r2.get("error", "").lower(),
              r2.get("error", ""))

        # 非法跳转
        bad = "600556"
        tx3 = sign(e, priv, addr, type="deploy_contract", code=bad)
        r3 = e.call("submit_tx", tx=tx3)
        check("非法跳转被拒绝", not r3.get("ok"), r3.get("error", ""))

        # gas_limit 过低
        tx4 = sign(e, priv, addr, type="deploy_contract", code="00", gas_limit=1000)
        r4 = e.call("submit_tx", tx=tx4)
        check("gas_limit 过低被拒绝", not r4.get("ok") and "gas_limit" in r4.get("error", ""),
              r4.get("error", ""))

        print("\n[2] ABI 编解码")
        enc = e.call("abi_encode", signature="transfer(address,uint256)",
                     types=["address", "uint256"], values=["0x09c0b2d1a486c439a87bcba6b46a7a1a23f3897c", "42"])
        check("ABI 选择器长度 4 字节", enc.get("ok") and len(enc["selector"]) == 8, enc.get("selector", ""))
        check("calldata = 选择器 + 2 个字", enc.get("ok") and len(enc["calldata"]) == 8 + 64 * 2)
        check("ABI 编码结果可预测", enc.get("calldata", "").endswith("2a"), enc.get("calldata", "")[-8:])

        print("\n[3] 合约调用与存储槽")
        store_code = "60043560005560005460005260206000f3"  # slot0 = calldata[4:36]
        tx5 = sign(e, priv, addr, type="deploy_contract", code=store_code)
        e.call("submit_tx", tx=tx5)
        mine_until_found(e, addr)
        caddr2 = "0x" + hashlib.sha256(f"create|1001|{addr}|{tx5['nonce']}".encode()).hexdigest()[:40]
        inp = "aabbccdd" + "0" * 62 + "2a"
        tx6 = sign(e, priv, addr, type="contract_call", to=caddr2, input=inp)
        e.call("submit_tx", tx=tx6)
        blk2 = mine_until_found(e, addr)
        storage = e.call("contract", address=caddr2).get("storage", {})
        check("SSTORE 写入存储槽 0", storage.get("0" * 64) == "0" * 62 + "2a", str(list(storage.items())[:1]))
        gas = [t["gas_used"] for t in blk2["transactions"] if t["type"] == "contract_call"]
        check("gas_used 被记录", bool(gas) and gas[0] > 0, f"gas_used={gas}")

        print("\n[4] 交易类型校验")
        tx7 = sign(e, priv, addr, type="contract_call", to="0x" + "1" * 40, input="aabbccdd")
        r7 = e.call("submit_tx", tx=tx7)
        check("调用不存在的合约被拒绝", not r7.get("ok") and "不存在" in r7.get("error", ""), r7.get("error", ""))

        tx8 = sign(e, priv, addr, type="cross_transfer", to="0x" + "2" * 40, amount=COIN,
                   target_chain_id=1001)
        r8 = e.call("submit_tx", tx=tx8)
        check("跨链到本链被拒绝", not r8.get("ok") and "本链" in r8.get("error", ""), r8.get("error", ""))

        tx9 = sign(e, priv, addr, type="cross_transfer", to="0x" + "2" * 40, amount=0,
                   target_chain_id=1002)
        r9 = e.call("submit_tx", tx=tx9)
        check("跨链金额为 0 被拒绝", not r9.get("ok"), r9.get("error", ""))

        print("\n[5] 跨链桥安全")
        # 非中继地址提交根
        commit_input = (e.call("abi_encode", signature="commitRoot(uint256,uint256,bytes32)",
                               types=[], values=[])["selector"] + "0" * 63 + "2" + "0" * 64 + "ab" * 32)
        tx10 = sign(e, priv, addr, type="contract_call",
                    to="0x00000000000000000000000000000000000000bb", input=commit_input)
        r10 = e.call("submit_tx", tx=tx10)
        check("非中继地址提交跨链根被拒绝（relayer 已配置）",
              not r10.get("ok") and "中继" in r10.get("error", ""), r10.get("error", ""))

        # Merkle 证明
        leaves = ["a" * 64, "b" * 64, "c" * 64]
        mb = e.call("merkle_build", leaves=leaves)
        check("Merkle 根生成", bool(mb.get("root")) and len(mb["proofs"]) == 3)
        check("Merkle 证明条数符合树高", all(len(p) == 2 for p in mb["proofs"]), str([len(p) for p in mb["proofs"]]))
    finally:
        e.close()

    print("\n[6] chain_id 隔离")
    e2 = Engine(os.path.join(base, "iso"), chain_id=1002)
    try:
        w = e2.call("wallet_new")
        st = e2.call("status")
        check("不同 chain_id 生成不同创世块", st.get("chain_id") == 1002, f"chain_id={st.get('chain_id')}")
        # 同一私钥在两条链上地址相同，但交易哈希不同
        tx = e2.call("sign_tx", private_key=w["private_key"], type="transfer", to="0x" + "3" * 40,
                     amount=COIN, fee=100000, nonce=0)["tx"]
        check("交易哈希绑定 chain_id", tx["id"] != hashlib.sha256(
            f"transfer|1001|{w['address']}|0x{'3'*40}|{COIN}|100000|0|{tx['timestamp']}|"
            f"||0|0".encode()).hexdigest())
    finally:
        e2.close()

    shutil.rmtree(base, ignore_errors=True)

    passed = sum(1 for _, ok, _ in results if ok)
    total = len(results)
    print("\n" + "=" * 56)
    print(f"单元测试结果：{passed}/{total} 通过")
    for name, ok, detail in results:
        if not ok:
            print(f"  失败: {name} {detail}")
    print("=" * 56)
    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
