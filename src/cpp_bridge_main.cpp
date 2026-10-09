// ---------------------------------------------------------------------------
// cpp_bridge —— 把「本地 C++ 控制台程序」包装成 HTTP 接口
//
//   Client --GET/POST(JSON)-->  [cpp_bridge] --stdin(JSON)-->  本地控制台程序
//           <--JSON--                        <--stdout(JSON)--
//
// 路由：
//   GET  /health                     健康检查
//   GET  /info                       查看当前桥接配置
//   GET  /invoke?input=<urlencoded JSON>   用 GET 传入 JSON
//   POST /invoke                     body 为 JSON
//   GET  /                           接口说明
// ---------------------------------------------------------------------------

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "child_process.hpp"
#include "http_server.hpp"
#include "json.hpp"

using minijson::Json;
using httpbridge::HttpRequest;
using httpbridge::HttpResponse;

namespace {

struct Options {
    std::string host = "127.0.0.1";
    int port = 8080;
    std::string command;             // 要桥接的本地控制台程序命令
    std::string route = "/invoke";   // 业务路由
    std::string cwd;                 // 子进程工作目录
    int timeoutMs = 3000;            // 子进程超时
    std::size_t maxBodyBytes = 4u * 1024 * 1024;
    bool appendNewline = true;       // 写入 stdin 时补一个换行
    bool pretty = false;             // 响应 JSON 是否缩进
    bool cors = true;                // 是否开启 CORS
    bool verbose = true;
};

std::atomic<bool> gRunning{true};
std::atomic<long long> gRequestCount{0};
std::chrono::steady_clock::time_point gStartTime = std::chrono::steady_clock::now();

std::string gCommand;
Options gOpts;

void printUsage(const char* prog) {
    std::cout
        << "用法: " << prog << " [选项]\n"
        << "\n必选:\n"
        << "  --cmd <command>     要桥接的本地控制台程序命令（经 /bin/sh -c 执行）\n"
        << "                      例: --cmd ./calc_worker\n"
        << "                      省略时进入内置 echo 演示模式\n"
        << "\n常用:\n"
        << "  --host <addr>       监听地址，默认 127.0.0.1（0.0.0.0 表示允许外部访问）\n"
        << "  --port <port>       监听端口，默认 8080\n"
        << "  --route <path>      业务路由，默认 /invoke\n"
        << "  --cwd <dir>         子进程工作目录\n"
        << "  --timeout <ms>      子进程超时毫秒，默认 3000（0 表示不限时）\n"
        << "  --max-body <bytes>  请求体上限，默认 4194304\n"
        << "  --pretty            响应 JSON 缩进美化输出\n"
        << "  --no-newline        写入子进程 stdin 时不补换行\n"
        << "  --no-cors           关闭 CORS 响应头\n"
        << "  -q, --quiet         不打印访问日志\n"
        << "  -h, --help          显示本帮助\n"
        << "\n接口:\n"
        << "  GET  /health                          健康检查\n"
        << "  GET  /info                            查看桥接配置\n"
        << "  GET  " << "/invoke?input=" << "%7B%22a%22%3A1%7D   GET 传 JSON\n"
        << "  GET  " << "/invoke?a=1&b=2                  GET 传键值（自动组装为 JSON 对象）\n"
        << "  POST " << "/invoke                          body 为 JSON\n";
}

bool parseOptions(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "错误: " << name << " 缺少参数值\n";
                std::exit(1);
            }
            return argv[++i];
        };
        if (a == "--cmd") opt.command = need("--cmd");
        else if (a == "--host") opt.host = need("--host");
        else if (a == "--port") opt.port = std::atoi(need("--port").c_str());
        else if (a == "--route") opt.route = need("--route");
        else if (a == "--cwd") opt.cwd = need("--cwd");
        else if (a == "--timeout") opt.timeoutMs = std::atoi(need("--timeout").c_str());
        else if (a == "--max-body") opt.maxBodyBytes = static_cast<std::size_t>(std::atoll(need("--max-body").c_str()));
        else if (a == "--pretty") opt.pretty = true;
        else if (a == "--no-newline") opt.appendNewline = false;
        else if (a == "--no-cors") opt.cors = false;
        else if (a == "-q" || a == "--quiet") opt.verbose = false;
        else if (a == "-h" || a == "--help") { return false; }
        else {
            std::cerr << "未知选项: " << a << "\n";
            std::exit(1);
        }
    }
    if (!opt.route.empty() && opt.route[0] != '/') opt.route = "/" + opt.route;
    return true;
}

HttpResponse makeJson(const Options& opt, int status, const Json& body) {
    HttpResponse r = HttpResponse::json(status, body.dump(opt.pretty ? 2 : -1));
    if (opt.cors) {
        r.addHeader("Access-Control-Allow-Origin", "*");
        r.addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        r.addHeader("Access-Control-Allow-Headers", "Content-Type");
    }
    return r;
}

