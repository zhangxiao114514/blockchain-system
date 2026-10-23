// test_crypto.cpp — 输出已知测试向量，供 Node.js(OpenSSL) 交叉验证
#include "crypto.h"
#include <cstdio>
#include <string>
#include <vector>

int main() {
    std::string err;
    if (!bc::ecc::self_test(err)) {
        printf("{\"self_test\":false,\"error\":\"%s\"}\n", err.c_str());
        return 1;
    }
    printf("{\"self_test\":true}\n");

    // 固定私钥的确定性签名（RFC6979），可与其他实现比对
    const char* privs[] = {
        "0000000000000000000000000000000000000000000000000000000000000001",
        "0000000000000000000000000000000000000000000000000000000000000002",
        "4c0883a69102937d6231471b5dbb6204fe5129617082792ae468d01a3f362318",
        "b7e151628aed2a6abf7158809cf4f3c762e7160f38b4da56a784d9045190cfef"};
    const char* msgs[] = {"hello blockchain", "区块 1 交易", "abc", ""};
    for (int i = 0; i < 4; ++i) {
        std::string pub, addr;
        if (!bc::ecc::derive_public(privs[i], pub, addr)) { printf("{\"err\":\"derive\"}\n"); return 1; }
        std::string sig = bc::ecc::sign(msgs[i], privs[i]);
        bool ok = bc::ecc::verify(msgs[i], sig, pub);
        printf("{\"priv\":\"%s\",\"pub\":\"%s\",\"addr\":\"%s\",\"msg\":\"%s\",\"sig\":\"%s\",\"self_verify\":%s}\n",
               privs[i], pub.c_str(), addr.c_str(), msgs[i], sig.c_str(), ok ? "true" : "false");
    }

    // 随机密钥 20 组
    for (int i = 0; i < 20; ++i) {
        bc::ecc::KeyPair kp = bc::ecc::generate();
        std::string msg = "random-msg-" + std::to_string(i);
        std::string sig = bc::ecc::sign(msg, kp.private_key);
        bool ok = bc::ecc::verify(msg, sig, kp.public_key);
        printf("{\"rnd\":%d,\"priv\":\"%s\",\"pub\":\"%s\",\"addr\":\"%s\",\"msg\":\"%s\",\"sig\":\"%s\",\"self_verify\":%s}\n",
               i, kp.private_key.c_str(), kp.public_key.c_str(), kp.address.c_str(), msg.c_str(), sig.c_str(),
               ok ? "true" : "false");
    }
    return 0;
}
