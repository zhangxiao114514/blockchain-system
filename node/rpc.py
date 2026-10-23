"""rpc.py — HTTP JSON API + 静态控制台

接口一览：
  GET  /api/status                  节点与链状态
  GET  /api/blocks?limit=&offset=   区块列表（最新在前）
  GET  /api/block?index=            单个区块
  GET  /api/mempool                 交易池
  GET  /api/peers                   已连接节点
  GET  /api/network                 网络拓扑（用于控制台切换节点）
  GET  /api/wallets                 本节点钱包（含余额）
  GET  /api/account?address=        账户余额与 nonce
  GET  /api/validate                全链校验
  GET  /api/tx?id=                  交易查询
  POST /api/wallets                 新建钱包
  POST /api/tx                      转账（节点代签名）
  POST /api/faucet                  领取测试币
  POST /api/mine                    立即出块
  POST /api/automine                开关自动挖矿
"""
from __future__ import annotations

import json
import mimetypes
import os
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

from util import format_amount, parse_amount

WEB_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "web")


class Handler(BaseHTTPRequestHandler):
    node = None  # 由工厂注入
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # 静默默认日志
        pass

    # ---------- 基础 ----------
    def _send_json(self, obj, code=200):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def _send_file(self, path):
        if not os.path.isfile(path):
            self._send_json({"ok": False, "error": "not found"}, 404)
            return
        ctype, _ = mimetypes.guess_type(path)
        if path.endswith(".js"):
            ctype = "application/javascript; charset=utf-8"
        elif path.endswith(".css"):
            ctype = "text/css; charset=utf-8"
        elif path.endswith(".html"):
            ctype = "text/html; charset=utf-8"
        ctype = ctype or "application/octet-stream"
        with open(path, "rb") as f:
            body = f.read()
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def _body(self) -> dict:
        try:
            n = int(self.headers.get("Content-Length") or 0)
            if n <= 0:
                return {}
            raw = self.rfile.read(n)
            return json.loads(raw.decode("utf-8"))
        except Exception:
            return {}

    # ---------- 路由 ----------
    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET,POST,OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_GET(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)
        p = u.path
        if not p.startswith("/api/"):
            if p in ("/", "/index.html"):
                self._send_file(os.path.join(WEB_DIR, "index.html"))
            elif p == "/app.js":
                self._send_file(os.path.join(WEB_DIR, "app.js"))
            elif p == "/style.css":
                self._send_file(os.path.join(WEB_DIR, "style.css"))
            elif p == "/favicon.ico":
                self.send_response(204)
                self.send_header("Content-Length", "0")
                self.end_headers()
            else:
                self._send_json({"ok": False, "error": "not found"}, 404)
            return
        try:
            self._api_get(p, q)
        except Exception as e:
            self._send_json({"ok": False, "error": str(e)}, 500)

    def _api_get(self, path, q):
        n = self.node
        if path == "/api/status":
            self._send_json({"ok": True, **n.status_view()})
        elif path == "/api/blocks":
            limit = int(q.get("limit", ["20"])[0])
            limit = max(1, min(limit, 200))
            st = n.cached_status()
            height = int(st.get("height", 0))
            start = max(0, height - limit + 1)
            resp = n.engine.get_chain(start, limit, True)
            blocks = list(reversed(resp.get("blocks", [])))
            now = int(time.time() * 1000)
            for b in blocks:
                b["age_ms"] = max(0, now - int(b.get("timestamp", 0)))
                for tx in b.get("transactions", []):
                    tx["amount_str"] = format_amount(tx.get("amount", 0))
            self._send_json({"ok": True, "blocks": blocks, "height": height, "start": start})
        elif path == "/api/block":
            idx = int(q.get("index", ["0"])[0])
            resp = n.engine.get_chain(idx, 1, True)
            blocks = resp.get("blocks", [])
            if not blocks:
                self._send_json({"ok": False, "error": "区块不存在"}, 404)
            else:
                self._send_json({"ok": True, "block": blocks[0]})
        elif path == "/api/mempool":
            resp = n.engine.mempool()
            txs = resp.get("txs", [])
            for tx in txs:
                tx["amount_str"] = format_amount(tx.get("amount", 0))
            self._send_json({"ok": True, "txs": txs, "size": len(txs)})
        elif path == "/api/peers":
            self._send_json({"ok": True, "peers": n.p2p.snapshot()})
        elif path == "/api/network":
            nodes = [{"node_id": n.node_id, "http_port": n.http_port, "p2p_port": n.p2p_port, "self": True}]
            for p in n.p2p.snapshot():
                if p.get("http_port"):
                    nodes.append({"node_id": p["node_id"], "http_port": p["http_port"],
                                  "p2p_port": p["p2p_port"], "self": False})
            self._send_json({"ok": True, "nodes": nodes})
        elif path == "/api/wallets":
            self._send_json({"ok": True, "wallets": n.wallets_view(), "miner": n.miner_address})
        elif path == "/api/account":
            addr = (q.get("address") or [""])[0]
            if not addr:
                self._send_json({"ok": False, "error": "缺少 address"}, 400)
                return
            acc = n.engine.account(addr)
            acc["balance_str"] = format_amount(acc.get("balance", 0))
            self._send_json(acc)
        elif path == "/api/validate":
            r = n.engine.validate()
            self._send_json(r)
        elif path == "/api/tx":
            tid = (q.get("id") or [""])[0]
            r = n.engine.call("tx", id=tid)
            self._send_json(r)
        else:
            self._send_json({"ok": False, "error": "unknown endpoint"}, 404)

    def do_POST(self):
        u = urlparse(self.path)
        body = self._body()
        try:
            self._api_post(u.path, body)
        except ValueError as e:
            self._send_json({"ok": False, "error": str(e)}, 400)
        except Exception as e:
            self._send_json({"ok": False, "error": str(e)}, 500)

    def _api_post(self, path, body):
        n = self.node
        if path == "/api/wallets":
            w = n.engine.new_wallet()
            if not w.get("ok"):
                raise RuntimeError(w.get("error", "生成钱包失败"))
            item = n.keystore.add(w["address"], w["public_key"], w["private_key"],
                                  label=str(body.get("label") or "")[:20])
            self._send_json({"ok": True, "wallet": {"address": item["address"], "label": item["label"]}})
        elif path in ("/api/wallets/delete", "/api/wallet/delete"):
            addr = str(body.get("address") or "")
            if not addr:
                raise ValueError("缺少 address")
            if addr == n.miner_address:
                raise ValueError("节点矿工钱包不可删除（它负责出块奖励与水龙头）")
            if not n.keystore.remove(addr):
                raise ValueError("钱包不存在")
            n.log(f"已删除钱包 {addr}")
            self._send_json({"ok": True, "deleted": addr})
        elif path == "/api/tx":
            frm = str(body.get("from") or "")
            to = str(body.get("to") or "")
            amount = body.get("amount", "0")
            fee = body.get("fee", "0.001")
            if not to.startswith("0x") or len(to) != 42:
                raise ValueError("收款地址格式非法")
            tx = n.send_tx(frm, to, amount, fee)
            self._send_json({"ok": True, "txid": tx["id"], "tx": tx})
        elif path == "/api/faucet":
            addr = str(body.get("address") or "")
            amount = body.get("amount", "100")
            if not addr.startswith("0x") or len(addr) != 42:
                raise ValueError("地址格式非法")
            tx = n.faucet(addr, amount)
            self._send_json({"ok": True, "txid": tx["id"]})
        elif path == "/api/mine":
            count = int(body.get("count", 1))
            blocks = n.mine_once(count)
            self._send_json({"ok": True, "mined": len(blocks),
                             "height": blocks[-1]["index"] if blocks else n.cached_status().get("height", 0)})
        elif path == "/api/automine":
            n.auto_mine = bool(body.get("enabled"))
            n.log(f"自动挖矿 {'开启' if n.auto_mine else '关闭'}")
            self._send_json({"ok": True, "auto_mine": n.auto_mine})
        else:
            self._send_json({"ok": False, "error": "unknown endpoint"}, 404)


class RPC:
    def __init__(self, node, port: int):
        self.node = node
        self.port = port
        self.httpd: ThreadingHTTPServer | None = None
        self.thread: threading.Thread | None = None

    def start(self):
        handler = type("BoundHandler", (Handler,), {"node": self.node})
        self.httpd = ThreadingHTTPServer(("127.0.0.1", self.port), handler)
        self.httpd.daemon_threads = True
        self.thread = threading.Thread(target=self.httpd.serve_forever, name="rpc", daemon=True)
        self.thread.start()

    def stop(self):
        if self.httpd:
            self.httpd.shutdown()
            self.httpd.server_close()
