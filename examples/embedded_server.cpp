// 示例：不使用子进程，直接把 HTTP 接口嵌进自己的 C++ 程序
//
// 如果你的「控制台程序逻辑」本来就在同一进程里（或想省掉进程启动开销），
// 可以像下面这样直接复用 HttpServer + Json，把业务逻辑挂到 handler 上。
//
// 编译: g++ -std=c++17 -O2 embedded_server.cpp ../src/http_server.cpp -I../include -o embedded_server -pthread

#include <cstdio>
#include <iostream>
#include <string>

#include "http_server.hpp"
#include "json.hpp"

using minijson::Json;
using httpbridge::HttpRequest;
using httpbridge::HttpResponse;

namespace {

// 业务函数：输入 JSON，输出 JSON（等价于原控制台程序的 main 逻辑）
Json businessLogic(const Json& in) {
    Json out = Json::object();
    const std::string name = in["name"].asStringOr("world");
    const long long times = in["times"].asIntOr(1);

    Json lines = Json::array();
    for (long long i = 0; i < times; ++i)
        lines.push_back("Hello, " + name + "! #" + std::to_string(i + 1));

    out.set("ok", true);
    out.set("greetings", lines);
    return out;
}

HttpResponse handler(const HttpRequest& req) {
    if (req.method == "OPTIONS") {
        HttpResponse r = HttpResponse::text(204, "");
        r.addHeader("Access-Control-Allow-Origin", "*");
        r.addHeader("Access-Control-Allow-Headers", "Content-Type");
        return r;
    }

    Json body = Json::object();
    if (req.path == "/health") {
        body.set("ok", true);
        return HttpResponse::json(200, body.dump());
    }
    if (req.path != "/greet") {
        body.set("ok", false);
        body.set("error", "未找到: " + req.path);
        return HttpResponse::json(404, body.dump());
    }

    // POST: 解析请求体；GET: 用 query 参数组装
    Json input;
    std::string err;
    if (req.method == "POST") {
        if (!Json::tryParse(req.body, input, &err)) {
            body.set("ok", false);
            body.set("error", "非法 JSON: " + err);
            return HttpResponse::json(400, body.dump());
        }
    } else {
        input = Json::object();
        for (const auto& kv : httpbridge::parseQueryString(req.query))
            input.set(kv.first, kv.second);
    }

    Json result = businessLogic(input);
    HttpResponse resp = HttpResponse::json(200, result.dump());
    resp.addHeader("Access-Control-Allow-Origin", "*");
    return resp;
}

}  // namespace

int main(int argc, char** argv) {
    int port = 8081;
    if (argc > 1) port = std::atoi(argv[1]);

    httpbridge::HttpServer server("127.0.0.1", port);
    server.setHandler(handler);
    if (!server.start()) {
        std::cerr << "启动失败: " << server.error() << "\n";
        return 1;
    }
    std::printf("嵌入式服务已启动: http://127.0.0.1:%d/greet?name=Tom&times=3\n", port);
    server.serveForever();  // 阻塞
    return 0;
}
