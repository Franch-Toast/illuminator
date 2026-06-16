// ============================================================================
// Illuminator HTTP 服务器 — cpp-httplib 薄封装 + WebSocket 同端口升级
// ============================================================================
// 直接暴露 httplib::Server，Handler 使用 httplib::Request/Response。
// 通过子类化 httplib::Server 并覆盖 process_and_close_socket，在同一端口
// 同时处理 HTTP 和 WebSocket 连接（MSG_PEEK 检测 Upgrade 请求）。
// ============================================================================

#pragma once

#include <chrono>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <thread>

#include <sys/socket.h>

#include "httplib.h"

#include "core/common/logging.h"
#include "core/common/status.h"
#include "core/threading/thread_util.h"

namespace illuminator {

using WebSocketUpgradeHandler =
    std::function<bool(int fd, const std::string& raw_request)>;

// httplib::Server subclass that intercepts WebSocket Upgrade requests
// at the socket level, before httplib parses them as normal HTTP.
class WsAwareServer : public httplib::Server {
public:
    void set_websocket_upgrade_handler(WebSocketUpgradeHandler handler) {
        ws_handler_ = std::move(handler);
    }

private:
    bool process_and_close_socket(socket_t sock) override {
        if (ws_handler_) {
            char peek_buf[4096];
            ssize_t n = ::recv(sock, peek_buf, sizeof(peek_buf) - 1, MSG_PEEK);
            if (n > 0) {
                peek_buf[n] = '\0';
                if (HasUpgradeWebsocket(peek_buf, n)) {
                    char buf[4096];
                    ssize_t actual = ::recv(sock, buf, sizeof(buf) - 1, 0);
                    if (actual > 0) {
                        std::string request(buf, actual);
                        if (ws_handler_(static_cast<int>(sock), request)) {
                            return true;
                        }
                    }
                    httplib::detail::shutdown_socket(sock);
                    httplib::detail::close_socket(sock);
                    return false;
                }
            }
        }
        // Not a WS upgrade — replicate base class logic using protected members
        auto ret = httplib::detail::process_server_socket(
            svr_sock_, sock, keep_alive_max_count_, keep_alive_timeout_sec_,
            read_timeout_sec_, read_timeout_usec_, write_timeout_sec_,
            write_timeout_usec_,
            [this](httplib::Stream& strm, bool close_connection,
                   bool& connection_closed) {
                return process_request(strm, close_connection,
                                       connection_closed, nullptr);
            });
        httplib::detail::shutdown_socket(sock);
        httplib::detail::close_socket(sock);
        return ret;
    }

    static bool HasUpgradeWebsocket(const char* buf, ssize_t len) {
        for (ssize_t i = 0; i < len - 9; ++i) {
            if ((buf[i] == 'U' || buf[i] == 'u') &&
                (buf[i+1] == 'p' || buf[i+1] == 'P') &&
                (buf[i+2] == 'g' || buf[i+2] == 'G') &&
                (buf[i+3] == 'r' || buf[i+3] == 'R') &&
                (buf[i+4] == 'a' || buf[i+4] == 'A') &&
                (buf[i+5] == 'd' || buf[i+5] == 'D') &&
                (buf[i+6] == 'e' || buf[i+6] == 'E') &&
                buf[i+7] == ':') {
                ssize_t j = i + 8;
                while (j < len && buf[j] == ' ') ++j;
                for (ssize_t k = j; k < len - 8 && k < j + 20; ++k) {
                    if ((buf[k] == 'w' || buf[k] == 'W') &&
                        (buf[k+1] == 'e' || buf[k+1] == 'E') &&
                        (buf[k+2] == 'b' || buf[k+2] == 'B') &&
                        (buf[k+3] == 's' || buf[k+3] == 'S') &&
                        (buf[k+4] == 'o' || buf[k+4] == 'O') &&
                        (buf[k+5] == 'c' || buf[k+5] == 'C') &&
                        (buf[k+6] == 'k' || buf[k+6] == 'K') &&
                        (buf[k+7] == 'e' || buf[k+7] == 'E') &&
                        (buf[k+8] == 't' || buf[k+8] == 'T')) {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    WebSocketUpgradeHandler ws_handler_;
};

class HttpServer {
public:
    HttpServer() = default;
    ~HttpServer() { Stop(); }

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;
    HttpServer(HttpServer&&) = delete;
    HttpServer& operator=(HttpServer&&) = delete;

    httplib::Server& server() { return server_; }

    void SetWebSocketUpgradeHandler(WebSocketUpgradeHandler handler) {
        server_.set_websocket_upgrade_handler(std::move(handler));
    }

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
    WsAwareServer server_;
    std::thread thread_;
    std::string static_dir_;
};

}  // namespace illuminator
