#include "http_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace httpbridge {

namespace {

const char* statusText(int code) {
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 413: return "Payload Too Large";
        case 415: return "Unsupported Media Type";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 504: return "Gateway Timeout";
        default: return "Unknown Status";
    }
}

std::string toLower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string trim(std::string s) {
    const char* ws = " \t\r\n";
    std::size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos) return std::string();
    std::size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

// ---------------- HttpRequest ----------------

std::string HttpRequest::header(const std::string& name) const {
    const std::string target = toLower(name);
    for (const auto& kv : headers)
        if (toLower(kv.first) == target) return kv.second;
    return std::string();
}

std::size_t HttpRequest::contentLength() const {
    std::string v = header("Content-Length");
    if (v.empty()) return 0;
    return static_cast<std::size_t>(std::strtoull(v.c_str(), nullptr, 10));
}

// ---------------- HttpResponse ----------------

HttpResponse HttpResponse::json(int status, const std::string& jsonBody) {
    HttpResponse r;
    r.status = status;
    r.body = jsonBody;
    r.contentType = "application/json; charset=utf-8";
    return r;
}

HttpResponse HttpResponse::text(int status, const std::string& textBody) {
    HttpResponse r;
    r.status = status;
    r.body = textBody;
    r.contentType = "text/plain; charset=utf-8";
    return r;
}

void HttpResponse::addHeader(const std::string& key, const std::string& value) {
    headers.emplace_back(key, value);
}

std::string HttpResponse::toBytes() const {
    std::string out;
    out.reserve(body.size() + 256);
    out += "HTTP/1.1 ";
    out += std::to_string(status);
    out += ' ';
    out += statusText(status);
    out += "\r\n";

    bool hasContentType = false;
    bool hasContentLength = false;
    for (const auto& kv : headers) {
        if (toLower(kv.first) == "content-type") hasContentType = true;
        if (toLower(kv.first) == "content-length") hasContentLength = true;
        out += kv.first;
        out += ": ";
        out += kv.second;
        out += "\r\n";
    }
    if (!hasContentType && !contentType.empty()) {
        out += "Content-Type: ";
        out += contentType;
        out += "\r\n";
    }
    if (!hasContentLength) {
        out += "Content-Length: ";
        out += std::to_string(body.size());
        out += "\r\n";
    }
    out += "Connection: close\r\n\r\n";
    out += body;
    return out;
}

// ---------------- URL 工具 ----------------

std::string urlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '%' && i + 2 < s.size()) {
            int h = hexValue(s[i + 1]);
            int l = hexValue(s[i + 2]);
            if (h >= 0 && l >= 0) {
                out += static_cast<char>((h << 4) | l);
                i += 2;
                continue;
            }
        }
        if (c == '+') {
            out += ' ';
            continue;
        }
        out += c;
    }
    return out;
}

std::string urlEncode(const std::string& s) {
    static const char* kUnreserved = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.~";
    std::string out;
    for (unsigned char c : s) {
        if (std::strchr(kUnreserved, c)) {
            out += static_cast<char>(c);
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> parseQueryString(const std::string& query) {
    std::vector<std::pair<std::string, std::string>> out;
    std::size_t start = 0;
    while (start <= query.size()) {
        std::size_t amp = query.find('&', start);
        std::string part = query.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
        if (!part.empty()) {
            std::size_t eq = part.find('=');
            std::string key = urlDecode(eq == std::string::npos ? part : part.substr(0, eq));
            std::string val = eq == std::string::npos ? std::string() : urlDecode(part.substr(eq + 1));
            out.emplace_back(key, val);
        }
        if (amp == std::string::npos) break;
        start = amp + 1;
    }
    return out;
}

// ---------------- HttpServer ----------------

HttpServer::HttpServer(std::string host, int port, std::size_t maxBodyBytes,
                       int readTimeoutSec, int writeTimeoutSec)
    : host_(std::move(host)), port_(port), maxBodyBytes_(maxBodyBytes),
      readTimeoutSec_(readTimeoutSec), writeTimeoutSec_(writeTimeoutSec) {
    std::signal(SIGPIPE, SIG_IGN);  // 忽略 SIGPIPE，写已关闭的连接时返回错误而非终止进程
}

HttpServer::~HttpServer() {
    stop();
    if (listenFd_ >= 0) {
        ::close(listenFd_);
        listenFd_ = -1;
    }
}

void HttpServer::setHandler(Handler handler) { handler_ = std::move(handler); }

bool HttpServer::start() {
    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        error_ = std::string("socket() failed: ") + std::strerror(errno);
        return false;
    }
    int on = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port_));
    if (host_.empty() || host_ == "0.0.0.0" || host_ == "*") {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1) {
        error_ = "invalid host address: " + host_;
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }

    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        error_ = std::string("bind() failed: ") + std::strerror(errno);
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }
    if (port_ == 0) {  // 端口 0：由系统分配，回读实际端口
        sockaddr_in actual{};
        socklen_t len = sizeof(actual);
        if (::getsockname(listenFd_, reinterpret_cast<sockaddr*>(&actual), &len) == 0)
            port_ = ntohs(actual.sin_port);
    }
    if (::listen(listenFd_, 128) != 0) {
        error_ = std::string("listen() failed: ") + std::strerror(errno);
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }
    running_.store(true);
    return true;
}

