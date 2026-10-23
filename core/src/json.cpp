// json.cpp — JSON 解析与序列化实现
#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace bc {

J J::obj() { J j; j.type_ = OBJ; return j; }
J J::arr() { J j; j.type_ = ARR; return j; }
J J::str(const std::string& s) { J j; j.type_ = STR; j.s_ = s; return j; }
J J::num(int64_t v) { J j; j.type_ = INT; j.i_ = v; return j; }
J J::dbl(double v) { J j; j.type_ = DBL; j.d_ = v; return j; }
J J::boolean(bool v) { J j; j.type_ = BOOL; j.b_ = v; return j; }

J& J::operator[](const std::string& k) {
    if (type_ != OBJ) { type_ = OBJ; }
    for (size_t i = 0; i < obj_.size(); ++i)
        if (obj_[i].first == k) return obj_[i].second;
    obj_.push_back(std::make_pair(k, J()));
    return obj_.back().second;
}

const J* J::find(const std::string& k) const {
    if (type_ != OBJ) return 0;
    for (size_t i = 0; i < obj_.size(); ++i)
        if (obj_[i].first == k) return &obj_[i].second;
    return 0;
}

bool J::has(const std::string& k) const { return find(k) != 0; }

void J::set(const std::string& k, const J& v) { (*this)[k] = v; }

void J::erase(const std::string& k) {
    for (size_t i = 0; i < obj_.size(); ++i)
        if (obj_[i].first == k) { obj_.erase(obj_.begin() + i); return; }
}

std::vector<std::string> J::keys() const {
    std::vector<std::string> r;
    for (size_t i = 0; i < obj_.size(); ++i) r.push_back(obj_[i].first);
    return r;
}

void J::push(const J& v) {
    if (type_ != ARR) { type_ = ARR; arr_.clear(); }
    arr_.push_back(v);
}

const J& J::at(size_t i) const {
    static J nullv;
    if (type_ != ARR || i >= arr_.size()) return nullv;
    return arr_[i];
}

J& J::at(size_t i) {
    static J nullv;
    if (type_ != ARR || i >= arr_.size()) return nullv;
    return arr_[i];
}

size_t J::size() const {
    if (type_ == ARR) return arr_.size();
    if (type_ == OBJ) return obj_.size();
    return 0;
}

std::string J::s(const std::string& def) const { return type_ == STR ? s_ : def; }
int64_t J::i(int64_t def) const {
    if (type_ == INT) return i_;
    if (type_ == DBL) return (int64_t)d_;
    return def;
}
double J::d(double def) const {
    if (type_ == DBL) return d_;
    if (type_ == INT) return (double)i_;
    return def;
}
bool J::b(bool def) const { return type_ == BOOL ? b_ : def; }

// ---------------- 序列化 ----------------
static void escape_to(std::string& out, const std::string& s) {
    out.push_back('"');
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back((char)c);
                }
        }
    }
    out.push_back('"');
}

static void put_i64(std::string& out, int64_t v) {
    if (v < 0) {
        out.push_back('-');
        uint64_t u = (uint64_t)(-(v + 1)) + 1ULL;
        char buf[24];
        int n = 0;
        do { buf[n++] = (char)('0' + (int)(u % 10)); u /= 10; } while (u);
        while (n) out.push_back(buf[--n]);
    } else {
        uint64_t u = (uint64_t)v;
        char buf[24];
        int n = 0;
        do { buf[n++] = (char)('0' + (int)(u % 10)); u /= 10; } while (u);
        while (n) out.push_back(buf[--n]);
    }
}

void J::dump_to(std::string& out) const {
    switch (type_) {
        case NUL: out += "null"; break;
        case BOOL: out += b_ ? "true" : "false"; break;
        case INT: put_i64(out, i_); break;
        case DBL: {
            if (d_ == (double)(int64_t)d_ && std::fabs(d_) < 9e15) {
                put_i64(out, (int64_t)d_);
            } else {
                char buf[40];
                snprintf(buf, sizeof(buf), "%.10g", d_);
                out += buf;
            }
            break;
        }
        case STR: escape_to(out, s_); break;
        case ARR: {
            out.push_back('[');
            for (size_t i = 0; i < arr_.size(); ++i) {
                if (i) out.push_back(',');
                arr_[i].dump_to(out);
            }
            out.push_back(']');
            break;
        }
        case OBJ: {
            out.push_back('{');
            for (size_t i = 0; i < obj_.size(); ++i) {
                if (i) out.push_back(',');
                escape_to(out, obj_[i].first);
                out.push_back(':');
                obj_[i].second.dump_to(out);
            }
            out.push_back('}');
            break;
        }
    }
}

std::string J::dump() const {
    std::string out;
    out.reserve(256);
    dump_to(out);
    return out;
}

