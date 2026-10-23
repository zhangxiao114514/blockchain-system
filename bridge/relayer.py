"""relayer.py — 可信中继（跨链）

工作流程（每个方向独立）：
  1. 扫描源链的 CrossOut 事件日志（托管桥在 cross_transfer 时发出）
  2. 按 (seq, to, amount) 重建 Merkle 叶子，调用引擎 merkle_build 得到根与证明
  3. 把根提交到目标链（commitRoot，只有中继地址有权提交，且高度必须递增）
  4. 对每条尚未认领的事件，向目标链提交 claim + Merkle 证明

安全性由三件事保证：
  * 根只能由中继地址提交（chain_id 隔离 + 高度单调）
  * 叶子绑定 (源链, 序列号, 收款地址, 金额)，无法篡改
  * 目标链对 (源链, 序列号) 做去重，防重放与超发

用法：
  python bridge/relayer.py --chains 3001:1001,3002:1002 [--interval 2] [--once]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import time
import urllib.error
import urllib.request

CROSS_OUT_TOPIC = hashlib.sha256(b"CrossOut(uint256,uint256,address,uint256,uint256)").hexdigest()
CROSS_IN_TOPIC = hashlib.sha256(b"CrossIn(uint256,uint256)").hexdigest()


def api(port, path, method="GET", body=None, timeout=20):
    url = f"http://127.0.0.1:{port}{path}"
    data = json.dumps(body).encode() if body is not None else None
    headers = {"Content-Type": "application/json"} if data else {}
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode())


def word_to_int(hexstr: str) -> int:
    s = hexstr[2:] if hexstr.startswith("0x") else hexstr
    return int(s or "0", 16)


def addr_from_word(hexstr: str) -> str:
    s = hexstr[2:] if hexstr.startswith("0x") else hexstr
    return "0x" + s[-40:]


class Chain:
    def __init__(self, port: int, chain_id: int):
        self.port = port
        self.chain_id = chain_id

    def __repr__(self):
        return f"chain{self.chain_id}@{self.port}"


class Relayer:
    def __init__(self, chains, interval=2.0, verbose=True):
        self.chains = chains
        self.interval = interval
        self.verbose = verbose
        self.stats = {"roots": 0, "claims": 0, "errors": 0}

    def log(self, msg):
        if self.verbose:
            print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)

    # ---------- 源链事件 ----------
    def cross_out_events(self, src: Chain, dst_chain_id: int):
        r = api(src.port, f"/api/logs?topic={CROSS_OUT_TOPIC}&limit=1000")
        out = []
        for lg in r.get("logs", []):
            topics = lg.get("topics") or []
            data = lg.get("data") or ""
            if len(topics) < 3 or len(data) < 64 * 4:
                continue
            words = [data[i:i + 64] for i in range(0, len(data), 64)]
            target_chain = word_to_int(words[3])
            if target_chain != dst_chain_id:
                continue
            out.append({
                "src_chain": word_to_int(topics[1]),
                "seq": word_to_int(topics[2]),
                "from": addr_from_word(words[0]),
                "to": addr_from_word(words[1]),
                "amount": word_to_int(words[2]),
                "target_chain": target_chain,
                "block": lg.get("block_index"),
            })
        out.sort(key=lambda e: e["seq"])
        return out

    def claimed_set(self, dst: Chain, src_chain_id: int):
        r = api(dst.port, f"/api/logs?topic={CROSS_IN_TOPIC}&limit=1000")
        done = set()
        for lg in r.get("logs", []):
            topics = lg.get("topics") or []
            if len(topics) < 3:
                continue
            if word_to_int(topics[1]) != src_chain_id:
                continue
            done.add(word_to_int(topics[2]))
        return done

    @staticmethod
    def leaf_of(ev):
        payload = f"cross|{ev['src_chain']}|{ev['seq']}|{ev['to']}|{ev['amount']}"
        return hashlib.sha256(payload.encode()).hexdigest()

    # ---------- 单方向处理 ----------
    def bridge(self, src: Chain, dst: Chain) -> int:
        events = self.cross_out_events(src, dst.chain_id)
        if not events:
            return 0

        claimed = self.claimed_set(dst, src.chain_id)
        pending = [e for e in events if e["seq"] not in claimed]
        if not pending:
            return 0

        leaves = [self.leaf_of(e) for e in events]
        # 用引擎构建 Merkle 树，保证与链上校验规则完全一致
        mb = self._merkle_build(src, leaves)
        root = mb["root"]
        proofs = mb["proofs"]
        index_of = {e["seq"]: i for i, e in enumerate(events)}

        # 提交根（高度递增）
        br = api(dst.port, "/api/bridge")
        cur = (br.get("relay_roots") or {}).get(str(src.chain_id), {})
        height = max([e["block"] or 0 for e in events] + [0])
        if not cur or int(cur.get("height", 0)) < height:
            try:
                api(dst.port, "/api/bridge/commit", "POST",
                    {"src_chain_id": src.chain_id, "height": height, "root": root})
                self.stats["roots"] += 1
                self.log(f"提交根 {src.chain_id}→{dst.chain_id} height={height} root={root[:16]}…")
            except urllib.error.HTTPError as e:
                self.stats["errors"] += 1
                self.log(f"提交根失败: {e.read().decode()[:120]}")
                return 0

        n = 0
        for ev in pending:
            idx = index_of[ev["seq"]]
            try:
                api(dst.port, "/api/bridge/claim", "POST", {
                    "src_chain_id": ev["src_chain"],
                    "seq": ev["seq"],
                    "index": idx,
                    "to": ev["to"],
                    "amount": ev["amount"],
                    "root": root,
                    "proof": proofs[idx],
                })
                self.stats["claims"] += 1
                n += 1
                self.log(f"认领 seq={ev['seq']} {ev['amount'] / 1e8:.8f} → {ev['to'][:10]}… "
                         f"({src.chain_id}→{dst.chain_id})")
            except urllib.error.HTTPError as e:
                self.stats["errors"] += 1
                body = e.read().decode()
                self.log(f"认领 seq={ev['seq']} 失败: {body[:120]}")
        return n

    def _merkle_build(self, src: Chain, leaves):
        """引擎命令通过节点转发（节点提供 /api/_merkle 调试接口）。"""
        return api(src.port, "/api/_merkle", "POST", {"leaves": leaves}, timeout=30)

    def run_once(self):
        total = 0
        for src in self.chains:
            for dst in self.chains:
                if src is dst:
                    continue
                total += self.bridge(src, dst)
        return total

    def run(self, once=False):
        self.log(f"中继启动：{', '.join(repr(c) for c in self.chains)}")
        while True:
            try:
                self.run_once()
            except Exception as e:
                self.stats["errors"] += 1
                self.log(f"轮询异常: {e}")
            if once:
                break
            time.sleep(self.interval)


def main():
    ap = argparse.ArgumentParser(description="跨链可信中继")
    ap.add_argument("--chains", required=True, help="port:chain_id，逗号分隔，如 3001:1001,3002:1002")
    ap.add_argument("--interval", type=float, default=2.0)
    ap.add_argument("--once", action="store_true", help="只跑一轮（调试用）")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    chains = []
    for part in args.chains.split(","):
        port, _, cid = part.partition(":")
        chains.append(Chain(int(port), int(cid)))
    if len(chains) < 2:
        print("至少需要两条链")
        return 1

    Relayer(chains, interval=args.interval, verbose=not args.quiet).run(once=args.once)
    return 0


if __name__ == "__main__":
    sys.exit(main())