void HttpServer::stop() { running_.store(false); }

void HttpServer::serveForever() {
    while (running_.load()) {
        sockaddr_in peer{};
        socklen_t peerLen = sizeof(peer);
        int cfd = ::accept(listenFd_, reinterpret_cast<sockaddr*>(&peer), &peerLen);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            if (!running_.load()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        char ip[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        std::string peerStr = std::string(ip) + ":" + std::to_string(ntohs(peer.sin_port));

        std::thread([this, cfd, peerStr]() { handleConnection(cfd, peerStr); }).detach();
    }
}

bool HttpServer::writeAll(int fd, const char* data, std::size_t len) {
    std::size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, data + sent, len - sent, 0);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        return false;
    }
    return true;
}

bool HttpServer::readRequest(int fd, HttpRequest& req) {
    timeval tv{};
    tv.tv_sec = readTimeoutSec_;
    tv.tv_usec = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::string buf;
    buf.reserve(4096);
    char chunk[4096];

    // 1) 读到头部结束符 "\r\n\r\n"
    std::size_t headerEnd = std::string::npos;
    while (headerEnd == std::string::npos) {
        ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n > 0) {
            buf.append(chunk, static_cast<std::size_t>(n));
            headerEnd = buf.find("\r\n\r\n");
        } else if (n == 0) {
            if (buf.empty()) return false;
            headerEnd = buf.size();  // 客户端提前关闭，按已有内容解析
        } else {
            if (errno == EINTR) continue;
            if (buf.empty()) return false;
            headerEnd = buf.size();
        }
        if (buf.size() > maxBodyBytes_) return false;
    }

    std::size_t bodyStart = headerEnd + 4;
    std::string head = buf.substr(0, headerEnd);

    // 2) 解析请求行
    std::size_t lineEnd = head.find("\r\n");
    std::string requestLine = lineEnd == std::string::npos ? head : head.substr(0, lineEnd);
    std::size_t rest = lineEnd == std::string::npos ? head.size() : lineEnd + 2;

    std::size_t sp1 = requestLine.find(' ');
    if (sp1 == std::string::npos) return false;
    std::size_t sp2 = requestLine.find(' ', sp1 + 1);
    req.method = requestLine.substr(0, sp1);
    std::string target = sp2 == std::string::npos ? requestLine.substr(sp1 + 1)
                                                  : requestLine.substr(sp1 + 1, sp2 - sp1 - 1);
    req.version = sp2 == std::string::npos ? "HTTP/1.1" : requestLine.substr(sp2 + 1);

    std::size_t qpos = target.find('?');
    if (qpos == std::string::npos) {
        req.path = target;
    } else {
        req.path = target.substr(0, qpos);
        req.query = target.substr(qpos + 1);
    }

    // 3) 解析头部
    std::size_t pos = rest;
    while (pos < head.size()) {
        std::size_t e = head.find("\r\n", pos);
        std::string line = head.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        if (line.empty()) break;
        std::size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = trim(line.substr(0, colon));
            std::string val = trim(line.substr(colon + 1));
            req.headers.emplace_back(key, val);
        }
        if (e == std::string::npos) break;
        pos = e + 2;
    }

    // 4) 按 Content-Length 收齐请求体
    std::size_t want = req.contentLength();
    if (want > maxBodyBytes_) {
        req.body = buf.substr(bodyStart);
        return true;  // 交给上层按 413 处理
    }
    if (buf.size() < bodyStart + want) {
        while (buf.size() < bodyStart + want) {
            ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
            if (n > 0) buf.append(chunk, static_cast<std::size_t>(n));
            else if (n == 0) break;
            else {
                if (errno == EINTR || errno == EAGAIN) continue;
                break;
            }
        }
    }
    if (buf.size() > bodyStart) req.body = buf.substr(bodyStart, want);
    return true;
}

void HttpServer::handleConnection(int clientFd, const std::string& peer) {
    HttpRequest req;
    if (!readRequest(clientFd, req)) {
        ::close(clientFd);
        return;
    }
    req.remoteAddress = peer;

    HttpResponse resp;
    if (handler_) {
        try {
            resp = handler_(req);
        } catch (const std::exception& e) {
            resp = HttpResponse::json(500, std::string("{\"ok\":false,\"error\":\"handler exception: ") + e.what() + "\"}");
        } catch (...) {
            resp = HttpResponse::json(500, "{\"ok\":false,\"error\":\"unknown handler exception\"}");
        }
    } else {
        resp = HttpResponse::json(500, "{\"ok\":false,\"error\":\"no handler\"}");
    }

    timeval tv{};
    tv.tv_sec = writeTimeoutSec_;
    ::setsockopt(clientFd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    std::string bytes = resp.toBytes();
    writeAll(clientFd, bytes.data(), bytes.size());
    ::shutdown(clientFd, SHUT_WR);
    ::close(clientFd);
}

}  // namespace httpbridge