// 把 query 中的字符串值尽量转成 JSON 标量：true/false/null/数字/字符串
Json scalarFromString(const std::string& raw) {
    if (raw.empty()) return Json(raw);
    if (raw == "true") return Json(true);
    if (raw == "false") return Json(false);
    if (raw == "null") return Json();
    bool numeric = true;
    bool isReal = false;
    std::size_t start = (raw[0] == '-' || raw[0] == '+') ? 1 : 0;
    if (start >= raw.size()) numeric = false;
    for (std::size_t i = start; i < raw.size() && numeric; ++i) {
        char c = raw[i];
        if (c >= '0' && c <= '9') continue;
        if ((c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') && i > start) { isReal = true; continue; }
        numeric = false;
    }
    if (numeric) {
        try {
            if (isReal) return Json(std::strtod(raw.c_str(), nullptr));
            return Json(std::strtoll(raw.c_str(), nullptr, 10));
        } catch (...) {
            return Json(raw);
        }
    }
    Json parsed;
    if (Json::tryParse(raw, parsed)) return parsed;  // 支持 a={"x":1} 这类嵌套写法
    return Json(raw);
}

// 从请求中提取「要传给控制台程序的输入 JSON」
bool buildInput(const HttpRequest& req, Json& input, std::string& err) {
    auto params = httpbridge::parseQueryString(req.query);

    if (req.method == "POST") {
        std::string body = req.body;
        // 去掉首尾空白
        while (!body.empty() && (body.back() == '\n' || body.back() == '\r' || body.back() == ' ')) body.pop_back();
        while (!body.empty() && (body.front() == '\n' || body.front() == '\r' || body.front() == ' ')) body.erase(body.begin());
        if (body.empty()) {
            // 没有 body 时退化为 query 参数
            Json obj = Json::object();
            for (const auto& kv : params) obj.set(kv.first, scalarFromString(kv.second));
            input = std::move(obj);
            return true;
        }
        if (!Json::tryParse(body, input, &err)) {
            err = "请求体不是合法 JSON: " + err;
            return false;
        }
        return true;
    }

    if (req.method == "GET") {
        // 优先使用 input=<JSON> 参数
        for (const auto& kv : params) {
            if (kv.first == "input") {
                if (!Json::tryParse(kv.second, input, &err)) {
                    err = "input 参数不是合法 JSON: " + err;
                    return false;
                }
                return true;
            }
        }
        Json obj = Json::object();
        for (const auto& kv : params) obj.set(kv.first, scalarFromString(kv.second));
        input = std::move(obj);
        return true;
    }

    err = "不支持的请求方法: " + req.method;
    return false;
}

// 调用本地控制台程序并组装响应
HttpResponse invokeProgram(const Options& opt, const Json& input) {
    std::string payload = input.dump();
    if (opt.appendNewline) payload += "\n";

    procbridge::ProcessResult pr;
    if (opt.command.empty()) {
        // 内置 echo 演示模式：不启动子进程，直接回显
        Json body = Json::object();
        body.set("ok", true);
        body.set("exit_code", 0);
        body.set("data", Json::object());
        body["data"].set("echo", input);
        body["data"].set("message", "内置 echo 模式（未配置 --cmd）");
        body.set("stdout", Json());
        body.set("stderr", "");
        body.set("duration_ms", 0);
        body.set("timed_out", false);
        body.set("error", "");
        return makeJson(opt, 200, body);
    }

    pr = procbridge::runProcess(opt.command, payload, opt.timeoutMs, opt.cwd);

    Json body = Json::object();
    body.set("exit_code", pr.exitCode);
    body.set("timed_out", pr.timedOut);
    body.set("duration_ms", pr.durationMs);
    body.set("stdout", pr.stdoutData);
    body.set("stderr", pr.stderrData);

    if (pr.failedToStart) {
        body.set("ok", false);
        body.set("data", Json());
        body.set("error", std::string("无法启动程序: ") + pr.error);
        return makeJson(opt, 500, body);
    }
    if (pr.timedOut) {
        body.set("ok", false);
        body.set("data", Json());
        body.set("error", std::string("程序执行超时（") + std::to_string(opt.timeoutMs) + " ms）");
        return makeJson(opt, 504, body);
    }

    // 解析子进程 stdout 中的 JSON
    std::string trimmed = pr.stdoutData;
    while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r' || trimmed.back() == ' ')) trimmed.pop_back();
    while (!trimmed.empty() && (trimmed.front() == '\n' || trimmed.front() == '\r' || trimmed.front() == ' ')) trimmed.erase(trimmed.begin());

    Json parsed;
    std::string perr;
    bool okJson = !trimmed.empty() && Json::tryParse(trimmed, parsed, &perr);
    body.set("data", okJson ? parsed : Json());

    if (pr.exitCode != 0) {
        body.set("ok", false);
        body.set("error", std::string("程序退出码 ") + std::to_string(pr.exitCode));
        return makeJson(opt, 200, body);
    }
    if (!okJson) {
        body.set("ok", false);
        body.set("error", std::string("程序 stdout 不是合法 JSON: ") + perr);
        return makeJson(opt, 502, body);
    }
    body.set("ok", true);
    body.set("error", "");
    return makeJson(opt, 200, body);
}

HttpResponse handleRequest(const HttpRequest& req) {
    gRequestCount.fetch_add(1);
    const long long seq = gRequestCount.load();

    if (req.method == "OPTIONS") {
        HttpResponse r = HttpResponse::text(204, "");
        if (gOpts.cors) {
            r.addHeader("Access-Control-Allow-Origin", "*");
            r.addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
            r.addHeader("Access-Control-Allow-Headers", "Content-Type");
        }
        return r;
    }

    if (req.path == "/health") {
        Json body = Json::object();
        body.set("ok", true);
        body.set("service", "cpp-http-bridge");
        long long up = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::steady_clock::now() - gStartTime).count();
        body.set("uptime_sec", up);
        body.set("requests", seq);
        return makeJson(gOpts, 200, body);
    }

    if (req.path == "/info") {
        Json body = Json::object();
        body.set("ok", true);
        body.set("command", gOpts.command.empty() ? Json() : Json(gOpts.command));
        body.set("route", gOpts.route);
        body.set("cwd", gOpts.cwd);
        body.set("timeout_ms", gOpts.timeoutMs);
        body.set("host", gOpts.host);
        body.set("port", gOpts.port);
        return makeJson(gOpts, 200, body);
    }

    if (req.path == "/") {
        Json body = Json::object();
        body.set("ok", true);
        body.set("service", "cpp-http-bridge");
        Json endpoints = Json::array();
        endpoints.push_back("GET  /health");
        endpoints.push_back("GET  /info");
        endpoints.push_back("GET  " + gOpts.route + "?input=<urlencoded JSON>");
        endpoints.push_back("POST " + gOpts.route + "  (body: JSON)");
        body.set("endpoints", endpoints);
        body.set("note", "输入 JSON 会写入被桥接程序的 stdin，其 stdout 需为 JSON，原样返回给客户端");
        return makeJson(gOpts, 200, body);
    }

    if (req.path != gOpts.route) {
        Json body = Json::object();
        body.set("ok", false);
        body.set("error", "路径不存在: " + req.path + "，请使用 " + gOpts.route);
        return makeJson(gOpts, 404, body);
    }

    if (req.contentLength() > gOpts.maxBodyBytes) {
        Json body = Json::object();
        body.set("ok", false);
        body.set("error", "请求体过大，上限 " + std::to_string(gOpts.maxBodyBytes) + " 字节");
        return makeJson(gOpts, 413, body);
    }

    Json input;
    std::string err;
    if (!buildInput(req, input, err)) {
        Json body = Json::object();
        body.set("ok", false);
        body.set("error", err);
        return makeJson(gOpts, 400, body);
    }

    if (gOpts.verbose) {
        std::printf("[%lld] %s %s <- %s\n", seq, req.method.c_str(), req.path.c_str(), input.dump().c_str());
        std::fflush(stdout);
    }
    HttpResponse resp = invokeProgram(gOpts, input);
    if (gOpts.verbose) {
        std::printf("[%lld] -> %d %s\n", seq, resp.status, resp.body.c_str());
        std::fflush(stdout);
    }
    return resp;
}

}  // namespace

