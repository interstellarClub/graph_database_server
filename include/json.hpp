#ifndef MINIJSON_JSON_HPP
#define MINIJSON_JSON_HPP

// ---------------------------------------------------------------------------
// minijson —— 单头文件、零第三方依赖的 C++17 JSON 解析 / 序列化库
//
// 支持：null / bool / 整数 / 浮点 / 字符串 / 数组 / 对象
//       * 严格的递归下降解析（符合 RFC 8259，含 \uXXXX 与代理对）
//       * 紧凑输出与缩进美化输出
//       * 保留对象键的插入顺序
// ---------------------------------------------------------------------------

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace minijson {

class Json;

class JsonError : public std::runtime_error {
public:
    explicit JsonError(const std::string& msg) : std::runtime_error(msg) {}
};

class Json {
public:
    using Array = std::vector<Json>;
    using Object = std::vector<std::pair<std::string, Json>>;

    enum class Type { Null, Bool, Int, Real, String, Array, Object };

    // ---------------- 构造 ----------------
    Json() : type_(Type::Null) {}
    Json(std::nullptr_t) : type_(Type::Null) {}
    Json(bool v) : type_(Type::Bool) { bool_ = v; }

    template <typename T,
              typename std::enable_if<std::is_integral<T>::value && !std::is_same<T, bool>::value, int>::type = 0>
    Json(T v) : type_(Type::Int) { int_ = static_cast<long long>(v); }

    template <typename T,
              typename std::enable_if<std::is_floating_point<T>::value, int>::type = 0>
    Json(T v) : type_(Type::Real) { real_ = static_cast<double>(v); }

    Json(const char* v) : type_(Type::String) { str_ = v ? std::string(v) : std::string(); }
    Json(const std::string& v) : type_(Type::String), str_(v) {}
    Json(std::string&& v) : type_(Type::String), str_(std::move(v)) {}
    Json(const Array& v) : type_(Type::Array), arr_(v) {}
    Json(Array&& v) : type_(Type::Array), arr_(std::move(v)) {}
    Json(const Object& v) : type_(Type::Object), obj_(v) {}
    Json(Object&& v) : type_(Type::Object), obj_(std::move(v)) {}

    static Json object() { return Json(Object{}); }
    static Json array() { return Json(Array{}); }

    // ---------------- 类型判断 ----------------
    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isInt() const { return type_ == Type::Int; }
    bool isReal() const { return type_ == Type::Real; }
    bool isNumber() const { return type_ == Type::Int || type_ == Type::Real; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    // ---------------- 取值（类型不符时抛 JsonError） ----------------
    bool asBool() const {
        require(Type::Bool);
        return bool_;
    }
    long long asInt() const {
        if (type_ == Type::Int) return int_;
        if (type_ == Type::Real) return static_cast<long long>(real_);
        require(Type::Int);
        return 0;
    }
    double asDouble() const {
        if (type_ == Type::Real) return real_;
        if (type_ == Type::Int) return static_cast<double>(int_);
        require(Type::Real);
        return 0;
    }
    const std::string& asString() const {
        require(Type::String);
        return str_;
    }
    const Array& asArray() const {
        require(Type::Array);
        return arr_;
    }
    const Object& asObject() const {
        require(Type::Object);
        return obj_;
    }
    std::string asStringOr(const std::string& def) const { return isString() ? str_ : def; }
    long long asIntOr(long long def) const { return isNumber() ? asInt() : def; }
    double asDoubleOr(double def) const { return isNumber() ? asDouble() : def; }
    bool asBoolOr(bool def) const { return isBool() ? bool_ : def; }

    // ---------------- 容器操作 ----------------
    std::size_t size() const {
        switch (type_) {
            case Type::Array: return arr_.size();
            case Type::Object: return obj_.size();
            case Type::String: return str_.size();
            default: return 0;
        }
    }
    bool empty() const { return size() == 0; }

    void push_back(const Json& v) {
        makeArray();
        arr_.push_back(v);
    }
    void push_back(Json&& v) {
        makeArray();
        arr_.push_back(std::move(v));
    }
    void set(const std::string& key, const Json& v) {
        makeObject();
        for (auto& kv : obj_) {
            if (kv.first == key) {
                kv.second = v;
                return;
            }
        }
        obj_.emplace_back(key, v);
    }
    void set(const std::string& key, Json&& v) {
        makeObject();
        for (auto& kv : obj_) {
            if (kv.first == key) {
                kv.second = std::move(v);
                return;
            }
        }
        obj_.emplace_back(key, std::move(v));
    }

    bool contains(const std::string& key) const {
        if (!isObject()) return false;
        for (const auto& kv : obj_)
            if (kv.first == key) return true;
        return false;
    }
    const Json* find(const std::string& key) const {
        if (!isObject()) return nullptr;
        for (const auto& kv : obj_)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
    Json* find(const std::string& key) {
        if (!isObject()) return nullptr;
        for (auto& kv : obj_)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }

    // 非 const 下标：对象缺失则自动插入 null（类似 nlohmann::json）
    Json& operator[](const std::string& key) {
        makeObject();
        for (auto& kv : obj_)
            if (kv.first == key) return kv.second;
        obj_.emplace_back(key, Json());
        return obj_.back().second;
    }
    Json& operator[](std::size_t idx) {
        makeArray();
        if (idx >= arr_.size()) arr_.resize(idx + 1);
        return arr_[idx];
    }
    // const 下标：越界返回静态空值，不抛异常
    const Json& operator[](const std::string& key) const {
        const Json* v = find(key);
        if (v) return *v;
        static const Json kNull;
        return kNull;
    }
    const Json& operator[](std::size_t idx) const {
        if (isArray() && idx < arr_.size()) return arr_[idx];
        static const Json kNull;
        return kNull;
    }

    // ---------------- 序列化 ----------------
    // indent < 0 表示紧凑输出，否则表示缩进空格数
    std::string dump(int indent = -1) const {
        std::string out;
        dumpInternal(*this, out, indent, 0);
        return out;
    }

    static std::string dump(const Json& v, int indent = -1) { return v.dump(indent); }

    // ---------------- 解析 ----------------
    static Json parse(const std::string& text);
    static Json parse(const char* begin, const char* end);

    // 解析失败返回 false，不抛异常
    static bool tryParse(const std::string& text, Json& out, std::string* err = nullptr) {
        try {
            out = parse(text);
            return true;
        } catch (const JsonError& e) {
            if (err) *err = e.what();
            return false;
        }
    }

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    long long int_ = 0;
    double real_ = 0.0;
    std::string str_;
    Array arr_;
    Object obj_;

    void require(Type t) const {
        if (type_ != t) throw JsonError("minijson: type mismatch (expected " + typeName(t) + ", got " + typeName(type_) + ")");
    }
    static std::string typeName(Type t) {
        switch (t) {
            case Type::Null: return "null";
            case Type::Bool: return "bool";
            case Type::Int: return "int";
            case Type::Real: return "real";
            case Type::String: return "string";
            case Type::Array: return "array";
            case Type::Object: return "object";
        }
        return "unknown";
    }
    void makeArray() {
        if (type_ == Type::Null) type_ = Type::Array;
        require(Type::Array);
    }
    void makeObject() {
        if (type_ == Type::Null) type_ = Type::Object;
        require(Type::Object);
    }

    static void appendUtf8(std::string& out, unsigned int cp) {
        if (cp <= 0x7Fu) {
            out += static_cast<char>(cp);
        } else if (cp <= 0x7FFu) {
            out += static_cast<char>(0xC0u | (cp >> 6));
            out += static_cast<char>(0x80u | (cp & 0x3Fu));
        } else if (cp <= 0xFFFFu) {
            out += static_cast<char>(0xE0u | (cp >> 12));
            out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
            out += static_cast<char>(0x80u | (cp & 0x3Fu));
        } else {
            out += static_cast<char>(0xF0u | (cp >> 18));
            out += static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
            out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
            out += static_cast<char>(0x80u | (cp & 0x3Fu));
        }
    }

    static void escapeString(const std::string& s, std::string& out) {
        out += '"';
        for (unsigned char c : s) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                        out += buf;
                    } else {
                        out += static_cast<char>(c);
                    }
            }
        }
        out += '"';
    }

    static void dumpInternal(const Json& v, std::string& out, int indent, int level) {
        const std::string pad = indent > 0 ? std::string(static_cast<std::size_t>(indent) * level, ' ') : std::string();
        const std::string padNext = indent > 0 ? std::string(static_cast<std::size_t>(indent) * (level + 1), ' ') : std::string();
        const bool pretty = indent > 0;

        switch (v.type_) {
            case Type::Null: out += "null"; break;
            case Type::Bool: out += v.bool_ ? "true" : "false"; break;
            case Type::Int: out += std::to_string(v.int_); break;
            case Type::Real: {
                if (!std::isfinite(v.real_)) {
                    out += "null";
                    break;
                }
                char buf[40];
                std::snprintf(buf, sizeof(buf), "%.17g", v.real_);
                std::string s(buf);
                if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
                    s.find('E') == std::string::npos && s.find('n') == std::string::npos) {
                    s += ".0";
                }
                out += s;
                break;
            }
            case Type::String: escapeString(v.str_, out); break;
            case Type::Array: {
                if (v.arr_.empty()) {
                    out += "[]";
                    break;
                }
                out += '[';
                for (std::size_t i = 0; i < v.arr_.size(); ++i) {
                    if (i) out += ',';
                    if (pretty) out += "\n" + padNext;
                    dumpInternal(v.arr_[i], out, indent, level + 1);
                }
                if (pretty) out += "\n" + pad;
                out += ']';
                break;
            }
            case Type::Object: {
                if (v.obj_.empty()) {
                    out += "{}";
                    break;
                }
                out += '{';
                for (std::size_t i = 0; i < v.obj_.size(); ++i) {
                    if (i) out += ',';
                    if (pretty) out += "\n" + padNext;
                    escapeString(v.obj_[i].first, out);
                    out += pretty ? ": " : ":";
                    dumpInternal(v.obj_[i].second, out, indent, level + 1);
                }
                if (pretty) out += "\n" + pad;
                out += '}';
                break;
            }
        }
    }

    // ---------------- 解析器 ----------------
    class Parser {
    public:
        Parser(const char* begin, const char* end) : b_(begin), e_(end), p_(begin) {}

        Json parse() {
            skipWs();
            Json v = parseValue();
            skipWs();
            if (p_ != e_) fail("unexpected trailing content");
            return v;
        }

    private:
        const char* b_;
        const char* e_;
        const char* p_;

        bool atEnd() const { return p_ >= e_; }
        char peek() const {
            if (atEnd()) fail("unexpected end of input");
            return *p_;
        }
        char next() {
            char c = peek();
            ++p_;
            return c;
        }
        [[noreturn]] void fail(const std::string& msg) const {
            throw JsonError("minijson: " + msg + " (offset " + std::to_string(p_ - b_) + ")");
        }
        void skipWs() {
            while (!atEnd()) {
                char c = *p_;
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++p_;
                else break;
            }
        }
        void expectLiteral(const char* lit) {
            std::size_t n = std::strlen(lit);
            if (static_cast<std::size_t>(e_ - p_) < n || std::memcmp(p_, lit, n) != 0)
                fail(std::string("invalid literal, expected ") + lit);
            p_ += n;
        }

        Json parseValue() {
            switch (peek()) {
                case '{': return parseObject();
                case '[': return parseArray();
                case '"': return Json(parseString());
                case 't': expectLiteral("true"); return Json(true);
                case 'f': expectLiteral("false"); return Json(false);
                case 'n': expectLiteral("null"); return Json();
                default: return parseNumber();
            }
        }

        Json parseObject() {
            next();  // '{'
            Json::Object obj;
            skipWs();
            if (peek() == '}') {
                next();
                return Json(std::move(obj));
            }
            while (true) {
                skipWs();
                if (peek() != '"') fail("expected object key");
                std::string key = parseString();
                skipWs();
                if (next() != ':') fail("expected ':' after object key");
                skipWs();
                Json val = parseValue();
                obj.emplace_back(std::move(key), std::move(val));
                skipWs();
                char c = next();
                if (c == ',') continue;
                if (c == '}') break;
                fail("expected ',' or '}' in object");
            }
            return Json(std::move(obj));
        }

        Json parseArray() {
            next();  // '['
            Json::Array arr;
            skipWs();
            if (peek() == ']') {
                next();
                return Json(std::move(arr));
            }
            while (true) {
                skipWs();
                arr.push_back(parseValue());
                skipWs();
                char c = next();
                if (c == ',') continue;
                if (c == ']') break;
                fail("expected ',' or ']' in array");
            }
            return Json(std::move(arr));
        }

        std::string parseString() {
            next();  // opening quote
            std::string out;
            while (true) {
                if (atEnd()) fail("unterminated string");
                unsigned char c = static_cast<unsigned char>(next());
                if (c == '"') break;
                if (c != '\\') {
                    out += static_cast<char>(c);
                    continue;
                }
                if (atEnd()) fail("unterminated escape sequence");
                char esc = next();
                switch (esc) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': {
                        unsigned int cp = parseHex4();
                        // 代理对
                        if (cp >= 0xD800u && cp <= 0xDBFFu && e_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
                            const char* save = p_;
                            p_ += 2;
                            unsigned int lo = parseHex4();
                            if (lo >= 0xDC00u && lo <= 0xDFFFu) {
                                cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                            } else {
                                p_ = save;  // 非法代理对，按原字符处理
                            }
                        }
                        appendUtf8(out, cp);
                        break;
                    }
                    default: fail("invalid escape character");
                }
            }
            return out;
        }

        unsigned int parseHex4() {
            if (e_ - p_ < 4) fail("invalid \\u escape");
            unsigned int v = 0;
            for (int i = 0; i < 4; ++i) {
                char c = *p_++;
                v <<= 4;
                if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
                else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
                else fail("invalid hex digit in \\u escape");
            }
            return v;
        }

        Json parseNumber() {
            const char* start = p_;
            if (!atEnd() && *p_ == '-') ++p_;
            if (atEnd() || !isDigit(*p_)) fail("invalid number");
            if (*p_ == '0') {
                ++p_;
            } else {
                while (!atEnd() && isDigit(*p_)) ++p_;
            }
            bool isReal = false;
            if (!atEnd() && *p_ == '.') {
                isReal = true;
                ++p_;
                if (atEnd() || !isDigit(*p_)) fail("invalid fraction");
                while (!atEnd() && isDigit(*p_)) ++p_;
            }
            if (!atEnd() && (*p_ == 'e' || *p_ == 'E')) {
                isReal = true;
                ++p_;
                if (!atEnd() && (*p_ == '+' || *p_ == '-')) ++p_;
                if (atEnd() || !isDigit(*p_)) fail("invalid exponent");
                while (!atEnd() && isDigit(*p_)) ++p_;
            }
            std::string text(start, p_);
            if (isReal) {
                return Json(std::strtod(text.c_str(), nullptr));
            }
            errno = 0;
            long long v = std::strtoll(text.c_str(), nullptr, 10);
            if (errno == ERANGE) return Json(std::strtod(text.c_str(), nullptr));
            return Json(v);
        }

        static bool isDigit(char c) { return c >= '0' && c <= '9'; }
    };
};

inline Json Json::parse(const std::string& text) {
    Parser p(text.data(), text.data() + text.size());
    return p.parse();
}

inline Json Json::parse(const char* begin, const char* end) {
    Parser p(begin, end);
    return p.parse();
}

}  // namespace minijson

#endif  // MINIJSON_JSON_HPP
