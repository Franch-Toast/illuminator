#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <fstream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <sstream>

#include "core/common/logging.h"
#include "core/common/status.h"

namespace illuminator {

// Minimal embedded HTTP server for the API and metrics endpoints.
// Full-featured server (Drogon/cpp-httplib) is a Phase 4 enhancement.
class HttpServer {
public:
    using Handler = std::function<std::string(const std::string& path)>;

    Status Start(const std::string& listen_addr, int port) {
        server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd_ < 0) {
            return Status::Error(StatusCode::kInternal, "Socket creation failed");
        }

        int opt = 1;
        setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);

        if (bind(server_fd_, reinterpret_cast<struct sockaddr*>(&addr),
                 sizeof(addr)) < 0) {
            close(server_fd_);
            return Status::Error(StatusCode::kInternal,
                "Bind failed on port " + std::to_string(port));
        }

        if (listen(server_fd_, 16) < 0) {
            close(server_fd_);
            return Status::Error(StatusCode::kInternal, "Listen failed");
        }

        running_ = true;
        thread_ = std::thread([this] { AcceptLoop(); });
        IL_INFO("HTTP server started on port %d", port);
        return Status::Ok();
    }

    void Stop() {
        running_ = false;
        if (server_fd_ >= 0) {
            shutdown(server_fd_, SHUT_RDWR);
            close(server_fd_);
            server_fd_ = -1;
        }
        if (thread_.joinable()) thread_.join();
    }

    void RegisterHandler(const std::string& path, Handler handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        handlers_[path] = std::move(handler);
    }

    void SetStaticDir(const std::string& dir) {
        static_dir_ = dir;
    }

private:
    void AcceptLoop() {
        while (running_) {
            struct sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept(server_fd_,
                reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);
            if (client_fd < 0) continue;
            HandleClient(client_fd);
        }
    }

    void HandleClient(int fd) {
        char buf[4096] = {};
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n <= 0) { close(fd); return; }

        // Parse HTTP request line
        std::string request(buf, n);
        std::string path = ParsePath(request);

        std::string body;
        std::string content_type = "text/plain";

        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = handlers_.find(path);
            if (it != handlers_.end()) {
                body = it->second(path);
                if (path.find("/api/") == 0) content_type = "application/json";
            } else if (!static_dir_.empty()) {
                std::string file_path = static_dir_;
                file_path += (path == "/" ? "/index.html" : path);

                // SPA fallback: serve index.html for non-file paths
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

    static std::string ParsePath(const std::string& request) {
        auto pos = request.find(' ');
        if (pos == std::string::npos) return "/";
        auto end = request.find(' ', pos + 1);
        if (end == std::string::npos) return "/";
        return request.substr(pos + 1, end - pos - 1);
    }

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

    std::string static_dir_;
    int server_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::unordered_map<std::string, Handler> handlers_;
};

}  // namespace illuminator
