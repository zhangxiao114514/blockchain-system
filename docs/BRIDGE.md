# 可信中继跨链

## 1. 方案

**托管合约（锁定/铸造）+ 默克尔证明中继**：

```
链 A (chain_id=1001)                          链 B (chain_id=1002)
┌──────────────────────────┐                 ┌──────────────────────────┐
│ 用户 cross_transfer       │                 │                          │
│   ├─ 扣款 amount+fee      │                 │                          │
│   ├─ amount 转入托管地址   │                 │                          │
│   ├─ seq = ++bridge.seq   │  中继 relayer    │                          │
│   └─ 发出 CrossOut 事件    │ ───────────────▶ │ commitRoot(src,height,root)│
│                          │  (Merkle 根)     │   └─ 仅 relayer 可提交      │
│  区块日志 → 叶子 → 树      │                 │ claim(seq,to,amount,proof) │
│                          │                 │   ├─ 校验证明               │
│                          │                 │   ├─ 去重 (src,seq)        │
│                          │                 │   └─ 铸给收款地址           │
└──────────────────────────┘                 └──────────────────────────┘
```

- **托管**：源链把资产锁进系统托管地址 `0x0000…00bb`，目标链按证明铸造等额资产。
- **中继**：链下扫描 `CrossOut` 事件 → 构建 Merkle 树 → 把根提交到目标链 → 逐条提交 `claim`。
- **证明**：`claim` 携带 `(叶子索引, 证明路径)`，目标链在原生桥合约里重算根并比对。

## 2. 关键设计

### 2.1 全局序列号

每条链维护单调递增的 `bridge.seq`，每次 `cross_transfer` 自增并写入事件。
叶子绑定 `(源链, 序列号, 收款地址, 金额)`：

```
leaf = sha256("cross|" + src_chain_id + "|" + seq + "|" + to + "|" + amount)
```

目标链对 `(src_chain_id, seq)` 做**一次性去重**，因此：

- 同一笔跨链无法被认领两次 → 防重放
- 金额被篡改则叶子变化 → 证明失效
- 序列号跨链唯一 → 不存在两链互相覆盖

### 2.2 chain_id 隔离

- 交易哈希 `tx_id = sha256(type|chain_id|from|to|amount|fee|nonce|timestamp|...)`，
  在 A 链签名的交易拿到 B 链会因哈希不匹配被拒。
- 创世块包含 `chain_id`，两条链从根上就不同。
- 桥拒绝 `src_chain_id == 本链` 与 `target_chain_id == 本链` 的调用。

### 2.3 中继授权

`commitRoot` 只接受 `relayer` 地址发起的交易（节点启动参数 `--relayer`）。
未配置时进入**开发模式**（任何人可提交），仅用于本地调试，日志会打印警告。

根的高度必须严格递增，防止用旧根回滚已认领状态。

### 2.4 超发防护

| 环节 | 约束 |
|---|---|
| 源链锁定 | `amount ≤ 余额`，且 `amount` 被真正转入托管地址 |
| 目标链铸造 | 只有合法证明才能铸，且 `(src,seq)` 只能铸一次 |
| 守恒检查 | `源链 locked == 目标链 minted`（测试断言） |

## 3. 快速体验

```bash
# 1) 启动双链（自动创建中继钱包、导入、发测试币）
python start_dual.py --fresh

# 2) 另开终端：启动中继
python bridge/relayer.py --chains 3001:1001,3002:1002

# 3) 发起 A → B 的跨链转账（收款地址用 B 链上的钱包）
python node/cli.py xfer --port 3001 --from <A链钱包> --to <B链地址> --target-chain 1002 --amount 10

# 4) 查看两侧状态
python node/cli.py bridge --port 3001
python node/cli.py bridge --port 3002
```

## 4. 接口

| 方法 | 路径 | 说明 |
|---|---|---|
| POST | `/api/bridge/transfer` | 发起跨链 `{from,to,target_chain_id,amount,fee}` |
| POST | `/api/bridge/commit` | 提交源链根 `{src_chain_id,height,root}`（仅中继） |
| POST | `/api/bridge/claim` | 认领 `{src_chain_id,seq,index,to,amount,root,proof[]}` |
| GET | `/api/bridge` | 桥状态：序列号、锁定、铸造、已提交根、托管余额 |
| GET | `/api/logs?topic=` | 事件日志（`CrossOut` / `CrossIn` 的 topic0 见下） |

```
CrossOut topic0 = sha256("CrossOut(uint256,uint256,address,uint256,uint256)")
CrossIn  topic0 = sha256("CrossIn(uint256,uint256)")
```

## 5. 已知边界

- 中继是**可信**的（联邦模式），不是轻客户端验证；生产环境需要多签或链上轻客户端。
- 只支持同质资产跨链（锁定/铸造），不支持跨链消息调用。
- 单中继轮询，未做并行确认与重组回滚（源链深度不足时可能认领到会被回滚的事件）。
