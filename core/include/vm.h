// vm.h — 精简 EVM 子集（256 位字长栈机）+ 极简 ABI 编解码
//
// 设计取舍：只实现合约演示与跨链桥所需的指令集，不做 gas 市场、不做 CALL/CREATE 嵌套调用。
// 字长与 EVM 一致（256 位），算术按 2^256 取模，存储槽为 32 字节。
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bc {
namespace vm {

// ---------------- 256 位字 ----------------
struct Word {
    uint64_t v[4]; // 小端：v[0] 最低位
};

Word wu64(uint64_t x);
Word wfrom_hex(const std::string& hex); // 允许 0x 前缀，不足 32 字节左侧补零
std::string whex(const Word& w);        // 64 个 hex 字符
std::string hex_u64(uint64_t v);        // u64 的 64 hex 表示
std::string wdec(const Word& w);        // 十进制字符串
bool wzero(const Word& w);
int wcmp(const Word& a, const Word& b);
Word wadd(const Word& a, const Word& b);
Word wsub(const Word& a, const Word& b);
Word wmul(const Word& a, const Word& b);
Word wdiv(const Word& a, const Word& b);
Word wmod(const Word& a, const Word& b);
Word wshl(const Word& a, uint32_t n);
Word wshr(const Word& a, uint32_t n);

// ---------------- 事件日志 ----------------
struct Log {
    std::string address;             // 0x + 40 hex
    std::vector<std::string> topics; // 每个 64 hex
    std::string data;                // hex（可为空）
    std::string tx_id;               // 产生该日志的交易（不参与哈希）
    uint64_t tx_index;               // 交易在区块内的序号
    Log() : tx_index(0) {}
};

std::string log_serialize(const Log& l); // 用于计算 logs_root
std::string logs_merkle_root(const std::vector<Log>& logs);
// 通用 Merkle：叶子先 sha256，逐层两两 sha256，奇数补最后一个
std::string merkle_root_hex(const std::vector<std::string>& leaves_hex);

// Merkle 证明（与 merkle_root_hex 同一套规则：叶子 sha256、奇数补尾、左||右）
struct MerkleBuildResult {
    std::string root;
    std::vector<std::vector<std::string> > proofs; // proofs[i] 为第 i 个叶子的证明路径
};
MerkleBuildResult merkle_build(const std::vector<std::string>& leaves_hex);
bool merkle_verify(const std::string& leaf_hex, size_t index, const std::vector<std::string>& proof,
                   const std::string& root);

// ---------------- ABI ----------------
namespace abi {
// 函数选择器：sha256(签名) 前 4 字节
std::string selector(const std::string& signature);
// 参数编码：uint256 用十进制字符串，address 用 0x..，bytes32 用 64 hex
bool encode_arg(const std::string& type, const std::string& value, std::string& out_hex, std::string& err);
std::string encode_call(const std::string& signature, const std::vector<std::string>& types,
                        const std::vector<std::string>& values);
std::vector<Word> decode_words(const std::string& hex);
} // namespace abi

// ---------------- 执行 ----------------
struct Context {
    std::string address;  // 合约地址
    std::string caller;   // 调用者地址
    uint64_t value;       // 附带金额（本版本仅支持 0）
    std::string calldata; // hex
    uint64_t gas_limit;
    std::string chain_id; // 十进制字符串
};

struct Result {
    bool success;
    uint64_t gas_used;
    std::string return_data; // hex
    std::vector<Log> logs;
    std::map<std::string, std::string> storage; // 执行后的完整存储
    std::string error;
};

Result execute(const std::string& code_hex, const Context& ctx,
               const std::map<std::string, std::string>& storage_in);

// 指令集自检
bool self_test(std::string& err);

} // namespace vm
} // namespace bc
