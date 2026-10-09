#ifndef HTTPBRIDGE_HTTP_SERVER_HPP
#define HTTPBRIDGE_HTTP_SERVER_HPP

// ---------------------------------------------------------------------------
// httpbridge::HttpServer —— 基于 POSIX socket 的极简 HTTP/1.1 服务器
//
//   * 支持 GET / POST（以及任意方法，由业务 handler 自行判断）
//   * 自动解析：method / path / query / headers / body（按 Content-Length）
//   * 每连接一个线程，主线程 accept
//   * 无第三方依赖；macOS / Linux 可直接编译
// ---------------------------------------------------------------------------

#include <atomic>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace httpbridge {

struct HttpRequest {
    std::string method;
    std::string path;              // 不含 query，如 "/invoke"
    std::string query;             // 原始 query 串，如 "a=1&b=2"
    std::string version;           // 如 "HTTP/1.1"
    std::string body;              // 请求体原文
    std::vector<std::pair<std::string, std::string>> headers;
    std::string remoteAddress;

    // 大小写不敏感地获取 header，不存在返回空串
    std::string header(const std::string& name) const;
    std::string contentType() const { return header("Content-Type"); }
    std::size_t contentLength() const;
};

struct HttpResponse {
    int status = 200;
    std::string body;
    std::string contentType = "application/json; charset=utf-8";
    std::vector<std::pair<std::string, std::string>> headers;

    static HttpResponse json(int status, const std::string& jsonBody);
    static HttpResponse text(int status, const std::string& textBody);
    void addHeader(const std::string& key, const std::string& value);
    std::string toBytes() const;   // 序列化为完整 HTTP 响应报文
};

class HttpServer {
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;

    HttpServer(std::string host, int port, std::size_t maxBodyBytes = 4u * 1024 * 1024,
               int readTimeoutSec = 15, int writeTimeoutSec = 15);
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    void setHandler(Handler handler);
    bool start();                  // 创建 socket / bind / listen
    void serveForever();           // 阻塞循环，直到 stop()
    void stop();                   // 停止 accept 循环
    bool running() const { return running_.load(); }
    int port() const { return port_; }
    const std::string& error() const { return error_; }

private:
    void handleConnection(int clientFd, const std::string& peer);
    bool readRequest(int fd, HttpRequest& req);
    bool writeAll(int fd, const char* data, std::size_t len);

    std::string host_;
    int port_;
    std::size_t maxBodyBytes_;
    int readTimeoutSec_;
    int writeTimeoutSec_;
    int listenFd_ = -1;
    std::atomic<bool> running_{false};
    std::string error_;
    Handler handler_;
};

// ---------- URL 编解码 / query 解析工具 ----------
std::string urlDecode(const std::string& s);
std::string urlEncode(const std::string& s);
std::vector<std::pair<std::string, std::string>> parseQueryString(const std::string& query);

}  // namespace httpbridge

#endif  // HTTPBRIDGE_HTTP_SERVER_HPP
