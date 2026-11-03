// 本机管理界面用的极简 HTTP/1.1 服务器（T7）：只监听 127.0.0.1，逐个处理连接（Connection: close），
// 请求头与正文有上限、收发有超时。只服务本机浏览器，不追求并发性能。
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace iis::svc {

struct HttpRequest {
    std::string method, path, query, body;
    std::map<std::string, std::string> headers;  // 键为小写
    std::string param(const std::string& key) const;  // 查询参数（已做 % 解码）
};

struct HttpResponse {
    int status = 200;
    std::string type = "application/json; charset=utf-8";
    std::string body;
};

class HttpServer {
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;
    ~HttpServer();
    // port 为 0 时由系统选一个空闲端口（测试用）；实际端口见 port()
    bool start(uint16_t port, Handler handler, std::string& err);
    void stop();
    uint16_t port() const { return port_; }

private:
    void loop();
    void serve(uintptr_t sock);
    Handler handler_;
    uintptr_t listen_ = ~uintptr_t(0);
    uint16_t port_ = 0;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
};

}  // namespace iis::svc
