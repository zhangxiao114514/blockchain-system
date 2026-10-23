# 区块链系统（C++ 共识内核 + Python 节点服务）

[![build-and-test](https://github.com/zhangxiao114514/blockchain-system/actions/workflows/ci.yml/badge.svg)](https://github.com/zhangxiao114514/blockchain-system/actions/workflows/ci.yml)
[![C++14](https://img.shields.io/badge/C%2B%2B-14-blue.svg)](#)
[![Python](https://img.shields.io/badge/Python-3.10%2B-3776ab.svg)](#)
[![deps](https://img.shields.io/badge/dependencies-0-brightgreen.svg)](#)

一条**可运行、可交互、可校验**的完整区块链：自研 SHA-256 与 secp256k1 ECDSA、PoW 共识、
多节点 P2P 同步、最长链分叉重组、加密钱包、HTTP API 与浏览器控制台。

**v0.2 新增**：精简 EVM 子集合约（部署/调用/存储/事件日志/ABI/gas）、
可信中继跨链（托管合约 + 默克尔证明 + 全局序列号 + chain_id 隔离）。
不依赖任何第三方库（无 OpenSSL、无 Boost、无 pip 包），Windows / Linux 开箱即用。

> 合约与 ABI → [docs/CONTRACT.md](docs/CONTRACT.md) ｜ 跨链设计 → [docs/BRIDGE.md](docs/BRIDGE.md) ｜ 接口与 curl → [docs/API.md](docs/API.md)

---

## 1. 架构

```
┌──────────────────────────── Python 节点服务（node/） ────────────────────────────┐
│  cli.py      命令行入口            rpc.py     HTTP JSON API + 静态控制台          │
│  node.py     编排/挖矿调度          p2p.py     asyncio TCP 对等网络（gossip）      │
│  keystore.py 加密钱包库            engine.py  引擎客户端（子进程 + 行式 JSON）     │
│  web/        浏览器控制台（原生 JS，零依赖）                                      │
│  ../bridge/relayer.py  可信中继：扫描事件 → 建 Merkle 树 → 提交根 → 认领          │
└───────────────────────────────────┬──────────────────────────────────────────────┘
                                    │ stdin/stdout 行式 JSON 协议
┌───────────────────────────────────▼──────────────────────────────────────────────┐
│                        C++ 共识内核（core/，静态库 libchaincore）                 │
│  crypto.cpp  SHA-256 / HMAC-SHA256 / secp256k1 ECDSA（RFC6979）/ Base64          │
│  vm.cpp      精简 EVM 子集（256 位栈机 / gas / 存储槽 / 事件日志）+ ABI + Merkle  │
│  chain.cpp   区块与交易模型、PoW、难度调整、账户与合约状态机、交易池、            │
│              分叉重组、全链校验、跨链托管桥、NDJSON 持久化                        │
│  json.cpp    极简 JSON DOM（解析 + 序列化）                                       │
│  engine.cpp  引擎进程：把内核能力暴露为命令                                       │
│  cli/chainctl.cpp  C++ 运维命令行（可完全脱离 Python 使用）                        │
└──────────────────────────────────────────────────────────────────────────────────┘
```

**为什么这样分层**：共识与密码学是性能与正确性关键，放在 C++；网络编排、进程管理、
Web 交互迭代快，放在 Python。两者用稳定的行式 JSON 协议解耦，C++ 侧不依赖任何网络库，
Python 侧不碰密码学实现，密码学只有一份实现（C++），避免多语言实现不一致。

---

## 2. 快速开始

```bash
# 1) 编译 C++ 内核（自动选 CMake，缺构建工具时回退 g++）
bash build.sh

# 2) 启动 3 节点网络（节点 1 自动挖矿，作为种子）
python start.py --fresh

# 3) 打开控制台
#    http://127.0.0.1:3001   （右上角可切换 3001/3002/3003 三个节点）
```

Windows 下若 `cmake` 不在 PATH，脚本会自动查找 `C:\Program Files\CMake\bin\cmake.exe`。
若使用 CMake 手动构建：

```bash
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

> **注意**：MinGW 的 make 无法处理含中文的绝对路径（Makefile 会按 ANSI 码页解析而乱码）。
> `build.sh` 检测到非 ASCII 路径时会自动把源码暂存到临时 ASCII 目录编译，再把产物复制回 `bin/`。

### 常用命令

```bash
python node/cli.py status   --port 3001     # 节点状态
python node/cli.py wallets  --port 3001     # 钱包与余额
python node/cli.py mine     --port 3001 --count 1
python node/cli.py validate --port 3001     # 全链校验
python node/cli.py faucet   --port 3001 --address 0x... --amount 100
python node/cli.py send     --port 3001 --from 0x... --to 0x... --amount 1.5

# C++ 侧（不经过 Python，直接操作数据目录）
./bin/chainctl --datadir data/n3001 status
./bin/chainctl --datadir data/n3001 validate
./bin/chainctl --datadir data/n3001 mine --miner 0x... --count 3
./bin/chainctl --datadir data/n3001 block 5
```

---

## 3. 控制台能做什么

- **实时区块流**：每 1.5 秒刷新，点击区块展开交易明细（出块奖励 / 转账、金额、txid）
- **我的钱包**：新建钱包、复制地址、一键领取 100 测试币、实时余额
- **发起转账**：选付款钱包 → 填收款地址与金额 → 节点签名并广播
- **交易池**：查看待确认交易，出块后自动清空
- **自动挖矿开关**：可在任意节点开启，多个节点同时挖矿即可观察**分叉与最长链重组**；
  关闭后节点仍会在收到交易时立刻出块，保证「发送即到账」
- **删除钱包**：钱包行右侧 `×`，二次确认后从本地加密库移除（矿工钱包受保护）
- **校验全链**：逐块重算哈希、Merkle 根、PoW、签名与余额，任何篡改都会被抓出
- **节点切换**：顶部下拉切换查看不同节点的视角（含对等节点高度对比）

---

## 4. 共识规则

| 项目 | 取值 |
|---|---|
| 哈希 | SHA-256（自研实现，已用 OpenSSL 交叉验证） |
| 签名 | ECDSA secp256k1，RFC6979 确定性 k，low-s 规范化 |
| 地址 | `0x` + SHA256(未压缩公钥) 前 20 字节 |
| 工作量证明 | 区块哈希前 `difficulty` 个十六进制 0 |
| 出块奖励 | 50 币，每 210000 块减半；奖励 = 补贴 + 本块手续费 |
| 难度调整 | 每 10 块一次，目标出块 2000ms，范围 3~4（`--max-difficulty` 可调高） |
| 出块节流 | 自动挖矿时空块间隔 1500ms（`--mine-interval`），避免空转刷块 |
| 即时确认 | 提交交易后立刻触发一次出块，**到账实测 ~0.6s** |
| 交易模型 | 账户模型：`nonce` 防重放，余额与 nonce 由链上状态推导 |
| 交易类型 | `transfer` / `coinbase` / `deploy_contract` / `contract_call` / `cross_transfer` |
| 事件日志 | 区块新增 `logs` 字段与 `logs_root`（Merkle 根参与区块哈希） |
| 合约 | 256 位栈机、gas 计量、32 字节存储槽、ABI 编解码、确定性合约地址 |
| 跨链 | 托管桥 + 默克尔证明中继，`chain_id` 隔离 + 全局序列号防重放 |
| 分叉选择 | 累计工作量（Σ2^difficulty）更大者胜；等长不切换 |
| 区块上限 | 500 笔交易 |
| 金额精度 | 1 币 = 1e8 最小单位，全整数运算（无浮点误差） |

**校验清单**（任一不满足即拒收）：区块高度连续、前哈希匹配、区块哈希可重算、
PoW 达标、Merkle 根匹配、首笔必须是出块奖励且金额正确、每笔转账签名有效且
公钥与地址匹配、nonce 严格递增、余额充足、时间戳单调递增。

---

## 5. HTTP API

完整接口表与 curl 示例见 **[docs/API.md](docs/API.md)**，这里只列主干：

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/status` | 节点与链状态（高度、难度、交易数、对等节点） |
| GET | `/api/blocks?limit=20` | 最新区块（含交易明细） |
| GET | `/api/block?index=5` | 单个区块 |
| GET | `/api/mempool` | 交易池 |
| GET | `/api/peers` | 已连接节点 |
| GET | `/api/network` | 网络拓扑（控制台节点切换用） |
| GET | `/api/wallets` | 本节点钱包及余额 |
| GET | `/api/account?address=0x..` | 账户余额 / nonce / next_nonce |
| GET | `/api/validate` | 全链校验 |
| GET | `/api/tx?id=..` | 交易查询 |
| POST | `/api/wallets` | 新建钱包 `{label}` |
| POST | `/api/wallets/delete` | 删除钱包 `{address}`（矿工钱包受保护） |
| POST | `/api/tx` | 转账 `{from,to,amount,fee}`（节点代签名） |
| POST | `/api/faucet` | 领测试币 `{address,amount}` |
| POST | `/api/mine` | 立即出块 `{count}` |
| POST | `/api/automine` | 开关自动挖矿 `{enabled}` |
| POST | `/api/contracts/deploy` | 部署合约 `{from,code,gas_limit}` |
| POST | `/api/contracts/call` | 调用合约 `{from,to,method,args}` |
| GET | `/api/contract?address=` | 合约详情与存储槽 |
| GET | `/api/logs` | 事件日志 |
| POST | `/api/bridge/transfer` | 发起跨链转账 |
| GET | `/api/bridge` | 跨链桥状态 |

金额一律用十进制字符串（如 `"12.5"`），内部换算为 1e8 最小单位整数。

---

## 6. 安全设计

- **私钥不出节点**：私钥只存在于 `keystore.json` 的密文中，API 永不返回私钥，
  签名在 C++ 内完成，浏览器只传地址与金额。
- **钱包库加密**：`PBKDF2-HMAC-SHA256(200000)` 派生密钥 → `HMAC-SHA256` 派生
  enc/mac 子密钥 → HMAC-CTR 伪随机流加密 → HMAC-SHA256 先加密后认证。
  未指定口令时自动生成随机口令文件（`keystore.json.key`，权限 600）。
- **签名**：RFC6979 确定性 k（杜绝 k 复用导致私钥泄露），low-s 规范化（防延展性攻击）。
- **验签前校验**：公钥与地址绑定、公钥点在曲线上（`y² = x³ + 7`）、r/s 范围检查。
- **抗篡改**：本地链文件每次启动全量校验，失败则备份为 `blocks.ndjson.corrupt`
  并重置为创世块，绝不带着坏数据继续运行。
- **金额溢出**：全整数运算 + 上限校验。

> 生产环境还需：TLS、节点身份认证与准入、私钥硬件保护、网络层限流与防女巫、
> 检查点/轻客户端证明。本项目定位是**可运行的完整教学与验证系统**，不是可直接承载
> 真实资产的公链。

---

## 7. 测试

```bash
# 密码学自检（SHA-256 / HMAC 已知向量、k=1 公钥向量、签名自验）
./bin/test_crypto.exe

# 用 OpenSSL(Node crypto) 独立验证 C++ 的 secp256k1 签名（含篡改必须失败）
node tests/cross_verify.js

# 端到端集成测试：3 节点 / P2P 同步 / 领币 / 转账 / 分叉收敛 / 全链校验 / 持久化
python tests/integration.py

# 内核单元测试：VM 指令 / gas / ABI / 合约存储 / 跨链桥安全
python tests/unit_core.py

# 双链跨链测试：托管锁定 / 中继 / 默克尔证明 / 防重放 / 防超发 / chain_id 隔离
python tests/crosschain.py
```

当前结果：密码学自检通过，OpenSSL 交叉验证 24/24，集成测试 18/18，
内核单元测试 18/18，跨链测试 16/16。

---

## 8. 目录结构

```
├── CMakeLists.txt          # CMake 工程（顶层）
├── build.sh                # 构建脚本（CMake 优先，g++ 兜底）
├── start.py                # 一键启动多节点网络
├── core/                   # C++ 共识内核
│   ├── include/            # crypto.h / chain.h / vm.h / json.h
│   ├── src/                # crypto.cpp / vm.cpp / chain.cpp / json.cpp / engine.cpp
│   ├── cli/chainctl.cpp    # C++ 运维命令行
│   └── tests/test_crypto.cpp
├── node/                   # Python 节点服务
│   ├── cli.py  node.py  engine.py  p2p.py  rpc.py  keystore.py  util.py
│   └── web/                # 控制台（index.html / app.js / style.css）
├── bridge/relayer.py       # 跨链可信中继
├── contracts/              # 示例合约（vault.hex / vault.asm）
├── docs/                   # API.md / CONTRACT.md / BRIDGE.md
├── start_dual.py           # 双链跨链调试环境
├── tests/                  # cross_verify.js / integration.py / unit_core.py / crosschain.py
├── bin/                    # 编译产物
└── data/                   # 各节点数据目录（链、交易池、加密钱包库、日志）
```

---

## 9. 版本与兼容性

**v0.2 破坏性变更**（链数据格式升级，旧数据会被自动备份为 `blocks.ndjson.corrupt` 并重置）：

- 区块新增 `logs` / `logs_root` 字段，区块哈希计算加入 `logs_root`
- 交易哈希加入 `chain_id`、`code`、`input`、`gas_limit`、`target_chain_id`
- 创世块包含 `chain_id`，不同链的创世哈希不同
- 启动脚本参数保持兼容（`--datadir/--port/--p2p/--auto-mine` 语义不变），新增 `--chain-id/--relayer`

## 10. 已知边界

- 单机多进程模拟网络，未做跨机器 NAT 穿透与加密传输（P2P 明文 JSON，仅本地回环）。
- 挖矿为单线程协作式（每次 25000 次哈希后让出 CPU），保证 RPC 不被阻塞。
- 出块节奏由「难度 + 挖矿节流间隔」共同决定：难度控制单次 PoW 成本，间隔控制空块频率。
- 交易池不做手续费市场（无动态费率、无替换交易），按手续费降序打包。
- 未实现 SPV/轻客户端与检查点。
