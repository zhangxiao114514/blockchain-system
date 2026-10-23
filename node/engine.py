"""engine.py — C++ 共识引擎的 Python 客户端。

C++ 引擎作为独立子进程运行，通过 stdin/stdout 的行式 JSON 协议通信。
使用独立读线程 + 事件等待，保证调用可超时、可重启，不会因单次异常卡死。
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import threading
import time


class EngineError(RuntimeError):
    pass


class EngineTimeout(EngineError):
    pass


class Engine:
    def __init__(self, exe: str, datadir: str, params: dict | None = None, logger=None):
        self.exe = exe
        self.datadir = datadir
        self.params = params or {}
        self.log = logger or (lambda *a: None)
        self._proc: subprocess.Popen | None = None
        self._lock = threading.Lock()
        self._pending: dict[int, list] = {}
        self._rid = 0
        self._reader: threading.Thread | None = None
        self._stderr_reader: threading.Thread | None = None
        self._alive = False
        self._stderr_tail: list[str] = []

    # ---------- 生命周期 ----------
    def start(self):
        if not os.path.exists(self.exe):
            raise EngineError(f"引擎可执行文件不存在: {self.exe}，请先运行 build.sh")
        creationflags = 0
        if os.name == "nt":
            creationflags = 0x08000000  # CREATE_NO_WINDOW
        self._proc = subprocess.Popen(
            [self.exe],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=0,
            creationflags=creationflags,
        )
        self._alive = True
        self._reader = threading.Thread(target=self._read_loop, name="engine-reader", daemon=True)
        self._reader.start()
        self._stderr_reader = threading.Thread(target=self._stderr_loop, name="engine-stderr", daemon=True)
        self._stderr_reader.start()
        resp = self.call("init", datadir=self.datadir, **self.params)
        if not resp.get("ok"):
            raise EngineError(f"引擎初始化失败: {resp.get('error')}")
        return resp

    def stop(self):
        self._alive = False
        try:
            if self._proc and self._proc.poll() is None:
                self._proc.stdin.close()
                try:
                    self._proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    self._proc.kill()
        except Exception:
            pass

    def restart(self):
        self.log("engine: 正在重启引擎进程")
        self.stop()
        time.sleep(0.2)
        self._pending.clear()
        return self.start()

    @property
    def alive(self) -> bool:
        return bool(self._alive and self._proc and self._proc.poll() is None)

    # ---------- IO ----------
    def _read_loop(self):
        proc = self._proc
        if proc is None:
            return
        while True:
            line = proc.stdout.readline()
            if not line:
                break
            try:
                msg = json.loads(line.decode("utf-8", "replace"))
            except Exception:
                continue
            rid = msg.get("rid")
            slot = self._pending.get(rid)
            if slot is not None:
                slot[1] = msg
                slot[0].set()
        self._alive = False
        # 唤醒所有等待者
        for slot in list(self._pending.values()):
            slot[0].set()

    def _stderr_loop(self):
        proc = self._proc
        if proc is None:
            return
        while True:
            line = proc.stderr.readline()
            if not line:
                break
            text = line.decode("utf-8", "replace").rstrip()
            if text:
                self._stderr_tail.append(text)
                if len(self._stderr_tail) > 50:
                    self._stderr_tail.pop(0)
                self.log("engine[stderr]: " + text)

    def call(self, cmd: str, timeout: float = 30.0, **kwargs) -> dict:
        if self._proc is None or self._proc.poll() is not None:
            raise EngineError("引擎进程未运行")
        with self._lock:
            self._rid += 1
            rid = self._rid
            req = {"rid": rid, "cmd": cmd}
            req.update(kwargs)
            slot = [threading.Event(), None]
            self._pending[rid] = slot
            try:
                data = (json.dumps(req, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")
                self._proc.stdin.write(data)
                self._proc.stdin.flush()
            except Exception as e:
                self._pending.pop(rid, None)
                raise EngineError(f"写入引擎失败: {e}")
            got = slot[0].wait(timeout)
            self._pending.pop(rid, None)
            if not got:
                raise EngineTimeout(f"引擎调用超时: {cmd}")
            if slot[1] is None:
                raise EngineError(f"引擎无响应或已退出: {cmd}")
            resp = slot[1]
            resp.pop("rid", None)   # 内部请求号不对外暴露
            return resp

    # ---------- 便捷封装 ----------
    def status(self) -> dict:
        return self.call("status")

    def account(self, address: str) -> dict:
        return self.call("balance", address=address)

    def new_wallet(self) -> dict:
        return self.call("wallet_new", timeout=20)

    def sign_tx(self, private_key: str, to: str, amount: int, fee: int, nonce: int,
                tx_type: str = "transfer", code: str = "", input: str = "",
                gas_limit: int = 0, target_chain_id: int = 0) -> dict:
        return self.call(
            "sign_tx",
            type=tx_type,
            private_key=private_key,
            to=to,
            amount=amount,
            fee=fee,
            nonce=nonce,
            code=code,
            input=input,
            gas_limit=gas_limit,
            target_chain_id=target_chain_id,
            timestamp=int(time.time() * 1000),
        )

    def submit_tx(self, tx: dict) -> dict:
        return self.call("submit_tx", tx=tx)

    def mine_step(self, miner: str, iters: int = 20000, timeout: float = 20.0) -> dict:
        return self.call("mine_step", miner=miner, iters=iters, timeout=timeout)

    def add_block(self, block: dict) -> dict:
        return self.call("add_block", block=block)

    def get_chain(self, start: int = 0, limit: int = 100000, with_txs: bool = True) -> dict:
        return self.call("get_chain", **{"from": start, "limit": limit, "with_txs": with_txs}, timeout=60)

    def replace_chain(self, blocks: list) -> dict:
        return self.call("replace_chain", blocks=blocks, timeout=120)

    def validate(self) -> dict:
        return self.call("validate", timeout=60)

    def mempool(self) -> dict:
        return self.call("mempool")

    # ---------- 合约 ----------
    def contract(self, address: str) -> dict:
        return self.call("contract", address=address)

    def contracts(self) -> dict:
        return self.call("contracts")

    def abi_encode(self, signature: str, types: list, values: list) -> dict:
        return self.call("abi_encode", signature=signature, types=types, values=values)

    # ---------- 事件日志 / 跨链 ----------
    def logs(self, from_block: int = 0, limit: int = 200, address: str = "", topic: str = "") -> dict:
        return self.call("logs", from_block=from_block, limit=limit, address=address, topic=topic)

    def bridge(self) -> dict:
        return self.call("bridge")

    def merkle_build(self, leaves: list) -> dict:
        return self.call("merkle_build", leaves=leaves)
