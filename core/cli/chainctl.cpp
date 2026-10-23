// chainctl.cpp — C++ 命令行运维工具（不依赖 Python 即可建链/挖矿/校验）
//
// 用法：
//   chainctl --datadir data/n1 status
//   chainctl --datadir data/n1 validate
//   chainctl --datadir data/n1 mine --miner 0x... --count 3
//   chainctl --datadir data/n1 block 5
//   chainctl --datadir data/n1 wallet-new
//   chainctl --datadir data/n1 send --key <priv> --to 0x... --amount 1.5 --fee 0.001

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "chain.h"
#include "crypto.h"
#include "json.h"

using namespace bc;

static void usage() {
    printf("chainctl --datadir <dir> <command> [options]\n");
    printf("commands:\n");
    printf("  status                              显示链状态\n");
    printf("  validate                            全链校验\n");
    printf("  mine --miner <addr> [--count N]     挖矿 N 个区块\n");
    printf("  block <index>                       显示区块\n");
    printf("  wallet-new                          生成新钱包\n");
    printf("  balance <addr>                      查询余额\n");
    printf("  send --key <priv> --to <addr> --amount <x> [--fee <y>]  构造并提交交易\n");
    printf("  mempool                             显示交易池\n");
}

static std::string arg_value(int argc, char** argv, const char* name, const char* def) {
    for (int i = 1; i + 1 < argc; ++i)
        if (strcmp(argv[i], name) == 0) return std::string(argv[i + 1]);
    return std::string(def ? def : "");
}

