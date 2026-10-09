// -----------------------------------------------------------------------------
// graph_server —— 把 MiniGraph 图数据库通过 HTTP 暴露出去（支持多张图）
//
//   ./bin/graph_server --dir data --graph default
//
// 启动后：
//   浏览器打开 http://127.0.0.1:8080/        可视化控制台
//   curl http://127.0.0.1:8080/api/health    健康检查
//   curl http://127.0.0.1:8080/api/graphs    图（数据库）列表
//   curl http://127.0.0.1:8080/api/manifest  接口清单
//
// 一张图 = data/ 目录下的一个 <图名>.mgdb 文件，启动时自动全部载入。
// -----------------------------------------------------------------------------

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "graph_server/GraphService.h"
#include "http_server.hpp"
#include "json.hpp"

namespace {

graphserver::GraphService* g_service = nullptr;
httpbridge::HttpServer*    g_server  = nullptr;

// Ctrl-C / kill：先落盘再退出，避免丢数据
void onTerminate(int) {
    if (g_service) g_service->flush();
    if (g_server)  g_server->stop();
    std::printf("\n[graph_server] 已落盘并退出\n");
    std::fflush(stdout);
    ::_exit(0);
}

// 确保数据文件所在目录存在（避免首次启动时因目录缺失导致落盘失败）
bool ensureParentDir(const std::string& filePath, std::string& err) {
    const std::size_t slash = filePath.find_last_of('/');
    if (slash == std::string::npos || slash == 0) return true;  // 无目录或根目录
    const std::string dir = filePath.substr(0, slash);

    std::string partial;
    auto tryMkdir = [&err](const std::string& d) {
        if (d.empty() || d == "/") return true;
        if (::mkdir(d.c_str(), 0755) != 0 && errno != EEXIST) {
            err = "无法创建目录 " + d + ": " + std::strerror(errno);
            return false;
        }
        return true;
    };
    for (std::size_t i = 0; i < dir.size(); ++i) {
        if (dir[i] == '/' && !tryMkdir(partial)) return false;
        partial += dir[i];
    }
    return tryMkdir(partial);
}

bool readTextFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

const char* kFallbackPage = R"HTML(<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="utf-8"><title>graph_server</title></head>
<body style="font-family:-apple-system,BlinkMacSystemFont,'PingFang SC',sans-serif;max-width:760px;margin:60px auto;line-height:1.7">
<h1>graph_server 已启动</h1>
<p>可视化控制台文件未找到（web/index.html）。请在与启动时相同的目录下放置该文件，或用 <code>--web</code> 指定路径。</p>
<p>可以直接用 HTTP 访问这些接口：</p>
<ul>
  <li><a href="/api/health">/api/health</a> —— 健康检查</li>
  <li><a href="/api/manifest">/api/manifest</a> —— 接口清单</li>
  <li><a href="/api/graphs">/api/graphs</a> —— 图（数据库）列表</li>
  <li><a href="/api/stats">/api/stats</a> —— 当前图统计</li>
  <li><a href="/api/nodes?full=1">/api/nodes?full=1</a> —— 所有节点</li>
</ul>
</body></html>)HTML";

httpbridge::HttpResponse serveConsole(const std::string& webFile) {
    httpbridge::HttpResponse resp;
    resp.status = 200;
    if (readTextFile(webFile, resp.body)) {
        resp.contentType = "text/html; charset=utf-8";
    } else {
        resp.body = kFallbackPage;
        resp.contentType = "text/html; charset=utf-8";
    }
    return resp;
}

httpbridge::HttpResponse withCors(httpbridge::HttpResponse r) {
    r.addHeader("Access-Control-Allow-Origin", "*");
    r.addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, PATCH, DELETE, OPTIONS");
    r.addHeader("Access-Control-Allow-Headers", "Content-Type");
    return r;
}

