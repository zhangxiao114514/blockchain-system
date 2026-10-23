// chain.cpp — 共识状态机实现
#include "chain.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

#include "crypto.h"

#ifdef _WIN32
#include <direct.h>
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

// Windows 下 fopen 用 ANSI 码页解析路径，含中文的 UTF-8 路径会失败，
// 因此统一走宽字符 API。
#ifdef _WIN32
#include <windows.h>
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
    std::string s = in;
    // 去空格
    std::string t;
    for (size_t i = 0; i < s.size(); ++i)
        if (s[i] != ' ' && s[i] != '\t') t.push_back(s[i]);
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
std::string tx_compute_id(const Tx& tx) {
    std::string payload = tx.type + "|" + tx.from + "|" + tx.to + "|" + u64s(tx.amount) + "|" + u64s(tx.fee) +
                          "|" + u64s(tx.nonce) + "|" + i64s(tx.timestamp);
    return sha256_hex(payload);
}

std::string tx_coinbase_id(uint64_t index, const std::string& to, uint64_t amount, int64_t timestamp) {
    return sha256_hex("coinbase|" + u64s(index) + "|" + to + "|" + u64s(amount) + "|" + i64s(timestamp));
}

std::string merkle_root_of(const std::vector<Tx>& txs) {
    if (txs.empty()) return sha256_hex("");
    std::vector<std::string> level;
    level.reserve(txs.size());
    for (size_t i = 0; i < txs.size(); ++i) level.push_back(txs[i].id);
    while (level.size() > 1) {
        if (level.size() % 2) level.push_back(level.back());
        std::vector<std::string> next;
        next.reserve(level.size() / 2);
        for (size_t i = 0; i + 1 < level.size(); i += 2) {
            std::vector<uint8_t> a, b;
            if (!from_hex(level[i], a) || !from_hex(level[i + 1], b)) return "";
            a.insert(a.end(), b.begin(), b.end());
            uint8_t h[32];
            sha256(&a[0], a.size(), h);
            next.push_back(to_hex(h, 32));
        }
        level.swap(next);
    }
    return level[0];
}

std::string block_compute_hash(const Block& b) {
    std::string payload = u64s(b.index) + "|" + b.prev_hash + "|" + b.merkle_root + "|" + i64s(b.timestamp) +
                          "|" + u64s(b.difficulty) + "|" + u64s(b.nonce);
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
    return t;
}

J block_to_json(const Block& b, bool with_txs) {
    J j = J::obj();
    j.set("index", J::num((int64_t)b.index));
    j.set("timestamp", J::num(b.timestamp));
    j.set("prev_hash", J::str(b.prev_hash));
    j.set("merkle_root", J::str(b.merkle_root));
    j.set("difficulty", J::num((int64_t)b.difficulty));
    j.set("nonce", J::num((int64_t)b.nonce));
    j.set("hash", J::str(b.hash));
    j.set("tx_count", J::num((int64_t)b.txs.size()));
    if (with_txs) {
        J arr = J::arr();
        for (size_t i = 0; i < b.txs.size(); ++i) arr.push(tx_to_json(b.txs[i]));
        j.set("transactions", arr);
    }
    return j;
}

