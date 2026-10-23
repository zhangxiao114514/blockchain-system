# HTTP API 与 curl 示例

所有接口返回 JSON，`ok` 字段表示成功；金额一律用十进制字符串（如 `"12.5"`），
内部换算为 1e8 最小单位整数。默认节点端口 `3001`。

## 1. 节点与链

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/status` | 节点与链状态（chain_id、高度、难度、合约数、桥序列号、对等节点） |
| GET | `/api/blocks?limit=20` | 最新区块（含交易与事件日志） |
| GET | `/api/block?index=5` | 单个区块 |
| GET | `/api/mempool` | 交易池 |
| GET | `/api/peers` | 已连接节点 |
| GET | `/api/network` | 网络拓扑（控制台节点切换用） |
| GET | `/api/validate` | 全链校验 |
| GET | `/api/tx?id=..` | 交易查询 |

```bash
curl -s http://127.0.0.1:3001/api/status
curl -s "http://127.0.0.1:3001/api/blocks?limit=3"
curl -s http://127.0.0.1:3001/api/validate
```

## 2. 账户与钱包

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/account?address=0x..` | 余额 / nonce / next_nonce |
| GET | `/api/wallets` | 本节点钱包及余额（不含私钥） |
| POST | `/api/wallets` | 新建钱包 `{label}` |
| POST | `/api/wallets/import` | 导入私钥 `{private_key,label}` |
| POST | `/api/wallets/delete` | 删除钱包 `{address}`（矿工钱包受保护） |
| POST | `/api/faucet` | 领测试币 `{address,amount}` |
| POST | `/api/tx` | 转账 `{from,to,amount,fee}` |
| POST | `/api/tx/raw` | 提交外部已签名交易 `{tx}` |

```bash
# 新建钱包 + 领币
W=$(curl -s -X POST http://127.0.0.1:3001/api/wallets -H 'Content-Type: application/json' -d '{"label":"demo"}')
ADDR=$(echo "$W" | python -c "import json,sys;print(json.load(sys.stdin)['wallet']['address'])")
curl -s -X POST http://127.0.0.1:3001/api/faucet -H 'Content-Type: application/json' \
  -d "{\"address\":\"$ADDR\",\"amount\":\"100\"}"

# 转账
curl -s -X POST http://127.0.0.1:3001/api/tx -H 'Content-Type: application/json' \
  -d "{\"from\":\"$ADDR\",\"to\":\"0x0000000000000000000000000000000000000001\",\"amount\":\"1.5\",\"fee\":\"0.001\"}"
```

## 3. 合约

| 方法 | 路径 | 说明 |
|---|---|---|
| POST | `/api/contracts/deploy` | 部署 `{from,code,gas_limit,fee}`，返回 `contract_address` |
| POST | `/api/contracts/call` | 调用 `{from,to,method,args}` 或 `{from,to,input}` |
| GET | `/api/contracts` | 合约列表 |
| GET | `/api/contract?address=` | 合约详情（字节码 + 全部存储槽） |
| GET | `/api/logs?from_block=&limit=&address=&topic=` | 事件日志 |

```bash
# 部署示例合约
DEPLOY=$(curl -s -X POST http://127.0.0.1:3001/api/contracts/deploy -H 'Content-Type: application/json' \
  -d "{\"from\":\"$ADDR\",\"code\":\"$(cat contracts/vault.hex)\"}")
CONTRACT=$(echo "$DEPLOY" | python -c "import json,sys;print(json.load(sys.stdin)['contract_address'])")

# 调用 deposit(uint256)，参数 7
curl -s -X POST http://127.0.0.1:3001/api/contracts/call -H 'Content-Type: application/json' \
  -d "{\"from\":\"$ADDR\",\"to\":\"$CONTRACT\",\"method\":\"deposit(uint256)\",\"args\":[\"7\"]}"

# 查存储与事件
curl -s "http://127.0.0.1:3001/api/contract?address=$CONTRACT"
curl -s "http://127.0.0.1:3001/api/logs?limit=5"
```

## 4. 跨链

| 方法 | 路径 | 说明 |
|---|---|---|
| POST | `/api/bridge/transfer` | 发起跨链 `{from,to,target_chain_id,amount,fee}` |
| POST | `/api/bridge/commit` | 提交源链根 `{src_chain_id,height,root}`（仅中继地址） |
| POST | `/api/bridge/claim` | 认领 `{src_chain_id,seq,index,to,amount,root,proof[]}` |
| GET | `/api/bridge` | 桥状态 |
| POST | `/api/_merkle` | 调试：`{leaves}` → `{root,proofs}` |

```bash
# A 链（1001）发起跨链到 B 链（1002）
curl -s -X POST http://127.0.0.1:3001/api/bridge/transfer -H 'Content-Type: application/json' \
  -d "{\"from\":\"$ADDR\",\"to\":\"$B_ADDR\",\"target_chain_id\":1002,\"amount\":\"10\"}"

# 查看桥状态（锁定、序列号、已提交的源链根）
curl -s http://127.0.0.1:3001/api/bridge
curl -s http://127.0.0.1:3002/api/bridge

# 中继（自动完成 commit + claim）
python bridge/relayer.py --chains 3001:1001,3002:1002 --once
```

## 5. 运维

| 方法 | 路径 | 说明 |
|---|---|---|
| POST | `/api/mine` | 立即出块 `{count}` |
| POST | `/api/automine` | 开关自动挖矿 `{enabled}` |

## 6. 错误约定

```json
{"ok": false, "error": "该跨链序列号已被认领（防重放）"}
```

HTTP 状态码：`400` 参数或业务校验失败，`404` 资源不存在，`500` 内部错误。
