// 示例控制台程序：回显 worker（演示「原样收 JSON、原样吐 JSON」）
//
//   输入: {"name":"Tom","age":15}
//   输出: {"ok":true,"received":{"age":15,"name":"Tom"},"length":26,"server_time":"..."}
//
// 编译: g++ -std=c++17 -O2 echo_worker.cpp -I../include -o echo_worker

#include <chrono>
#include <ctime>
#include <iostream>
#include <sstream>
#include <string>

#include "json.hpp"

using minijson::Json;

std::string nowString() {
    std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return std::string(buf);
}

int main() {
    std::ostringstream ss;
    ss << std::cin.rdbuf();
    const std::string input = ss.str();

    Json received;
    std::string err;
    const bool parsed = Json::tryParse(input, received, &err);

    Json out = Json::object();
    out.set("ok", parsed);
    out.set("received", parsed ? received : Json(input));
    out.set("length", static_cast<long long>(input.size()));
    out.set("server_time", nowString());
    if (!parsed) out.set("error", err);

    std::cout << out.dump() << std::endl;

    // 顺带往 stderr 写点东西，用于演示 stderr 的采集
    std::cerr << "echo_worker: handled " << input.size() << " bytes" << std::endl;
    return 0;
}
