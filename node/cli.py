"""cli.py — 节点命令行入口

  # 启动节点（单节点）
  python node/cli.py run --datadir data/n1 --port 3001 --p2p 6001 --auto-mine

  # 运维命令（对运行中的节点发起 HTTP 调用）
  python node/cli.py status   --port 3001
  python node/cli.py mine     --port 3001 --count 1
  python node/cli.py validate --port 3001
  python node/cli.py wallets  --port 3001
  python node/cli.py faucet   --port 3001 --address 0x... --amount 100
  python node/cli.py send     --port 3001 --from 0x... --to 0x... --amount 1.5
"""
from __future__ import annotations

import argparse
import asyncio
import json
import os
import signal
import sys
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from node import Node  # noqa: E402
from util import Logger, format_amount  # noqa: E402


def http_json(port: int, path: str, method: str = "GET", body: dict | None = None, timeout: float = 30.0):
    url = f"http://127.0.0.1:{port}{path}"
    data = None
    headers = {}
    if body is not None:
        data = json.dumps(body).encode("utf-8")
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def cmd_run(args):
    if args.log:
        log_path = os.path.abspath(args.log)
        os.makedirs(os.path.dirname(log_path), exist_ok=True)
        f = open(log_path, "w", encoding="utf-8", buffering=1)
        sys.stdout = f
        sys.stderr = f
    seeds = []
    for s in args.seed or []:
        host, _, port = s.partition(":")
        seeds.append((host or "127.0.0.1", int(port)))
    params = {
        "difficulty": args.difficulty,
        "target_block_ms": args.target_block_ms,
        "retarget_interval": args.retarget,
        "max_difficulty": args.max_difficulty,
    }
    node = Node(
        datadir=args.datadir,
        http_port=args.port,
        p2p_port=args.p2p,
        seeds=seeds,
        params=params,
        auto_mine=args.auto_mine,
        passphrase=args.passphrase,
        mine_interval_ms=args.mine_interval,
    )

    async def main():
        node.start()
        await node.p2p.start()
        stop = asyncio.Event()

        def _sig(*_):
            stop.set()

        try:
            for s in (signal.SIGINT, signal.SIGTERM):
                try:
                    asyncio.get_running_loop().add_signal_handler(s, _sig)
                except NotImplementedError:
                    pass
        except Exception:
            pass
        node.log("节点已启动，Ctrl+C 退出")
        await stop.wait()
        node.log("正在关闭…")
        node.stop()

    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        node.stop()
    return 0


def cmd_status(args):
    r = http_json(args.port, "/api/status")
    c = r["chain"]
    print(f"节点        {r['node_id']}  (http {r['http_port']} / p2p {r['p2p_port']})")
    print(f"区块高度    {c['height']}")
    print(f"当前难度    {c['difficulty']}  (下一块 {c['next_difficulty']})")
    print(f"链上交易    {c['total_txs']}   待确认 {c['mempool']}")
    print(f"账户数      {c['accounts']}   累计工作量 {c['total_work']}")
    print(f"链尖哈希    {c['tip_hash']}")
    print(f"已连接节点  {r['peer_count']}   自动挖矿 {'开' if r['auto_mine'] else '关'}")
    print(f"本节点出块  {r['counters']['blocks_mined']}   收到区块 {r['counters']['blocks_in']}")
    return 0


def cmd_wallets(args):
    r = http_json(args.port, "/api/wallets")
    print(f"矿工地址 {r['miner']}\n")
    for w in r["wallets"]:
        flag = " [矿工]" if w["is_miner"] else ""
        print(f"  {w['address']}  {w['balance_str']:>14}  非{ w['nonce'] }{flag}  {w['label']}")
    return 0


def cmd_mine(args):
    r = http_json(args.port, "/api/mine", "POST", {"count": args.count}, timeout=120)
    print(f"已出块 {r['mined']} 个，当前高度 {r['height']}")
    return 0


def cmd_validate(args):
    r = http_json(args.port, "/api/validate", timeout=120)
    print(f"链校验 {'通过' if r.get('valid') else '失败'}  高度 {r.get('height')}  {r.get('error') or ''}")
    return 0 if r.get("valid") else 1


def cmd_faucet(args):
    r = http_json(args.port, "/api/faucet", "POST", {"address": args.address, "amount": args.amount}, timeout=60)
    print(f"已提交水龙头交易 {r.get('txid')}")
    return 0


def cmd_send(args):
    r = http_json(args.port, "/api/tx", "POST",
                  {"from": args.from_addr, "to": args.to, "amount": args.amount, "fee": args.fee}, timeout=60)
    print(f"已提交交易 {r.get('txid')}")
    return 0


def cmd_peers(args):
    r = http_json(args.port, "/api/peers")
    for p in r["peers"]:
        print(f"  {p['node_id']:<12} http={p.get('http_port')} p2p={p['p2p_port']} 高度={p['height']}")
    return 0


def main():
    ap = argparse.ArgumentParser(description="区块链节点 CLI")
    sub = ap.add_subparsers(dest="cmd", required=True)

    r = sub.add_parser("run", help="启动节点")
    r.add_argument("--datadir", default="data/node")
    r.add_argument("--port", type=int, default=3001)
    r.add_argument("--p2p", type=int, default=6001)
    r.add_argument("--seed", action="append", help="种子节点 host:port，可多次指定")
    r.add_argument("--auto-mine", action="store_true")
    r.add_argument("--difficulty", type=int, default=4)
    r.add_argument("--max-difficulty", type=int, default=4)
    r.add_argument("--target-block-ms", type=int, default=2000)
    r.add_argument("--retarget", type=int, default=10)
    r.add_argument("--passphrase", default=None)
    r.add_argument("--log", default=None, help="日志文件路径（后台运行时使用）")
    r.add_argument("--mine-interval", type=int, default=1500, help="自动挖矿时的出块间隔（毫秒）")
    r.set_defaults(func=cmd_run)

    for name, fn, help_text in [
        ("status", cmd_status, "查询状态"),
        ("wallets", cmd_wallets, "列出钱包"),
        ("mine", cmd_mine, "立即出块"),
        ("validate", cmd_validate, "全链校验"),
        ("peers", cmd_peers, "列出对等节点"),
    ]:
        p = sub.add_parser(name, help=help_text)
        p.add_argument("--port", type=int, default=3001)
        p.set_defaults(func=fn)
        if name == "mine":
            p.add_argument("--count", type=int, default=1)

    f = sub.add_parser("faucet", help="领取测试币")
    f.add_argument("--port", type=int, default=3001)
    f.add_argument("--address", required=True)
    f.add_argument("--amount", default="100")
    f.set_defaults(func=cmd_faucet)

    s = sub.add_parser("send", help="转账")
    s.add_argument("--port", type=int, default=3001)
    s.add_argument("--from", dest="from_addr", required=True)
    s.add_argument("--to", required=True)
    s.add_argument("--amount", required=True)
    s.add_argument("--fee", default="0.001")
    s.set_defaults(func=cmd_send)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