Block block_from_json(const J& j) {
    Block b;
    if (const J* p = j.find("index")) b.index = (uint64_t)p->i();
    if (const J* p = j.find("timestamp")) b.timestamp = p->i();
    if (const J* p = j.find("prev_hash")) b.prev_hash = p->s();
    if (const J* p = j.find("merkle_root")) b.merkle_root = p->s();
    if (const J* p = j.find("difficulty")) b.difficulty = (uint32_t)p->i();
    if (const J* p = j.find("nonce")) b.nonce = (uint64_t)p->i();
    if (const J* p = j.find("hash")) b.hash = p->s();
    if (const J* p = j.find("transactions")) {
        for (size_t i = 0; i < p->size(); ++i) b.txs.push_back(tx_from_json(p->at(i)));
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
    state_.clear();
    mempool_.clear();
    mining_active_ = false;
    Block g;
    g.index = 0;
    g.timestamp = params_.genesis_time;
    g.prev_hash = std::string(64, '0');
    g.difficulty = 0;
    g.nonce = 0;
    g.merkle_root = sha256_hex("");
    g.hash = block_compute_hash(g);
    blocks_.push_back(g);
}

uint64_t Chain::subsidy(uint64_t height) const {
    uint32_t halvings = (uint32_t)(height / params_.halving_interval);
    if (halvings >= 64) return 0;
    uint64_t s = params_.initial_reward >> halvings;
    return s;
}

uint32_t Chain::expected_difficulty(const std::vector<Block>& chain) const {
    size_t n = chain.size(); // 下一个区块的 index
    if (n <= (size_t)params_.retarget_interval) return params_.initial_difficulty;
    // 每 retarget_interval 个区块调整一次（在第 interval+1 块生效）
    if ((n - 1) % (size_t)params_.retarget_interval != 0) return chain.back().difficulty;
    const Block& last = chain.back();
    const Block& first = chain[n - 1 - (size_t)params_.retarget_interval];
    // 调整窗口包含创世块时其时间戳是固定值，不参与计时，避免误判为超长出块时间
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

void Chain::apply_tx(State& st, const Tx& tx) const {
    if (tx.type == "coinbase") {
        st[tx.to].balance += tx.amount;
    } else {
        Account& a = st[tx.from];
        a.balance -= (tx.amount + tx.fee);
        a.nonce += 1;
        st[tx.to].balance += tx.amount;
    }
}

bool Chain::check_and_apply_tx(State& st, const Tx& tx, std::string& err) const {
    if (tx.type == "coinbase") {
        if (!is_address(tx.to)) { err = "出块奖励地址非法"; return false; }
        if (tx.amount == 0) { err = "出块奖励为 0"; return false; }
        st[tx.to].balance += tx.amount;
        return true;
    }
    if (tx.type != "transfer") { err = "未知交易类型: " + tx.type; return false; }
    if (!is_address(tx.from) || !is_address(tx.to)) { err = "地址格式非法"; return false; }
    if (tx.amount == 0) { err = "转账金额必须大于 0"; return false; }
    if (tx.amount > 2100000000000000ULL) { err = "金额超出上限"; return false; }
    if (tx.id != tx_compute_id(tx)) { err = "交易哈希与内容不匹配"; return false; }
    if (tx.pubkey.size() != 128 || !is_hex_str(tx.pubkey, 128)) { err = "公钥格式非法"; return false; }
    if (ecc::address_of(tx.pubkey) != tx.from) { err = "公钥与发送方地址不匹配"; return false; }
    if (!ecc::verify(tx.id, tx.signature, tx.pubkey)) { err = "签名验证失败"; return false; }

    Account& a = st[tx.from];
    if (tx.nonce != a.nonce) {
        err = "nonce 不匹配（期望 " + u64s(a.nonce) + "，收到 " + u64s(tx.nonce) + "）";
        return false;
    }
    uint64_t need = tx.amount + tx.fee;
    if (need < tx.amount) { err = "金额溢出"; return false; }
    if (a.balance < need) {
        err = "余额不足（可用 " + amount_to_str(a.balance) + "，需要 " + amount_to_str(need) + "）";
        return false;
    }
    a.balance -= need;
    a.nonce += 1;
    st[tx.to].balance += tx.amount;
    return true;
}

bool Chain::validate_block_against(const Block& b, const Block& prev, State& st, std::string& err) const {
    if (b.index != prev.index + 1) { err = "区块高度不连续"; return false; }
    if (b.prev_hash != prev.hash) { err = "前哈希不匹配"; return false; }
    if (b.hash != block_compute_hash(b)) { err = "区块哈希与内容不匹配"; return false; }
    if (b.timestamp <= prev.timestamp) { err = "时间戳未递增"; return false; }
    if (b.txs.empty()) { err = "区块无交易"; return false; }
    if (b.txs.size() > params_.max_block_txs) { err = "区块交易数超限"; return false; }
    if (b.merkle_root != merkle_root_of(b.txs)) { err = "Merkle 根不匹配"; return false; }

    // PoW
    std::string zeros((size_t)b.difficulty, '0');
    if (b.difficulty > 64 || b.hash.compare(0, zeros.size(), zeros) != 0) { err = "工作量证明无效"; return false; }

    // coinbase 必须是第一笔
    const Tx& cb = b.txs[0];
    if (cb.type != "coinbase") { err = "首笔交易必须是出块奖励"; return false; }
    if (cb.id != tx_coinbase_id(b.index, cb.to, cb.amount, cb.timestamp)) { err = "出块奖励哈希非法"; return false; }

    uint64_t fees = 0;
    std::vector<std::string> seen;
    for (size_t i = 1; i < b.txs.size(); ++i) {
        const Tx& tx = b.txs[i];
        if (tx.type != "transfer") { err = "区块内存在非法类型交易"; return false; }
        for (size_t k = 0; k < seen.size(); ++k)
            if (seen[k] == tx.id) { err = "区块内交易重复"; return false; }
        seen.push_back(tx.id);
        if (!check_and_apply_tx(st, tx, err)) return false;
        fees += tx.fee;
    }
    uint64_t expect = subsidy(b.index) + fees;
    if (cb.amount != expect) {
        err = "出块奖励金额错误（应为 " + amount_to_str(expect) + "，实为 " + amount_to_str(cb.amount) + "）";
        return false;
    }
    st[cb.to].balance += cb.amount;
    return true;
}

void Chain::append_block(const Block& b, bool persist, bool apply_state) {
    blocks_.push_back(b);
    for (size_t i = 0; i < b.txs.size(); ++i) {
        // 若调用方已在校验阶段把交易应用进 state_，此处不得重复应用
        if (apply_state) apply_tx(state_, b.txs[i]);
        mempool_.erase(b.txs[i].id);
    }
    mining_active_ = false; // 顶端变化，重建模板
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
    remove(path_of("mempool.ndjson").c_str());
    rename(tmp.c_str(), path_of("mempool.ndjson").c_str());
}

bool Chain::add_block(const Block& b, std::string& err, bool& need_sync) {
    need_sync = false;
    if (b.index == 0) { err = "忽略创世块"; return false; }
    if (b.index <= height()) {
        if (b.hash == tip().hash) { err = "区块已存在"; return false; }
        // 同高度或更低：可能是分叉，请求全链
        need_sync = true;
        err = "链高度不足或分叉";
        return false;
    }
    if (b.index != height() + 1) {
        need_sync = true;
        err = "缺少前置区块";
        return false;
    }
    if (b.prev_hash != tip().hash) {
        // 收到的是另一条分叉上的区块：本链可能更短，请求对端完整链
        need_sync = true;
        err = "前哈希不匹配（分叉），需要同步";
        return false;
    }
    if (b.difficulty != expected_difficulty(blocks_)) {
        err = "难度与共识规则不符（期望 " + u64s(expected_difficulty(blocks_)) + "）";
        return false;
    }
    if (b.timestamp > (int64_t)time(0) * 1000 + params_.max_future_ms) { err = "时间戳超前"; return false; }

    State st = state_;
    if (!validate_block_against(b, tip(), st, err)) return false;
    state_ = st;                       // 校验时已应用交易
    append_block(b, true, false);
    return true;
}

bool Chain::validate_chain(std::string& err) const {
    if (blocks_.empty()) { err = "空链"; return false; }
    const Block& g = blocks_[0];
    if (g.index != 0 || g.prev_hash != std::string(64, '0')) { err = "创世块非法"; return false; }
    if (g.hash != block_compute_hash(g)) { err = "创世块哈希非法"; return false; }
    State st;
    for (size_t i = 1; i < blocks_.size(); ++i) {
        if (blocks_[i].difficulty != expected_difficulty(std::vector<Block>(blocks_.begin(), blocks_.begin() + i))) {
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

    State st;
    for (size_t i = 1; i < nc.size(); ++i) {
        if (nc[i].hash != block_compute_hash(nc[i])) { err = "区块哈希非法"; return false; }
        if (nc[i].difficulty != expected_difficulty(std::vector<Block>(nc.begin(), nc.begin() + i))) {
            err = "难度非法";
            return false;
        }
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

bool Chain::load(const std::string& datadir, std::string& err) {
    datadir_ = datadir;
    ensure_dir(datadir_);
    reset_to_genesis();

    FILE* in = bc_fopen(path_of("blocks.ndjson"), "rb");
    if (!in) {
        save_all();
        return true; // 新链
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
        ++lineno;
        if (line.empty()) continue;
        J j;
        std::string perr;
        if (!J::parse(line, j, perr)) {
            err = "第 " + u64s(lineno) + " 行解析失败: " + perr + "（已忽略该行之后的区块）";
            break;
        }
        loaded.push_back(block_from_json(j));
        if (eof) break;
    }
    fclose(in);

    if (loaded.empty()) {
        save_all();
        return true;
    }
    // 校验后采纳
    std::vector<Block> bak = blocks_;
    blocks_ = loaded;
    state_.clear();
    std::string verr;
    bool ok = validate_chain(verr);
    if (!ok) {
        err = "本地链数据校验失败: " + verr + "，已备份为 blocks.ndjson.corrupt 并重置为创世块";
        blocks_ = bak;
        state_.clear();
        reset_to_genesis();
        bc_remove(path_of("blocks.ndjson.corrupt"));
        bc_rename(path_of("blocks.ndjson"), path_of("blocks.ndjson.corrupt"));
        save_all();
        return false;
    }
    // 重建状态
    state_.clear();
    for (size_t i = 0; i < blocks_.size(); ++i)
        for (size_t k = 0; k < blocks_[i].txs.size(); ++k) apply_tx(state_, blocks_[i].txs[k]);
    prune_mempool();

    // 恢复交易池（逐笔重新校验，失败的丢弃）
    {
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
    if (tx.type != "transfer") { err = "只接受转账交易"; return false; }
    if (mempool_.count(tx.id)) { err = "交易已在池中"; return false; }
    if (!is_address(tx.from) || !is_address(tx.to)) { err = "地址格式非法"; return false; }
    if (tx.amount == 0) { err = "转账金额必须大于 0"; return false; }
    if (tx.id != tx_compute_id(tx)) { err = "交易哈希与内容不匹配"; return false; }
    if (ecc::address_of(tx.pubkey) != tx.from) { err = "公钥与发送方地址不匹配"; return false; }
    if (!ecc::verify(tx.id, tx.signature, tx.pubkey)) { err = "签名验证失败"; return false; }

    // 该地址在池中的待处理交易
    uint64_t pending_count = 0;
    uint64_t pending_out = 0;
    for (std::map<std::string, Tx>::const_iterator it = mempool_.begin(); it != mempool_.end(); ++it) {
        if (it->second.from == tx.from) {
            pending_count++;
            pending_out += it->second.amount + it->second.fee;
        }
    }
    Account acc;
    account(tx.from, acc);
    uint64_t expect_nonce = acc.nonce + pending_count;
    if (tx.nonce != expect_nonce) {
        err = "nonce 不匹配（期望 " + u64s(expect_nonce) + "，收到 " + u64s(tx.nonce) + "）";
        return false;
    }
    if (acc.balance < pending_out + tx.amount + tx.fee) {
        err = "余额不足（可用 " + amount_to_str(acc.balance - pending_out) + "）";
        return false;
    }
    mempool_[tx.id] = tx;
    save_mempool();
    return true;
}

void Chain::prune_mempool() {
    // 按 (发送方, nonce) 排序并模拟状态推进，剔除已失效或前序缺失的交易
    std::vector<Tx> pool = mempool_list();
    std::sort(pool.begin(), pool.end(), [](const Tx& a, const Tx& b) {
        if (a.from != b.from) return a.from < b.from;
        return a.nonce < b.nonce;
    });
    State st = state_;
    std::map<std::string, Tx> keep;
    for (size_t i = 0; i < pool.size(); ++i) {
        const Tx& tx = pool[i];
        Account& a = st[tx.from];
        if (tx.nonce < a.nonce) continue;  // 已上链
        if (tx.nonce > a.nonce) continue;  // 前序缺失，等客户端重发
        if (a.balance < tx.amount + tx.fee) continue;
        a.balance -= (tx.amount + tx.fee);
        a.nonce += 1;
        st[tx.to].balance += tx.amount;
        keep[tx.id] = tx;
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

    // 选交易：按手续费降序，多轮扫描以处理同一发送方的 nonce 顺序依赖
    State st = state_;
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
            const Tx& tx = pool[i];
            Account& a = st[tx.from];
            if (tx.nonce != a.nonce) continue;
            if (a.balance < tx.amount + tx.fee) continue;
            a.balance -= (tx.amount + tx.fee);
            a.nonce += 1;
            st[tx.to].balance += tx.amount;
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
    b.nonce = 0;
    b.hash = "";
    out = b;
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
    std::string base = u64s(t.index) + "|" + t.prev_hash + "|" + t.merkle_root + "|" + i64s(t.timestamp) + "|" +
                       u64s(t.difficulty) + "|";
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
        if (ok && half) {
            if ((h[full_bytes] & 0xF0) != 0) ok = false;
        }
        if (ok) {
            tmpl_.nonce = work_nonce_;
            tmpl_.hash = to_hex(h, 32);
            found_block = tmpl_;
            mining_active_ = false;
            append_block(tmpl_, true);
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
    State::const_iterator it = state_.find(addr);
    if (it == state_.end()) { acc.balance = 0; acc.nonce = 0; return true; }
    acc = it->second;
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

J Chain::status_json() const {
    J j = J::obj();
    j.set("height", J::num((int64_t)height()));
    j.set("tip_hash", J::str(tip().hash));
    j.set("difficulty", J::num((int64_t)tip().difficulty));
    j.set("next_difficulty", J::num((int64_t)expected_difficulty(blocks_)));
    j.set("mempool", J::num((int64_t)mempool_.size()));
    j.set("total_work", J::num((int64_t)total_work(blocks_)));
    j.set("accounts", J::num((int64_t)state_.size()));
    uint64_t txs = 0;
    for (size_t i = 0; i < blocks_.size(); ++i) txs += blocks_[i].txs.size();
    j.set("total_txs", J::num((int64_t)txs));
    j.set("genesis_hash", J::str(blocks_[0].hash));
    j.set("tip_time", J::num(tip().timestamp));
    j.set("target_block_ms", J::num(params_.target_block_ms));
    return j;
}

} // namespace bc