int main(int argc, char** argv) {
    if (!parseOptions(argc, argv, gOpts)) {
        printUsage(argv[0]);
        return 0;
    }
    gCommand = gOpts.command;

    std::signal(SIGINT, [](int) { gRunning.store(false); });
    std::signal(SIGTERM, [](int) { gRunning.store(false); });

    httpbridge::HttpServer server(gOpts.host, gOpts.port, gOpts.maxBodyBytes);
    server.setHandler(handleRequest);
    if (!server.start()) {
        std::cerr << "启动失败: " << server.error() << "\n";
        return 1;
    }
    gOpts.port = server.port();

    std::cout << "cpp-http-bridge 已启动\n"
              << "  监听: http://" << gOpts.host << ":" << gOpts.port << "\n"
              << "  桥接程序: " << (gOpts.command.empty() ? std::string("(内置 echo 演示模式)") : gOpts.command) << "\n"
              << "  路由: GET/POST " << gOpts.route << "  (GET 也支持 ?input=<JSON> 或 ?k=v)\n"
              << "  超时: " << gOpts.timeoutMs << " ms\n"
              << "  示例: curl 'http://" << gOpts.host << ":" << gOpts.port << gOpts.route
              << "?input=%7B%22a%22%3A1%7D'\n"
              << "  按 Ctrl+C 退出\n";

    // serveForever 为阻塞调用，这里用后台线程跑，主线程等待退出信号
    std::thread serverThread([&server]() { server.serveForever(); });
    while (gRunning.load()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    server.stop();
    serverThread.join();
    std::cout << "已停止\n";
    return 0;
}