// MinGW 的 printf 不支持 %llu，这里统一手工转换
static std::string num(uint64_t v) {
    if (v == 0) return "0";
    char b[24];
    int n = 0;
    while (v) { b[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
    std::string s;
    while (n) s.push_back(b[--n]);
    return s;
}
static std::string num(int64_t v) {
    if (v < 0) return "-" + num((uint64_t)((uint64_t)(-(v + 1)) + 1ULL));
    return num((uint64_t)v);
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 1; }

    std::string datadir = arg_value(argc, argv, "--datadir", "data/chain");
    // 定位子命令（跳过前置全局选项）
    int ci = 1;
    while (ci < argc && strncmp(argv[ci], "--", 2) == 0) ci += 2;
    if (ci >= argc) { usage(); return 1; }
    std::string cmd = argv[ci];
    // 子命令后的位置参数
    struct Pos {
        int argc; char** argv; int base;
        std::string operator()(int n) const { return (base + n < argc) ? std::string(argv[base + n]) : std::string(); }
    } pos = {argc, argv, ci + 1};

    Params p;
    std::string pd = arg_value(argc, argv, "--difficulty", "");
    if (!pd.empty()) p.initial_difficulty = (uint32_t)atoi(pd.c_str());
    std::string pc = arg_value(argc, argv, "--chain-id", "");
    if (!pc.empty()) p.chain_id = (uint64_t)strtoull(pc.c_str(), 0, 10);
    p.relayer = arg_value(argc, argv, "--relayer", "");

    Chain chain(p);
    std::string err;
    chain.load(datadir, err);
    if (!err.empty()) fprintf(stderr, "[warn] %s\n", err.c_str());

    if (cmd == "status") {
        printf("%s\n", chain.status_json().dump().c_str());
        return 0;
    }
    if (cmd == "validate") {
        std::string e;
        bool ok = chain.validate_chain(e);
        printf("{\"valid\":%s,\"height\":%s,\"error\":\"%s\"}\n", ok ? "true" : "false",
               num(chain.height()).c_str(), e.c_str());
        return ok ? 0 : 1;
    }
    if (cmd == "wallet-new") {
        ecc::KeyPair kp = ecc::generate();
        J j = J::obj();
        j.set("address", J::str(kp.address));
        j.set("public_key", J::str(kp.public_key));
        j.set("private_key", J::str(kp.private_key));
        printf("%s\n", j.dump().c_str());
        return 0;
    }
    if (cmd == "balance") {
        std::string addr = pos(0);
        Account a;
        chain.account(addr, a);
        J j = J::obj();
        j.set("address", J::str(addr));
        j.set("balance", J::num((int64_t)a.balance));
        j.set("balance_str", J::str(amount_to_str(a.balance)));
        j.set("nonce", J::num((int64_t)a.nonce));
        printf("%s\n", j.dump().c_str());
        return 0;
    }
    if (cmd == "block") {
        int idx = pos(0).empty() ? (int)chain.height() : atoi(pos(0).c_str());
        if (idx < 0 || (size_t)idx >= chain.block_count()) { fprintf(stderr, "区块不存在\n"); return 1; }
        printf("%s\n", block_to_json(chain.blocks()[(size_t)idx], true).dump().c_str());
        return 0;
    }
    if (cmd == "contract") {
        std::string addr = pos(0);
        Contract c;
        if (!chain.contract_lookup(addr, c)) { fprintf(stderr, "合约不存在\n"); return 1; }
        J j = J::obj();
        j.set("address", J::str(addr));
        j.set("code", J::str(c.code));
        j.set("creator", J::str(c.creator));
        J st = J::obj();
        for (std::map<std::string, std::string>::const_iterator it = c.storage.begin(); it != c.storage.end(); ++it)
            st.set(it->first, J::str(it->second));
        j.set("storage", st);
        printf("%s\n", j.dump().c_str());
        return 0;
    }
    if (cmd == "contracts") {
        printf("%s\n", chain.contracts_json().dump().c_str());
        return 0;
    }
    if (cmd == "logs") {
        uint64_t from = pos(0).empty() ? 0 : (uint64_t)atoi(pos(0).c_str());
        printf("%s\n", chain.logs_json(from, 200, "", "").dump().c_str());
        return 0;
    }
    if (cmd == "bridge") {
        printf("%s\n", chain.bridge_json().dump().c_str());
        return 0;
    }
    if (cmd == "mempool") {
        std::vector<Tx> v = chain.mempool_list();
        J arr = J::arr();
        for (size_t i = 0; i < v.size(); ++i) arr.push(tx_to_json(v[i]));
        printf("%s\n", arr.dump().c_str());
        return 0;
    }
    if (cmd == "mine") {
        std::string miner = arg_value(argc, argv, "--miner", "");
        int count = atoi(arg_value(argc, argv, "--count", "1").c_str());
        if (miner.empty()) { fprintf(stderr, "缺少 --miner\n"); return 1; }
        for (int i = 0; i < count; ++i) {
            bool found = false;
            Block b;
            uint64_t total = 0;
            int64_t t0 = (int64_t)time(0) * 1000;
            while (!found) {
                uint64_t done = 0;
                std::string e;
                if (!chain.mine_step(miner, 50000, b, found, e, &done)) {
                    fprintf(stderr, "挖矿失败: %s\n", e.c_str());
                    return 1;
                }
                total += done;
            }
            int64_t dt = (int64_t)time(0) * 1000 - t0;
            printf("区块 #%s 已出块 hash=%s txs=%d 哈希次数=%s 耗时=%sms\n", num(b.index).c_str(), b.hash.c_str(),
                   (int)b.txs.size(), num(total).c_str(), num(dt).c_str());
        }
        return 0;
    }
    if (cmd == "send") {
        std::string key = arg_value(argc, argv, "--key", "");
        std::string to = arg_value(argc, argv, "--to", "");
        std::string amount_s = arg_value(argc, argv, "--amount", "");
        std::string fee_s = arg_value(argc, argv, "--fee", "0.0001");
        uint64_t amount = 0, fee = 0;
        if (!amount_from_str(amount_s, amount)) { fprintf(stderr, "金额非法\n"); return 1; }
        if (!amount_from_str(fee_s, fee)) { fprintf(stderr, "手续费非法\n"); return 1; }
        std::string pub, addr;
        if (!ecc::derive_public(key, pub, addr)) { fprintf(stderr, "私钥非法\n"); return 1; }
        Account acc;
        chain.account(addr, acc);
        Tx t;
        t.type = "transfer";
        t.from = addr;
        t.to = to;
        t.amount = amount;
        t.fee = fee;
        t.nonce = acc.nonce;
        t.timestamp = (int64_t)time(0) * 1000;
        t.pubkey = pub;
        t.id = tx_compute_id(t, chain.chain_id());
        t.signature = ecc::sign(t.id, key);
        if (!chain.submit_tx(t, err)) { fprintf(stderr, "交易被拒: %s\n", err.c_str()); return 1; }
        printf("{\"txid\":\"%s\",\"from\":\"%s\",\"to\":\"%s\",\"amount\":\"%s\"}\n", t.id.c_str(), t.from.c_str(),
               t.to.c_str(), amount_to_str(amount).c_str());
        return 0;
    }

    usage();
    return 1;
}
