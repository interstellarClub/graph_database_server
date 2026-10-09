// -----------------------------------------------------------------------------
// MiniGraph —— 属性值类型
//
// 图数据库里所有属性（节点属性 / 关系属性）都是 Value。
// 支持 7 种类型，与 JSON 一一对应：
//
//   null / int / double / bool / string / array / object
//
// 数组与对象可以任意嵌套（{"addr": {"city": "北京"}, "tags": ["vip", 1]}），
// 二进制落盘格式从 v2 起支持容器类型（v1 文件仍可读）。
// -----------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <ostream>
#include <string>
#include <variant>
#include <vector>

namespace minigraph {

class Value;

// 数组：Value 的有序列表；对象：键 -> Value（std::map 保证键唯一且输出顺序稳定）
using ValueList = std::vector<Value>;
using ValueMap  = std::map<std::string, Value>;

// 属性表：key -> value。用 std::map 保证输出顺序稳定（便于打印与落盘一致性）
using Props = ValueMap;

class Value {
public:
    enum class Type { Null, Int, Double, Bool, String, Array, Object };

    Value() = default;                       // 空值
    Value(int v);                            // NOLINT 允许隐式转换，方便调用
    Value(int64_t v);                        // NOLINT
    Value(double v);                         // NOLINT
    Value(bool v);                           // NOLINT
    Value(std::string v);                    // NOLINT
    Value(const char* v);                    // NOLINT
    Value(ValueList v);                      // NOLINT 数组
    Value(ValueMap v);                       // NOLINT 对象

    // -------- 类型信息 --------
    Type type() const;
    bool isNull() const;
    bool isInt() const;
    bool isDouble() const;
    bool isBool() const;
    bool isString() const;
    bool isArray() const;
    bool isObject() const;
    bool isContainer() const;                // 数组或对象
    bool isScalar() const;                   // null/整数/浮点/布尔/字符串（可建索引）
    const char* typeName() const;

    // -------- 取值（带自动类型转换，取不到时返回默认值）--------
    int64_t     asInt(int64_t def = 0) const;
    double      asDouble(double def = 0.0) const;
    bool        asBool(bool def = false) const;
    std::string asString(const std::string& def = "") const;

    // -------- 容器访问 --------
    // 非数组/非对象时返回静态空容器（不抛异常，方便链式调用）
    const ValueList& asArray() const;
    const ValueMap&  asObject() const;

    std::size_t size() const;                // 数组长度 / 对象键数 / 字符串长度
    bool empty() const;

    const Value* at(std::size_t index) const;          // 数组下标，越界返回 nullptr
    const Value* at(const std::string& key) const;     // 对象取键，不存在返回 nullptr
    bool hasKey(const std::string& key) const;         // 对象是否含某键
    // 是否包含某"值"：数组看元素、对象看键值、字符串看子串
    bool contains(const Value& v) const;

    // -------- 容器修改 --------
    void set(const std::string& key, const Value& v);  // 对象写入（非对象则先变为空对象）
    void push_back(const Value& v);                    // 数组追加（非数组则先变为空数组）
    bool removeElement(const Value& v);                // 数组：删除首个匹配元素
    bool removeKey(const std::string& key);            // 对象：删除键

    // -------- 序列化 / 打印 --------
    std::string toString() const;            // 字符串类型返回原始内容，容器返回 JSON 文本

    bool operator==(const Value& other) const;
    bool operator!=(const Value& other) const;
    bool operator<(const Value& other) const; // 使 Value 可作为 std::map 的键（索引用）

    friend std::ostream& operator<<(std::ostream& os, const Value& v);

private:
    std::variant<std::monostate, int64_t, double, bool, std::string, ValueList, ValueMap> data_;
};

} // namespace minigraph
