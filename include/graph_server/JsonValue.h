// -----------------------------------------------------------------------------
// JsonValue —— 两个项目之间的"类型胶水层"
//
//   网络侧：minijson::Json  （null / bool / int / real / string / array / object）
//   存储侧：minigraph::Value（同样 7 种，数组与对象可任意嵌套）
//
// 两侧类型一一对应，转换是无损的：
//   JSON 数组 -> Value::Array，JSON 对象 -> Value::Object，反之亦然。
// 为防止恶意深度嵌套把栈打爆，两端都限制嵌套层数（kMaxJsonDepth）。
// -----------------------------------------------------------------------------
#pragma once

#include <string>

#include "json.hpp"
#include "minigraph/Value.h"

namespace graphserver {

// 允许的属性嵌套层数上限（与存储层的 kMaxValueDepth 呼应）
constexpr int kMaxJsonDepth = 32;

// JSON -> Value（含数组/对象的递归转换）。类型不支持或嵌套过深时返回 false。
bool valueFromJson(const minijson::Json& j, minigraph::Value& out, std::string& err);

// Value -> JSON（无损：null / bool / int / double / string / array / object）
minijson::Json valueToJson(const minigraph::Value& v);

// JSON 对象 -> Props。要求 j 是对象，且每个值都可转成 Value。
bool propsFromJson(const minijson::Json& j, minigraph::Props& out, std::string& err);

// Props -> JSON 对象
minijson::Json propsToJson(const minigraph::Props& props);

// 文本 -> Value（用于解析 query string 里的值）。
// type 取 "auto"(默认) / "int" / "double" / "bool" / "string" / "json"
// 其中 "json" 用于传数组或对象，如 ?vtype=json&value=["vip","new"]
bool valueFromText(const std::string& text, const std::string& type,
                   minigraph::Value& out, std::string& err);

// 宽松比较：数值型跨 int/double 互通（30 == 30.0），数组/对象递归逐项比较
bool valueEquals(const minigraph::Value& a, const minigraph::Value& b);

}  // namespace graphserver
