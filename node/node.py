"""node.py — 节点编排：共识引擎 + 钱包 + P2P + RPC + 挖矿调度"""
from __future__ import annotations

import os
import threading
import time

from engine import Engine, EngineError
from keystore import Keystore
from p2p import P2P
from util import Logger, format_amount, now_ms, parse_amount

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BRIDGE_ADDR = "0x00000000000000000000000000000000000000bb"


def _word(v) -> str:
    """把整数或十六进制串编码成 32 字节 ABI 字（大端）。"""
    if isinstance(v, int):
        return v.to_bytes(32, "big").hex()
    s = str(v)
    if s.startswith("0x") or s.startswith("0X"):
        s = s[2:]
    return s.rjust(64, "0")[-64:]


def _addr_word(addr: str) -> str:
    s = addr[2:] if addr.startswith("0x") else addr
    return s.rjust(64, "0")
ENGINE_BIN = os.path.join(ROOT, "bin", "chain-engine.exe" if os.name == "nt" else "chain-engine")


class Node:
    def __init__(self, datadir: str, http_port: int, p2p_port: int, seeds=(), params=None,
                 auto_mine: bool = False, passphrase: str | None = None, quiet: bool = False,
                 mine_interval_ms: int = 1500, chain_id: int = 1001, relayer: str = ""):
        self.datadir = os.path.abspath(datadir)
        self.http_port = http_port
        self.p2p_port = p2p_port
        self.node_id = f"node-{p2p_port}"
        self.params = dict(params or {})
        self.params.setdefault("chain_id", chain_id)
        self.params["relayer"] = relayer
        self.chain_id = chain_id
        self.relayer = relayer
        self.log = Logger(self.node_id, quiet=quiet)
        self.started_at = time.time()

        os.makedirs(self.datadir, exist_ok=True)
        self.engine = Engine(ENGINE_BIN, self.datadir, params=self.params, logger=self.log)
        self.keystore = Keystore(os.path.join(self.datadir, "keystore.json"), passphrase)
        self.p2p = P2P(self, host="127.0.0.1", port=p2p_port, seeds=seeds)

        from rpc import RPC  # 延迟导入，避免循环依赖

        self.rpc = RPC(self, http_port)

        self.auto_mine = auto_mine
        # 空块节流：难度决定单次 PoW 成本，间隔决定出块节奏。
        # 本地演示若不节流，难度 4 会以每秒数十块的速度刷块。
        self.mine_interval = max(0.0, mine_interval_ms / 1000.0)
        self.miner_address: str = ""
        self._stop = threading.Event()
        self._wake = threading.Event()      # 唤醒挖矿线程
        self._mine_request = threading.Event()  # 有交易待确认，立刻出一块
        self._mining_thread: threading.Thread | None = None
        self._status_cache = {"height": 0, "genesis_hash": ""}
        self._status_at = 0.0
        self.counters = {"blocks_mined": 0, "txs_sent": 0, "faucet": 0, "blocks_in": 0, "txs_in": 0}

    # ---------------- 启动 / 停止 ----------------
    def start(self):
        self.log("启动共识引擎…")
        init = self.engine.start()
        if init.get("warning"):
            self.log(init["warning"], "WARN")
        self.log(f"链已就绪：高度 {init.get('height')}，创世 {str(init.get('genesis'))[:16]}…")

        self.keystore.load()
        if not self.keystore.wallets:
            w = self.engine.new_wallet()
            if not w.get("ok"):
                raise EngineError("无法生成节点钱包")
            self.keystore.add(w["address"], w["public_key"], w["private_key"], label="节点钱包")
            self.log(f"已创建节点钱包 {w['address']}")
        self.miner_address = self.keystore.first()["address"]
        self.log(f"矿工地址 {self.miner_address}")

        self._mining_thread = threading.Thread(target=self._mining_loop, name="miner", daemon=True)
        self._mining_thread.start()

        self.rpc.start()
        self.log(f"RPC/控制台 http://127.0.0.1:{self.http_port}")
        return self

    def stop(self):
        self._stop.set()
        self.rpc.stop()
        try:
            self.p2p.loop and self.p2p.loop.call_soon_threadsafe(self.p2p.server.close)
        except Exception:
            pass
        self.engine.stop()

    # ---------------- 状态缓存 ----------------
    def refresh_status(self):
        try:
            st = self.engine.status()
            if st.get("ok"):
                self._status_cache = st
                self._status_at = time.time()
        except Exception:
            pass
        return self._status_cache

    def cached_status(self, max_age: float = 1.0):
        if time.time() - self._status_at > max_age:
            self.refresh_status()
        return self._status_cache

    # ---------------- 挖矿 ----------------
    def _mining_loop(self):
        fails = 0
        while not self._stop.is_set():
            # 自动挖矿 = 持续出块；否则仅在收到交易请求时出块（保证「发送即到账」）
            if not self.auto_mine and not self._mine_request.is_set():
                self._wake.wait(0.1)
                self._wake.clear()
                continue
            try:
                r = self.engine.mine_step(self.miner_address, iters=25000)
                fails = 0
            except Exception as e:
                fails += 1
                self.log(f"挖矿异常: {e}", "WARN")
                if fails >= 3:
                    try:
                        self.engine.restart()
                        fails = 0
                    except Exception as e2:
                        self.log(f"引擎重启失败: {e2}", "ERROR")
                        time.sleep(2)
                continue
            if r.get("ok") and r.get("found"):
                block = r["block"]
                self.counters["blocks_mined"] += 1
                self._mine_request.clear()
                n_tx = len(block.get("transactions", []))
                self.log(f"⛏  出块 #{block['index']}  hash={block['hash'][:20]}…  交易 {n_tx} 笔")
                self.p2p.broadcast_threadsafe({"type": "block", "block": block})
                self._status_at = 0
                # 自动挖矿时节流；有新交易待确认则立刻继续出块
                if self.auto_mine and self.mine_interval > 0 and not self._mine_request.is_set():
                    self._wake.wait(self.mine_interval)
                    self._wake.clear()
            else:
                time.sleep(0.002)

    def mine_once(self, count: int = 1, timeout: float = 60.0) -> list:
        """同步挖 count 个块（用于手动出块按钮）。"""
        mined = []
        deadline = time.time() + timeout
        for _ in range(max(1, min(count, 10))):
            while time.time() < deadline:
                r = self.engine.mine_step(self.miner_address, iters=30000)
                if r.get("ok") and r.get("found"):
                    block = r["block"]
                    mined.append(block)
                    self.counters["blocks_mined"] += 1
                    self.log(f"⛏  出块 #{block['index']}  hash={block['hash'][:20]}…")
                    self.p2p.broadcast_threadsafe({"type": "block", "block": block})
                    self._status_at = 0
                    break
                if not r.get("ok"):
                    raise EngineError(r.get("error", "挖矿失败"))
        return mined

    # ---------------- 交易 ----------------
    def _submit(self, from_address: str, tx_type: str, to: str = "", amount_u: int = 0, fee_u: int = 0,
                code: str = "", input_hex: str = "", gas_limit: int = 0, target_chain_id: int = 0) -> dict:
        """签名 → 本地校验 → 广播 → 触发立即出块。"""
        if from_address not in self.keystore.wallets:
            raise ValueError("该钱包不在本节点，无法签名")
        pk = self.keystore.private_key(from_address)
        acc = self.engine.account(from_address)
        if not acc.get("ok"):
            raise EngineError("查询余额失败")
        if int(acc.get("balance", 0)) < amount_u + fee_u:
            raise ValueError(f"余额不足：可用 {format_amount(acc.get('balance', 0))}")
        signed = self.engine.sign_tx(pk, to, amount_u, fee_u, int(acc.get("next_nonce", 0)),
                                     tx_type=tx_type, code=code, input=input_hex,
                                     gas_limit=gas_limit, target_chain_id=target_chain_id)
        if not signed.get("ok"):
            raise EngineError(signed.get("error", "签名失败"))
        tx = signed["tx"]
        resp = self.engine.submit_tx(tx)
        if not resp.get("ok"):
            raise ValueError(resp.get("error", "交易被拒绝"))
        self.counters["txs_sent"] += 1
        self.p2p.broadcast_threadsafe({"type": "tx", "tx": tx})
        self.log(f"广播 {tx_type} {tx['id'][:16]}…")
        self.request_block()
        return tx

    def send_tx(self, from_address: str, to: str, amount, fee) -> dict:
        if from_address not in self.keystore.wallets:
            raise ValueError("该钱包不在本节点，无法签名")
        amount_u = parse_amount(amount)
        fee_u = parse_amount(fee)
        if amount_u <= 0:
            raise ValueError("金额必须大于 0")
        if fee_u < 0:
            raise ValueError("手续费不能为负")
        return self._submit(from_address, "transfer", to, amount_u, fee_u)

    # ---------------- 合约 ----------------
    @staticmethod
    def _parse_signature(sig: str):
        """从 "transfer(address,uint256)" 解析出参数类型列表。"""
        if "(" not in sig:
            raise ValueError("函数签名格式应为 name(type1,type2)")
        inner = sig[sig.find("(") + 1:sig.rfind(")")].strip()
        types = [t.strip() for t in inner.split(",")] if inner else []
        return types

    def deploy_contract(self, from_address: str, code: str, fee="0.001", gas_limit: int = 200000) -> dict:
        code = code.strip()
        if code.startswith("0x"):
            code = code[2:]
        if not code:
            raise ValueError("缺少合约字节码")
        return self._submit(from_address, "deploy_contract", "", 0, parse_amount(fee),
                            code=code, gas_limit=gas_limit)

    def call_contract(self, from_address: str, to: str, method: str = "", args=None,
                      input_hex: str = "", fee="0.001", gas_limit: int = 200000) -> dict:
        calldata = input_hex
        if method:
            types = self._parse_signature(method)
            values = [str(a) for a in (args or [])]
            if len(types) != len(values):
                raise ValueError(f"参数数量不匹配：需要 {len(types)} 个，收到 {len(values)} 个")
            enc = self.engine.abi_encode(method, types, values)
            if not enc.get("ok"):
                raise ValueError(enc.get("error", "ABI 编码失败"))
            calldata = enc["calldata"]
        if not calldata:
            raise ValueError("缺少 calldata 或 method")
        return self._submit(from_address, "contract_call", to, 0, parse_amount(fee),
                            input_hex=calldata, gas_limit=gas_limit)

    # ---------------- 跨链 ----------------
    def cross_transfer(self, from_address: str, to_address: str, target_chain_id: int,
                       amount, fee="0.001") -> dict:
        amount_u = parse_amount(amount)
        if amount_u <= 0:
            raise ValueError("金额必须大于 0")
        if int(target_chain_id) == int(self.chain_id):
            raise ValueError("目标链不能是当前链")
        return self._submit(from_address, "cross_transfer", to_address, amount_u, parse_amount(fee),
                            target_chain_id=int(target_chain_id))

    def commit_relay_root(self, src_chain_id: int, height: int, root: str, fee="0.001") -> dict:
        """中继提交源链事件根的 Merkle 根。"""
        sel = self.engine.abi_encode("commitRoot(uint256,uint256,bytes32)", [], [])["selector"]
        calldata = sel + _word(int(src_chain_id)) + _word(int(height)) + _word(root)
        return self._submit(self.relayer or self.miner_address, "contract_call", BRIDGE_ADDR, 0,
                            parse_amount(fee), input_hex=calldata, gas_limit=200000)

    def claim_cross(self, src_chain_id: int, seq: int, index: int, to_address: str, amount_u: int,
                    root: str, proof: list, fee="0.001") -> dict:
        """凭 Merkle 证明在目标链认领跨链资产。"""
        sel = self.engine.abi_encode("claim(uint256,uint256,uint256,address,uint256,bytes32,bytes32[])", [], [])["selector"]
        calldata = (sel + _word(int(src_chain_id)) + _word(int(seq)) + _word(int(index)) +
                    _addr_word(to_address) + _word(int(amount_u)) + _word(root) +
                    _word(len(proof)) + "".join(_word(p) for p in proof))
        return self._submit(self.relayer or self.miner_address, "contract_call", BRIDGE_ADDR, 0,
                            parse_amount(fee), input_hex=calldata, gas_limit=200000)

    def request_block(self):
        """请求立即出一块（用于让刚提交的交易快速确认）。"""
        self._mine_request.set()
        self._wake.set()

    def faucet(self, address: str, amount_text="100") -> dict:
        amount_u = parse_amount(amount_text)
        if amount_u <= 0 or amount_u > 1000 * 100_000_000:
            raise ValueError("单次领取额度需在 0~1000 之间")
        acc = self.engine.account(self.miner_address)
        fee_u = 100_000  # 0.001
        if int(acc.get("balance", 0)) < amount_u + fee_u:
            raise ValueError("水龙头余额不足，请先让本节点挖几个块")
        tx = self.send_tx(self.miner_address, address, amount_u, fee_u)
        self.counters["faucet"] += 1
        return tx

    # ---------------- 事件回调 ----------------
    def on_block_accepted(self, block: dict, exclude_id: str | None = None):
        self.counters["blocks_in"] += 1
        self._status_at = 0
        self.p2p.broadcast_threadsafe({"type": "block", "block": block}, exclude_id)

    def on_tx_accepted(self, tx: dict, exclude_id: str | None = None):
        self.counters["txs_in"] += 1
        self.p2p.broadcast_threadsafe({"type": "tx", "tx": tx}, exclude_id)

    def on_chain_replaced(self):
        self._status_at = 0

    # ---------------- 视图 ----------------
    def wallets_view(self):
        out = []
        for w in self.keystore.list():
            acc = self.engine.account(w["address"])
            out.append({
                **w,
                "balance": int(acc.get("balance", 0)),
                "balance_str": format_amount(acc.get("balance", 0)),
                "nonce": int(acc.get("nonce", 0)),
                "is_miner": w["address"] == self.miner_address,
            })
        return out

    def status_view(self):
        st = self.cached_status()
        peers = self.p2p.snapshot()
        return {
            "node_id": self.node_id,
            "http_port": self.http_port,
            "p2p_port": self.p2p_port,
            "auto_mine": self.auto_mine,
            "miner_address": self.miner_address,
            "uptime": int(time.time() - self.started_at),
            "peer_count": len(peers),
            "counters": self.counters,
            "p2p_stats": self.p2p.stats,
            "chain_id": st.get("chain_id", self.chain_id),
            "contracts": st.get("contracts", 0),
            "bridge_seq": st.get("bridge_seq", 0),
            "chain": {
                "height": st.get("height", 0),
                "tip_hash": st.get("tip_hash", ""),
                "difficulty": st.get("difficulty", 0),
                "next_difficulty": st.get("next_difficulty", 0),
                "mempool": st.get("mempool", 0),
                "total_txs": st.get("total_txs", 0),
                "total_work": st.get("total_work", 0),
                "accounts": st.get("accounts", 0),
                "genesis_hash": st.get("genesis_hash", ""),
                "tip_time": st.get("tip_time", 0),
                "target_block_ms": st.get("target_block_ms", 0),
            },
            "server_time": now_ms(),
        }
