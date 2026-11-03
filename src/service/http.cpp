#include "http.hpp"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

namespace iis::svc {

namespace {

constexpr size_t kMaxHead = 32 * 1024, kMaxBody = 4 * 1024 * 1024;

std::string lower(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

std::string url_decode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') o += ' ';
        else if (s[i] == '%' && i + 2 < s.size() && isxdigit(uint8_t(s[i + 1])) && isxdigit(uint8_t(s[i + 2]))) {
            o += char(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else o += s[i];
    }
    return o;
}

const char* reason(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 429: return "Too Many Requests";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        default: return "Internal Server Error";
    }
}

bool send_all(SOCKET s, const std::string& d) {
    for (size_t off = 0; off < d.size();) {
        const int n = send(s, d.data() + off, int(std::min<size_t>(d.size() - off, 1 << 20)), 0);
        if (n <= 0) return false;
        off += size_t(n);
    }
    return true;
}

}  // namespace

std::string HttpRequest::param(const std::string& key) const {
    for (size_t i = 0; i <= query.size();) {
        size_t j = query.find('&', i);
        if (j == std::string::npos) j = query.size();
        const std::string kv = query.substr(i, j - i);
        const size_t eq = kv.find('=');
        if (url_decode(kv.substr(0, eq)) == key) return eq == std::string::npos ? "" : url_decode(kv.substr(eq + 1));
        i = j + 1;
    }
    return {};
}

HttpServer::~HttpServer() { stop(); }

bool HttpServer::start(uint16_t port, Handler handler, std::string& err) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        err = "无法初始化网络";
        return false;
    }
    handler_ = std::move(handler);
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        err = "无法创建套接字";
        return false;
    }
    BOOL excl = TRUE;  // 不允许其他程序抢占同一端口
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof excl);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 只监听本机，绝不对外
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(s, 16) != 0) {
        err = "管理界面端口 " + std::to_string(port) + " 无法监听（错误 " + std::to_string(WSAGetLastError()) +
              "），可能被其他程序占用";
        closesocket(s);
        return false;
    }
    int len = sizeof a;
    getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
    port_ = ntohs(a.sin_port);
    listen_ = uintptr_t(s);
    stopping_ = false;
    thread_ = std::thread([this] { loop(); });
    return true;
}

void HttpServer::stop() {
    if (listen_ == ~uintptr_t(0)) return;
    stopping_ = true;
    closesocket(SOCKET(listen_));  // 让 accept 返回
    if (thread_.joinable()) thread_.join();
    listen_ = ~uintptr_t(0);
    WSACleanup();
}

void HttpServer::loop() {
    while (!stopping_) {
        SOCKET c = accept(SOCKET(listen_), nullptr, nullptr);
        if (c == INVALID_SOCKET) {
            if (stopping_) return;
            continue;
        }
        DWORD timeout = 5000;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
        serve(uintptr_t(c));
        closesocket(c);
    }
}

void HttpServer::serve(uintptr_t sock) {
    const SOCKET s = SOCKET(sock);
    std::string buf;
    char tmp[8192];
    size_t head_end;
    while ((head_end = buf.find("\r\n\r\n")) == std::string::npos) {
        if (buf.size() > kMaxHead) return;
        const int n = recv(s, tmp, sizeof tmp, 0);
        if (n <= 0) return;
        buf.append(tmp, size_t(n));
    }
    HttpRequest req;
    HttpResponse res;
    const std::string head = buf.substr(0, head_end);
    std::string body = buf.substr(head_end + 4);
    size_t line_end = head.find("\r\n");
    const std::string first = head.substr(0, line_end);
    const size_t sp1 = first.find(' '), sp2 = first.rfind(' ');
    if (sp1 == std::string::npos || sp2 == sp1) return;
    req.method = first.substr(0, sp1);
    std::string target = first.substr(sp1 + 1, sp2 - sp1 - 1);
    const size_t q = target.find('?');
    req.path = url_decode(target.substr(0, q));
    if (q != std::string::npos) req.query = target.substr(q + 1);
    for (size_t i = line_end == std::string::npos ? head.size() : line_end + 2; i < head.size();) {
        size_t e = head.find("\r\n", i);
        if (e == std::string::npos) e = head.size();
        const std::string line = head.substr(i, e - i);
        const size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string v = line.substr(colon + 1);
            while (!v.empty() && v.front() == ' ') v.erase(0, 1);
            req.headers[lower(line.substr(0, colon))] = v;
        }
        i = e + 2;
    }
    size_t want = 0;
    if (req.headers.count("content-length")) {
        try {
            want = size_t(std::stoull(req.headers["content-length"]));
        } catch (...) {
            return;
        }
    }
    if (want > kMaxBody) {
        res = {413, "text/plain; charset=utf-8", "请求太大"};
    } else {
        while (body.size() < want) {
            const int n = recv(s, tmp, sizeof tmp, 0);
            if (n <= 0) return;
            body.append(tmp, size_t(n));
        }
        req.body = body.substr(0, want);
        try {
            res = handler_(req);
        } catch (const std::exception& e) {
            res = {500, "application/json; charset=utf-8", std::string("{\"error\":\"内部错误\"}")};
            (void)e;
        }
    }
    std::string out = "HTTP/1.1 " + std::to_string(res.status) + " " + reason(res.status) + "\r\nContent-Type: " +
                      res.type + "\r\nContent-Length: " + std::to_string(res.body.size()) +
                      "\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
                      "X-Frame-Options: DENY\r\nReferrer-Policy: no-referrer\r\n\r\n";
    send_all(s, out);
    send_all(s, res.body);
}

}  // namespace iis::svc
