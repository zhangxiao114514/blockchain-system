// crypto.cpp — SHA-256 / HMAC-SHA256 / secp256k1 ECDSA / Base64 的自包含实现
//
// 说明：
//   * 全部使用 32 位无符号整数运算（不依赖 __int128），可在 32 位 MinGW 下编译。
//   * 椭圆曲线点运算使用 Jacobian 坐标，仅在最终转仿射时做一次求逆。
//   * 签名采用 RFC6979 确定性随机数（HMAC-SHA256 DRBG），无需外部随机源即可复现。
//   * 签名结果做 low-s 规范化，防签名延展性攻击。

#include "crypto.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#else
#include <unistd.h>
#endif

namespace bc {

typedef uint32_t u32;
typedef uint64_t u64;
typedef uint8_t u8;

// ============================================================
// 1. SHA-256
// ============================================================
static const u32 K256[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

static inline u32 rotr32(u32 x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_transform(u32 st[8], const u8 blk[64]) {
    u32 w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = ((u32)blk[i * 4] << 24) | ((u32)blk[i * 4 + 1] << 16) | ((u32)blk[i * 4 + 2] << 8) | (u32)blk[i * 4 + 3];
    for (int i = 16; i < 64; ++i) {
        u32 s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        u32 s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u32 a = st[0], b = st[1], c = st[2], d = st[3], e = st[4], f = st[5], g = st[6], h = st[7];
    for (int i = 0; i < 64; ++i) {
        u32 S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        u32 ch = (e & f) ^ ((~e) & g);
        u32 t1 = h + S1 + ch + K256[i] + w[i];
        u32 S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        u32 mj = (a & b) ^ (a & c) ^ (b & c);
        u32 t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
    st[4] += e; st[5] += f; st[6] += g; st[7] += h;
}

void sha256(const u8* data, size_t len, u8 out[32]) {
    u32 st[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    size_t i = 0;
    for (; i + 64 <= len; i += 64) sha256_transform(st, data + i);

    u8 tail[128];
    size_t rem = len - i;
    if (rem) memcpy(tail, data + i, rem);
    tail[rem] = 0x80;
    size_t padlen = (rem < 56) ? 64 : 128;
    memset(tail + rem + 1, 0, padlen - rem - 1 - 8);
    u64 bits = (u64)len * 8;
    for (int k = 0; k < 8; ++k) tail[padlen - 1 - k] = (u8)(bits >> (8 * k));
    for (size_t off = 0; off < padlen; off += 64) sha256_transform(st, tail + off);

    for (int k = 0; k < 8; ++k) {
        out[k * 4]     = (u8)(st[k] >> 24);
        out[k * 4 + 1] = (u8)(st[k] >> 16);
        out[k * 4 + 2] = (u8)(st[k] >> 8);
        out[k * 4 + 3] = (u8)(st[k]);
    }
}

std::string to_hex(const u8* p, size_t n) {
    static const char* H = "0123456789abcdef";
    std::string s;
    s.resize(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s[i * 2]     = H[p[i] >> 4];
        s[i * 2 + 1] = H[p[i] & 0x0f];
    }
    return s;
}

std::string to_hex(const std::vector<u8>& v) { return to_hex(v.empty() ? (const u8*)"" : &v[0], v.size()); }

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool from_hex(const std::string& s, std::vector<u8>& out) {
    size_t start = (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? 2 : 0;
    if ((s.size() - start) % 2 != 0) return false;
    out.clear();
    out.reserve((s.size() - start) / 2);
    for (size_t i = start; i < s.size(); i += 2) {
        int hi = hexval(s[i]), lo = hexval(s[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back((u8)((hi << 4) | lo));
    }
    return true;
}

std::string sha256_hex(const std::string& data) {
    u8 d[32];
    sha256((const u8*)(data.empty() ? "" : data.data()), data.size(), d);
    return to_hex(d, 32);
}

std::string sha256d_hex(const std::string& data) {
    u8 d[32];
    sha256((const u8*)(data.empty() ? "" : data.data()), data.size(), d);
    u8 e[32];
    sha256(d, 32, e);
    return to_hex(e, 32);
}

// ============================================================
// 2. HMAC-SHA256
// ============================================================
static void hmac_raw(const u8* key, size_t keylen, const u8* msg, size_t msglen, u8 out[32]) {
    u8 k[64];
    memset(k, 0, 64);
    if (keylen > 64) {
        sha256(key, keylen, k);
    } else {
        memcpy(k, key, keylen);
    }
    u8 ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    // inner = SHA256(ipad || msg)
    std::vector<u8> inner;
    inner.reserve(64 + msglen);
    inner.insert(inner.end(), ipad, ipad + 64);
    inner.insert(inner.end(), msg, msg + msglen);
    u8 ih[32];
    sha256(&inner[0], inner.size(), ih);
    // outer
    u8 buf[96];
    memcpy(buf, opad, 64);
    memcpy(buf + 64, ih, 32);
    sha256(buf, 96, out);
}

std::string hmac_sha256(const std::string& key, const std::string& data) {
    u8 out[32];
    hmac_raw((const u8*)(key.empty() ? "" : key.data()), key.size(),
             (const u8*)(data.empty() ? "" : data.data()), data.size(), out);
    return std::string((const char*)out, 32);
}

std::string hmac_sha256_hex(const std::string& key, const std::string& data) {
    u8 out[32];
    hmac_raw((const u8*)(key.empty() ? "" : key.data()), key.size(),
             (const u8*)(data.empty() ? "" : data.data()), data.size(), out);
    return to_hex(out, 32);
}

// ============================================================
// 3. Base64
// ============================================================
static const char* B64C = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const u8* p, size_t n) {
    std::string s;
    s.reserve(((n + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        u32 v = ((u32)p[i] << 16) | ((u32)p[i + 1] << 8) | p[i + 2];
        s.push_back(B64C[(v >> 18) & 63]);
        s.push_back(B64C[(v >> 12) & 63]);
        s.push_back(B64C[(v >> 6) & 63]);
        s.push_back(B64C[v & 63]);
    }
    if (i + 1 == n) {
        u32 v = (u32)p[i] << 16;
        s.push_back(B64C[(v >> 18) & 63]);
        s.push_back(B64C[(v >> 12) & 63]);
        s.push_back('=');
        s.push_back('=');
    } else if (i + 2 == n) {
        u32 v = ((u32)p[i] << 16) | ((u32)p[i + 1] << 8);
        s.push_back(B64C[(v >> 18) & 63]);
        s.push_back(B64C[(v >> 12) & 63]);
        s.push_back(B64C[(v >> 6) & 63]);
        s.push_back('=');
    }
    return s;
}

bool base64_decode(const std::string& s, std::vector<u8>& out) {
    int rev[256];
    for (int i = 0; i < 256; ++i) rev[i] = -1;
    for (int i = 0; i < 64; ++i) rev[(u8)B64C[i]] = i;
    out.clear();
    u32 acc = 0;
    int bits = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        int v = rev[(u8)c];
        if (v < 0) return false;
        acc = (acc << 6) | (u32)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((u8)((acc >> bits) & 0xff));
        }
    }
    return true;
}

// ============================================================
// 4. 256 位整数 / 有限域运算
// ============================================================
namespace {

// p = 2^256 - 2^32 - 977
const u32 P[8] = {0xFFFFFC2Fu, 0xFFFFFFFEu, 0xFFFFFFFFu, 0xFFFFFFFFu,
                  0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
// 曲线阶 n
const u32 N[8] = {0xD0364141u, 0xBFD25E8Cu, 0xAF48A03Bu, 0xBAAEDCE6u,
                  0xFFFFFFFEu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
// 常用小常量
const u32 TWO8[8] = {2, 0, 0, 0, 0, 0, 0, 0};
const u32 SEVEN8[8] = {7, 0, 0, 0, 0, 0, 0, 0};

// 生成元 G
const u32 GX[8] = {0x16F81798u, 0x59F2815Bu, 0x2DCE28D9u, 0x029BFCDBu,
                   0xCE870B07u, 0x55A06295u, 0xF9DCBBACu, 0x79BE667Eu};
const u32 GY[8] = {0xFB10D4B8u, 0x9C47D08Fu, 0xA6855419u, 0xFD17B448u,
                   0x0E1108A8u, 0x5DA4FBFCu, 0x26A3C465u, 0x483ADA77u};

inline int cmp8(const u32 a[8], const u32 b[8]) {
    for (int i = 7; i >= 0; --i)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}
inline bool is_zero8(const u32 a[8]) {
    u32 o = 0;
    for (int i = 0; i < 8; ++i) o |= a[i];
    return o == 0;
}
inline bool is_one8(const u32 a[8]) {
    if (a[0] != 1) return false;
    for (int i = 1; i < 8; ++i)
        if (a[i]) return false;
    return true;
}
inline u32 add8(u32 r[8], const u32 a[8], const u32 b[8]) {
    u64 carry = 0;
    for (int i = 0; i < 8; ++i) {
        u64 cur = (u64)a[i] + b[i] + carry;
        r[i] = (u32)cur;
        carry = cur >> 32;
    }
    return (u32)carry;
}
inline u32 sub8(u32 r[8], const u32 a[8], const u32 b[8]) { // 返回借位
    u64 borrow = 0;
    for (int i = 0; i < 8; ++i) {
        u64 cur = (u64)a[i] - b[i] - borrow;
        r[i] = (u32)cur;
        borrow = (cur >> 63) & 1;
    }
    return (u32)borrow;
}

// 512 位乘积（8x8 32 位 limb）
inline void mul512(u32 t[16], const u32 a[8], const u32 b[8]) {
    memset(t, 0, sizeof(u32) * 16);
    for (int i = 0; i < 8; ++i) {
        u64 carry = 0;
        for (int j = 0; j < 8; ++j) {
            u64 cur = (u64)a[i] * b[j] + t[i + j] + carry;
            t[i + j] = (u32)cur;
            carry = cur >> 32;
        }
        t[i + 8] = (u32)carry;
    }
}

// 利用 p = 2^256 - c (c = 2^32 + 977) 快速归约 512 位到 mod p
inline void reduce_p(u32 r[8], const u32 t[16]) {
    u32 acc[10];
    for (int i = 0; i < 8; ++i) acc[i] = t[i];
    acc[8] = acc[9] = 0;

    // hc = H * c = H*977 + (H<<32)
    u32 hc[10];
    memset(hc, 0, sizeof(hc));
    u64 carry = 0;
    for (int i = 0; i < 8; ++i) {
        u64 cur = (u64)t[8 + i] * 977ULL + carry;
        hc[i] = (u32)cur;
        carry = cur >> 32;
    }
    hc[8] = (u32)carry;
    carry = 0;
    for (int i = 0; i < 8; ++i) {
        u64 cur = (u64)hc[i + 1] + t[8 + i] + carry;
        hc[i + 1] = (u32)cur;
        carry = cur >> 32;
    }
    {
        u64 cur = (u64)hc[9] + carry;
        hc[9] = (u32)cur;
    }
    // acc += hc
    carry = 0;
    for (int i = 0; i < 10; ++i) {
        u64 cur = (u64)acc[i] + hc[i] + carry;
        acc[i] = (u32)cur;
        carry = cur >> 32;
    }

    // 反复折叠高位（最多 2~3 次）
    while (acc[8] || acc[9]) {
        u64 h = (u64)acc[8] | ((u64)acc[9] << 32);
        acc[8] = acc[9] = 0;
        u64 p1 = h * 977ULL;                 // < 2^44
        u64 c2 = 0;
        u64 cur = (u64)acc[0] + (p1 & 0xFFFFFFFFULL);
        acc[0] = (u32)cur;
        c2 = cur >> 32;
        cur = (u64)acc[1] + ((p1 >> 32) & 0xFFFFFFFFULL) + c2;
        acc[1] = (u32)cur;
        c2 = cur >> 32;
        for (int i = 2; i < 10 && c2; ++i) {
            cur = (u64)acc[i] + c2;
            acc[i] = (u32)cur;
            c2 = cur >> 32;
        }
        // acc += h << 32
        u64 c3 = 0;
        for (int i = 0; i < 10; ++i) {
            u64 add = 0;
            if (i == 1) add = h & 0xFFFFFFFFULL;
            else if (i == 2) add = (h >> 32) & 0xFFFFFFFFULL;
            u64 s = (u64)acc[i] + add + c3;
            acc[i] = (u32)s;
            c3 = s >> 32;
        }
    }

    u32 v[8];
    memcpy(v, acc, 32);
    while (cmp8(v, P) >= 0) sub8(v, v, P);
    memcpy(r, v, 32);
}

inline void fmul(u32 r[8], const u32 a[8], const u32 b[8]) {
    u32 t[16];
    mul512(t, a, b);
    reduce_p(r, t);
}

inline void fsqr(u32 r[8], const u32 a[8]) { fmul(r, a, a); }

inline void fadd(u32 r[8], const u32 a[8], const u32 b[8]) {
    u32 t[8];
    u32 c = add8(t, a, b);
    if (c || cmp8(t, P) >= 0) sub8(t, t, P);
    memcpy(r, t, 32);
}

inline void fsub(u32 r[8], const u32 a[8], const u32 b[8]) {
    u32 t[8];
    u32 borrow = sub8(t, a, b);
    if (borrow) add8(t, t, P);
    memcpy(r, t, 32);
}

inline void fneg(u32 r[8], const u32 a[8]) {
    if (is_zero8(a)) { memset(r, 0, 32); return; }
    sub8(r, P, a);
}

// 模 p 求逆（费马小定理 a^(p-2)）
void finv(u32 r[8], const u32 a[8]) {
    u32 e[8];
    memcpy(e, P, 32);
    // p - 2
    e[0] = 0xFFFFFC2Du;
    u32 res[8] = {1, 0, 0, 0, 0, 0, 0, 0};
    u32 base[8];
    memcpy(base, a, 32);
    for (int i = 0; i < 256; ++i) {
        if ((e[i >> 5] >> (i & 31)) & 1) fmul(res, res, base);
        fsqr(base, base);
    }
    memcpy(r, res, 32);
}

// 通用模 n 乘法：先取 512 位乘积，再逐位归约
inline void mulmod_n(u32 r[8], const u32 a[8], const u32 b[8]) {
    u32 t[16];
    mul512(t, a, b);
    u32 rem[9];
    memset(rem, 0, sizeof(rem));
    u32 nn[9];
    memcpy(nn, N, 32);
    nn[8] = 0;
    for (int bit = 511; bit >= 0; --bit) {
        u32 inbit = (t[bit >> 5] >> (bit & 31)) & 1u;
        u32 carry = inbit;
        for (int i = 0; i < 9; ++i) {
            u32 nc = rem[i] >> 31;
            rem[i] = (rem[i] << 1) | carry;
            carry = nc;
        }
        if (rem[8] || cmp8(rem, nn) >= 0) {
            u64 borrow = 0;
            for (int i = 0; i < 9; ++i) {
                u64 cur = (u64)rem[i] - nn[i] - borrow;
                rem[i] = (u32)cur;
                borrow = (cur >> 63) & 1;
            }
        }
    }
    memcpy(r, rem, 32);
}

void modinv_n(u32 r[8], const u32 a[8]) {
    u32 e[8];
    memcpy(e, N, 32);
    // n - 2
    sub8(e, e, TWO8);
    u32 res[8] = {1, 0, 0, 0, 0, 0, 0, 0};
    u32 base[8];
    memcpy(base, a, 32);
    for (int i = 0; i < 256; ++i) {
        if ((e[i >> 5] >> (i & 31)) & 1) mulmod_n(res, res, base);
        mulmod_n(base, base, base);
    }
    memcpy(r, res, 32);
}

// ---------------- 椭圆曲线点运算（Jacobian） ----------------
struct Jac {
    u32 x[8], y[8], z[8];
};

inline void jac_inf(Jac& p) {
    memset(p.x, 0, 32);
    memset(p.y, 0, 32);
    memset(p.z, 0, 32);
}

void jac_double(Jac& r, const Jac& p) {
    if (is_zero8(p.z) || is_zero8(p.y)) { jac_inf(r); return; }
    u32 A[8], B[8], C[8], D[8], E[8], F[8], t1[8], t2[8], X3[8], Y3[8], Z3[8];
    fsqr(A, p.x);                        // A = X^2
    fsqr(B, p.y);                        // B = Y^2
    fsqr(C, B);                          // C = B^2
    fadd(t1, p.x, B);
    fsqr(t1, t1);
    fsub(t1, t1, A);
    fsub(t1, t1, C);
    fadd(D, t1, t1);                     // D = 2*((X+B)^2 - A - C)
    fadd(E, A, A);
    fadd(E, E, A);                       // E = 3A
    fsqr(F, E);                          // F = E^2
    fadd(t1, D, D);
    fsub(X3, F, t1);                     // X3 = F - 2D
    fsub(t1, D, X3);
    fmul(t1, E, t1);
    fadd(t2, C, C);
    fadd(t2, t2, t2);
    fadd(t2, t2, t2);                    // 8C
    fsub(Y3, t1, t2);                    // Y3 = E*(D-X3) - 8C
    fmul(t1, p.y, p.z);
    fadd(Z3, t1, t1);                    // Z3 = 2*Y*Z
    memcpy(r.x, X3, 32);
    memcpy(r.y, Y3, 32);
    memcpy(r.z, Z3, 32);
}

void jac_add(Jac& r, const Jac& a, const Jac& b) {
    if (is_zero8(a.z)) { r = b; return; }
    if (is_zero8(b.z)) { r = a; return; }
    u32 Z1Z1[8], Z2Z2[8], U1[8], U2[8], S1[8], S2[8], H[8], R[8];
    u32 HH[8], HHH[8], V[8], t1[8], t2[8], X3[8], Y3[8], Z3[8];
    fsqr(Z1Z1, a.z);
    fsqr(Z2Z2, b.z);
    fmul(U1, a.x, Z2Z2);
    fmul(U2, b.x, Z1Z1);
    fmul(t1, b.z, Z2Z2);
    fmul(S1, a.y, t1);
    fmul(t2, a.z, Z1Z1);
    fmul(S2, b.y, t2);
    fsub(H, U2, U1);
    fsub(R, S2, S1);
    if (is_zero8(H)) {
        if (is_zero8(R)) { jac_double(r, a); return; }
        jac_inf(r);
        return;
    }
    fsqr(HH, H);
    fmul(HHH, H, HH);
    fmul(V, U1, HH);
    fsqr(t1, R);
    fsub(t1, t1, HHH);
    fsub(t1, t1, V);
    fsub(X3, t1, V);                     // X3 = R^2 - HHH - 2V
    fsub(t1, V, X3);
    fmul(t1, R, t1);
    fmul(t2, S1, HHH);
    fsub(Y3, t1, t2);
    fmul(t1, a.z, b.z);
    fmul(Z3, t1, H);
    memcpy(r.x, X3, 32);
    memcpy(r.y, Y3, 32);
    memcpy(r.z, Z3, 32);
}

void jac_mul(Jac& r, const u32 k[8], const Jac& p) {
    Jac acc;
    jac_inf(acc);
    for (int i = 255; i >= 0; --i) {
        jac_double(acc, acc);
        if ((k[i >> 5] >> (i & 31)) & 1) jac_add(acc, acc, p);
    }
    r = acc;
}

bool jac_to_affine(u32 x[8], u32 y[8], const Jac& p) {
    if (is_zero8(p.z)) return false;
    u32 zi[8], zi2[8], zi3[8];
    finv(zi, p.z);
    fsqr(zi2, zi);
    fmul(zi3, zi2, zi);
    fmul(x, p.x, zi2);
    fmul(y, p.y, zi3);
    return true;
}

Jac g_point() {
    Jac g;
    memcpy(g.x, GX, 32);
    memcpy(g.y, GY, 32);
    memset(g.z, 0, 32);
    g.z[0] = 1;
    return g;
}

void bytes_to_u32(u32 r[8], const u8 b[32]) { // 大端
    for (int i = 0; i < 8; ++i) {
        int o = (7 - i) * 4;
        r[i] = ((u32)b[o] << 24) | ((u32)b[o + 1] << 16) | ((u32)b[o + 2] << 8) | b[o + 3];
    }
}
void u32_to_bytes(u8 b[32], const u32 a[8]) {
    for (int i = 0; i < 8; ++i) {
        int o = (7 - i) * 4;
        b[o] = (u8)(a[i] >> 24);
        b[o + 1] = (u8)(a[i] >> 16);
        b[o + 2] = (u8)(a[i] >> 8);
        b[o + 3] = (u8)a[i];
    }
}

// 熵源：优先 Windows CryptoAPI，失败则混合多源
void random_bytes(u8* out, size_t n) {
#ifdef _WIN32
    HCRYPTPROV hProv = 0;
    if (CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        if (CryptGenRandom(hProv, (DWORD)n, out)) {
            CryptReleaseContext(hProv, 0);
            return;
        }
        CryptReleaseContext(hProv, 0);
    }
#endif
    // 退路：多源混合（时间、计数器、栈地址、进程号）
    static u32 counter = 0;
    u8 seed[64];
    u64 t = (u64)time(NULL);
    u64 c = (u64)clock();
    u64 addr = (u64)(uintptr_t)&seed;
#ifdef _WIN32
    u64 tick = (u64)GetTickCount();
#else
    u64 tick = 0;
    FILE* f = fopen("/dev/urandom", "rb");
    if (f) {
        size_t got = fread(out, 1, n, f);
        fclose(f);
        if (got == n) return;
    }
#endif
    size_t pos = 0;
    struct Put {
        static void go(u8* buf, size_t& p, const void* src, size_t k) {
            if (p + k <= 64) { memcpy(buf + p, src, k); p += k; }
        }
    };
    for (int round = 0; round < 8 && pos < 64; ++round) {
        Put::go(seed, pos, &t, 8);
        Put::go(seed, pos, &c, 8);
        Put::go(seed, pos, &addr, 8);
        Put::go(seed, pos, &tick, 8);
        Put::go(seed, pos, &counter, 4);
        counter++;
    }
    u8 h[32];
    sha256(seed, 64, h);
    for (size_t i = 0; i < n; ++i) out[i] = h[i % 32] ^ (u8)(i * 31);
    // 二次扩散
    u8 h2[32];
    sha256(out, n > 32 ? 32 : n, h2);
    for (size_t i = 0; i < n && i < 32; ++i) out[i] ^= h2[i];
}

} // namespace

// ============================================================
// 5. ECDSA
// ============================================================
namespace ecc {

std::string address_of(const std::string& public_key_hex) {
    std::vector<u8> pk;
    if (!from_hex(public_key_hex, pk)) return "";
    u8 h[32];
    sha256(pk.empty() ? (const u8*)"" : &pk[0], pk.size(), h);
    return "0x" + to_hex(h, 20);
}

bool derive_public(const std::string& private_key_hex, std::string& public_key_hex, std::string& address) {
    std::vector<u8> d;
    if (!from_hex(private_key_hex, d) || d.size() != 32) return false;
    u32 k[8];
    bytes_to_u32(k, &d[0]);
    if (is_zero8(k) || cmp8(k, N) >= 0) return false;
    Jac g = g_point(), r;
    jac_mul(r, k, g);
    u32 ax[8], ay[8];
    if (!jac_to_affine(ax, ay, r)) return false;
    u8 buf[64];
    u32_to_bytes(buf, ax);
    u32_to_bytes(buf + 32, ay);
    public_key_hex = to_hex(buf, 64);
    address = address_of(public_key_hex);
    return true;
}

KeyPair generate() {
    KeyPair kp;
    for (int attempt = 0; attempt < 32; ++attempt) {
        u8 buf[32];
        random_bytes(buf, 32);
        u32 d[8];
        bytes_to_u32(d, buf);
        if (is_zero8(d) || cmp8(d, N) >= 0) continue;
        kp.private_key = to_hex(buf, 32);
        if (derive_public(kp.private_key, kp.public_key, kp.address)) return kp;
    }
    return kp;
}

// RFC6979 确定性 k
static void rfc6979_k(u32 kout[8], const u8 priv[32], const u8 hash[32]) {
    u8 V[32], K[32];
    memset(V, 0x01, 32);
    memset(K, 0x00, 32);

    std::string data;
    data.reserve(32 + 1 + 32 + 32);
    data.assign((const char*)V, 32);
    data.push_back(0x00);
    data.append((const char*)priv, 32);
    data.append((const char*)hash, 32);
    {
        std::string t = hmac_sha256(std::string((const char*)K, 32), data);
        memcpy(K, t.data(), 32);
        std::string v2 = hmac_sha256(std::string((const char*)K, 32), std::string((const char*)V, 32));
        memcpy(V, v2.data(), 32);
    }
    data.assign((const char*)V, 32);
    data.push_back(0x01);
    data.append((const char*)priv, 32);
    data.append((const char*)hash, 32);
    {
        std::string t = hmac_sha256(std::string((const char*)K, 32), data);
        memcpy(K, t.data(), 32);
        std::string v2 = hmac_sha256(std::string((const char*)K, 32), std::string((const char*)V, 32));
        memcpy(V, v2.data(), 32);
    }
    for (int tries = 0; tries < 1024; ++tries) {
        std::string v2 = hmac_sha256(std::string((const char*)K, 32), std::string((const char*)V, 32));
        memcpy(V, v2.data(), 32);
        bytes_to_u32(kout, V);
        if (!is_zero8(kout) && cmp8(kout, N) < 0) return;
        std::string d2((const char*)V, 32);
        d2.push_back(0x00);
        std::string t = hmac_sha256(std::string((const char*)K, 32), d2);
        memcpy(K, t.data(), 32);
        std::string v3 = hmac_sha256(std::string((const char*)K, 32), std::string((const char*)V, 32));
        memcpy(V, v3.data(), 32);
    }
}

std::string sign(const std::string& msg, const std::string& private_key_hex) {
    std::vector<u8> dv;
    if (!from_hex(private_key_hex, dv) || dv.size() != 32) return "";
    u32 d[8];
    bytes_to_u32(d, &dv[0]);
    if (is_zero8(d) || cmp8(d, N) >= 0) return "";

    u8 hash[32];
    sha256((const u8*)msg.data(), msg.size(), hash);
    u32 e[8];
    bytes_to_u32(e, hash);
    if (cmp8(e, N) >= 0) sub8(e, e, N);

    for (int attempt = 0; attempt < 8; ++attempt) {
        u32 k[8];
        if (attempt == 0) {
            rfc6979_k(k, &dv[0], hash);
        } else {
            // 极端情况：k 无效时重新派生
            std::string tweak = std::string((const char*)hash, 32) + (char)attempt;
            u8 h2[32];
            sha256((const u8*)tweak.data(), tweak.size(), h2);
            rfc6979_k(k, &dv[0], h2);
        }
        if (is_zero8(k) || cmp8(k, N) >= 0) continue;
        Jac g = g_point(), rp;
        jac_mul(rp, k, g);
        u32 rx[8], ry[8];
        if (!jac_to_affine(rx, ry, rp)) continue;
        u32 r[8];
        memcpy(r, rx, 32);
        if (cmp8(r, N) >= 0) sub8(r, r, N);
        if (is_zero8(r)) continue;

        // s = k^-1 * (e + r*d) mod n
        u32 kinv[8], rd[8], sum[8], s[8];
        modinv_n(kinv, k);
        mulmod_n(rd, r, d);
        // sum = (e + rd) mod n
        u32 carry = add8(sum, e, rd);
        if (carry || cmp8(sum, N) >= 0) sub8(sum, sum, N);
        mulmod_n(s, kinv, sum);
        if (is_zero8(s)) continue;

        // low-s 规范化
        u32 neg[8];
        sub8(neg, N, s);
        if (cmp8(neg, s) < 0) memcpy(s, neg, 32);

        u8 out[64];
        u32_to_bytes(out, r);
        u32_to_bytes(out + 32, s);
        return base64_encode(out, 64);
    }
    return "";
}

bool verify(const std::string& msg, const std::string& signature_b64, const std::string& public_key_hex) {
    std::vector<u8> sig;
    if (!base64_decode(signature_b64, sig) || sig.size() != 64) return false;
    std::vector<u8> pk;
    if (!from_hex(public_key_hex, pk) || pk.size() != 64) return false;

    u32 r[8], s[8];
    bytes_to_u32(r, &sig[0]);
    bytes_to_u32(s, &sig[32]);
    if (is_zero8(r) || is_zero8(s)) return false;
    if (cmp8(r, N) >= 0 || cmp8(s, N) >= 0) return false;

    u32 qx[8], qy[8];
    bytes_to_u32(qx, &pk[0]);
    bytes_to_u32(qy, &pk[32]);
    if (is_zero8(qx) && is_zero8(qy)) return false;
    if (cmp8(qx, P) >= 0 || cmp8(qy, P) >= 0) return false;
    // 检查点在曲线上：y^2 = x^3 + 7
    {
        u32 y2[8], x3[8], rhs[8];
        fsqr(y2, qy);
        fsqr(x3, qx);
        fmul(x3, x3, qx);
        fadd(rhs, x3, SEVEN8);
        if (cmp8(y2, rhs) != 0) return false;
    }

    u8 hash[32];
    sha256((const u8*)msg.data(), msg.size(), hash);
    u32 e[8];
    bytes_to_u32(e, hash);
    if (cmp8(e, N) >= 0) sub8(e, e, N);

    u32 w[8], u1[8], u2[8];
    modinv_n(w, s);
    mulmod_n(u1, e, w);
    mulmod_n(u2, r, w);

    Jac g = g_point(), q, p1, p2, sum;
    memcpy(q.x, qx, 32);
    memcpy(q.y, qy, 32);
    memset(q.z, 0, 32);
    q.z[0] = 1;
    jac_mul(p1, u1, g);
    jac_mul(p2, u2, q);
    jac_add(sum, p1, p2);
    u32 ax[8], ay[8];
    if (!jac_to_affine(ax, ay, sum)) return false;
    u32 rx[8];
    memcpy(rx, ax, 32);
    if (cmp8(rx, N) >= 0) sub8(rx, rx, N);
    return cmp8(rx, r) == 0;
}

bool self_test(std::string& err) {
    // SHA-256("abc")
    if (sha256_hex("abc") != "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") {
        err = "sha256 vector failed";
        return false;
    }
    if (sha256_hex("") != "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") {
        err = "sha256 empty vector failed";
        return false;
    }
    // HMAC-SHA256 RFC4231 case 2
    if (hmac_sha256_hex("Jefe", "what do ya want for nothing?") !=
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843") {
        err = "hmac vector failed";
        return false;
    }
    // 已知私钥 -> 公钥（secp256k1 标准测试向量）
    {
        std::string pk, addr;
        if (!derive_public("0000000000000000000000000000000000000000000000000000000000000001", pk, addr)) {
            err = "derive_public failed";
            return false;
        }
        if (pk != "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"
                  "483ada7726a3c4655da4fbfc0e1108a8fd17b448a68554199c47d08ffb10d4b8") {
            err = "pubkey for k=1 mismatch: " + pk;
            return false;
        }
    }
    // 签名/验签一致性
    {
        KeyPair kp = generate();
        if (kp.private_key.empty()) { err = "keygen failed"; return false; }
        std::string sig = sign("hello blockchain", kp.private_key);
        if (sig.empty()) { err = "sign failed"; return false; }
        if (!verify("hello blockchain", sig, kp.public_key)) { err = "verify failed"; return false; }
        if (verify("hello blockchain!", sig, kp.public_key)) { err = "verify false-positive"; return false; }
    }
    return true;
}

} // namespace ecc
} // namespace bc
