// vm.cpp — 精简 EVM 子集实现（256 位字长、gas 计量、事件日志）+ ABI 编解码
#include "vm.h"

#include <algorithm>
#include <cstring>
#include <set>

#include "crypto.h"

namespace bc {
namespace vm {

typedef uint32_t u32;
typedef uint64_t u64;

// ============================================================
// 256 位字运算
// ============================================================
Word wu64(uint64_t x) {
    Word w;
    w.v[0] = x;
    w.v[1] = w.v[2] = w.v[3] = 0;
    return w;
}

bool wzero(const Word& w) { return (w.v[0] | w.v[1] | w.v[2] | w.v[3]) == 0; }

int wcmp(const Word& a, const Word& b) {
    for (int i = 3; i >= 0; --i)
        if (a.v[i] != b.v[i]) return a.v[i] < b.v[i] ? -1 : 1;
    return 0;
}

Word wadd(const Word& a, const Word& b) {
    Word r;
    u64 c = 0;
    for (int i = 0; i < 4; ++i) {
        u64 s = a.v[i] + b.v[i];
        u64 c1 = (s < a.v[i]) ? 1 : 0;
        u64 s2 = s + c;
        u64 c2 = (s2 < s) ? 1 : 0;
        r.v[i] = s2;
        c = c1 | c2;
    }
    return r;
}

Word wsub(const Word& a, const Word& b) {
    Word r;
    u64 borrow = 0;
    for (int i = 0; i < 4; ++i) {
        u64 d = a.v[i] - b.v[i];
        u64 b1 = (a.v[i] < b.v[i]) ? 1 : 0;
        u64 d2 = d - borrow;
        u64 b2 = (d < borrow) ? 1 : 0;
        r.v[i] = d2;
        borrow = b1 | b2;
    }
    return r;
}

// 8x32 位 limb 乘法（低 256 位）
Word wmul(const Word& a, const Word& b) {
    u32 A[8], B[8], T[16];
    for (int i = 0; i < 4; ++i) {
        A[i * 2] = (u32)(a.v[i] & 0xFFFFFFFFULL);
        A[i * 2 + 1] = (u32)(a.v[i] >> 32);
        B[i * 2] = (u32)(b.v[i] & 0xFFFFFFFFULL);
        B[i * 2 + 1] = (u32)(b.v[i] >> 32);
    }
    memset(T, 0, sizeof(T));
    for (int i = 0; i < 8; ++i) {
        u64 carry = 0;
        for (int j = 0; j < 8; ++j) {
            u64 cur = (u64)A[i] * B[j] + T[i + j] + carry;
            T[i + j] = (u32)cur;
            carry = cur >> 32;
        }
        T[i + 8] = (u32)carry;
    }
    Word r;
    for (int i = 0; i < 4; ++i) r.v[i] = (u64)T[i * 2] | ((u64)T[i * 2 + 1] << 32);
    return r;
}

static inline bool wbit(const Word& a, int i) { return (a.v[i >> 6] >> (i & 63)) & 1ULL; }
static inline void wsetbit(Word& a, int i) { a.v[i >> 6] |= (1ULL << (i & 63)); }

static void wdivmod(const Word& a, const Word& b, Word& q, Word& r) {
    if (wzero(b)) { q = wu64(0); r = wu64(0); return; }
    Word rem = wu64(0), quo = wu64(0);
    for (int i = 255; i >= 0; --i) {
        // rem = (rem << 1) | bit(a, i)
        Word shifted;
        shifted.v[0] = rem.v[0] << 1;
        shifted.v[1] = (rem.v[1] << 1) | (rem.v[0] >> 63);
        shifted.v[2] = (rem.v[2] << 1) | (rem.v[1] >> 63);
        shifted.v[3] = (rem.v[3] << 1) | (rem.v[2] >> 63);
        if (wbit(a, i)) shifted.v[0] |= 1ULL;
        rem = shifted;
        if (wcmp(rem, b) >= 0) {
            rem = wsub(rem, b);
            wsetbit(quo, i);
        }
    }
    q = quo;
    r = rem;
}

Word wdiv(const Word& a, const Word& b) {
    Word q, r;
    wdivmod(a, b, q, r);
    return q;
}

Word wmod(const Word& a, const Word& b) {
    Word q, r;
    wdivmod(a, b, q, r);
    return r;
}

Word wshl(const Word& a, uint32_t n) {
    if (n >= 256) return wu64(0);
    Word r = wu64(0);
    uint32_t limb = n / 64, bits = n % 64;
    for (int i = 3; i >= 0; --i) {
        int src = i - (int)limb;
        if (src < 0) continue;
        r.v[i] = a.v[src] << bits;
        if (bits && src - 1 >= 0) r.v[i] |= a.v[src - 1] >> (64 - bits);
    }
    return r;
}

Word wshr(const Word& a, uint32_t n) {
    if (n >= 256) return wu64(0);
    Word r = wu64(0);
    uint32_t limb = n / 64, bits = n % 64;
    for (int i = 0; i < 4; ++i) {
        int src = i + (int)limb;
        if (src > 3) continue;
        r.v[i] = a.v[src] >> bits;
        if (bits && src + 1 <= 3) r.v[i] |= a.v[src + 1] << (64 - bits);
    }
    return r;
}

Word wfrom_hex(const std::string& hex) {
    std::vector<uint8_t> b;
    std::string s = hex;
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s = s.substr(2);
    if (s.size() % 2) s = "0" + s;
    if (!from_hex(s, b)) return wu64(0);
    if (b.size() > 32) b.erase(b.begin(), b.end() - 32);
    Word w = wu64(0);
    // 右侧对齐（大端字节序）
    size_t pad = 32 - b.size();
    for (size_t i = 0; i < b.size(); ++i) {
        size_t pos = pad + i; // 0 = 最高字节
        int limb = 3 - (int)(pos / 8);
        int shift = 8 * (7 - (int)(pos % 8));
        w.v[limb] |= (uint64_t)b[i] << shift;
    }
    return w;
}

std::string whex(const Word& w) {
    uint8_t b[32];
    for (int i = 0; i < 4; ++i) {
        int base = 3 - i;
        for (int k = 0; k < 8; ++k) b[base * 8 + k] = (uint8_t)(w.v[i] >> (8 * (7 - k)));
    }
    return to_hex(b, 32);
}

std::string hex_u64(uint64_t v) { return whex(wu64(v)); }

std::string wdec(const Word& w) {
    if (wzero(w)) return "0";
    std::string out;
    Word cur = w, ten = wu64(10);
    while (!wzero(cur)) {
        Word q, r;
        wdivmod(cur, ten, q, r);
        out.push_back((char)('0' + (int)r.v[0]));
        cur = q;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

// ============================================================
// 事件日志
// ============================================================
std::string log_serialize(const Log& l) {
    std::string s = l.address;
    for (size_t i = 0; i < l.topics.size(); ++i) s += "|" + l.topics[i];
    s += "|" + l.data;
    return s;
}

std::string merkle_root_hex(const std::vector<std::string>& leaves_hex) {
    if (leaves_hex.empty()) return sha256_hex("");
    std::vector<std::string> level;
    level.reserve(leaves_hex.size());
    for (size_t i = 0; i < leaves_hex.size(); ++i) level.push_back(sha256_hex(leaves_hex[i]));
    while (level.size() > 1) {
        if (level.size() % 2) level.push_back(level.back());
        std::vector<std::string> next;
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

MerkleBuildResult merkle_build(const std::vector<std::string>& leaves_hex) {
    MerkleBuildResult out;
    if (leaves_hex.empty()) { out.root = sha256_hex(""); return out; }
    std::vector<std::vector<std::string> > levels;
    std::vector<std::string> level;
    level.reserve(leaves_hex.size());
    for (size_t i = 0; i < leaves_hex.size(); ++i) level.push_back(sha256_hex(leaves_hex[i]));
    levels.push_back(level);
    while (level.size() > 1) {
        if (level.size() % 2) level.push_back(level.back());
        std::vector<std::string> next;
        for (size_t i = 0; i + 1 < level.size(); i += 2) {
            std::vector<uint8_t> a, b;
            from_hex(level[i], a);
            from_hex(level[i + 1], b);
            a.insert(a.end(), b.begin(), b.end());
            uint8_t h[32];
            sha256(&a[0], a.size(), h);
            next.push_back(to_hex(h, 32));
        }
        levels.push_back(next);
        level = next;
    }
    out.root = levels.back()[0];
    out.proofs.resize(leaves_hex.size());
    for (size_t idx = 0; idx < leaves_hex.size(); ++idx) {
        size_t i = idx;
        for (size_t lv = 0; lv + 1 < levels.size(); ++lv) {
            const std::vector<std::string>& cur = levels[lv];
            size_t sib = (i % 2 == 0) ? (i + 1) : (i - 1);
            if (sib >= cur.size()) sib = i; // 奇数补尾：兄弟是自己
            out.proofs[idx].push_back(cur[sib]);
            i /= 2;
        }
    }
    return out;
}

bool merkle_verify(const std::string& leaf_hex, size_t index, const std::vector<std::string>& proof,
                   const std::string& root) {
    std::string h = sha256_hex(leaf_hex);
    size_t i = index;
    for (size_t k = 0; k < proof.size(); ++k) {
        std::vector<uint8_t> a, b;
        if (!from_hex(h, a) || !from_hex(proof[k], b)) return false;
        std::vector<uint8_t> cat;
        if (i % 2 == 0) {
            cat = a;
            cat.insert(cat.end(), b.begin(), b.end());
        } else {
            cat = b;
            cat.insert(cat.end(), a.begin(), a.end());
        }
        uint8_t out[32];
        sha256(&cat[0], cat.size(), out);
        h = to_hex(out, 32);
        i /= 2;
    }
    return h == root;
}

std::string logs_merkle_root(const std::vector<Log>& logs) {
    std::vector<std::string> leaves;
    leaves.reserve(logs.size());
    for (size_t i = 0; i < logs.size(); ++i) leaves.push_back(log_serialize(logs[i]));
    return merkle_root_hex(leaves);
}

// ============================================================
// ABI
// ============================================================
namespace abi {

std::string selector(const std::string& signature) { return sha256_hex(signature).substr(0, 8); }

bool encode_arg(const std::string& type, const std::string& value, std::string& out_hex, std::string& err) {
    if (type == "uint256") {
        // 十进制字符串
        Word w = wu64(0);
        std::string s = value;
        if (s.empty()) s = "0";
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] < '0' || s[i] > '9') { err = "uint256 需要十进制数字: " + value; return false; }
            w = wadd(wmul(w, wu64(10)), wu64((u64)(s[i] - '0')));
        }
        out_hex = whex(w);
        return true;
    }
    if (type == "address") {
        std::string s = value;
        if (s.size() != 42 || s[0] != '0' || (s[1] != 'x' && s[1] != 'X')) {
            err = "address 需要 0x + 40 hex: " + value;
            return false;
        }
        out_hex = std::string(24, '0') + s.substr(2);
        return true;
    }
    if (type == "bytes32") {
        std::string s = value;
        if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s = s.substr(2);
        if (s.size() > 64) { err = "bytes32 过长"; return false; }
        while (s.size() < 64) s = "0" + s;
        out_hex = s;
        return true;
    }
    err = "不支持的 ABI 类型: " + type;
    return false;
}

std::string encode_call(const std::string& signature, const std::vector<std::string>& types,
                        const std::vector<std::string>& values) {
    if (types.size() != values.size()) return "";
    std::string out = selector(signature);
    for (size_t i = 0; i < types.size(); ++i) {
        std::string h, err;
        if (!encode_arg(types[i], values[i], h, err)) return "";
        out += h;
    }
    return out;
}

std::vector<Word> decode_words(const std::string& hex) {
    std::vector<Word> out;
    std::string s = hex;
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s = s.substr(2);
    for (size_t i = 0; i + 64 <= s.size(); i += 64) out.push_back(wfrom_hex(s.substr(i, 64)));
    return out;
}

} // namespace abi

// ============================================================
// 执行引擎
// ============================================================
namespace {

const size_t MAX_STACK = 1024;
const size_t MAX_MEMORY = 16384;

struct Machine {
    std::vector<Word> stack;
    std::vector<uint8_t> mem;
    std::vector<uint8_t> code;
    std::vector<uint8_t> calldata;
    std::set<size_t> jumpdests;
    std::map<std::string, std::string> storage;
    std::vector<Log> logs;
    uint64_t gas_used;
    uint64_t gas_limit;
    size_t pc;
    const Context* ctx;

    bool charge(uint64_t g) {
        if (gas_used + g > gas_limit) return false;
        gas_used += g;
        return true;
    }
    bool pop(Word& w) {
        if (stack.empty()) return false;
        w = stack.back();
        stack.pop_back();
        return true;
    }
    bool push(const Word& w) {
        if (stack.size() >= MAX_STACK) return false;
        stack.push_back(w);
        return true;
    }
    bool mem_reserve(size_t off, size_t len, uint64_t& extra_gas) {
        size_t need = off + len;
        if (need > MAX_MEMORY) return false;
        if (need > mem.size()) {
            size_t words = (need + 31) / 32 - mem.size() / 32;
            extra_gas = 3 * (uint64_t)words;
            mem.resize(need, 0);
        } else {
            extra_gas = 0;
        }
        return true;
    }
    Word mem_load(size_t off) {
        Word w = wu64(0);
        uint8_t buf[32];
        memset(buf, 0, 32);
        for (size_t i = 0; i < 32; ++i)
            if (off + i < mem.size()) buf[i] = mem[off + i];
        std::vector<uint8_t> v(buf, buf + 32);
        w = wfrom_hex(to_hex(v));
        return w;
    }
    void mem_store(size_t off, const Word& w) {
        std::string h = whex(w);
        std::vector<uint8_t> b;
        from_hex(h, b);
        for (size_t i = 0; i < 32 && off + i < mem.size(); ++i) mem[off + i] = b[i];
    }
    std::string mem_slice(size_t off, size_t len) {
        std::vector<uint8_t> out;
        for (size_t i = 0; i < len; ++i) out.push_back(off + i < mem.size() ? mem[off + i] : 0);
        return to_hex(out);
    }
};

size_t word_to_size(const Word& w) {
    if (w.v[1] || w.v[2] || w.v[3]) return (size_t)-1;
    return (size_t)w.v[0];
}

} // namespace

Result execute(const std::string& code_hex, const Context& ctx,
               const std::map<std::string, std::string>& storage_in) {
    Result res;
    res.success = false;
    res.gas_used = 0;
    res.storage = storage_in;

    Machine m;
    m.gas_used = 0;
    m.gas_limit = ctx.gas_limit ? ctx.gas_limit : 200000;
    m.pc = 0;
    m.ctx = &ctx;
    m.storage = storage_in;

    std::string ch = code_hex;
    if (ch.size() >= 2 && ch[0] == '0' && (ch[1] == 'x' || ch[1] == 'X')) ch = ch.substr(2);
    if (!from_hex(ch, m.code)) { res.error = "字节码非法"; return res; }
    {
        std::string cd = ctx.calldata;
        if (cd.size() >= 2 && cd[0] == '0' && (cd[1] == 'x' || cd[1] == 'X')) cd = cd.substr(2);
        if (!from_hex(cd, m.calldata)) { res.error = "calldata 非法"; return res; }
    }
    // 预扫描合法跳转点
    for (size_t i = 0; i < m.code.size(); ++i) {
        uint8_t op = m.code[i];
        if (op == 0x5b) m.jumpdests.insert(i);
        if (op >= 0x60 && op <= 0x7f) i += (size_t)(op - 0x5f);
    }

    std::string ret_hex;
    bool reverted = false;
    std::string err;

    while (m.pc < m.code.size()) {
        uint8_t op = m.code[m.pc];
        size_t at = m.pc;
        m.pc++;

        // ---- 基础 gas ----
        uint64_t base_gas = 3;
        if (op == 0x54) base_gas = 100;      // SLOAD
        else if (op == 0x55) base_gas = 200; // SSTORE
        else if (op == 0x56) base_gas = 8;   // JUMP
        else if (op == 0x57) base_gas = 10;  // JUMPI
        else if (op >= 0xa0 && op <= 0xa2) base_gas = 375;
        else if (op == 0xf3 || op == 0xfd) base_gas = 0;
        if (!m.charge(base_gas)) { err = "gas 不足"; break; }

        if (op == 0x00) break; // STOP
        if (op == 0x01 || op == 0x02 || op == 0x03 || op == 0x04 || op == 0x05) {
            Word a, b;
            if (!m.pop(a) || !m.pop(b)) { err = "栈下溢"; break; }
            Word r;
            if (op == 0x01) r = wadd(a, b);
            else if (op == 0x02) r = wmul(a, b);
            else if (op == 0x03) r = wsub(a, b);
            else if (op == 0x04) r = wdiv(a, b);
            else r = wmod(a, b);
            if (!m.push(r)) { err = "栈上溢"; break; }
            continue;
        }
        if (op >= 0x10 && op <= 0x13) {
            Word a, b;
            if (op == 0x13) {
                if (!m.pop(a)) { err = "栈下溢"; break; }
                if (!m.push(wu64(wzero(a) ? 1 : 0))) { err = "栈上溢"; break; }
                continue;
            }
            if (!m.pop(a) || !m.pop(b)) { err = "栈下溢"; break; }
            int c = wcmp(a, b);
            bool v = false;
            if (op == 0x10) v = c < 0;
            else if (op == 0x11) v = c > 0;
            else v = c == 0;
            if (!m.push(wu64(v ? 1 : 0))) { err = "栈上溢"; break; }
            continue;
        }
        if (op == 0x16 || op == 0x17 || op == 0x18) {
            Word a, b;
            if (!m.pop(a) || !m.pop(b)) { err = "栈下溢"; break; }
            Word r;
            for (int i = 0; i < 4; ++i) {
                if (op == 0x16) r.v[i] = a.v[i] & b.v[i];
                else if (op == 0x17) r.v[i] = a.v[i] | b.v[i];
                else r.v[i] = a.v[i] ^ b.v[i];
            }
            if (!m.push(r)) { err = "栈上溢"; break; }
            continue;
        }
        if (op == 0x19) {
            Word a;
            if (!m.pop(a)) { err = "栈下溢"; break; }
            for (int i = 0; i < 4; ++i) a.v[i] = ~a.v[i];
            if (!m.push(a)) { err = "栈上溢"; break; }
            continue;
        }
        if (op == 0x1b || op == 0x1c) {
            Word a, n;
            if (!m.pop(a) || !m.pop(n)) { err = "栈下溢"; break; }
            uint64_t shift = n.v[0];
            if (n.v[1] || n.v[2] || n.v[3]) shift = 256;
            if (shift > 255) shift = 256;
            Word r = (op == 0x1b) ? wshr(a, (uint32_t)shift) : wshl(a, (uint32_t)shift);
            if (!m.push(r)) { err = "栈上溢"; break; }
            continue;
        }
        if (op == 0x30) { // ADDRESS
            if (!m.push(wfrom_hex(ctx.address))) { err = "栈上溢"; break; }
            continue;
        }
        if (op == 0x33) { // CALLER
            if (!m.push(wfrom_hex(ctx.caller))) { err = "栈上溢"; break; }
            continue;
        }
        if (op == 0x34) { // CALLVALUE
            if (!m.push(wu64(ctx.value))) { err = "栈上溢"; break; }
            continue;
        }
        if (op == 0x36) { // CALLDATASIZE
            if (!m.push(wu64((uint64_t)m.calldata.size()))) { err = "栈上溢"; break; }
            continue;
        }
        if (op == 0x35) { // CALLDATALOAD
            Word off;
            if (!m.pop(off)) { err = "栈下溢"; break; }
            size_t o = word_to_size(off);
            if (o == (size_t)-1) o = m.calldata.size();
            uint8_t buf[32];
            memset(buf, 0, 32);
            for (size_t i = 0; i < 32; ++i)
                if (o + i < m.calldata.size()) buf[i] = m.calldata[o + i];
            std::vector<uint8_t> v(buf, buf + 32);
            if (!m.push(wfrom_hex(to_hex(v)))) { err = "栈上溢"; break; }
            continue;
        }
        if (op == 0x50) { // POP
            Word junk;
            if (!m.pop(junk)) { err = "栈下溢"; break; }
            continue;
        }
        if (op == 0x54) { // SLOAD
            Word key;
            if (!m.pop(key)) { err = "栈下溢"; break; }
            std::string k = whex(key);
            std::map<std::string, std::string>::iterator it = m.storage.find(k);
            Word v = (it == m.storage.end()) ? wu64(0) : wfrom_hex(it->second);
            if (!m.push(v)) { err = "栈上溢"; break; }
            continue;
        }
        if (op == 0x55) { // SSTORE
            Word key, val;
            if (!m.pop(key) || !m.pop(val)) { err = "栈下溢"; break; }
            std::string k = whex(key);
            if (wzero(val)) m.storage.erase(k);
            else m.storage[k] = whex(val);
            continue;
        }
        if (op == 0x51) { // MLOAD
            Word off;
            if (!m.pop(off)) { err = "栈下溢"; break; }
            size_t o = word_to_size(off);
            if (o == (size_t)-1) { err = "偏移越界"; break; }
            uint64_t g = 0;
            if (!m.mem_reserve(o, 32, g) || !m.charge(g)) { err = "内存超限"; break; }
            if (!m.push(m.mem_load(o))) { err = "栈上溢"; break; }
            continue;
        }
        if (op == 0x52) { // MSTORE
            Word off, val;
            if (!m.pop(off) || !m.pop(val)) { err = "栈下溢"; break; }
            size_t o = word_to_size(off);
            if (o == (size_t)-1) { err = "偏移越界"; break; }
            uint64_t g = 0;
            if (!m.mem_reserve(o, 32, g) || !m.charge(g)) { err = "内存超限"; break; }
            m.mem_store(o, val);
            continue;
        }
        if (op == 0x56 || op == 0x57) { // JUMP / JUMPI
            Word dest;
            if (!m.pop(dest)) { err = "栈下溢"; break; }
            if (op == 0x57) {
                Word cond;
                if (!m.pop(cond)) { err = "栈下溢"; break; }
                if (wzero(cond)) continue;
            }
            size_t d = word_to_size(dest);
            if (d == (size_t)-1 || !m.jumpdests.count(d)) { err = "非法跳转目标"; break; }
            m.pc = d;
            continue;
        }
        if (op == 0x58) { // PC
            if (!m.push(wu64((uint64_t)at))) { err = "栈上溢"; break; }
            continue;
        }
        if (op == 0x5b) continue; // JUMPDEST
        if (op >= 0x60 && op <= 0x7f) { // PUSH1..PUSH32
            size_t n = (size_t)(op - 0x5f);
            std::vector<uint8_t> padded(32, 0);
            for (size_t i = 0; i < n; ++i) {
                uint8_t b = (m.pc + i < m.code.size()) ? m.code[m.pc + i] : 0;
                padded[32 - n + i] = b;
            }
            m.pc += n;
            if (!m.push(wfrom_hex(to_hex(padded)))) { err = "栈上溢"; break; }
            continue;
        }
        if (op >= 0x80 && op <= 0x87) { // DUPn
            size_t n = (size_t)(op - 0x7f);
            if (m.stack.size() < n) { err = "栈下溢"; break; }
            if (!m.push(m.stack[m.stack.size() - n])) { err = "栈上溢"; break; }
            continue;
        }
        if (op >= 0x90 && op <= 0x97) { // SWAPn
            size_t n = (size_t)(op - 0x8f);
            if (m.stack.size() < n + 1) { err = "栈下溢"; break; }
            std::swap(m.stack[m.stack.size() - 1], m.stack[m.stack.size() - 1 - n]);
            continue;
        }
        if (op >= 0xa0 && op <= 0xa2) { // LOG0..LOG2
            size_t ntopics = (size_t)(op - 0xa0);
            Word off, len;
            if (!m.pop(off) || !m.pop(len)) { err = "栈下溢"; break; }
            size_t o = word_to_size(off), l = word_to_size(len);
            if (o == (size_t)-1 || l == (size_t)-1) { err = "偏移越界"; break; }
            uint64_t g = 0;
            if (!m.mem_reserve(o, l, g) || !m.charge(g)) { err = "内存超限"; break; }
            if (!m.charge(8 * (uint64_t)l)) { err = "gas 不足"; break; }
            Log lg;
            lg.address = ctx.address;
            for (size_t i = 0; i < ntopics; ++i) {
                Word t;
                if (!m.pop(t)) { err = "栈下溢"; break; }
                lg.topics.push_back(whex(t));
            }
            lg.data = m.mem_slice(o, l);
            m.logs.push_back(lg);
            continue;
        }
        if (op == 0xf3 || op == 0xfd) { // RETURN / REVERT
            Word off, len;
            if (!m.pop(off) || !m.pop(len)) { err = "栈下溢"; break; }
            size_t o = word_to_size(off), l = word_to_size(len);
            if (o == (size_t)-1 || l == (size_t)-1) { err = "偏移越界"; break; }
            uint64_t g = 0;
            if (!m.mem_reserve(o, l, g)) { err = "内存超限"; break; }
            ret_hex = m.mem_slice(o, l);
            reverted = (op == 0xfd);
            break;
        }
        err = "未实现的指令";
        break;
    }

    if (!err.empty()) {
        res.success = false;
        res.gas_used = m.gas_used;
        res.error = err;
        res.storage = storage_in; // 失败回滚
        return res;
    }
    if (reverted) {
        res.success = false;
        res.gas_used = m.gas_used;
        res.error = "revert";
        res.return_data = ret_hex;
        res.storage = storage_in;
        return res;
    }
    res.success = true;
    res.gas_used = m.gas_used;
    res.return_data = ret_hex;
    res.logs = m.logs;
    res.storage = m.storage;
    return res;
}

bool self_test(std::string& err) {
    // ABI 选择器
    if (abi::selector("transfer(address,uint256)").size() != 8) { err = "selector 长度错误"; return false; }
    // 字运算
    if (wdec(wadd(wu64(2), wu64(3))) != "5") { err = "加法错误"; return false; }
    if (wdec(wmul(wu64(1000000000ULL), wu64(1000000000ULL))) != "1000000000000000000") {
        err = "乘法错误";
        return false;
    }
    if (wdec(wsub(wu64(0), wu64(1))) !=
        "115792089237316195423570985008687907853269984665640564039457584007913129639935") {
        err = "下溢回绕错误";
        return false;
    }
    // 执行：PUSH1 2 PUSH1 3 ADD PUSH1 0 MSTORE PUSH1 32 PUSH1 0 RETURN
    std::string code = "6002600301600052602060 00f3";
    std::string clean;
    for (size_t i = 0; i < code.size(); ++i)
        if (code[i] != ' ') clean.push_back(code[i]);
    Context ctx;
    ctx.address = "0x00000000000000000000000000000000000000aa";
    ctx.caller = "0x00000000000000000000000000000000000000bb";
    ctx.value = 0;
    ctx.gas_limit = 100000;
    ctx.chain_id = "1001";
    std::map<std::string, std::string> st;
    Result r = execute(clean, ctx, st);
    if (!r.success) { err = "执行失败: " + r.error; return false; }
    if (r.return_data != std::string(62, '0') + "05") { err = "返回值错误: " + r.return_data; return false; }
    // SSTORE/SLOAD
    // PUSH1 1 PUSH1 0 SSTORE PUSH1 0 SLOAD PUSH1 0 MSTORE PUSH1 32 PUSH1 0 RETURN
    std::string code2 = "6001600055600054600052602060 00f3";
    clean.clear();
    for (size_t i = 0; i < code2.size(); ++i)
        if (code2[i] != ' ') clean.push_back(code2[i]);
    Result r2 = execute(clean, ctx, st);
    if (!r2.success) { err = "存储执行失败: " + r2.error; return false; }
    if (r2.storage.size() != 1) { err = "SSTORE 未生效"; return false; }
    return true;
}

} // namespace vm
} // namespace bc
