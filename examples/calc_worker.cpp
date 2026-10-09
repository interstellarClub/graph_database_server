// 示例控制台程序：四则运算 worker
// 协议：从 stdin 读一行 JSON，向 stdout 输出一行 JSON
//
//   输入: {"op":"add","a":3,"b":4}
//   输出: {"result":7}
//
// 编译: g++ -std=c++17 -O2 calc_worker.cpp -I../include -o calc_worker

#include <iostream>
#include <sstream>
#include <string>

#include "json.hpp"

using minijson::Json;

int main() {
    // 1) 读取 stdin 全部内容
    std::ostringstream ss;
    ss << std::cin.rdbuf();
    const std::string input = ss.str();

    Json out = Json::object();

    Json in;
    std::string err;
    if (!Json::tryParse(input, in, &err)) {
        out.set("ok", false);
        out.set("error", std::string("stdin 不是合法 JSON: ") + err);
        std::cout << out.dump() << std::endl;
        return 1;
    }

    if (!in.isObject() || !in.contains("op")) {
        out.set("ok", false);
        out.set("error", "缺少 op 字段，示例: {\"op\":\"add\",\"a\":1,\"b\":2}");
        std::cout << out.dump() << std::endl;
        return 1;
    }

    const std::string op = in["op"].asStringOr("");
    const double a = in["a"].asDoubleOr(0);
    const double b = in["b"].asDoubleOr(0);

    auto ok = [&](double v) {
        out.set("ok", true);
        out.set("op", op);
        out.set("a", a);
        out.set("b", b);
        out.set("result", v);
    };

    if (op == "add") ok(a + b);
    else if (op == "sub") ok(a - b);
    else if (op == "mul") ok(a * b);
    else if (op == "div") {
        if (b == 0) {
            out.set("ok", false);
            out.set("error", "除数不能为 0");
        } else {
            ok(a / b);
        }
    } else {
        out.set("ok", false);
        out.set("error", "不支持的 op: " + op + "（支持 add/sub/mul/div）");
    }

    // 2) 输出一行 JSON 到 stdout
    std::cout << out.dump() << std::endl;
    return out["ok"].asBoolOr(false) ? 0 : 1;
}
