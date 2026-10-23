// crypto.h — 自包含密码学原语：SHA-256 / HMAC-SHA256 / secp256k1 ECDSA / Base64
// 设计目标：不依赖 OpenSSL 等外部库，32 位无符号整数实现，可移植到任意 C++14 编译器。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bc {

// ---------------- 哈希 ----------------
void sha256(const uint8_t* data, size_t len, uint8_t out[32]);
std::string sha256_hex(const std::string& data);
std::string sha256d_hex(const std::string& data); // 双 SHA-256（比特币式）
std::string hmac_sha256(const std::string& key, const std::string& data); // 32 字节原始输出
std::string hmac_sha256_hex(const std::string& key, const std::string& data);

// ---------------- 编码 ----------------
std::string to_hex(const uint8_t* p, size_t n);
std::string to_hex(const std::vector<uint8_t>& v);
bool from_hex(const std::string& s, std::vector<uint8_t>& out);
std::string base64_encode(const uint8_t* p, size_t n);
bool base64_decode(const std::string& s, std::vector<uint8_t>& out);

// ---------------- secp256k1 ----------------
namespace ecc {

struct KeyPair {
    std::string address;    // 0x + 40 hex（SHA256(pubkey) 前 20 字节）
    std::string public_key; // 128 hex，未压缩点 X||Y（不含 0x04）
    std::string private_key;// 64 hex
};

// 生成密钥对（随机源：OS 熵 + SHA-256 计数器混合）
KeyPair generate();
// 由私钥推导公钥与地址
bool derive_public(const std::string& private_key_hex, std::string& public_key_hex, std::string& address);
// 地址 = 0x + hex(SHA256(pubkey)[0:20])
std::string address_of(const std::string& public_key_hex);
// 签名：对 msg 的 SHA-256 摘要做 ECDSA，输出 base64(r||s) 共 64 字节，low-s 规范化
std::string sign(const std::string& msg, const std::string& private_key_hex);
// 验签
bool verify(const std::string& msg, const std::string& signature_b64, const std::string& public_key_hex);
// 自检：内部一致性检查（启动时调用一次）
bool self_test(std::string& err);

} // namespace ecc

} // namespace bc
