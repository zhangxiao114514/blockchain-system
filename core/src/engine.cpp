// engine.cpp — 共识引擎进程
//
// 与上层（Python 节点服务）通过 stdin/stdout 的行式 JSON 协议通信：
//   请求: {"rid":1,"cmd":"status"}
//   响应: {"rid":1,"ok":true,...}
//
// 这样 C++ 只负责共识与密码学，网络/存储编排交给 Python，二者通过稳定协议解耦。

#include <cstdio>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>
#include <vector>

#include "chain.h"
#include "crypto.h"
#include "json.h"

using namespace bc;

static Chain* g_chain = NULL;
static std::string g_datadir;

static J ok_resp() {
    J j = J::obj();
    j.set("ok", J::boolean(true));
    return j;
}

static J err_resp(const std::string& msg) {
    J j = J::obj();
    j.set("ok", J::boolean(false));
    j.set("error", J::str(msg));
    return j;
}

static J handle(const J& req, bool& quit) {
    const J* pc = req.find("cmd");
    std::string cmd = pc ? pc->s() : "";
    if (cmd.empty()) return err_resp("缺少 cmd");

    if (cmd == "ping") {
        J r = ok_resp();
        r.set("pong", J::num((int64_t)time(0) * 1000));
        return r;
    }

    if (cmd == "init") {
        Params p;
        if (const J* d = req.find("difficulty")) p.initial_difficulty = (uint32_t)d->i(4);
        if (const J* d = req.find("min_difficulty")) p.min_difficulty = (uint32_t)d->i(2);
        if (const J* d = req.find("max_difficulty")) p.max_difficulty = (uint32_t)d->i(6);
        if (const J* d = req.find("target_block_ms")) p.target_block_ms = d->i(15000);
        if (const J* d = req.find("retarget_interval")) p.retarget_interval = (int)d->i(10);
        if (const J* d = req.find("halving_interval")) p.halving_interval = (uint32_t)d->i(210000);
        if (const J* d = req.find("max_block_txs")) p.max_block_txs = (size_t)d->i(500);
        std::string datadir = req.find("datadir") ? req.find("datadir")->s() : "data/chain";
        if (g_chain) delete g_chain;
        g_chain = new Chain(p);
        g_datadir = datadir;
        std::string err;
        bool loaded = g_chain->load(datadir, err);
        J r = ok_resp();
        r.set("genesis", J::str(g_chain->blocks()[0].hash));
        r.set("height", J::num((int64_t)g_chain->height()));
        r.set("datadir", J::str(datadir));
        r.set("warning", J::str(err));
        r.set("loaded", J::boolean(loaded));
        return r;
    }

    if (!g_chain) return err_resp("引擎未初始化，请先发送 init");

    if (cmd == "status") {
        J r = ok_resp();
        J st = g_chain->status_json();
        for (size_t i = 0; i < st.keys().size(); ++i) {
            std::string k = st.keys()[i];
            r.set(k, *st.find(k));
        }
        return r;
    }

    if (cmd == "wallet_new") {
        ecc::KeyPair kp = ecc::generate();
        if (kp.private_key.empty()) return err_resp("密钥生成失败");
        J r = ok_resp();
        r.set("address", J::str(kp.address));
        r.set("public_key", J::str(kp.public_key));
        r.set("private_key", J::str(kp.private_key));
        return r;
    }

    if (cmd == "wallet_from_private") {
        std::string priv = req.find("private_key") ? req.find("private_key")->s() : "";
        std::string pub, addr;
        if (!ecc::derive_public(priv, pub, addr)) return err_resp("私钥非法");
        J r = ok_resp();
        r.set("address", J::str(addr));
        r.set("public_key", J::str(pub));
        r.set("private_key", J::str(priv));
        return r;
    }

    if (cmd == "verify") {
        std::string msg = req.find("msg") ? req.find("msg")->s() : "";
        std::string sig = req.find("signature") ? req.find("signature")->s() : "";
        std::string pub = req.find("public_key") ? req.find("public_key")->s() : "";
        J r = ok_resp();
        r.set("valid", J::boolean(ecc::verify(msg, sig, pub)));
        return r;
    }

    if (cmd == "sign_tx") {
        std::string priv = req.find("private_key") ? req.find("private_key")->s() : "";
        std::string pub, addr;
        if (!ecc::derive_public(priv, pub, addr)) return err_resp("私钥非法");
        Tx t;
        t.type = "transfer";
        t.from = addr;
        t.to = req.find("to") ? req.find("to")->s() : "";
        t.amount = (uint64_t)(req.find("amount") ? req.find("amount")->i() : 0);
        t.fee = (uint64_t)(req.find("fee") ? req.find("fee")->i() : 0);
        t.nonce = (uint64_t)(req.find("nonce") ? req.find("nonce")->i() : 0);
        t.timestamp = req.find("timestamp") ? req.find("timestamp")->i() : (int64_t)time(0) * 1000;
        t.pubkey = pub;
        t.id = tx_compute_id(t);
        t.signature = ecc::sign(t.id, priv);
        if (t.signature.empty()) return err_resp("签名失败");
        J r = ok_resp();
        r.set("tx", tx_to_json(t));
        return r;
    }

    if (cmd == "submit_tx") {
        if (!req.has("tx")) return err_resp("缺少 tx");
        Tx t = tx_from_json(*req.find("tx"));
        std::string err;
        if (!g_chain->submit_tx(t, err)) return err_resp(err);
        J r = ok_resp();
        r.set("txid", J::str(t.id));
        return r;
    }

    if (cmd == "mempool") {
        std::vector<Tx> v = g_chain->mempool_list();
        J arr = J::arr();
        for (size_t i = 0; i < v.size(); ++i) arr.push(tx_to_json(v[i]));
        J r = ok_resp();
        r.set("txs", arr);
        r.set("size", J::num((int64_t)v.size()));
        return r;
    }

    if (cmd == "mine_step") {
        std::string miner = req.find("miner") ? req.find("miner")->s() : "";
        uint32_t iters = (uint32_t)(req.find("iters") ? req.find("iters")->i(20000) : 20000);
        Block found;
        bool is_found = false;
        std::string err;
        uint64_t done = 0;
        if (!g_chain->mine_step(miner, iters, found, is_found, err, &done)) return err_resp(err);
        J r = ok_resp();
        r.set("found", J::boolean(is_found));
        r.set("hashes", J::num((int64_t)done));
        if (is_found) {
            r.set("block", block_to_json(found, true));
            r.set("height", J::num((int64_t)g_chain->height()));
        }
        return r;
    }

    if (cmd == "add_block") {
        if (!req.has("block")) return err_resp("缺少 block");
        Block b = block_from_json(*req.find("block"));
        std::string err;
        bool need_sync = false;
        if (!g_chain->add_block(b, err, need_sync)) {
            J r = err_resp(err);
            r.set("need_sync", J::boolean(need_sync));
            return r;
        }
        J r = ok_resp();
        r.set("height", J::num((int64_t)g_chain->height()));
        r.set("tip", J::str(g_chain->tip().hash));
        return r;
    }

    if (cmd == "get_chain") {
        size_t from = (size_t)(req.find("from") ? req.find("from")->i(0) : 0);
        size_t limit = (size_t)(req.find("limit") ? req.find("limit")->i(100000) : 100000);
        bool with_txs = req.find("with_txs") ? req.find("with_txs")->b(true) : true;
        J r = ok_resp();
        r.set("blocks", g_chain->chain_json(from, limit, with_txs));
        r.set("height", J::num((int64_t)g_chain->height()));
        r.set("genesis", J::str(g_chain->blocks()[0].hash));
        return r;
    }

    if (cmd == "replace_chain") {
        if (!req.has("blocks")) return err_resp("缺少 blocks");
        const J* arr = req.find("blocks");
        std::vector<Block> nc;
        for (size_t i = 0; i < arr->size(); ++i) nc.push_back(block_from_json(arr->at(i)));
        std::string err;
        if (!g_chain->replace_chain(nc, err)) return err_resp(err);
        J r = ok_resp();
        r.set("height", J::num((int64_t)g_chain->height()));
        r.set("tip", J::str(g_chain->tip().hash));
        return r;
    }

    if (cmd == "balance") {
        std::string addr = req.find("address") ? req.find("address")->s() : "";
        Account a;
        g_chain->account(addr, a);
        // 待处理交易数（用于计算下一笔交易的 nonce）
        std::vector<Tx> pool = g_chain->mempool_list();
        uint64_t pending = 0;
        for (size_t i = 0; i < pool.size(); ++i)
            if (pool[i].from == addr) pending++;
        J r = ok_resp();
        r.set("address", J::str(addr));
        r.set("balance", J::num((int64_t)a.balance));
        r.set("balance_str", J::str(amount_to_str(a.balance)));
        r.set("nonce", J::num((int64_t)a.nonce));
        r.set("pending", J::num((int64_t)pending));
        r.set("next_nonce", J::num((int64_t)(a.nonce + pending)));
        return r;
    }

    if (cmd == "validate") {
        std::string err;
        bool good = g_chain->validate_chain(err);
        J r = ok_resp();
        r.set("valid", J::boolean(good));
        r.set("height", J::num((int64_t)g_chain->height()));
        r.set("error", J::str(err));
        return r;
    }

    if (cmd == "tx") {
        std::string id = req.find("id") ? req.find("id")->s() : "";
        Tx t;
        uint64_t bi = 0;
        if (!g_chain->tx_lookup(id, t, bi)) return err_resp("交易不存在");
        J r = ok_resp();
        r.set("tx", tx_to_json(t));
        r.set("block_index", J::num((int64_t)bi));
        r.set("confirmed", J::boolean(bi != (uint64_t)-1));
        return r;
    }

    if (cmd == "shutdown") {
        quit = true;
        return ok_resp();
    }

    return err_resp("未知命令: " + cmd);
}

int main(int argc, char** argv) {
    // 自检模式
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--selftest") == 0) {
            std::string err;
            bool ok = ecc::self_test(err);
            printf("{\"ok\":%s,\"error\":\"%s\"}\n", ok ? "true" : "false", err.c_str());
            return ok ? 0 : 1;
        }
    }

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        J req, resp;
        std::string perr;
        if (!J::parse(line, req, perr)) {
            resp = err_resp("请求解析失败: " + perr);
        } else {
            bool quit = false;
            resp = handle(req, quit);
            if (req.has("rid")) resp.set("rid", *req.find("rid"));
            if (quit) {
                printf("%s\n", resp.dump().c_str());
                fflush(stdout);
                break;
            }
        }
        printf("%s\n", resp.dump().c_str());
        fflush(stdout);
    }
    if (g_chain) delete g_chain;
    return 0;
}
