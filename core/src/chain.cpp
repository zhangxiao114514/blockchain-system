// chain.cpp — 共识状态机实现（v0.2：合约 / 事件日志 / 跨链托管桥）
#include "chain.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "crypto.h"

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
// -std=c++14 会定义 __STRICT_ANSI__，部分工具链头文件此时不暴露宽字符文件 API，这里显式声明
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"
#endif
extern "C" FILE* _wfopen(const wchar_t*, const wchar_t*);
extern "C" int _wremove(const wchar_t*);
extern "C" int _wrename(const wchar_t*, const wchar_t*);
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#else
#include <sys/stat.h>
#endif

namespace bc {

// ---------------- 小工具 ----------------
static std::string u64s(uint64_t v) {
    if (v == 0) return "0";
    char t[24];
    int i = 0;
    while (v) { t[i++] = (char)('0' + (int)(v % 10)); v /= 10; }
    std::string s;
    s.reserve(i);
    while (i) s.push_back(t[--i]);
    return s;
}

static std::string i64s(int64_t v) {
    if (v < 0) return "-" + u64s((uint64_t)(-(v + 1)) + 1ULL);
    return u64s((uint64_t)v);
}

// Windows 下 fopen 用 ANSI 码页解析路径，含中文的 UTF-8 路径会失败，统一走宽字符 API
#ifdef _WIN32
static std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static FILE* bc_fopen(const std::string& path, const char* mode) {
    std::wstring wp = widen(path), wm = widen(mode);
    return _wfopen(wp.c_str(), wm.c_str());
}
static int bc_remove(const std::string& path) { return _wremove(widen(path).c_str()); }
static int bc_rename(const std::string& from, const std::string& to) {
    return _wrename(widen(from).c_str(), widen(to).c_str());
}
static void ensure_dir(const std::string& d) {
    if (d.empty()) return;
    _wmkdir(widen(d).c_str());
}
#else
static FILE* bc_fopen(const std::string& path, const char* mode) { return fopen(path.c_str(), mode); }
static int bc_remove(const std::string& path) { return remove(path.c_str()); }
static int bc_rename(const std::string& from, const std::string& to) { return rename(from.c_str(), to.c_str()); }
static void ensure_dir(const std::string& d) {
    if (d.empty()) return;
    mkdir(d.c_str(), 0755);
}
#endif

static bool is_hex_str(const std::string& s, size_t n) {
    if (s.size() != n) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

static bool is_address(const std::string& a) {
    if (a.size() != 42) return false;
    if (a[0] != '0' || (a[1] != 'x' && a[1] != 'X')) return false;
    return is_hex_str(a.substr(2), 40);
}

std::string amount_to_str(uint64_t v) {
    std::string s = u64s(v / COIN);
    uint64_t frac = v % COIN;
    char buf[16];
    for (int i = 7; i >= 0; --i) { buf[i] = (char)('0' + (int)(frac % 10)); frac /= 10; }
    buf[8] = 0;
    std::string f(buf);
    while (!f.empty() && f[f.size() - 1] == '0') f.erase(f.size() - 1);
    if (f.empty()) return s;
    return s + "." + f;
}

bool amount_from_str(const std::string& in, uint64_t& out) {
    std::string t;
    for (size_t i = 0; i < in.size(); ++i)
        if (in[i] != ' ' && in[i] != '\t') t.push_back(in[i]);
    if (t.empty()) return false;
    size_t dot = t.find('.');
    std::string ip = dot == std::string::npos ? t : t.substr(0, dot);
    std::string fp = dot == std::string::npos ? "" : t.substr(dot + 1);
    if (fp.size() > 8) fp = fp.substr(0, 8);
    while (fp.size() < 8) fp.push_back('0');
    uint64_t iv = 0;
    for (size_t i = 0; i < ip.size(); ++i) {
        if (ip[i] < '0' || ip[i] > '9') return false;
        iv = iv * 10 + (uint64_t)(ip[i] - '0');
        if (iv > 1000000000000ULL) return false;
    }
    uint64_t fv = 0;
    for (size_t i = 0; i < 8; ++i) {
        if (fp[i] < '0' || fp[i] > '9') return false;
        fv = fv * 10 + (uint64_t)(fp[i] - '0');
    }
    out = iv * COIN + fv;
    return true;
}

// ---------------- 交易 / 区块 ----------------
std::string tx_compute_id(const Tx& tx, uint64_t chain_id) {
    std::string payload = tx.type + "|" + u64s(chain_id) + "|" + tx.from + "|" + tx.to + "|" + u64s(tx.amount) + "|" +
                          u64s(tx.fee) + "|" + u64s(tx.nonce) + "|" + i64s(tx.timestamp) + "|" + tx.code + "|" +
                          tx.input + "|" + u64s(tx.gas_limit) + "|" + u64s(tx.target_chain_id);
    return sha256_hex(payload);
}

std::string tx_coinbase_id(uint64_t index, const std::string& to, uint64_t amount, int64_t timestamp) {
    return sha256_hex("coinbase|" + u64s(index) + "|" + to + "|" + u64s(amount) + "|" + i64s(timestamp));
}

std::string merkle_root_of(const std::vector<Tx>& txs) {
    if (txs.empty()) return sha256_hex("");
    std::vector<std::string> leaves;
    leaves.reserve(txs.size());
    for (size_t i = 0; i < txs.size(); ++i) leaves.push_back(txs[i].id);
    return vm::merkle_root_hex(leaves);
}

std::string block_compute_hash(const Block& b) {
    std::string payload = u64s(b.index) + "|" + b.prev_hash + "|" + b.merkle_root + "|" + b.logs_root + "|" +
                          i64s(b.timestamp) + "|" + u64s(b.difficulty) + "|" + u64s(b.nonce);
    return sha256_hex(payload);
}

J tx_to_json(const Tx& t) {
    J j = J::obj();
    j.set("id", J::str(t.id));
    j.set("type", J::str(t.type));
    j.set("from", J::str(t.from));
    j.set("to", J::str(t.to));
    j.set("amount", J::num((int64_t)t.amount));
    j.set("amount_str", J::str(amount_to_str(t.amount)));
    j.set("fee", J::num((int64_t)t.fee));
    j.set("nonce", J::num((int64_t)t.nonce));
    j.set("timestamp", J::num(t.timestamp));
    j.set("pubkey", J::str(t.pubkey));
    j.set("signature", J::str(t.signature));
    j.set("code", J::str(t.code));
    j.set("input", J::str(t.input));
    j.set("gas_limit", J::num((int64_t)t.gas_limit));
    j.set("gas_used", J::num((int64_t)t.gas_used));
    j.set("target_chain_id", J::num((int64_t)t.target_chain_id));
    return j;
}

Tx tx_from_json(const J& j) {
    Tx t;
    if (const J* p = j.find("id")) t.id = p->s();
    if (const J* p = j.find("type")) t.type = p->s();
    if (const J* p = j.find("from")) t.from = p->s();
    if (const J* p = j.find("to")) t.to = p->s();
    if (const J* p = j.find("amount")) t.amount = (uint64_t)p->i();
    if (const J* p = j.find("fee")) t.fee = (uint64_t)p->i();
    if (const J* p = j.find("nonce")) t.nonce = (uint64_t)p->i();
    if (const J* p = j.find("timestamp")) t.timestamp = p->i();
    if (const J* p = j.find("pubkey")) t.pubkey = p->s();
    if (const J* p = j.find("signature")) t.signature = p->s();
    if (const J* p = j.find("code")) t.code = p->s();
    if (const J* p = j.find("input")) t.input = p->s();
    if (const J* p = j.find("gas_limit")) t.gas_limit = (uint64_t)p->i();
    if (const J* p = j.find("gas_used")) t.gas_used = (uint64_t)p->i();
    if (const J* p = j.find("target_chain_id")) t.target_chain_id = (uint64_t)p->i();
    return t;
}

J log_to_json(const vm::Log& l, const std::string& tx_id, uint64_t tx_index) {
    J j = J::obj();
    j.set("address", J::str(l.address));
    J topics = J::arr();
    for (size_t i = 0; i < l.topics.size(); ++i) topics.push(J::str(l.topics[i]));
    j.set("topics", topics);
    j.set("data", J::str(l.data));
    j.set("tx_id", J::str(tx_id.empty() ? l.tx_id : tx_id));
    j.set("tx_index", J::num((int64_t)(tx_id.empty() ? l.tx_index : tx_index)));
    return j;
}

J block_to_json(const Block& b, bool with_txs) {
    J j = J::obj();
    j.set("index", J::num((int64_t)b.index));
    j.set("timestamp", J::num(b.timestamp));
    j.set("prev_hash", J::str(b.prev_hash));
    j.set("merkle_root", J::str(b.merkle_root));
    j.set("logs_root", J::str(b.logs_root));
    j.set("difficulty", J::num((int64_t)b.difficulty));
    j.set("nonce", J::num((int64_t)b.nonce));
    j.set("hash", J::str(b.hash));
    j.set("tx_count", J::num((int64_t)b.txs.size()));
    j.set("log_count", J::num((int64_t)b.logs.size()));
    if (with_txs) {
        J arr = J::arr();
        for (size_t i = 0; i < b.txs.size(); ++i) arr.push(tx_to_json(b.txs[i]));
        j.set("transactions", arr);
        J logs = J::arr();
        for (size_t i = 0; i < b.logs.size(); ++i) logs.push(log_to_json(b.logs[i], "", 0));
        j.set("logs", logs);
    }
    return j;
}

Block block_from_json(const J& j) {
    Block b;
    if (const J* p = j.find("index")) b.index = (uint64_t)p->i();
    if (const J* p = j.find("timestamp")) b.timestamp = p->i();
    if (const J* p = j.find("prev_hash")) b.prev_hash = p->s();
    if (const J* p = j.find("merkle_root")) b.merkle_root = p->s();
    if (const J* p = j.find("logs_root")) b.logs_root = p->s();
    if (const J* p = j.find("difficulty")) b.difficulty = (uint32_t)p->i();
    if (const J* p = j.find("nonce")) b.nonce = (uint64_t)p->i();
    if (const J* p = j.find("hash")) b.hash = p->s();
    if (const J* p = j.find("transactions")) {
        for (size_t i = 0; i < p->size(); ++i) b.txs.push_back(tx_from_json(p->at(i)));
    }
    if (const J* p = j.find("logs")) {
        for (size_t i = 0; i < p->size(); ++i) {
            const J& lj = p->at(i);
            vm::Log lg;
            if (const J* q = lj.find("address")) lg.address = q->s();
            if (const J* q = lj.find("data")) lg.data = q->s();
            if (const J* q = lj.find("tx_id")) lg.tx_id = q->s();
            if (const J* q = lj.find("tx_index")) lg.tx_index = (uint64_t)q->i();
            if (const J* q = lj.find("topics")) {
                for (size_t k = 0; k < q->size(); ++k) lg.topics.push_back(q->at(k).s());
            }
            b.logs.push_back(lg);
        }
    }
    return b;
}

// ---------------- Chain ----------------
Chain::Chain() : mining_active_(false), work_nonce_(0) { reset_to_genesis(); }

Chain::Chain(const Params& p) : params_(p), mining_active_(false), work_nonce_(0) { reset_to_genesis(); }

std::string Chain::path_of(const std::string& name) const {
    if (datadir_.empty()) return name;
    return datadir_ + "/" + name;
}

void Chain::reset_to_genesis() {
    blocks_.clear();
    state_ = ChainState();
    mempool_.clear();
    mining_active_ = false;
    Block g;
    g.index = 0;
    g.timestamp = params_.genesis_time;
    g.prev_hash = std::string(64, '0');
    g.difficulty = 0;
    g.nonce = 0;
    g.merkle_root = sha256_hex("");
    g.logs_root = sha256_hex("");
    g.hash = block_compute_hash(g);
    blocks_.push_back(g);
}

uint64_t Chain::subsidy(uint64_t height) const {
    uint32_t halvings = (uint32_t)(height / params_.halving_interval);
    if (halvings >= 64) return 0;
    return params_.initial_reward >> halvings;
}

uint32_t Chain::expected_difficulty(const std::vector<Block>& chain) const {
    size_t n = chain.size();
    if (n <= (size_t)params_.retarget_interval) return params_.initial_difficulty;
    if ((n - 1) % (size_t)params_.retarget_interval != 0) return chain.back().difficulty;
    const Block& last = chain.back();
    const Block& first = chain[n - 1 - (size_t)params_.retarget_interval];
    if (first.index == 0) return last.difficulty;
    int64_t elapsed = last.timestamp - first.timestamp;
    int64_t avg = elapsed / params_.retarget_interval;
    int32_t d = (int32_t)last.difficulty;
    if (avg < params_.target_block_ms / 2) d += 1;
    else if (avg > params_.target_block_ms * 2) d -= 1;
    if (d < (int32_t)params_.min_difficulty) d = (int32_t)params_.min_difficulty;
    if (d > (int32_t)params_.max_difficulty) d = (int32_t)params_.max_difficulty;
    return (uint32_t)d;
}

uint64_t Chain::total_work(const std::vector<Block>& chain) const {
    uint64_t w = 0;
    for (size_t i = 0; i < chain.size(); ++i) w += (1ULL << (chain[i].difficulty > 40 ? 40 : chain[i].difficulty));
    return w;
}

std::string Chain::derive_contract_address(const std::string& from, uint64_t nonce) const {
    return "0x" + sha256_hex("create|" + u64s(params_.chain_id) + "|" + from + "|" + u64s(nonce)).substr(0, 40);
}

// ---- 合约执行 ----
bool Chain::exec_contract(ChainState& st, Tx& tx, std::string& err) const {
    std::map<std::string, Contract>::iterator it = st.contracts.find(tx.to);
    if (it == st.contracts.end()) { err = "合约不存在: " + tx.to; return false; }
    vm::Context ctx;
    ctx.address = tx.to;
    ctx.caller = tx.from;
    ctx.value = 0;
    ctx.calldata = tx.input;
    ctx.gas_limit = tx.gas_limit ? tx.gas_limit : params_.default_gas_limit;
    ctx.chain_id = u64s(params_.chain_id);

    vm::Result r = vm::execute(it->second.code, ctx, it->second.storage);
    if (!r.success) { err = "合约执行失败: " + r.error; return false; }
    tx.gas_used = r.gas_used;
    it->second.storage = r.storage;
    for (size_t i = 0; i < r.logs.size(); ++i) {
        r.logs[i].tx_id = tx.id;
        st.logs.push_back(r.logs[i]);
    }
    return true;
}

// ---- 跨链托管桥（原生实现） ----
static std::string bridge_topic(const char* sig) { return sha256_hex(sig); }

bool Chain::exec_bridge(ChainState& st, Tx& tx, std::string& err) const {
    std::string data = tx.input;
    if (data.size() >= 2 && data[0] == '0' && (data[1] == 'x' || data[1] == 'X')) data = data.substr(2);
    if (data.size() < 8) { err = "桥调用缺少选择器"; return false; }
    std::string sel = data.substr(0, 8);
    std::vector<vm::Word> w = vm::abi::decode_words(data.substr(8));

    const std::string SEL_COMMIT = vm::abi::selector("commitRoot(uint256,uint256,bytes32)");
    const std::string SEL_CLAIM =
        vm::abi::selector("claim(uint256,uint256,uint256,address,uint256,bytes32,bytes32[])");

    if (sel == SEL_COMMIT) {
        if (w.size() < 3) { err = "commitRoot 参数不足"; return false; }
        if (!params_.relayer.empty() && tx.from != params_.relayer) {
            err = "仅中继地址可提交跨链根";
            return false;
        }
        std::string src = vm::wdec(w[0]);
        uint64_t h = (w[1].v[1] || w[1].v[2] || w[1].v[3]) ? 0 : w[1].v[0];
        if (src == u64s(params_.chain_id)) { err = "不能提交本链的根"; return false; }
        std::map<std::string, uint64_t>::iterator ph = st.bridge.relay_height.find(src);
        if (ph != st.bridge.relay_height.end() && h <= ph->second) {
            err = "跨链根高度必须递增";
            return false;
        }
        st.bridge.relay_root[src] = vm::whex(w[2]);
        st.bridge.relay_height[src] = h;
        return true;
    }

    if (sel == SEL_CLAIM) {
        if (w.size() < 7) { err = "claim 参数不足"; return false; }
        std::string src = vm::wdec(w[0]);
        uint64_t seq = w[1].v[0];
        uint64_t index = w[2].v[0];
        std::string to = "0x" + vm::whex(w[3]).substr(24);
        uint64_t amount = w[4].v[0];
        std::string root = vm::whex(w[5]);
        uint64_t proof_len = w[6].v[0];
        if (src == u64s(params_.chain_id)) { err = "不能认领本链资产"; return false; }
        if (!is_address(to)) { err = "收款地址非法"; return false; }
        if (amount == 0) { err = "认领金额必须大于 0"; return false; }
        if (w.size() < 7 + proof_len) { err = "证明长度不足"; return false; }

        std::map<std::string, std::string>::iterator it = st.bridge.relay_root.find(src);
        if (it == st.bridge.relay_root.end()) { err = "该源链尚未提交跨链根"; return false; }
        if (it->second != root) { err = "跨链根与已提交根不一致"; return false; }

        std::string leaf = sha256_hex("cross|" + src + "|" + u64s(seq) + "|" + to + "|" + u64s(amount));
        std::vector<std::string> proof;
        for (size_t i = 0; i < proof_len; ++i) proof.push_back(vm::whex(w[7 + i]));
        if (!vm::merkle_verify(leaf, (size_t)index, proof, root)) { err = "默克尔证明校验失败"; return false; }

        std::string key = src + ":" + u64s(seq);
        if (st.bridge.processed.count(key)) { err = "该跨链序列号已被认领（防重放）"; return false; }

        st.bridge.processed.insert(key);
        st.bridge.minted += amount;
        st.accounts[to].balance += amount;

        vm::Log lg;
        lg.address = BRIDGE_ADDRESS;
        lg.topics.push_back(bridge_topic("CrossIn(uint256,uint256)"));
        lg.topics.push_back(vm::whex(w[0]));
        lg.topics.push_back(vm::whex(w[1]));
        lg.data = vm::whex(w[3]) + vm::whex(w[4]);
        lg.tx_id = tx.id;
        st.logs.push_back(lg);
        return true;
    }

    err = "未知的桥方法";
    return false;
}

// ---- 单笔交易校验与执行 ----
bool Chain::check_and_apply_tx(ChainState& st, Tx& tx, std::string& err) const {
    if (tx.type == "coinbase") {
        if (!is_address(tx.to)) { err = "出块奖励地址非法"; return false; }
        if (tx.amount == 0) { err = "出块奖励为 0"; return false; }
        st.accounts[tx.to].balance += tx.amount;
        return true;
    }

    bool known = tx.type == "transfer" || tx.type == "deploy_contract" || tx.type == "contract_call" ||
                 tx.type == "cross_transfer";
    if (!known) { err = "未知交易类型: " + tx.type; return false; }
    if (!is_address(tx.from)) { err = "发送方地址非法"; return false; }
    if (tx.id != tx_compute_id(tx, params_.chain_id)) { err = "交易哈希与内容不匹配"; return false; }
    if (tx.pubkey.size() != 128 || !is_hex_str(tx.pubkey, 128)) { err = "公钥格式非法"; return false; }
    if (ecc::address_of(tx.pubkey) != tx.from) { err = "公钥与发送方地址不匹配"; return false; }
    if (!ecc::verify(tx.id, tx.signature, tx.pubkey)) { err = "签名验证失败"; return false; }

    Account& a = st.accounts[tx.from];
    if (tx.nonce != a.nonce) {
        err = "nonce 不匹配（期望 " + u64s(a.nonce) + "，收到 " + u64s(tx.nonce) + "）";
        return false;
    }

    if (tx.type == "transfer") {
        if (!is_address(tx.to)) { err = "收款地址非法"; return false; }
        if (tx.amount == 0) { err = "转账金额必须大于 0"; return false; }
    } else if (tx.type == "deploy_contract") {
        if (!tx.to.empty()) { err = "部署交易不应带收款地址"; return false; }
        if (tx.amount != 0) { err = "部署交易金额必须为 0"; return false; }
        if (tx.code.empty()) { err = "缺少合约字节码"; return false; }
        if (tx.code.size() / 2 > params_.max_code_size) { err = "合约字节码超限"; return false; }
        if (tx.gas_limit < 21000) { err = "gas_limit 过低（至少 21000）"; return false; }
    } else if (tx.type == "contract_call") {
        if (!is_address(tx.to)) { err = "合约地址非法"; return false; }
        if (tx.amount != 0) { err = "合约调用金额必须为 0（本版本不支持附带金额）"; return false; }
        if (tx.input.size() < 8) { err = "calldata 缺少函数选择器"; return false; }
        if (tx.gas_limit < 21000) { err = "gas_limit 过低（至少 21000）"; return false; }
    } else if (tx.type == "cross_transfer") {
        if (!is_address(tx.to)) { err = "目标地址非法"; return false; }
        if (tx.amount == 0) { err = "跨链金额必须大于 0"; return false; }
        if (tx.target_chain_id == 0) { err = "缺少目标链 ID"; return false; }
        if (tx.target_chain_id == params_.chain_id) { err = "不能跨链到本链"; return false; }
    }

    uint64_t need = tx.amount + tx.fee;
    if (need < tx.amount) { err = "金额溢出"; return false; }
    if (a.balance < need) {
        err = "余额不足（可用 " + amount_to_str(a.balance) + "，需要 " + amount_to_str(need) + "）";
        return false;
    }
    a.balance -= need;
    a.nonce += 1;

    if (tx.type == "transfer") {
        st.accounts[tx.to].balance += tx.amount;
        return true;
    }
    if (tx.type == "deploy_contract") {
        std::string addr = derive_contract_address(tx.from, tx.nonce);
        if (st.contracts.count(addr)) { err = "合约地址已被占用"; return false; }
        vm::Context ctx;
        ctx.address = addr;
        ctx.caller = tx.from;
        ctx.value = 0;
        ctx.calldata = "";
        ctx.gas_limit = tx.gas_limit;
        ctx.chain_id = u64s(params_.chain_id);
        std::map<std::string, std::string> empty;
        vm::Result r = vm::execute(tx.code, ctx, empty);
        if (!r.success) { err = "构造函数执行失败: " + r.error; return false; }
        tx.gas_used = r.gas_used;
        Contract c;
        // 单代码模型：tx.code 既是初始化代码也是运行时代码，
        // 部署时以空 calldata 执行一次完成初始化（返回值忽略）。
        c.code = tx.code;
        c.storage = r.storage;
        c.created_at = tx.timestamp;
        c.creator = tx.from;
        st.contracts[addr] = c;
        for (size_t i = 0; i < r.logs.size(); ++i) {
            r.logs[i].tx_id = tx.id;
            st.logs.push_back(r.logs[i]);
        }
        return true;
    }
    if (tx.type == "contract_call") {
        if (tx.to == std::string(BRIDGE_ADDRESS)) return exec_bridge(st, tx, err);
        return exec_contract(st, tx, err);
    }

    // cross_transfer：锁定到托管地址，分配全局序列号
    st.accounts[BRIDGE_ADDRESS].balance += tx.amount;
    st.bridge.locked += tx.amount;
    uint64_t seq = st.bridge.seq + 1;
    st.bridge.seq = seq;

    vm::Log lg;
    lg.address = BRIDGE_ADDRESS;
    lg.topics.push_back(bridge_topic("CrossOut(uint256,uint256,address,uint256,uint256)"));
    lg.topics.push_back(vm::hex_u64(params_.chain_id));
    lg.topics.push_back(vm::hex_u64(seq));
    lg.data = vm::whex(vm::wfrom_hex(tx.from)) + vm::whex(vm::wfrom_hex(tx.to)) +
              vm::whex(vm::wu64(tx.amount)) + vm::whex(vm::wu64(tx.target_chain_id));
    lg.tx_id = tx.id;
    st.logs.push_back(lg);
    return true;
}

bool Chain::validate_block_against(const Block& b, const Block& prev, ChainState& st, std::string& err) const {
    if (b.index != prev.index + 1) { err = "区块高度不连续"; return false; }
    if (b.prev_hash != prev.hash) { err = "前哈希不匹配"; return false; }
    if (b.hash != block_compute_hash(b)) { err = "区块哈希与内容不匹配"; return false; }
    if (b.timestamp <= prev.timestamp) { err = "时间戳未递增"; return false; }
    if (b.txs.empty()) { err = "区块无交易"; return false; }
    if (b.txs.size() > params_.max_block_txs) { err = "区块交易数超限"; return false; }
    if (b.merkle_root != merkle_root_of(b.txs)) { err = "Merkle 根不匹配"; return false; }

    std::string zeros((size_t)b.difficulty, '0');
    if (b.difficulty > 64 || b.hash.compare(0, zeros.size(), zeros) != 0) { err = "工作量证明无效"; return false; }

    const Tx& cb = b.txs[0];
    if (cb.type != "coinbase") { err = "首笔交易必须是出块奖励"; return false; }
    if (cb.id != tx_coinbase_id(b.index, cb.to, cb.amount, cb.timestamp)) { err = "出块奖励哈希非法"; return false; }

    st.logs.clear();
    uint64_t fees = 0;
    std::vector<std::string> seen;
    for (size_t i = 1; i < b.txs.size(); ++i) {
        Tx tx = b.txs[i];
        for (size_t k = 0; k < seen.size(); ++k)
            if (seen[k] == tx.id) { err = "区块内交易重复"; return false; }
        seen.push_back(tx.id);
        size_t logs_before = st.logs.size();
        if (!check_and_apply_tx(st, tx, err)) return false;
        for (size_t k = logs_before; k < st.logs.size(); ++k) st.logs[k].tx_index = i;
        fees += tx.fee;
    }
    uint64_t expect = subsidy(b.index) + fees;
    if (cb.amount != expect) {
        err = "出块奖励金额错误（应为 " + amount_to_str(expect) + "，实为 " + amount_to_str(cb.amount) + "）";
        return false;
    }
    st.accounts[cb.to].balance += cb.amount;

    if (st.logs.size() != b.logs.size()) { err = "事件日志数量不一致"; return false; }
    for (size_t i = 0; i < st.logs.size(); ++i) {
        if (vm::log_serialize(st.logs[i]) != vm::log_serialize(b.logs[i])) {
            err = "事件日志内容不一致";
            return false;
        }
        st.logs[i].tx_id = b.logs[i].tx_id;
        st.logs[i].tx_index = b.logs[i].tx_index;
    }
    if (b.logs_root != vm::logs_merkle_root(b.logs)) { err = "事件日志 Merkle 根不匹配"; return false; }
    return true;
}

void Chain::append_block(const Block& b, bool persist, bool apply_state) {
    (void)apply_state; // v0.2 起状态统一在校验/打包阶段应用
    blocks_.push_back(b);
    for (size_t i = 0; i < b.txs.size(); ++i) mempool_.erase(b.txs[i].id);
    mining_active_ = false;
    if (persist) {
        std::string line = block_to_json(b, true).dump() + "\n";
        FILE* f = bc_fopen(path_of("blocks.ndjson"), "ab");
        if (f) {
            fwrite(line.data(), 1, line.size(), f);
            fflush(f);
            fclose(f);
        }
    }
    prune_mempool();
    save_mempool();
}

bool Chain::add_block(const Block& b, std::string& err, bool& need_sync) {
    need_sync = false;
    if (b.index == 0) { err = "忽略创世块"; return false; }
    if (b.index <= height()) {
        if (b.hash == tip().hash) { err = "区块已存在"; return false; }
        need_sync = true;
        err = "链高度不足或分叉";
        return false;
    }
    if (b.index != height() + 1) { need_sync = true; err = "缺少前置区块"; return false; }
    if (b.prev_hash != tip().hash) { need_sync = true; err = "前哈希不匹配（分叉），需要同步"; return false; }
    if (b.difficulty != expected_difficulty(blocks_)) {
        err = "难度与共识规则不符（期望 " + u64s(expected_difficulty(blocks_)) + "）";
        return false;
    }
    if (b.timestamp > (int64_t)time(0) * 1000 + params_.max_future_ms) { err = "时间戳超前"; return false; }

    ChainState st = state_;
    if (!validate_block_against(b, tip(), st, err)) return false;
    state_ = st;
    append_block(b, true, false);
    return true;
}

bool Chain::validate_chain(std::string& err) const {
    if (blocks_.empty()) { err = "空链"; return false; }
    const Block& g = blocks_[0];
    if (g.index != 0 || g.prev_hash != std::string(64, '0')) { err = "创世块非法"; return false; }
    if (g.hash != block_compute_hash(g)) { err = "创世块哈希非法"; return false; }
    ChainState st;
    for (size_t i = 1; i < blocks_.size(); ++i) {
        std::vector<Block> prefix(blocks_.begin(), blocks_.begin() + i);
        if (blocks_[i].difficulty != expected_difficulty(prefix)) {
            err = "第 " + u64s(blocks_[i].index) + " 块难度非法";
            return false;
        }
        if (!validate_block_against(blocks_[i], blocks_[i - 1], st, err)) {
            err = "第 " + u64s(blocks_[i].index) + " 块非法: " + err;
            return false;
        }
    }
    return true;
}

bool Chain::replace_chain(const std::vector<Block>& nc, std::string& err) {
    if (nc.size() <= blocks_.size()) { err = "链更短，忽略"; return false; }
    if (nc.empty()) { err = "空链"; return false; }
    if (nc[0].hash != blocks_[0].hash) { err = "创世块不一致"; return false; }
    if (total_work(nc) <= total_work(blocks_)) { err = "累计工作量不足，忽略"; return false; }

    ChainState st;
    for (size_t i = 1; i < nc.size(); ++i) {
        if (nc[i].hash != block_compute_hash(nc[i])) { err = "区块哈希非法"; return false; }
        std::vector<Block> prefix(nc.begin(), nc.begin() + i);
        if (nc[i].difficulty != expected_difficulty(prefix)) { err = "难度非法"; return false; }
        if (!validate_block_against(nc[i], nc[i - 1], st, err)) {
            err = "第 " + u64s(nc[i].index) + " 块非法: " + err;
            return false;
        }
    }
    blocks_ = nc;
    state_ = st;
    mining_active_ = false;
    save_all();
    prune_mempool();
    save_mempool();
    return true;
}

void Chain::save_all() const {
    if (datadir_.empty()) return;
    ensure_dir(datadir_);
    std::string tmp = path_of("blocks.ndjson.tmp");
    FILE* f = bc_fopen(tmp, "wb");
    if (!f) return;
    for (size_t i = 0; i < blocks_.size(); ++i) {
        std::string line = block_to_json(blocks_[i], true).dump() + "\n";
        fwrite(line.data(), 1, line.size(), f);
    }
    fflush(f);
    fclose(f);
    bc_remove(path_of("blocks.ndjson"));
    bc_rename(tmp, path_of("blocks.ndjson"));
}

void Chain::save_mempool() const {
    if (datadir_.empty()) return;
    ensure_dir(datadir_);
    std::string tmp = path_of("mempool.ndjson.tmp");
    FILE* f = bc_fopen(tmp, "wb");
    if (!f) return;
    for (std::map<std::string, Tx>::const_iterator it = mempool_.begin(); it != mempool_.end(); ++it) {
        std::string line = tx_to_json(it->second).dump() + "\n";
        fwrite(line.data(), 1, line.size(), f);
    }
    fflush(f);
    fclose(f);
    bc_remove(path_of("mempool.ndjson"));
    bc_rename(tmp, path_of("mempool.ndjson"));
}

bool Chain::load(const std::string& datadir, std::string& err) {
    datadir_ = datadir;
    ensure_dir(datadir_);
    reset_to_genesis();

    FILE* in = bc_fopen(path_of("blocks.ndjson"), "rb");
    if (!in) {
        save_all();
        return true;
    }
    std::string line;
    std::vector<Block> loaded;
    size_t lineno = 0;
    int ch;
    while (true) {
        line.clear();
        while ((ch = fgetc(in)) != EOF && ch != '\n') line.push_back((char)ch);
        bool eof = (ch == EOF);
        if (line.empty() && eof) break;
        if (!line.empty()) {
            ++lineno;
            J j;
            std::string perr;
            if (!J::parse(line, j, perr)) {
                err = "第 " + u64s(lineno) + " 行解析失败: " + perr;
                break;
            }
            loaded.push_back(block_from_json(j));
        }
        if (eof) break;
    }
    fclose(in);

    if (loaded.empty()) {
        save_all();
        return true;
    }
    std::vector<Block> bak = blocks_;
    blocks_ = loaded;
    state_ = ChainState();
    std::string verr;
    bool ok = validate_chain(verr);
    if (!ok) {
        err = "本地链数据校验失败: " + verr + "，已备份为 blocks.ndjson.corrupt 并重置为创世块";
        blocks_ = bak;
        state_ = ChainState();
        reset_to_genesis();
        bc_remove(path_of("blocks.ndjson.corrupt"));
        bc_rename(path_of("blocks.ndjson"), path_of("blocks.ndjson.corrupt"));
        save_all();
        return false;
    }
    state_ = ChainState();
    for (size_t i = 1; i < blocks_.size(); ++i) {
        std::string e;
        validate_block_against(blocks_[i], blocks_[i - 1], state_, e);
    }
    state_.logs.clear();
    prune_mempool();

    FILE* min = bc_fopen(path_of("mempool.ndjson"), "rb");
    if (min) {
        std::string l;
        int c2;
        while (true) {
            l.clear();
            while ((c2 = fgetc(min)) != EOF && c2 != '\n') l.push_back((char)c2);
            bool eof2 = (c2 == EOF);
            if (!l.empty()) {
                J j;
                std::string pe;
                if (J::parse(l, j, pe)) {
                    Tx t = tx_from_json(j);
                    std::string e;
                    submit_tx(t, e);
                }
            }
            if (eof2) break;
        }
        fclose(min);
    }
    return true;
}

// ---------------- 交易池 ----------------
std::vector<Tx> Chain::mempool_list() const {
    std::vector<Tx> v;
    for (std::map<std::string, Tx>::const_iterator it = mempool_.begin(); it != mempool_.end(); ++it) v.push_back(it->second);
    std::sort(v.begin(), v.end(), [](const Tx& a, const Tx& b) {
        if (a.fee != b.fee) return a.fee > b.fee;
        return a.timestamp < b.timestamp;
    });
    return v;
}

bool Chain::submit_tx(const Tx& tx, std::string& err) {
    if (tx.type == "coinbase") { err = "不能提交出块奖励交易"; return false; }
    if (mempool_.count(tx.id)) { err = "交易已在池中"; return false; }

    ChainState st = state_;
    for (std::map<std::string, Tx>::const_iterator it = mempool_.begin(); it != mempool_.end(); ++it) {
        Tx prev = it->second;
        std::string e;
        check_and_apply_tx(st, prev, e);
    }
    Tx copy = tx;
    if (!check_and_apply_tx(st, copy, err)) return false;
    mempool_[tx.id] = tx;
    save_mempool();
    return true;
}

void Chain::prune_mempool() {
    std::vector<Tx> pool = mempool_list();
    std::sort(pool.begin(), pool.end(), [](const Tx& a, const Tx& b) {
        if (a.from != b.from) return a.from < b.from;
        return a.nonce < b.nonce;
    });
    ChainState st = state_;
    std::map<std::string, Tx> keep;
    for (size_t i = 0; i < pool.size(); ++i) {
        Tx t = pool[i];
        std::string e;
        if (check_and_apply_tx(st, t, e)) keep[pool[i].id] = pool[i];
    }
    mempool_ = keep;
}

// ---------------- 挖矿 ----------------
bool Chain::build_template(const std::string& miner, Block& out, std::string& err) {
    if (!is_address(miner)) { err = "矿工地址非法"; return false; }
    Block b;
    b.index = height() + 1;
    b.timestamp = (int64_t)time(0) * 1000;
    if (b.timestamp <= tip().timestamp) b.timestamp = tip().timestamp + 1;
    b.prev_hash = tip().hash;
    b.difficulty = expected_difficulty(blocks_);

    ChainState st = state_;
    st.logs.clear();
    std::vector<Tx> selected;
    uint64_t fees = 0;
    std::vector<Tx> pool = mempool_list();
    std::vector<bool> used(pool.size(), false);
    bool progress = true;
    while (progress && selected.size() + 1 < params_.max_block_txs) {
        progress = false;
        for (size_t i = 0; i < pool.size(); ++i) {
            if (used[i]) continue;
            if (selected.size() + 1 >= params_.max_block_txs) break;
            Tx tx = pool[i];
            size_t logs_before = st.logs.size();
            std::string e;
            if (!check_and_apply_tx(st, tx, e)) continue;
            for (size_t k = logs_before; k < st.logs.size(); ++k) st.logs[k].tx_index = selected.size() + 1;
            fees += tx.fee;
            selected.push_back(tx);
            used[i] = true;
            progress = true;
        }
    }

    Tx cb;
    cb.type = "coinbase";
    cb.to = miner;
    cb.amount = subsidy(b.index) + fees;
    cb.fee = 0;
    cb.nonce = 0;
    cb.timestamp = b.timestamp;
    cb.id = tx_coinbase_id(b.index, cb.to, cb.amount, cb.timestamp);

    b.txs.push_back(cb);
    for (size_t i = 0; i < selected.size(); ++i) b.txs.push_back(selected[i]);
    b.merkle_root = merkle_root_of(b.txs);
    b.logs = st.logs;
    b.logs_root = vm::logs_merkle_root(b.logs);
    b.nonce = 0;
    b.hash = "";
    out = b;
    tmpl_state_ = st;
    tmpl_state_.accounts[miner].balance += cb.amount;
    return true;
}

bool Chain::mine_step(const std::string& miner, uint32_t iters, Block& found_block, bool& found, std::string& err,
                      uint64_t* hashes_done) {
    found = false;
    if (hashes_done) *hashes_done = 0;
    if (!mining_active_ || tmpl_prev_ != tip().hash) {
        if (!build_template(miner, tmpl_, err)) return false;
        work_nonce_ = 0;
        mining_active_ = true;
        tmpl_prev_ = tip().hash;
    }
    const Block& t = tmpl_;
    std::string base = u64s(t.index) + "|" + t.prev_hash + "|" + t.merkle_root + "|" + t.logs_root + "|" +
                       i64s(t.timestamp) + "|" + u64s(t.difficulty) + "|";
    const size_t blen = base.size();
    uint8_t buf[512];
    if (blen > 400) { err = "模板过长"; return false; }
    memcpy(buf, base.data(), blen);

    const uint32_t d = t.difficulty;
    const uint32_t full_bytes = d / 2;
    const bool half = (d % 2) != 0;

    for (uint32_t i = 0; i < iters; ++i) {
        uint64_t n = work_nonce_;
        char nb[24];
        int nl = 0;
        do { nb[nl++] = (char)('0' + (int)(n % 10)); n /= 10; } while (n);
        for (int k = 0; k < nl; ++k) buf[blen + k] = (uint8_t)nb[nl - 1 - k];
        uint8_t h[32];
        sha256(buf, blen + (size_t)nl, h);

        bool ok = true;
        for (uint32_t k = 0; k < full_bytes; ++k)
            if (h[k] != 0) { ok = false; break; }
        if (ok && half && (h[full_bytes] & 0xF0) != 0) ok = false;
        if (ok) {
            tmpl_.nonce = work_nonce_;
            tmpl_.hash = to_hex(h, 32);
            found_block = tmpl_;
            mining_active_ = false;
            state_ = tmpl_state_;
            append_block(tmpl_, true, false);
            found = true;
            if (hashes_done) *hashes_done = (uint64_t)i + 1;
            return true;
        }
        work_nonce_++;
    }
    if (hashes_done) *hashes_done = iters;
    return true;
}

// ---------------- 查询 ----------------
bool Chain::account(const std::string& addr, Account& acc) const {
    std::map<std::string, Account>::const_iterator it = state_.accounts.find(addr);
    if (it == state_.accounts.end()) { acc.balance = 0; acc.nonce = 0; return true; }
    acc = it->second;
    return true;
}

bool Chain::contract_lookup(const std::string& addr, Contract& c) const {
    std::map<std::string, Contract>::const_iterator it = state_.contracts.find(addr);
    if (it == state_.contracts.end()) return false;
    c = it->second;
    return true;
}

bool Chain::tx_lookup(const std::string& id, Tx& tx, uint64_t& block_index) const {
    std::map<std::string, Tx>::const_iterator it = mempool_.find(id);
    if (it != mempool_.end()) { tx = it->second; block_index = (uint64_t)-1; return true; }
    for (size_t i = blocks_.size(); i > 0; --i) {
        const Block& b = blocks_[i - 1];
        for (size_t k = 0; k < b.txs.size(); ++k)
            if (b.txs[k].id == id) { tx = b.txs[k]; block_index = b.index; return true; }
    }
    return false;
}

J Chain::chain_json(size_t from, size_t limit, bool with_txs) const {
    J arr = J::arr();
    size_t total = blocks_.size();
    if (from >= total) return arr;
    size_t end = from + limit;
    if (end > total) end = total;
    for (size_t i = from; i < end; ++i) arr.push(block_to_json(blocks_[i], with_txs));
    return arr;
}

J Chain::logs_json(uint64_t from_block, size_t limit, const std::string& address, const std::string& topic) const {
    J arr = J::arr();
    size_t n = 0;
    for (size_t i = 0; i < blocks_.size() && n < limit; ++i) {
        if (blocks_[i].index < from_block) continue;
        for (size_t k = 0; k < blocks_[i].logs.size() && n < limit; ++k) {
            const vm::Log& l = blocks_[i].logs[k];
            if (!address.empty() && l.address != address) continue;
            if (!topic.empty()) {
                bool hit = false;
                for (size_t t = 0; t < l.topics.size(); ++t)
                    if (l.topics[t] == topic) { hit = true; break; }
                if (!hit) continue;
            }
            J j = log_to_json(l, "", 0);
            j.set("block_index", J::num((int64_t)blocks_[i].index));
            j.set("block_hash", J::str(blocks_[i].hash));
            j.set("block_time", J::num(blocks_[i].timestamp));
            arr.push(j);
            ++n;
        }
    }
    return arr;
}

J Chain::contracts_json() const {
    J arr = J::arr();
    for (std::map<std::string, Contract>::const_iterator it = state_.contracts.begin(); it != state_.contracts.end(); ++it) {
        J j = J::obj();
        j.set("address", J::str(it->first));
        j.set("creator", J::str(it->second.creator));
        j.set("created_at", J::num((int64_t)it->second.created_at));
        j.set("code_size", J::num((int64_t)(it->second.code.size() / 2)));
        j.set("storage_slots", J::num((int64_t)it->second.storage.size()));
        arr.push(j);
    }
    return arr;
}

J Chain::bridge_json() const {
    J j = J::obj();
    j.set("address", J::str(BRIDGE_ADDRESS));
    j.set("chain_id", J::num((int64_t)params_.chain_id));
    j.set("seq", J::num((int64_t)state_.bridge.seq));
    j.set("locked", J::num((int64_t)state_.bridge.locked));
    j.set("locked_str", J::str(amount_to_str(state_.bridge.locked)));
    j.set("minted", J::num((int64_t)state_.bridge.minted));
    j.set("minted_str", J::str(amount_to_str(state_.bridge.minted)));
    j.set("processed_count", J::num((int64_t)state_.bridge.processed.size()));
    j.set("relayer", J::str(params_.relayer));
    J roots = J::obj();
    for (std::map<std::string, std::string>::const_iterator it = state_.bridge.relay_root.begin();
         it != state_.bridge.relay_root.end(); ++it) {
        J r = J::obj();
        r.set("root", J::str(it->second));
        std::map<std::string, uint64_t>::const_iterator h = state_.bridge.relay_height.find(it->first);
        r.set("height", J::num(h == state_.bridge.relay_height.end() ? 0 : (int64_t)h->second));
        roots.set(it->first, r);
    }
    j.set("relay_roots", roots);
    Account esc;
    std::map<std::string, Account>::const_iterator it = state_.accounts.find(BRIDGE_ADDRESS);
    if (it != state_.accounts.end()) esc = it->second;
    j.set("escrow_balance", J::num((int64_t)esc.balance));
    j.set("escrow_balance_str", J::str(amount_to_str(esc.balance)));
    return j;
}

J Chain::status_json() const {
    J j = J::obj();
    j.set("version", J::num((int64_t)params_.version));
    j.set("chain_id", J::num((int64_t)params_.chain_id));
    j.set("height", J::num((int64_t)height()));
    j.set("tip_hash", J::str(tip().hash));
    j.set("difficulty", J::num((int64_t)tip().difficulty));
    j.set("next_difficulty", J::num((int64_t)expected_difficulty(blocks_)));
    j.set("mempool", J::num((int64_t)mempool_.size()));
    j.set("total_work", J::num((int64_t)total_work(blocks_)));
    j.set("accounts", J::num((int64_t)state_.accounts.size()));
    j.set("contracts", J::num((int64_t)state_.contracts.size()));
    uint64_t txs = 0, logs = 0;
    for (size_t i = 0; i < blocks_.size(); ++i) {
        txs += blocks_[i].txs.size();
        logs += blocks_[i].logs.size();
    }
    j.set("total_txs", J::num((int64_t)txs));
    j.set("total_logs", J::num((int64_t)logs));
    j.set("bridge_seq", J::num((int64_t)state_.bridge.seq));
    j.set("bridge_locked", J::str(amount_to_str(state_.bridge.locked)));
    j.set("genesis_hash", J::str(blocks_[0].hash));
    j.set("tip_time", J::num(tip().timestamp));
    j.set("target_block_ms", J::num(params_.target_block_ms));
    j.set("relayer", J::str(params_.relayer));
    return j;
}

} // namespace bc
