// ============================================================================
// Illuminator HTTP 服务器 — 精简内嵌 HTTP 服务器
// ============================================================================
//
// 本文件实现了一个极简的 POSIX socket 级别 HTTP/1.1 服务器。
// 无需任何第三方 HTTP 库依赖（如 Drogon、cpp-httplib）。
// 适用于嵌入式部署和低依赖场景。
//
// 核心功能：
// ==========
// 1. 路由注册（RegisterHandler）
//    - 支持路径 → handler 函数的映射
//    - handler 接收 path 参数，返回响应 body 字符串
//    - 线程安全的路由表操作（mutex 保护）
//
// 2. 静态文件服务（SetStaticDir）
//    - 设置 web 静态文件根目录
//    - 自动处理 MIME 类型（html/js/css/json/svg/png/ico/wasm）
//    - SPA fallback：对非文件路径请求自动返回 index.html
//
// 3. 生命周期管理（Start/Stop）
//    - socket → bind → listen → accept 标准流程
//    - 每个连接在独立调用中处理（同步阻塞 accept 循环）
//    - 支持优雅关闭（shutdown + close）
//
// 架构限制：
// ==========
// - 单线程 accept + 同步处理（无 keep-alive，无并发连接池）
// - 适用于低负载的 API 和指标端点
// - 生产环境建议通过反向代理（nginx）前置
// ============================================================================

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <fstream>
#include <sys/socket.h>         // socket/bind/listen/accept
#include <netinet/in.h>         // sockaddr_in/INADDR_ANY
#include <unistd.h>             // close/shutdown
#include <cstring>
#include <sstream>

#include "core/common/logging.h"
#include "core/common/status.h"

namespace illuminator {

class HttpServer {
public:
    // Handler 类型：接收请求路径，返回响应 body 字符串
    using Handler = std::function<std::string(const std::string& path)>;

    // ---- 启动服务器 ----
    // listen_addr: 监听地址（当前不支持，固定 INADDR_ANY）
    // port:       监听端口
    Status Start(const std::string& listen_addr, int port) {
        // 创建 TCP socket（IPv4，流式）
        server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd_ < 0) {
            return Status::Error(StatusCode::kInternal, "Socket creation failed");
        }

        // 设置 SO_REUSEADDR 允许端口快速复用（避免 TIME_WAIT 绑定失败）
        int opt = 1;
        setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;   // 监听所有网络接口
        addr.sin_port = htons(port);         // 网络字节序端口

        if (bind(server_fd_, reinterpret_cast<struct sockaddr*>(&addr),
                 sizeof(addr)) < 0) {
            close(server_fd_);
            return Status::Error(StatusCode::kInternal,
                "Bind failed on port " + std::to_string(port));
        }

        // 开始监听，backlog 设为 16
        if (listen(server_fd_, 16) < 0) {
            close(server_fd_);
            return Status::Error(StatusCode::kInternal, "Listen failed");
        }

        // 启动 accept 线程
        running_ = true;
        thread_ = std::thread([this] { AcceptLoop(); });
        IL_INFO("HTTP server started on port %d", port);
        return Status::Ok();
    }

    // ---- 停止服务器 ----
    void Stop() {
        running_ = false;
        if (server_fd_ >= 0) {
            shutdown(server_fd_, SHUT_RDWR);  // 中断 accept 调用
            close(server_fd_);
            server_fd_ = -1;
        }
        if (thread_.joinable()) thread_.join();
    }

    // ---- 注册路径处理器 ----
    // 如 RegisterHandler("/healthz", [](auto&) { return "ok"; });
    void RegisterHandler(const std::string& path, Handler handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        handlers_[path] = std::move(handler);
    }

    // ---- 设置静态文件目录 ----
    // 用于服务 Web 前端构建产物（React SPA 的 dist/ 目录）
    void SetStaticDir(const std::string& dir) {
        static_dir_ = dir;
    }

private:
    // ---- Accept 循环（运行在独立线程中） ----
    void AcceptLoop() {
        while (running_) {
            struct sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept(server_fd_,
                reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);
            if (client_fd < 0) continue;  // 可能是 shutdown 中断导致的错误
            HandleClient(client_fd);      // 同步处理请求
        }
    }

    // ---- 处理单个客户端请求 ----
    void HandleClient(int fd) {
        char buf[4096] = {};
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n <= 0) { close(fd); return; }

        // 解析 HTTP 请求行，提取路径
        std::string request(buf, n);
        std::string path = ParsePath(request);

        std::string body;
        std::string content_type = "text/plain";

        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = handlers_.find(path);
            if (it != handlers_.end()) {
                // 匹配到注册的 handler
                body = it->second(path);
                if (path.find("/api/") == 0) content_type = "application/json";
            } else if (!static_dir_.empty()) {
                // 静态文件服务
                std::string file_path = static_dir_;
                file_path += (path == "/" ? "/index.html" : path);

                // SPA fallback：非文件路径请求返回 index.html
                std::ifstream file(file_path, std::ios::binary);
                if (!file.is_open() && path.find('.') == std::string::npos) {
                    file.open(static_dir_ + "/index.html", std::ios::binary);
                    file_path = static_dir_ + "/index.html";
                }

                if (file.is_open()) {
                    body = std::string(std::istreambuf_iterator<char>(file), {});
                    content_type = GuessContentType(file_path);
                } else {
                    body = "404 Not Found\n";
                }
            } else {
                body = "404 Not Found\n";
            }
        }

        // 构造 HTTP 响应
        std::ostringstream response;
        response << "HTTP/1.1 200 OK\r\n"
                 << "Content-Type: " << content_type << "\r\n"
                 << "Content-Length: " << body.size() << "\r\n"
                 << "Connection: close\r\n\r\n"
                 << body;

        std::string resp = response.str();
        write(fd, resp.c_str(), resp.size());
        close(fd);
    }

    // ---- 从 HTTP 请求行中提取路径 ----
    // "GET /api/v1/cpu/utilization HTTP/1.1" → "/api/v1/cpu/utilization"
    // 自动去除查询参数（?key=val 部分）
    static std::string ParsePath(const std::string& request) {
        auto pos = request.find(' ');
        if (pos == std::string::npos) return "/";
        auto end = request.find(' ', pos + 1);
        if (end == std::string::npos) return "/";
        auto full = request.substr(pos + 1, end - pos - 1);
        auto qmark = full.find('?');
        return (qmark != std::string::npos) ? full.substr(0, qmark) : full;
    }

    // ---- 根据文件扩展名猜测 MIME 类型 ----
    static std::string GuessContentType(const std::string& path) {
        auto ext = path.substr(path.rfind('.') + 1);
        if (ext == "html") return "text/html";
        if (ext == "js")   return "application/javascript";
        if (ext == "css")  return "text/css";
        if (ext == "json") return "application/json";
        if (ext == "svg")  return "image/svg+xml";
        if (ext == "png")  return "image/png";
        if (ext == "ico")  return "image/x-icon";
        if (ext == "wasm") return "application/wasm";
        return "text/plain";
    }

    std::string static_dir_;                              // 静态文件根目录
    int server_fd_ = -1;                                  // 监听 socket fd
    std::atomic<bool> running_{false};                    // 运行状态标志
    std::thread thread_;                                  // Accept 线程
    std::mutex mutex_;                                    // 保护 handlers_ 的互斥锁
    std::unordered_map<std::string, Handler> handlers_;   // 路径 → handler 映射
};

}  // namespace illuminator
