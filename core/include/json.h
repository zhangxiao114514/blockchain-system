// json.h — 极简 JSON DOM（无第三方依赖），支持对象/数组/字符串/整数/浮点/布尔/null
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace bc {

class J {
public:
    enum Type { NUL = 0, BOOL, INT, DBL, STR, ARR, OBJ };

    J() : type_(NUL), b_(false), i_(0), d_(0) {}
    static J obj();
    static J arr();
    static J str(const std::string& s);
    static J num(int64_t v);
    static J dbl(double v);
    static J boolean(bool v);

    Type type() const { return type_; }
    bool is_null() const { return type_ == NUL; }
    bool is_obj() const { return type_ == OBJ; }
    bool is_arr() const { return type_ == ARR; }
    bool is_num() const { return type_ == INT || type_ == DBL; }

    // 对象访问（不存在则创建）
    J& operator[](const std::string& k);
    const J* find(const std::string& k) const;
    bool has(const std::string& k) const;
    void set(const std::string& k, const J& v);
    void erase(const std::string& k);
    std::vector<std::string> keys() const;

    // 数组访问
    void push(const J& v);
    const J& at(size_t i) const;
    J& at(size_t i);
    size_t size() const;

    // 取值（带默认值，类型不符返回默认）
    std::string s(const std::string& def = "") const;
    int64_t i(int64_t def = 0) const;
    double d(double def = 0) const;
    bool b(bool def = false) const;

    std::string dump() const;
    static bool parse(const std::string& text, J& out, std::string& err);

private:
    void dump_to(std::string& out) const;
    Type type_;
    bool b_;
    int64_t i_;
    double d_;
    std::string s_;
    std::vector<J> arr_;
    std::vector<std::pair<std::string, J> > obj_;
};

} // namespace bc
