"""p2p.py — 节点间的 P2P 网络层（asyncio TCP + 行式 JSON 协议）

协议消息：
    hello     握手：节点标识、监听端口、当前高度、创世哈希
    get_chain 请求完整链
    chain     返回完整链
    block     广播新区块
    tx        广播新交易
    peers     交换已知节点
"""
from __future__ import annotations

import asyncio
import json
import time

MAX_LINE = 32 * 1024 * 1024  # 单条消息上限 32MB
RECONNECT_DELAY = 2.0


class Peer:
    __slots__ = ("host", "port", "node_id", "http_port", "height", "writer", "last_seen", "outbound", "hello_sent")

    def __init__(self, host, port, writer=None, outbound=False):
        self.host = host
        self.port = port
        self.node_id = None
        self.http_port = None
        self.height = 0
        self.writer = writer
        self.last_seen = time.time()
        self.outbound = outbound
        self.hello_sent = False

    def info(self):
        return {
            "node_id": self.node_id or f"{self.host}:{self.port}",
            "host": self.host,
            "p2p_port": self.port,
            "http_port": self.http_port,
            "height": self.height,
            "outbound": self.outbound,
            "last_seen": int(self.last_seen * 1000),
        }


class P2P:
    def __init__(self, node, host="127.0.0.1", port=6001, seeds=()):
        self.node = node
        self.host = host
        self.port = port
        self.seeds = list(seeds)
        self.peers: dict[str, Peer] = {}
        self.loop: asyncio.AbstractEventLoop | None = None
        self.server: asyncio.AbstractServer | None = None
        self._tasks: set[asyncio.Task] = set()
        self.stats = {"sent": 0, "recv": 0, "blocks_in": 0, "txs_in": 0}

    # ---------------- 启动 ----------------
    async def start(self):
        self.loop = asyncio.get_running_loop()
        self.server = await asyncio.start_server(self._on_conn, self.host, self.port, limit=MAX_LINE)
        self.node.log(f"P2P 监听 {self.host}:{self.port}")
        for host, port in self.seeds:
            self._spawn(self._connect_loop(host, port))

    async def stop(self):
        for p in list(self.peers.values()):
            try:
                p.writer.close()
            except Exception:
                pass
        if self.server:
            self.server.close()

    def _spawn(self, coro):
        task = asyncio.ensure_future(coro)
        self._tasks.add(task)

        def _done(t):
            self._tasks.discard(t)
            if t.cancelled():
                return
            exc = t.exception()
            if exc:
                self.node.log(f"后台任务异常: {exc!r}", "WARN")

        task.add_done_callback(_done)
        return task

    async def _connect_loop(self, host, port):
        while True:
            key = f"{host}:{port}"
            existing = self.peers.get(key)
            if existing and existing.writer and not existing.writer.is_closing():
                await asyncio.sleep(RECONNECT_DELAY)
                continue
            try:
                reader, writer = await asyncio.open_connection(host, port, limit=MAX_LINE)
            except Exception:
                await asyncio.sleep(RECONNECT_DELAY)
                continue
            peer = Peer(host, port, writer, outbound=True)
            self.peers[key] = peer
            peer.hello_sent = True
            await self._send(peer, self._hello_msg())
            await self._serve(peer, reader)
            await asyncio.sleep(RECONNECT_DELAY)

    async def _on_conn(self, reader, writer):
        sock = writer.get_extra_info("peername") or ("?", 0)
        peer = Peer(sock[0], sock[1], writer, outbound=False)
        key = f"{sock[0]}:{sock[1]}"
        self.peers[key] = peer
        await self._serve(peer, reader)

    async def _serve(self, peer: Peer, reader: asyncio.StreamReader):
        try:
            while True:
                line = await reader.readline()
                if not line:
                    break
                line = line.strip()
                if not line:
                    continue
                try:
                    msg = json.loads(line.decode("utf-8", "replace"))
                except Exception:
                    continue
                self.stats["recv"] += 1
                peer.last_seen = time.time()
                await self._dispatch(peer, msg)
        except (asyncio.IncompleteReadError, ConnectionResetError, OSError):
            pass
        finally:
            try:
                peer.writer.close()
            except Exception:
                pass
            for k, v in list(self.peers.items()):
                if v is peer:
                    self.peers.pop(k, None)

    # ---------------- 消息处理 ----------------
    async def _dispatch(self, peer: Peer, msg: dict):
        t = msg.get("type")
        if t == "hello":
            await self._on_hello(peer, msg)
        elif t == "get_chain":
            await self._on_get_chain(peer)
        elif t == "chain":
            await self._on_chain(peer, msg)
        elif t == "block":
            await self._on_block(peer, msg)
        elif t == "tx":
            await self._on_tx(peer, msg)
        elif t == "peers":
            await self._on_peers(peer, msg)
        elif t == "ping":
            await self._send(peer, {"type": "pong", "t": int(time.time() * 1000)})

    async def _on_hello(self, peer: Peer, msg: dict):
        node_id = msg.get("node_id")
        if not node_id:
            return
        # 同一节点重复连接：保留先到者
        for k, v in list(self.peers.items()):
            if v is not peer and v.node_id == node_id:
                try:
                    peer.writer.close()
                except Exception:
                    pass
                return
        peer.node_id = node_id
        peer.http_port = msg.get("http_port")
        peer.height = int(msg.get("height") or 0)
        # 被动连接时 peer.port 是对方的临时端口，必须用 hello 里声明的监听端口覆盖，
        # 否则转发给其它节点的地址表会是错的。
        if msg.get("p2p_port"):
            peer.port = int(msg["p2p_port"])
        self.node.log(f"握手成功 {node_id} (高度 {peer.height})")
        # 双向都回一次 hello，否则被动方永远不知道对方的 node_id
        if not peer.hello_sent:
            peer.hello_sent = True
            await self._send(peer, self._hello_msg())
        # 交换已知节点，让网络自动织成全互联（新节点只需接一个种子）
        known = [p for p in self.known_addresses() if p["p2p_port"] != peer.port]
        if known:
            await self._send(peer, {"type": "peers", "peers": known})
        self._spawn(self._announce_peers())
        await self._send(peer, {"type": "get_chain"})

    async def _announce_peers(self):
        """稍等其它同时握手的节点注册完成，再向全网广播当前节点表。"""
        await asyncio.sleep(0.6)
        msg = {"type": "peers", "peers": self.known_addresses()}
        if msg["peers"]:
            await self.broadcast(msg)

    async def _on_get_chain(self, peer: Peer):
        try:
            resp = await asyncio.to_thread(self.node.engine.get_chain, 0, 100000, True)
        except Exception as e:
            self.node.log(f"读取链失败: {e}", "WARN")
            return
        if not resp.get("ok"):
            return
        await self._send(peer, {
            "type": "chain",
            "blocks": resp.get("blocks", []),
            "height": resp.get("height", 0),
        })

    async def _on_chain(self, peer: Peer, msg: dict):
        blocks = msg.get("blocks") or []
        peer.height = len(blocks) - 1 if blocks else 0
        if not blocks:
            return
        try:
            cur = await asyncio.to_thread(self.node.engine.status)
            if len(blocks) - 1 <= int(cur.get("height", 0)):
                return
            resp = await asyncio.to_thread(self.node.engine.replace_chain, blocks)
        except Exception as e:
            self.node.log(f"替换链失败: {e}", "WARN")
            return
        if resp.get("ok"):
            self.node.log(f"已同步到区块 #{resp.get('height')}（来自 {peer.node_id or peer.port}）")
            self.node.on_chain_replaced()
        else:
            self.node.log(f"拒绝同步: {resp.get('error')}", "WARN")

    async def _on_block(self, peer: Peer, msg: dict):
        block = msg.get("block")
        if not block:
            return
        self.stats["blocks_in"] += 1
        try:
            resp = await asyncio.to_thread(self.node.engine.add_block, block)
        except Exception as e:
            self.node.log(f"接收区块失败: {e}", "WARN")
            return
        if resp.get("ok"):
            peer.height = max(peer.height, int(resp.get("height", 0)))
            self.node.log(f"接受新区块 #{block.get('index')} 来自 {peer.node_id or peer.port}")
            self.node.on_block_accepted(block, peer.node_id)
        elif resp.get("need_sync"):
            await self._send(peer, {"type": "get_chain"})

    async def _on_tx(self, peer: Peer, msg: dict):
        tx = msg.get("tx")
        if not tx:
            return
        self.stats["txs_in"] += 1
        try:
            resp = await asyncio.to_thread(self.node.engine.submit_tx, tx)
        except Exception:
            return
        if resp.get("ok"):
            self.node.on_tx_accepted(tx, peer.node_id)

    async def _on_peers(self, peer: Peer, msg: dict):
        got = msg.get("peers") or []
        for p in got:
            host, port = p.get("host"), int(p.get("p2p_port") or 0)
            if not host or not port:
                continue
            if port == self.port:
                continue
            key = f"{host}:{port}"
            if key not in self.peers:
                self._spawn(self._connect_loop(host, port))

    # ---------------- 发送 ----------------
    def _hello_msg(self):
        st = self.node.cached_status()
        return {
            "type": "hello",
            "node_id": self.node.node_id,
            "p2p_port": self.port,
            "http_port": self.node.http_port,
            "height": st.get("height", 0),
            "genesis": st.get("genesis_hash", ""),
            "agent": "bc-node/1.0",
        }

    async def _send(self, peer: Peer, msg: dict):
        try:
            if peer.writer is None or peer.writer.is_closing():
                return
            data = (json.dumps(msg, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")
            peer.writer.write(data)
            await peer.writer.drain()
            self.stats["sent"] += 1
        except Exception:
            try:
                peer.writer.close()
            except Exception:
                pass

    async def broadcast(self, msg: dict, exclude_id=None):
        for peer in list(self.peers.values()):
            if peer.node_id is None or peer.node_id == exclude_id:
                continue
            await self._send(peer, msg)

    def broadcast_threadsafe(self, msg: dict, exclude_id=None):
        """供挖矿线程等非事件循环线程调用。"""
        if self.loop is None:
            return
        asyncio.run_coroutine_threadsafe(self.broadcast(msg, exclude_id), self.loop)

    # ---------------- 查询 ----------------
    def snapshot(self):
        out = []
        for p in self.peers.values():
            if p.node_id:
                out.append(p.info())
        out.sort(key=lambda x: x["node_id"])
        return out

    def known_addresses(self):
        return [{"host": p.host, "p2p_port": p.port} for p in self.peers.values() if p.node_id]
