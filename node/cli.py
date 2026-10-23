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
        chain_id=args.chain_id,
        relayer=args.relayer,
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


def cmd_deploy(args):
    code = args.code
    if os.path.isfile(code):
        with open(code, "r", encoding="utf-8") as f:
            code = f.read().strip()
    r = http_json(args.port, "/api/contracts/deploy", "POST",
                  {"from": args.from_addr, "code": code, "gas_limit": args.gas, "fee": args.fee}, timeout=60)
    print(f"合约已部署: {r.get('contract_address')}  tx={r.get('txid')}")
    return 0


def cmd_call(args):
    body = {"from": args.from_addr, "to": args.to, "gas_limit": args.gas, "fee": args.fee}
    if args.method:
        body["method"] = args.method
        body["args"] = args.args or []
    elif args.input:
        body["input"] = args.input
    r = http_json(args.port, "/api/contracts/call", "POST", body, timeout=60)
    print(f"调用已提交: {r.get('txid')}")
    return 0


def cmd_contract(args):
    r = http_json(args.port, f"/api/contract?address={args.address}")
    print(f"合约 {r.get('address')}  creator={r.get('creator')}  code={len(r.get('code',''))//2} 字节")
    for k, v in (r.get("storage") or {}).items():
        print(f"  slot {k} = {v}")
    return 0


def cmd_logs(args):
    r = http_json(args.port, f"/api/logs?from_block={args.from_block}&limit={args.limit}")
    for lg in r.get("logs", []):
        print(f"  #{lg['block_index']} {lg['address']} topics={lg['topics'][:2]}")
    print(f"共 {len(r.get('logs', []))} 条事件")
    return 0


def cmd_bridge(args):
    r = http_json(args.port, "/api/bridge")
    print(f"链 {r['chain_id']} 托管地址 {r['address']}")
    print(f"  出向序列号 {r['seq']}  锁定 {r['locked_str']}  已铸造 {r['minted_str']}")
    print(f"  已认领 {r['processed_count']}  中继 {r.get('relayer') or '(开发模式)'}")
    for k, v in (r.get("relay_roots") or {}).items():
        print(f"  源链 {k} 根 {v['root'][:16]}… 高度 {v['height']}")
    return 0


def cmd_xfer(args):
    r = http_json(args.port, "/api/bridge/transfer", "POST",
                  {"from": args.from_addr, "to": args.to, "target_chain_id": args.target_chain,
                   "amount": args.amount, "fee": args.fee}, timeout=60)
    print(f"跨链交易已提交: {r.get('txid')}")
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
    r.add_argument("--chain-id", type=int, default=1001, help="链 ID（跨链隔离）")
    r.add_argument("--relayer", default="", help="可信中继地址（留空为开发模式）")
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

    d = sub.add_parser("deploy", help="部署合约")
    d.add_argument("--port", type=int, default=3001)
    d.add_argument("--from", dest="from_addr", required=True)
    d.add_argument("--code", required=True, help="字节码 hex 或文件路径")
    d.add_argument("--gas", type=int, default=200000)
    d.add_argument("--fee", default="0.001")
    d.set_defaults(func=cmd_deploy)

    c = sub.add_parser("call", help="调用合约")
    c.add_argument("--port", type=int, default=3001)
    c.add_argument("--from", dest="from_addr", required=True)
    c.add_argument("--to", required=True)
    c.add_argument("--method", default="", help="如 transfer(address,uint256)")
    c.add_argument("--args", nargs="*", default=[])
    c.add_argument("--input", default="", help="原始 calldata hex")
    c.add_argument("--gas", type=int, default=200000)
    c.add_argument("--fee", default="0.001")
    c.set_defaults(func=cmd_call)

    ct = sub.add_parser("contract", help="查看合约")
    ct.add_argument("--port", type=int, default=3001)
    ct.add_argument("--address", required=True)
    ct.set_defaults(func=cmd_contract)

    lg = sub.add_parser("logs", help="查询事件日志")
    lg.add_argument("--port", type=int, default=3001)
    lg.add_argument("--from-block", type=int, default=0)
    lg.add_argument("--limit", type=int, default=50)
    lg.set_defaults(func=cmd_logs)

    bg = sub.add_parser("bridge", help="查看跨链桥状态")
    bg.add_argument("--port", type=int, default=3001)
    bg.set_defaults(func=cmd_bridge)

    xf = sub.add_parser("xfer", help="跨链转账")
    xf.add_argument("--port", type=int, default=3001)
    xf.add_argument("--from", dest="from_addr", required=True)
    xf.add_argument("--to", required=True)
    xf.add_argument("--target-chain", type=int, required=True)
    xf.add_argument("--amount", required=True)
    xf.add_argument("--fee", default="0.001")
    xf.set_defaults(func=cmd_xfer)

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