void printUsage(const char* argv0) {
    std::printf(
        "用法: %s [选项]\n"
        "  --host <地址>      监听地址，默认 127.0.0.1\n"
        "  --port <端口>      监听端口，默认 8080\n"
        "  --dir  <目录>      数据目录（一张图一个 <图名>.mgdb），默认 data\n"
        "  --graph <名字>     默认图名，默认 default\n"
        "  --data <文件>      兼容旧参数：等价于 --dir 文件所在目录 --graph 文件名（去后缀）\n"
        "  --web  <文件>      控制台页面路径，默认 web/index.html\n"
        "  --no-autosave      关闭自动落盘（需自行调用 POST /api/admin/save）\n"
        "  -h, --help         显示本帮助\n"
        "\n"
        "多张图：启动时自动载入 --dir 目录下所有 *.mgdb；\n"
        "        请求时用 /api/g/{图名}/... 或请求头 X-Graph 指定操作哪张图。\n",
        argv0);
}

}  // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    int port = 8080;
    std::string dataDir = "data";
    std::string defaultGraph = graphserver::kDefaultGraphName;
    std::string webFile = "web/index.html";
    bool autoSave = true;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "错误: %s 缺少参数值\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--host")             host = next("--host");
        else if (a == "--port")        port = std::atoi(next("--port").c_str());
        else if (a == "--dir")         dataDir = next("--dir");
        else if (a == "--graph")       defaultGraph = next("--graph");
        else if (a == "--data") {
            // 兼容旧用法 --data data/graph.mgdb：目录 + 图名拆开
            const std::string file = next("--data");
            const std::size_t slash = file.find_last_of('/');
            if (slash != std::string::npos) dataDir = file.substr(0, slash);
            std::string base = (slash == std::string::npos) ? file : file.substr(slash + 1);
            const std::string suffix = graphserver::kGraphFileSuffix;
            if (base.size() > suffix.size() &&
                base.compare(base.size() - suffix.size(), suffix.size(), suffix) == 0) {
                base = base.substr(0, base.size() - suffix.size());
            }
            if (!base.empty()) defaultGraph = base;
        }
        else if (a == "--web")         webFile = next("--web");
        else if (a == "--no-autosave") autoSave = false;
        else if (a == "-h" || a == "--help") { printUsage(argv[0]); return 0; }
        else {
            std::fprintf(stderr, "未知参数: %s\n", a.c_str());
            printUsage(argv[0]);
            return 2;
        }
    }

    // ---------------- 打开数据目录（内含若干张图） ----------------
    std::string dirErr;
    if (!ensureParentDir(dataDir + "/", dirErr)) {
        std::fprintf(stderr, "[graph_server] %s\n", dirErr.c_str());
        return 1;
    }
    graphserver::GraphService service(dataDir, autoSave);
    service.setDefaultGraph(defaultGraph);
    std::string openErr;
    if (!service.open(openErr)) {
        std::fprintf(stderr, "[graph_server] %s\n", openErr.c_str());
        return 1;
    }

    // ---------------- 启动 HTTP 服务 ----------------
    httpbridge::HttpServer server(host, port, 8u * 1024 * 1024, 15, 15);
    server.setHandler([&service, &webFile](const httpbridge::HttpRequest& req) {
        if (req.method == "OPTIONS") {
            httpbridge::HttpResponse r;
            r.status = 204;
            r.body.clear();
            return withCors(r);
        }
        // 控制台页面
        if (req.path == "/" || req.path == "/index.html" || req.path == "/console") {
            return withCors(serveConsole(webFile));
        }
        // 其余全部交给业务层
        graphserver::ApiResult result = service.dispatch(req);
        return withCors(httpbridge::HttpResponse::json(result.status, result.body.dump(2)));
    });

    if (!server.start()) {
        std::fprintf(stderr, "[graph_server] 启动失败: %s\n", server.error().c_str());
        return 1;
    }

    g_service = &service;
    g_server = &server;
    std::signal(SIGINT, onTerminate);
    std::signal(SIGTERM, onTerminate);

    std::printf("=========================================================\n");
    std::printf(" graph_server 已启动（支持多张图）\n");
    std::printf("   控制台   : http://%s:%d/\n", host.c_str(), port);
    std::printf("   数据目录 : %s/ （默认图: %s）\n", dataDir.c_str(), defaultGraph.c_str());
    std::printf("   自动落盘 : %s\n", autoSave ? "开" : "关（需 POST /api/admin/save）");
    std::printf("   图列表   : http://%s:%d/api/graphs\n", host.c_str(), port);
    std::printf("   接口清单 : http://%s:%d/api/manifest\n", host.c_str(), port);
    std::printf("=========================================================\n");
    std::printf(" Ctrl-C 退出（退出前自动落盘）\n");
    std::fflush(stdout);

    server.serveForever();
    return 0;
}
