#include "minigraph/Value.h"

#include <cmath>
#include <cstdio>
#include <iomanip>
#include <sstream>

namespace minigraph {

namespace {

// 静态空容器：供 asArray()/asObject() 在类型不符时返回，避免抛异常
const ValueList& emptyList() {
    static const ValueList kEmpty;
    return kEmpty;
}

const ValueMap& emptyMap() {
    static const ValueMap kEmpty;
    return kEmpty;
}

// JSON 字符串转义
void writeJsonString(std::ostream& os, const std::string& s) {
    os << '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"':  os << "\\\""; break;
            case '\\': os << "\\\\"; break;
            case '\b': os << "\\b";  break;
            case '\f': os << "\\f";  break;
            case '\n': os << "\\n";  break;
            case '\r': os << "\\r";  break;
            case '\t': os << "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    os << buf;
                } else {
                    os << static_cast<char>(c);
                }
        }
    }
    os << '"';
}

}  // namespace

Value::Value(int v) : data_(static_cast<int64_t>(v)) {}
Value::Value(int64_t v) : data_(v) {}
Value::Value(double v) : data_(v) {}
Value::Value(bool v) : data_(v) {}
Value::Value(std::string v) : data_(std::move(v)) {}
Value::Value(const char* v) : data_(std::string(v ? v : "")) {}
Value::Value(ValueList v) : data_(std::move(v)) {}
Value::Value(ValueMap v) : data_(std::move(v)) {}

Value::Type Value::type() const {
    switch (data_.index()) {
        case 0:  return Type::Null;
        case 1:  return Type::Int;
        case 2:  return Type::Double;
        case 3:  return Type::Bool;
        case 4:  return Type::String;
        case 5:  return Type::Array;
        default: return Type::Object;
    }
}

bool Value::isNull() const   { return type() == Type::Null; }
bool Value::isInt() const    { return type() == Type::Int; }
bool Value::isDouble() const { return type() == Type::Double; }
bool Value::isBool() const   { return type() == Type::Bool; }
bool Value::isString() const { return type() == Type::String; }
bool Value::isArray() const  { return type() == Type::Array; }
bool Value::isObject() const { return type() == Type::Object; }
bool Value::isContainer() const { return isArray() || isObject(); }
bool Value::isScalar() const    { return !isContainer(); }

const char* Value::typeName() const {
    switch (type()) {
        case Type::Null:   return "null";
        case Type::Int:    return "int";
        case Type::Double: return "double";
        case Type::Bool:   return "bool";
        case Type::String: return "string";
        case Type::Array:  return "array";
        default:           return "object";
    }
}

int64_t Value::asInt(int64_t def) const {
    switch (type()) {
        case Type::Int:    return std::get<int64_t>(data_);
        case Type::Double: return static_cast<int64_t>(std::llround(std::get<double>(data_)));
        case Type::Bool:   return std::get<bool>(data_) ? 1 : 0;
        case Type::String: {
            try {
                return std::stoll(std::get<std::string>(data_));
            } catch (...) {
                return def;
            }
        }
        default: return def;
    }
}

double Value::asDouble(double def) const {
    switch (type()) {
        case Type::Int:    return static_cast<double>(std::get<int64_t>(data_));
        case Type::Double: return std::get<double>(data_);
        case Type::Bool:   return std::get<bool>(data_) ? 1.0 : 0.0;
        case Type::String: {
            try {
                return std::stod(std::get<std::string>(data_));
            } catch (...) {
                return def;
            }
        }
        default: return def;
    }
}

bool Value::asBool(bool def) const {
    switch (type()) {
        case Type::Int:    return std::get<int64_t>(data_) != 0;
        case Type::Double: return std::get<double>(data_) != 0.0;
        case Type::Bool:   return std::get<bool>(data_);
        case Type::String: {
            const std::string& s = std::get<std::string>(data_);
            if (s == "true" || s == "1" || s == "yes") return true;
            if (s == "false" || s == "0" || s == "no" || s.empty()) return false;
            return def;
        }
        case Type::Array:  return !std::get<ValueList>(data_).empty();
        case Type::Object: return !std::get<ValueMap>(data_).empty();
        default:           return def;
    }
}

