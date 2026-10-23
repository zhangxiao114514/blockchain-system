// chain.h — 区块链共识状态机（v0.2：合约 + 事件日志 + 跨链托管桥）
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "json.h"
#include "vm.h"

namespace bc {

static const uint64_t COIN = 100000000ULL; // 1 枚 = 1e8 最小单位

// 跨链托管桥的系统地址（原生实现，非字节码合约）
#define BRIDGE_ADDRESS "0x00000000000000000000000000000000000000bb"

// ---------------- 交易 ----------------
struct Tx {
    std::string id;
    std::string type; // transfer | coinbase | deploy_contract | contract_call | cross_transfer
    std::string from;
    std::string to;
    uint64_t amount;
    uint64_t fee;
    uint64_t nonce;
    int64_t timestamp;
    std::string pubkey;
    std::string signature;

    // 合约与跨链扩展字段
    std::string code;          // deploy_contract：初始化字节码（hex）
    std::string input;         // contract_call：calldata（hex）
    uint64_t gas_limit;        // 合约执行 gas 上限
    uint64_t target_chain_id;  // cross_transfer：目标链
    uint64_t gas_used;         // 执行结果回填（不参与交易哈希）

    Tx() : amount(0), fee(0), nonce(0), timestamp(0), gas_limit(0), target_chain_id(0), gas_used(0) {}
};

// ---------------- 区块 ----------------
struct Block {
    uint64_t index;
    int64_t timestamp;
    std::string prev_hash;
    std::string merkle_root;
    std::string logs_root; // v0.2 新增：事件日志的 Merkle 根
    uint32_t difficulty;
    uint64_t nonce;
    std::vector<Tx> txs;
    std::vector<vm::Log> logs; // v0.2 新增：区块事件日志
    std::string hash;

    Block() : index(0), timestamp(0), difficulty(0), nonce(0) {}
};

struct Account {
    uint64_t balance;
    uint64_t nonce;
    Account() : balance(0), nonce(0) {}
};

struct Contract {
    std::string code;
    std::map<std::string, std::string> storage; // 32 字节槽位：64hex -> 64hex
    uint64_t created_at;
    std::string creator;
    Contract() : created_at(0) {}
};

// 跨链托管桥状态
struct Bridge {
    uint64_t seq;    // 出向全局序列号（单调递增，防重放）
    uint64_t locked; // 托管锁定总额
    uint64_t minted; // 入向铸造总额
    std::map<std::string, std::string> relay_root; // 源链 -> 最新已提交根
    std::map<std::string, uint64_t> relay_height;  // 源链 -> 已提交高度
    std::set<std::string> processed;               // "源链:序列号" 已认领
    Bridge() : seq(0), locked(0), minted(0) {}
};

// 完整链状态（账户 + 合约 + 桥 + 本块日志）
struct ChainState {
    std::map<std::string, Account> accounts;
    std::map<std::string, Contract> contracts;
    Bridge bridge;
    std::vector<vm::Log> logs;
};

struct Params {
    uint32_t version;
    uint64_t chain_id;
    std::string relayer; // 允许提交跨链根的中继地址（为空表示开发模式：任何人可提交）

    uint32_t initial_difficulty;
    uint32_t min_difficulty;
    uint32_t max_difficulty;
    int64_t target_block_ms;
    int retarget_interval;
    uint64_t initial_reward;
    uint32_t halving_interval;
    size_t max_block_txs;
    uint64_t max_code_size;
    uint64_t default_gas_limit;
    int64_t genesis_time;
    int64_t max_future_ms;

    Params()
        : version(2), chain_id(1001), initial_difficulty(4), min_difficulty(3), max_difficulty(4),
          target_block_ms(2000), retarget_interval(10), initial_reward(50 * COIN), halving_interval(210000),
          max_block_txs(500), max_code_size(16384), default_gas_limit(200000),
          genesis_time(1735689600000LL), max_future_ms(2LL * 3600 * 1000) {}
};

// ---------------- 序列化 / 计算 ----------------
std::string tx_compute_id(const Tx& tx, uint64_t chain_id);
std::string tx_coinbase_id(uint64_t index, const std::string& to, uint64_t amount, int64_t timestamp);
std::string block_compute_hash(const Block& b);
std::string merkle_root_of(const std::vector<Tx>& txs);
J tx_to_json(const Tx& tx);
Tx tx_from_json(const J& j);
J block_to_json(const Block& b, bool with_txs = true);
Block block_from_json(const J& j);
J log_to_json(const vm::Log& l, const std::string& tx_id, uint64_t tx_index);
std::string amount_to_str(uint64_t v);
bool amount_from_str(const std::string& s, uint64_t& out);

class Chain {
public:
    Chain();
    explicit Chain(const Params& p);

    const Params& params() const { return params_; }
    void set_params(const Params& p) { params_ = p; }
    uint64_t chain_id() const { return params_.chain_id; }

    bool load(const std::string& datadir, std::string& err);
    void reset_to_genesis();
    void save_all() const;

    const Block& tip() const { return blocks_.back(); }
    uint64_t height() const { return blocks_.back().index; }
    size_t block_count() const { return blocks_.size(); }
    const std::vector<Block>& blocks() const { return blocks_; }
    const ChainState& state() const { return state_; }

    uint64_t subsidy(uint64_t height) const;
    uint32_t expected_difficulty(const std::vector<Block>& chain) const;
    uint64_t total_work(const std::vector<Block>& chain) const;

    bool submit_tx(const Tx& tx, std::string& err);
    size_t mempool_size() const { return mempool_.size(); }
    std::vector<Tx> mempool_list() const;
    void prune_mempool();

    bool mine_step(const std::string& miner, uint32_t iters, Block& found_block, bool& found, std::string& err,
                   uint64_t* hashes_done = 0);
    void reset_mining() { mining_active_ = false; }

    bool add_block(const Block& b, std::string& err, bool& need_sync);
    bool replace_chain(const std::vector<Block>& new_chain, std::string& err);
    bool validate_chain(std::string& err) const;

    bool account(const std::string& addr, Account& acc) const;
    bool tx_lookup(const std::string& id, Tx& tx, uint64_t& block_index) const;
    bool contract_lookup(const std::string& addr, Contract& c) const;

    J chain_json(size_t from, size_t limit, bool with_txs) const;
    J status_json() const;
    J bridge_json() const;
    J logs_json(uint64_t from_block, size_t limit, const std::string& address, const std::string& topic) const;
    J contracts_json() const;

    // 合约地址派生：sha256("create|chain_id|from|nonce") 前 20 字节
    std::string derive_contract_address(const std::string& from, uint64_t nonce) const;

private:
    bool check_and_apply_tx(ChainState& st, Tx& tx, std::string& err) const;
    bool exec_contract(ChainState& st, Tx& tx, std::string& err) const;
    bool exec_bridge(ChainState& st, Tx& tx, std::string& err) const;
    bool validate_block_against(const Block& b, const Block& prev, ChainState& st, std::string& err) const;
    bool build_template(const std::string& miner, Block& out, std::string& err);
    void append_block(const Block& b, bool persist, bool apply_state = true);
    void save_mempool() const;
    std::string path_of(const std::string& name) const;

    Params params_;
    std::vector<Block> blocks_;
    ChainState state_;
    std::map<std::string, Tx> mempool_;
    std::string datadir_;

    bool mining_active_;
    Block tmpl_;
    ChainState tmpl_state_;
    uint64_t work_nonce_;
    std::string tmpl_prev_;
};

} // namespace bc
