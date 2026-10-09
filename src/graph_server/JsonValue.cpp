#include "graph_server/JsonValue.h"

#include <cstdlib>

namespace graphserver {

namespace {

bool valueFromJsonImpl(const minijson::Json& j, minigraph::Value& out, std::string& err, int depth) {
    using JsonType = minijson::Json::Type;
    if (depth > kMaxJsonDepth) {
        err = "属性嵌套层数超过 " + std::to_string(kMaxJsonDepth) + " 层";
        return false;
    }
    switch (j.type()) {
        case JsonType::Null:   out = minigraph::Value();                                 return true;
        case JsonType::Bool:   out = minigraph::Value(j.asBool());                       return true;
        case JsonType::Int:    out = minigraph::Value(static_cast<int64_t>(j.asInt()));  return true;
        case JsonType::Real:   out = minigraph::Value(j.asDouble());                     return true;
        case JsonType::String: out = minigraph::Value(j.asString());                     return true;
        case JsonType::Array: {
            minigraph::ValueList list;
            list.reserve(j.asArray().size());
            for (const minijson::Json& item : j.asArray()) {
                minigraph::Value v;
                if (!valueFromJsonImpl(item, v, err, depth + 1)) return false;
                list.push_back(std::move(v));
            }
            out = minigraph::Value(std::move(list));
            return true;
        }
        case JsonType::Object: {
            minigraph::ValueMap map;
            for (const auto& kv : j.asObject()) {
                minigraph::Value v;
                if (!valueFromJsonImpl(kv.second, v, err, depth + 1)) return false;
                map[kv.first] = std::move(v);
            }
            out = minigraph::Value(std::move(map));
            return true;
        }
    }
    err = "不支持的属性类型";
    return false;
}

}  // namespace

bool valueFromJson(const minijson::Json& j, minigraph::Value& out, std::string& err) {
    return valueFromJsonImpl(j, out, err, 0);
}

minijson::Json valueToJson(const minigraph::Value& v) {
    using Type = minigraph::Value::Type;
    switch (v.type()) {
        case Type::Null:   return minijson::Json();
        case Type::Int:    return minijson::Json(static_cast<long long>(v.asInt()));
        case Type::Double: return minijson::Json(v.asDouble());
        case Type::Bool:   return minijson::Json(v.asBool());
        case Type::String: return minijson::Json(v.asString());
        case Type::Array: {
            minijson::Json arr = minijson::Json::array();
            for (const minigraph::Value& item : v.asArray()) arr.push_back(valueToJson(item));
            return arr;
        }
        default: {  // Object
            minijson::Json obj = minijson::Json::object();
            for (const auto& kv : v.asObject()) obj.set(kv.first, valueToJson(kv.second));
            return obj;
        }
    }
}

bool propsFromJson(const minijson::Json& j, minigraph::Props& out, std::string& err) {
    if (j.isNull()) return true;                 // 没有 props 字段时视为空
    if (!j.isObject()) {
        err = "props 必须是 JSON 对象";
        return false;
    }
    for (const auto& kv : j.asObject()) {
        minigraph::Value v;
        if (!valueFromJson(kv.second, v, err)) {
            err = "属性 \"" + kv.first + "\": " + err;
            return false;
        }
        out[kv.first] = v;
    }
    return true;
}

minijson::Json propsToJson(const minigraph::Props& props) {
    minijson::Json obj = minijson::Json::object();
    for (const auto& kv : props) obj.set(kv.first, valueToJson(kv.second));
    return obj;
}

namespace {

bool isIntegerText(const std::string& s) {
    if (s.empty()) return false;
    std::size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    if (i >= s.size()) return false;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    return true;
}

bool isRealText(const std::string& s) {
    std::string t = (!s.empty() && (s[0] == '-' || s[0] == '+')) ? s.substr(1) : s;
    if (t.empty()) return false;
    bool hasDigit = false, hasDot = false, hasExp = false;
    for (std::size_t i = 0; i < t.size(); ++i) {
        char c = t[i];
        if (c >= '0' && c <= '9') { hasDigit = true; continue; }
        if (c == '.' && !hasDot && !hasExp) { hasDot = true; continue; }
        if ((c == 'e' || c == 'E') && !hasExp && i + 1 < t.size()) {
            hasExp = true;
            if (t[i + 1] == '-' || t[i + 1] == '+') ++i;
            continue;
        }
        return false;
    }
    return hasDigit && (hasDot || hasExp);
}

}  // namespace

bool valueFromText(const std::string& text, const std::string& type,
                   minigraph::Value& out, std::string& err) {
    if (type == "string") { out = minigraph::Value(text); return true; }
    if (type == "int") {
        if (!isIntegerText(text)) { err = "\"" + text + "\" 不是合法整数"; return false; }
        out = minigraph::Value(static_cast<int64_t>(std::strtoll(text.c_str(), nullptr, 10)));
        return true;
    }
    if (type == "double") {
        if (!isIntegerText(text) && !isRealText(text)) { err = "\"" + text + "\" 不是合法浮点数"; return false; }
        out = minigraph::Value(std::strtod(text.c_str(), nullptr));
        return true;
    }
    if (type == "bool") {
        if (text == "true" || text == "1")       { out = minigraph::Value(true);  return true; }
        if (text == "false" || text == "0")      { out = minigraph::Value(false); return true; }
        err = "\"" + text + "\" 不是合法布尔值（应为 true/false/1/0）";
        return false;
    }
    if (type == "json") {                        // 数组 / 对象（也可以是标量）
        minijson::Json j;
        std::string perr;
        if (!minijson::Json::tryParse(text, j, &perr)) {
            err = "\"" + text + "\" 不是合法 JSON: " + perr;
            return false;
        }
        return valueFromJson(j, out, err);
    }

    // auto：整数 -> 浮点 -> 布尔 -> 字符串
    if (isIntegerText(text)) {
        out = minigraph::Value(static_cast<int64_t>(std::strtoll(text.c_str(), nullptr, 10)));
        return true;
    }
    if (isRealText(text)) {
        out = minigraph::Value(std::strtod(text.c_str(), nullptr));
        return true;
    }
    if (text == "true" || text == "false") {
        out = minigraph::Value(text == "true");
        return true;
    }
    out = minigraph::Value(text);
    return true;
}

bool valueEquals(const minigraph::Value& a, const minigraph::Value& b) {
    if (a == b) return true;
    const bool aNum = a.isInt() || a.isDouble();
    const bool bNum = b.isInt() || b.isDouble();
    if (aNum && bNum) return a.asDouble() == b.asDouble();

    // 数组：长度相同且逐项相等（用同一套宽松规则，30 与 30.0 视为相等）
    if (a.isArray() && b.isArray()) {
        const minigraph::ValueList& la = a.asArray();
        const minigraph::ValueList& lb = b.asArray();
        if (la.size() != lb.size()) return false;
        for (std::size_t i = 0; i < la.size(); ++i) {
            if (!valueEquals(la[i], lb[i])) return false;
        }
        return true;
    }
    // 对象：键集合相同且每个键的值相等
    if (a.isObject() && b.isObject()) {
        const minigraph::ValueMap& ma = a.asObject();
        const minigraph::ValueMap& mb = b.asObject();
        if (ma.size() != mb.size()) return false;
        for (const auto& kv : ma) {
            auto it = mb.find(kv.first);
            if (it == mb.end()) return false;
            if (!valueEquals(kv.second, it->second)) return false;
        }
        return true;
    }
    return false;
}

}  // namespace graphserver