// ---------------- 解析 ----------------
namespace {
struct Parser {
    const std::string& t;
    size_t p;
    std::string err;
    Parser(const std::string& s) : t(s), p(0) {}

    void ws() {
        while (p < t.size()) {
            char c = t[p];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++p;
            else break;
        }
    }
    bool fail(const std::string& m) {
        if (err.empty()) {
            char buf[128];
            snprintf(buf, sizeof(buf), "%s at %d", m.c_str(), (int)p);
            err = buf;
        }
        return false;
    }
    static void utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out.push_back((char)cp);
        else if (cp < 0x800) {
            out.push_back((char)(0xC0 | (cp >> 6)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back((char)(0xE0 | (cp >> 12)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            out.push_back((char)(0xF0 | (cp >> 18)));
            out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        }
    }
    bool parse_string(std::string& out) {
        if (p >= t.size() || t[p] != '"') return fail("expect string");
        ++p;
        out.clear();
        while (p < t.size()) {
            char c = t[p++];
            if (c == '"') return true;
            if (c == '\\') {
                if (p >= t.size()) return fail("bad escape");
                char e = t[p++];
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        if (p + 4 > t.size()) return fail("bad \\u");
                        unsigned cp = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = t[p++];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                            else return fail("bad hex");
                        }
                        // 代理对
                        if (cp >= 0xD800 && cp <= 0xDBFF && p + 6 <= t.size() && t[p] == '\\' && t[p + 1] == 'u') {
                            p += 2;
                            unsigned lo = 0;
                            for (int i = 0; i < 4; ++i) {
                                char h = t[p++];
                                lo <<= 4;
                                if (h >= '0' && h <= '9') lo |= (unsigned)(h - '0');
                                else if (h >= 'a' && h <= 'f') lo |= (unsigned)(h - 'a' + 10);
                                else if (h >= 'A' && h <= 'F') lo |= (unsigned)(h - 'A' + 10);
                                else return fail("bad hex");
                            }
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                        utf8(out, cp);
                        break;
                    }
                    default: return fail("bad escape char");
                }
            } else {
                out.push_back(c);
            }
        }
        return fail("unterminated string");
    }

    bool parse_value(J& out) {
        ws();
        if (p >= t.size()) return fail("unexpected end");
        char c = t[p];
        if (c == '{') {
            ++p;
            out = J::obj();
            ws();
            if (p < t.size() && t[p] == '}') { ++p; return true; }
            while (true) {
                ws();
                std::string key;
                if (!parse_string(key)) return false;
                ws();
                if (p >= t.size() || t[p] != ':') return fail("expect ':'");
                ++p;
                J v;
                if (!parse_value(v)) return false;
                out.set(key, v);
                ws();
                if (p < t.size() && t[p] == ',') { ++p; continue; }
                if (p < t.size() && t[p] == '}') { ++p; return true; }
                return fail("expect ',' or '}'");
            }
        }
        if (c == '[') {
            ++p;
            out = J::arr();
            ws();
            if (p < t.size() && t[p] == ']') { ++p; return true; }
            while (true) {
                J v;
                if (!parse_value(v)) return false;
                out.push(v);
                ws();
                if (p < t.size() && t[p] == ',') { ++p; continue; }
                if (p < t.size() && t[p] == ']') { ++p; return true; }
                return fail("expect ',' or ']'");
            }
        }
        if (c == '"') {
            std::string s;
            if (!parse_string(s)) return false;
            out = J::str(s);
            return true;
        }
        if (t.compare(p, 4, "true") == 0) { p += 4; out = J::boolean(true); return true; }
        if (t.compare(p, 5, "false") == 0) { p += 5; out = J::boolean(false); return true; }
        if (t.compare(p, 4, "null") == 0) { p += 4; out = J(); return true; }
        // 数字
        {
            size_t start = p;
            if (p < t.size() && (t[p] == '-' || t[p] == '+')) ++p;
            bool isdbl = false;
            while (p < t.size()) {
                char d = t[p];
                if (d >= '0' && d <= '9') { ++p; continue; }
                if (d == '.' || d == 'e' || d == 'E' || d == '-' || d == '+') { isdbl = true; ++p; continue; }
                break;
            }
            if (p == start) return fail("expect value");
            std::string num = t.substr(start, p - start);
            if (!isdbl) {
                out = J::num((int64_t)strtoll(num.c_str(), 0, 10));
            } else {
                out = J::dbl(strtod(num.c_str(), 0));
            }
            return true;
        }
    }
};
} // namespace

bool J::parse(const std::string& text, J& out, std::string& err) {
    Parser ps(text);
    if (!ps.parse_value(out)) { err = ps.err; return false; }
    ps.ws();
    if (ps.p != text.size()) { err = "trailing garbage"; return false; }
    return true;
}

} // namespace bc