std::string Value::asString(const std::string& def) const {
    switch (type()) {
        case Type::String: return std::get<std::string>(data_);
        case Type::Null:   return def;
        default:           return toString();
    }
}

const ValueList& Value::asArray() const {
    if (isArray()) return std::get<ValueList>(data_);
    return emptyList();
}

const ValueMap& Value::asObject() const {
    if (isObject()) return std::get<ValueMap>(data_);
    return emptyMap();
}

std::size_t Value::size() const {
    switch (type()) {
        case Type::String: return std::get<std::string>(data_).size();
        case Type::Array:  return std::get<ValueList>(data_).size();
        case Type::Object: return std::get<ValueMap>(data_).size();
        default:           return 0;
    }
}

bool Value::empty() const { return size() == 0; }

const Value* Value::at(std::size_t index) const {
    if (!isArray()) return nullptr;
    const ValueList& a = std::get<ValueList>(data_);
    return index < a.size() ? &a[index] : nullptr;
}

const Value* Value::at(const std::string& key) const {
    if (!isObject()) return nullptr;
    const ValueMap& m = std::get<ValueMap>(data_);
    auto it = m.find(key);
    return it == m.end() ? nullptr : &it->second;
}

bool Value::hasKey(const std::string& key) const { return at(key) != nullptr; }

bool Value::contains(const Value& v) const {
    switch (type()) {
        case Type::Array: {
            for (const Value& item : std::get<ValueList>(data_)) {
                if (item == v) return true;
            }
            return false;
        }
        case Type::Object: {
            for (const auto& kv : std::get<ValueMap>(data_)) {
                if (kv.second == v) return true;
            }
            return false;
        }
        case Type::String: {
            if (!v.isString()) return false;
            return std::get<std::string>(data_).find(v.asString()) != std::string::npos;
        }
        default:
            return false;
    }
}

void Value::set(const std::string& key, const Value& v) {
    if (!isObject()) data_ = ValueMap();
    std::get<ValueMap>(data_)[key] = v;
}

void Value::push_back(const Value& v) {
    if (!isArray()) data_ = ValueList();
    std::get<ValueList>(data_).push_back(v);
}

bool Value::removeElement(const Value& v) {
    if (!isArray()) return false;
    ValueList& a = std::get<ValueList>(data_);
    for (auto it = a.begin(); it != a.end(); ++it) {
        if (*it == v) {
            a.erase(it);
            return true;
        }
    }
    return false;
}

bool Value::removeKey(const std::string& key) {
    if (!isObject()) return false;
    return std::get<ValueMap>(data_).erase(key) > 0;
}

std::string Value::toString() const {
    switch (type()) {
        case Type::Null:   return "null";
        case Type::Int:    return std::to_string(std::get<int64_t>(data_));
        case Type::Double: {
            // 17 位精度保证 double 往返无损
            std::ostringstream os;
            os << std::setprecision(17) << std::get<double>(data_);
            return os.str();
        }
        case Type::Bool:   return std::get<bool>(data_) ? "true" : "false";
        case Type::String: return std::get<std::string>(data_);
        case Type::Array: {
            std::ostringstream os;
            os << "[";
            bool first = true;
            for (const Value& item : std::get<ValueList>(data_)) {
                if (!first) os << ", ";
                first = false;
                if (item.isString()) writeJsonString(os, item.asString());
                else os << item.toString();
            }
            os << "]";
            return os.str();
        }
        default: {  // Object
            std::ostringstream os;
            os << "{";
            bool first = true;
            for (const auto& kv : std::get<ValueMap>(data_)) {
                if (!first) os << ", ";
                first = false;
                writeJsonString(os, kv.first);
                os << ": ";
                if (kv.second.isString()) writeJsonString(os, kv.second.asString());
                else os << kv.second.toString();
            }
            os << "}";
            return os.str();
        }
    }
}

bool Value::operator==(const Value& other) const { return data_ == other.data_; }
bool Value::operator!=(const Value& other) const { return !(*this == other); }

bool Value::operator<(const Value& other) const {
    if (type() != other.type()) return type() < other.type();  // 先按类型排序
    return data_ < other.data_;
}

std::ostream& operator<<(std::ostream& os, const Value& v) {
    if (v.isString()) return os << "\"" << v.toString() << "\"";
    return os << v.toString();
}

} // namespace minigraph
