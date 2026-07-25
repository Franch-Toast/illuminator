// ============================================================================
// Illuminator HTTP 服务器 — cpp-httplib 薄封装
// ============================================================================
// 直接暴露 httplib::Server，Handler 使用 httplib::Request/Response。
// ============================================================================

#pragma once

#include <chrono>
#include <fstream>
#include <functional>
#include <string>
#include <thread>

#include "httplib.h"

#include "core/common/logging.h"
#include "core/common/status.h"
#include "core/threading/thread_util.h"

namespace illuminator {

class HttpServer {
public:
    HttpServer() = default;
    ~HttpServer() { Stop(); }

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;
    HttpServer(HttpServer&&) = delete;
    HttpServer& operator=(HttpServer&&) = delete;

    httplib::Server& server() { return server_; }

    Status Start(const std::string& listen_addr, int port) {
        if (!static_dir_.empty()) {
            server_.set_mount_point("/", static_dir_);

            server_.set_error_handler(
                [this](const httplib::Request& req, httplib::Response& res) {
                    if (res.status == httplib::StatusCode::NotFound_404 &&
                        req.path.find('.') == std::string::npos &&
                        req.path.find("/api/") != 0) {
                        std::string index = static_dir_ + "/index.html";
                        std::ifstream file(index, std::ios::binary);
                        if (file.is_open()) {
                            std::string content(
                                std::istreambuf_iterator<char>(file), {});
                            res.set_content(content, "text/html");
                            res.status = 200;
                        }
                    }
                });
        }

        thread_ = std::thread([this, listen_addr, port] {
            SetThreadName("http-server");
            IL_INFO("HTTP server started on {}:{}", listen_addr, port);
            server_.listen(listen_addr, port);
        });

        int tries = 0;
        while (!server_.is_running() && tries++ < 100) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!server_.is_running()) {
            return Status::Error(StatusCode::kInternal,
                "Failed to start HTTP server on port " + std::to_string(port));
        }
        return Status::Ok();
    }

    void Stop() {
        server_.stop();
        if (thread_.joinable()) thread_.join();
    }

    void SetStaticDir(const std::string& dir) {
        static_dir_ = dir;
    }

private:
    httplib::Server server_;
    std::thread thread_;
    std::string static_dir_;
};

}  // namespace illuminator
