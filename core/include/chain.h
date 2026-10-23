// chain.h — 区块链共识状态机（区块/交易/UTXO 之外的账户模型、PoW、难度调整、持久化）
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "json.h"

namespace bc {

static const uint64_t COIN = 100000000ULL; // 1 枚 = 1e8 最小单位

struct Tx {
    std::string id;
    std::string type; // "transfer" | "coinbase"
    std::string from; // coinbase 时为空
    std::string to;
    uint64_t amount;
    uint64_t fee;
    uint64_t nonce;
    int64_t timestamp;
    std::string pubkey;    // 128 hex
    std::string signature; // base64(r||s)

    Tx() : amount(0), fee(0), nonce(0), timestamp(0) {}
};

struct Block {
    uint64_t index;
    int64_t timestamp;
    std::string prev_hash;
    std::string merkle_root;
    uint32_t difficulty;
    uint64_t nonce;
    std::vector<Tx> txs;
    std::string hash;

    Block() : index(0), timestamp(0), difficulty(0), nonce(0) {}
};

struct Account {
    uint64_t balance;
    uint64_t nonce;
    Account() : balance(0), nonce(0) {}
};

typedef std::map<std::string, Account> State;

struct Params {
    uint32_t initial_difficulty;
    uint32_t min_difficulty;
    uint32_t max_difficulty;
    int64_t target_block_ms;
    int retarget_interval;
    uint64_t initial_reward;
    uint32_t halving_interval;
    size_t max_block_txs;
    int64_t genesis_time;
    int64_t max_future_ms;

    Params()
        : initial_difficulty(4), min_difficulty(3), max_difficulty(4), target_block_ms(2000),
          retarget_interval(10), initial_reward(50 * COIN), halving_interval(210000), max_block_txs(500),
          genesis_time(1735689600000LL), max_future_ms(2LL * 3600 * 1000) {}
};

// ---- 序列化 / 计算 ----
std::string tx_compute_id(const Tx& tx);
std::string tx_coinbase_id(uint64_t index, const std::string& to, uint64_t amount, int64_t timestamp);
std::string block_compute_hash(const Block& b);
std::string merkle_root_of(const std::vector<Tx>& txs);
J tx_to_json(const Tx& tx);
Tx tx_from_json(const J& j);
J block_to_json(const Block& b, bool with_txs = true);
Block block_from_json(const J& j);
std::string amount_to_str(uint64_t v); // 1e8 -> "1.00000000"
bool amount_from_str(const std::string& s, uint64_t& out);

class Chain {
public:
    Chain();
    explicit Chain(const Params& p);

    const Params& params() const { return params_; }
    void set_params(const Params& p) { params_ = p; }

    // 载入或初始化创世块
    bool load(const std::string& datadir, std::string& err);
    void reset_to_genesis();
    void save_all() const;

    const Block& tip() const { return blocks_.back(); }
    uint64_t height() const { return blocks_.back().index; }
    size_t block_count() const { return blocks_.size(); }
    const std::vector<Block>& blocks() const { return blocks_; }
    const State& state() const { return state_; }

    uint64_t subsidy(uint64_t height) const;
    uint32_t expected_difficulty(const std::vector<Block>& chain) const;
    uint64_t total_work(const std::vector<Block>& chain) const;

    // 交易池
    bool submit_tx(const Tx& tx, std::string& err);
    size_t mempool_size() const { return mempool_.size(); }
    std::vector<Tx> mempool_list() const;
    void prune_mempool();

    // 出块
    bool mine_step(const std::string& miner, uint32_t iters, Block& found_block, bool& found, std::string& err,
                   uint64_t* hashes_done = 0);
    void reset_mining() { mining_active_ = false; }

    // 接收外部区块
    bool add_block(const Block& b, std::string& err, bool& need_sync);
    bool replace_chain(const std::vector<Block>& new_chain, std::string& err);

    // 校验
    bool validate_chain(std::string& err) const;

    // 查询
    bool account(const std::string& addr, Account& acc) const;
    bool tx_lookup(const std::string& id, Tx& tx, uint64_t& block_index) const;

    J chain_json(size_t from, size_t limit, bool with_txs) const;
    J status_json() const;

private:
    void apply_tx(State& st, const Tx& tx) const;
    bool check_and_apply_tx(State& st, const Tx& tx, std::string& err) const;
    bool validate_block_against(const Block& b, const Block& prev, State& st, std::string& err) const;
    bool build_template(const std::string& miner, Block& out, std::string& err);
    void append_block(const Block& b, bool persist, bool apply_state = true);
    void save_mempool() const;
    std::string path_of(const std::string& name) const;

    Params params_;
    std::vector<Block> blocks_;
    State state_;
    std::map<std::string, Tx> mempool_;
    std::string datadir_;

    // 挖矿会话
    bool mining_active_;
    Block tmpl_;
    uint64_t work_nonce_;
    std::string tmpl_prev_;
};

} // namespace bc
