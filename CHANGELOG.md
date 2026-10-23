# 更新日志

本项目遵循 [语义化版本](https://semver.org/lang/zh-CN/)：`主版本.次版本.修订号`。
破坏共识/数据格式的改动升次版本（0.x 阶段），纯修复升修订号。

## v0.2.1 — 2026-10-24

### 修复
- 修正测试与启动脚本在 Linux 下的引擎路径拼接错误（`bin/bin/chain-engine`），
  此前导致 CI 的单元测试阶段在 Linux 上找不到可执行文件。Windows 不受影响。

---

## v0.2.0 — 合约与跨链（2026-10-24）

### 新增

**合约层**
- 新增交易类型 `deploy_contract`、`contract_call`（`transfer` / `coinbase` 保持兼容）
- 精简 EVM 子集 VM（`core/src/vm.cpp`）：256 位字长栈机、40+ 指令、
  gas 计量与耗尽回滚、32 字节存储槽、`LOG0..LOG2` 事件
- ABI 编解码：`sha256("name(type,...)")` 前 4 字节作选择器，参数按 32 字节字编码
- 确定性合约地址：`sha256("create|chain_id|from|nonce")[0:20]`
- 示例合约 `contracts/vault.hex`（累计存款 + Deposit 事件）
- 引擎命令：`contract` / `contracts` / `logs` / `abi_encode` / `merkle_build`

**事件日志**
- 区块新增 `logs` 字段与 `logs_root`（Merkle 根），`logs_root` 参与区块哈希
- 日志归属到具体交易（`tx_id` / `tx_index`）
- 接口 `GET /api/logs`，支持按地址与 topic 过滤

**跨链**
- 新增交易类型 `cross_transfer`：锁定到托管地址、分配全局序列号、发出 `CrossOut` 事件
- 原生托管桥合约 `0x0000…00bb`：`commitRoot` / `claim` 两个方法
- 默克尔证明中继 `bridge/relayer.py`：扫描事件 → 建树 → 提交根 → 逐条认领
- 防重放：`(源链, 序列号)` 一次性去重；防超发：叶子绑定金额、根高度单调递增
- `chain_id` 隔离：交易哈希与创世块均绑定 chain_id，跨链重放会被拒
- 双链调试环境 `start_dual.py`

**工程**
- 新增测试：`tests/unit_core.py`（18 项）、`tests/crosschain.py`（16 项）
- CI 增加单元测试与跨链测试两个阶段
- 新增文档：`docs/API.md`、`docs/CONTRACT.md`、`docs/BRIDGE.md`

### 变更（破坏性）

- 区块哈希加入 `logs_root`；交易哈希加入 `chain_id`/`code`/`input`/`gas_limit`/`target_chain_id`
- 创世块包含 `chain_id`，旧链数据无法通过校验（会自动备份为 `blocks.ndjson.corrupt` 后重置）
- `POST /api/tx` 语义不变；新增 `POST /api/tx/raw` 提交外部已签名交易

### 修复

- 修正 `CALLDATALOAD` 操作码（0x35，此前误用 0x37）
- 修正 `SLOAD` 操作码（0x54，此前误用 0x50）
- 合约部署不再把构造返回值当作运行时代码（改为单代码模型）
- `tx.gas_used` 现在会记录到区块中

### 升级指引

链数据格式不兼容，升级后请清空 `data/` 重新初始化；钱包库（`keystore.json`）不受影响。

---

## v0.1.0 — 初始版本

- 自研 SHA-256 / HMAC-SHA256 / secp256k1 ECDSA（RFC6979 + low-s）
- PoW 共识、难度自适应、Merkle 树、账户模型、交易池、最长链分叉重组
- 多节点 P2P（握手、节点发现、区块与交易 gossip）
- 加密钱包库（PBKDF2 + HMAC-CTR + HMAC 认证）
- HTTP JSON API + 浏览器控制台
- 测试：密码学自检、OpenSSL 交叉验证 24/24、集成测试 18/18
