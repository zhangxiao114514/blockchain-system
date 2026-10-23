# 合约与 ABI

## 1. 定位

一套**精简 EVM 子集**：只实现合约演示与跨链桥需要的指令，不做嵌套调用（CALL/CREATE）、
不做 gas 市场、不做动态数组 ABI。目标是把「合约部署 → 调用 → 存储 → 事件」这条链路做完整、可验证。

- 字长：**256 位**（与 EVM 一致），算术按 2^256 取模
- 存储：`key(32B) → value(32B)`，值写 0 等价于删除槽位
- gas：按指令计费，超出 `gas_limit` 立即失败并回滚（整笔交易被拒绝，不会打包进区块）
- 事件：`LOG0..LOG2`，写入区块的 `logs` 字段，并参与 `logs_root`（Merkle 根）

## 2. 指令集

| 区间 | 指令 | gas | 说明 |
|---|---|---|---|
| `0x00` | STOP | 0 | 结束执行 |
| `0x01-0x05` | ADD MUL SUB DIV MOD | 3 | 算术（DIV/MOD 除零返回 0） |
| `0x10-0x13` | LT GT EQ ISZERO | 3 | 比较 |
| `0x16-0x19` | AND OR XOR NOT | 3 | 位运算 |
| `0x1b/0x1c` | SHR SHL | 3 | 移位（≥256 位结果为 0） |
| `0x30` | ADDRESS | 3 | 当前合约地址 |
| `0x33/0x34` | CALLER / CALLVALUE | 3 | 调用者 / 附带金额 |
| `0x35/0x36` | CALLDATALOAD / CALLDATASIZE | 3 | 读 calldata |
| `0x50-0x52` | POP MLOAD MSTORE | 3(+3/字) | 栈与内存 |
| `0x54/0x55` | SLOAD / SSTORE | 100 / 200 | 存储槽读写 |
| `0x56-0x58` | JUMP JUMPI PC | 8/10/3 | 跳转（目标必须是 JUMPDEST） |
| `0x5b` | JUMPDEST | 3 | 合法跳转目标 |
| `0x60-0x7f` | PUSH1..PUSH32 | 3 | 压入立即数 |
| `0x80-0x87` | DUP1..DUP8 | 3 | 复制栈顶第 n 项 |
| `0x90-0x97` | SWAP1..SWAP7 | 3 | 交换栈顶与第 n 项 |
| `0xa0-0xa2` | LOG0..LOG2 | 375 + 375/主题 + 8/字节 | 事件日志 |
| `0xf3/0xfd` | RETURN / REVERT | 0 | 返回 / 回滚 |

限制：栈深 1024，内存 16KB，单笔 `gas_limit` 默认 200000。

## 3. 合约地址

```
address = "0x" + sha256("create|" + chain_id + "|" + from + "|" + nonce)[0:40]
```

与发送方、链 ID、nonce 三者绑定，**确定性可预测**，同一笔部署在任意节点算出同一地址。
地址里含 `chain_id`，因此同一条链上的 nonce 复用不会与其它链撞车。

## 4. 代码模型

本版本采用**单代码模型**：`deploy_contract` 的 `code` 既是初始化代码也是运行时代码。
部署时会以空 calldata 执行一次完成初始化（返回值忽略），之后每次 `contract_call` 用同一段代码执行。

> 与 EVM「构造函数返回运行时代码」的差异：本版本不区分构造/运行，省去手写 wrapper 的成本，
> 代价是构造函数逻辑必须对「空 calldata」幂等。

## 5. ABI 编解码

- 函数选择器：`sha256("name(type1,type2)")` 前 4 字节（8 hex）
- 参数：每个参数一个 32 字节字，大端右对齐
- 支持类型：`uint256`（十进制字符串）、`address`（0x + 40 hex）、`bytes32`（64 hex）

示例：

```
signature = "transfer(address,uint256)"
types     = ["address", "uint256"]
values    = ["0x09c0b2d1a486c439a87bcba6b46a7a1a23f3897c", "42"]
calldata  = 3b88ef57 00000000000000000000000009c0b2d1a486c439a87bcba6b46a7a1a23f3897c 000...02a
```

编码通过节点完成（保证与 C++ 侧一致）：

```bash
curl -s -X POST http://127.0.0.1:3001/api/contracts/call -H 'Content-Type: application/json' -d '{
  "from":"0x...","to":"0x...","method":"transfer(address,uint256)","args":["0x...","42"]
}'
```

## 6. 示例合约

`contracts/vault.hex`：累计存款合约，每次调用把 `calldata[4:36]` 累加到槽位 0，并发出
`Deposit(address,uint256)` 事件。源码注释见 `contracts/vault.asm`。

```bash
# 部署
curl -s -X POST http://127.0.0.1:3001/api/contracts/deploy -H 'Content-Type: application/json' \
  -d "{\"from\":\"$WALLET\",\"code\":\"$(cat contracts/vault.hex)\"}"

# 调用 +7（calldata = 选择器 + 32 字节参数）
curl -s -X POST http://127.0.0.1:3001/api/contracts/call -H 'Content-Type: application/json' \
  -d "{\"from\":\"$WALLET\",\"to\":\"$CONTRACT\",\"method\":\"deposit(uint256)\",\"args\":[\"7\"]}"

# 查存储与事件
curl -s "http://127.0.0.1:3001/api/contract?address=$CONTRACT"
curl -s "http://127.0.0.1:3001/api/logs?limit=10"
```
