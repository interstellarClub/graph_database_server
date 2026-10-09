// minijson 自测程序：解析 / 序列化 / 取值 / 错误处理
#include <cmath>
#include <iostream>
#include <string>

#include "json.hpp"

using minijson::Json;

static int gFailed = 0;
static int gPassed = 0;

static void check(bool cond, const std::string& name) {
    if (cond) {
        ++gPassed;
    } else {
        ++gFailed;
        std::cout << "  [FAIL] " << name << "\n";
    }
}

int main() {
    // ---- 基本解析 ----
    Json v = Json::parse(R"({"name":"Tom","age":15,"scores":[98,87.5,null,true],"addr":{"city":"TJ"}})");
    check(v.isObject(), "root is object");
    check(v["name"].asString() == "Tom", "string value");
    check(v["age"].asInt() == 15, "int value");
    check(v["scores"].isArray() && v["scores"].size() == 4, "array size");
    check(v["scores"][1].asDouble() == 87.5, "real value");
    check(v["scores"][2].isNull(), "null value");
    check(v["scores"][3].asBool() == true, "bool value");
    check(v["addr"]["city"].asString() == "TJ", "nested object");
    check(!v.contains("missing"), "contains missing key");

    // ---- 数字与边界 ----
    check(Json::parse("-0").asInt() == 0, "negative zero");
    check(Json::parse("1e3").asDouble() == 1000.0, "exponent");
    check(Json::parse("123456789012345").asInt() == 123456789012345LL, "big int");
    check(Json::parse("0.5").asDouble() == 0.5, "fraction");

    // ---- 转义与 Unicode ----
    Json u = Json::parse(R"({"cn":"你好","esc":"a\"b\\c\nd","emoji":"😀"})");
    check(u["cn"].asString() == "\xe4\xbd\xa0\xe5\xa5\xbd", "utf8 string");
    check(u["esc"].asString() == "a\"b\\c\nd", "escape sequences");
    check(u["emoji"].asString() == "\xF0\x9F\x98\x80", "surrogate pair -> utf8");

    // ---- 序列化 ----
    Json o = Json::object();
    o.set("b", true);
    o.set("i", 7);
    o.set("d", 2.5);
    o.set("s", "x\"y");
    o.set("arr", Json::array());
    o["arr"].push_back(1);
    o["arr"].push_back("two");
    check(o.dump() == R"({"b":true,"i":7,"d":2.5,"s":"x\"y","arr":[1,"two"]})", "dump compact");
    check(Json::parse(o.dump()).dump() == o.dump(), "round trip");
    check(Json::array().dump() == "[]", "empty array");
    check(Json::object().dump() == "{}", "empty object");

    // ---- 美化输出 ----
    Json p = Json::parse(R"({"a":1,"b":[1,2]})");
    std::string pretty = p.dump(2);
    check(pretty.find('\n') != std::string::npos, "pretty has newline");
    check(Json::parse(pretty)["b"][1].asInt() == 2, "pretty reparsed");

    // ---- 错误处理 ----
    std::string err;
    Json bad;
    check(!Json::tryParse(R"({"a":1,})", bad, &err) && !err.empty(), "trailing comma rejected");
    check(!Json::tryParse("{abc}", bad, &err), "unquoted key rejected");
    check(!Json::tryParse(R"({"a":01})", bad, &err), "leading zero rejected");
    check(!Json::tryParse("", bad, &err), "empty input rejected");
    check(!Json::tryParse(R"("unterminated)", bad, &err), "unterminated string rejected");

    // ---- 类型不符抛异常 ----
    bool threw = false;
    try {
        (void)Json::parse("123").asString();
    } catch (const minijson::JsonError&) {
        threw = true;
    }
    check(threw, "type mismatch throws");

    std::cout << "通过 " << gPassed << " 项，失败 " << gFailed << " 项\n";
    return gFailed == 0 ? 0 : 1;
}
