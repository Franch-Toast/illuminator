#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <poll.h>
#include <unistd.h>

#include "core/common/logging.h"
#include "server/websocket_server.h"
#include "sinks/websocket_sink/websocket_sink.h"

namespace illuminator {

using WsBroadcastSerializer =
    std::function<std::string(const std::string& pipeline_key, DataBatchPtr batch)>;

class WebSocketManager {
public:
    void SetBroadcastInterval(int ms) { broadcast_interval_ms_ = ms; }
    void SetSerializer(WsBroadcastSerializer fn) { serializer_ = std::move(fn); }

    void AddConnection(int fd, const std::string& subscribe_path) {
        std::string key = PathToPipelineKey(subscribe_path);
        IL_INFO("WebSocket: new connection fd=%d subscribe=%s", fd, key.c_str());

        std::lock_guard<std::mutex> lk(mu_);
        connections_[fd] = ConnInfo{key, {}};
        subscriptions_[key].insert(fd);
    }

    void Start() {
        running_.store(true);
        thread_ = std::thread([this] { BroadcastLoop(); });
        IL_INFO("WebSocketManager started (interval=%dms)", broadcast_interval_ms_);
    }

    void Stop() {
        running_.store(false);
        if (thread_.joinable()) thread_.join();
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& [fd, _] : connections_) {
            auto close_frame = WebSocketCodec::EncodeCloseFrame(1001);
            ::write(fd, close_frame.data(), close_frame.size());
            ::close(fd);
        }
        connections_.clear();
        subscriptions_.clear();
        IL_INFO("WebSocketManager stopped");
    }

    size_t ConnectionCount() const {
        std::lock_guard<std::mutex> lk(mu_);
        return connections_.size();
    }

private:
    struct ConnInfo {
        std::string pipeline_key;
        std::vector<uint8_t> recv_buf;
    };

    void BroadcastLoop() {
        while (running_.load()) {
            ProcessIncoming();
            BroadcastData();

            std::this_thread::sleep_for(
                std::chrono::milliseconds(broadcast_interval_ms_));
        }
    }

    void ProcessIncoming() {
        std::vector<struct pollfd> pfds;
        std::vector<int> fd_list;

        {
            std::lock_guard<std::mutex> lk(mu_);
            for (auto& [fd, _] : connections_) {
                struct pollfd pfd{};
                pfd.fd = fd;
                pfd.events = POLLIN;
                pfds.push_back(pfd);
                fd_list.push_back(fd);
            }
        }

        if (pfds.empty()) return;
        int ret = poll(pfds.data(), pfds.size(), 0);
        if (ret <= 0) return;

        std::vector<int> to_remove;
        for (size_t i = 0; i < pfds.size(); ++i) {
            if (!(pfds[i].revents & POLLIN)) continue;

            uint8_t buf[4096];
            ssize_t n = ::read(fd_list[i], buf, sizeof(buf));
            if (n <= 0) {
                to_remove.push_back(fd_list[i]);
                continue;
            }

            std::lock_guard<std::mutex> lk(mu_);
            auto it = connections_.find(fd_list[i]);
            if (it == connections_.end()) continue;

            auto& rb = it->second.recv_buf;
            rb.insert(rb.end(), buf, buf + n);

            while (!rb.empty()) {
                size_t consumed = 0;
                auto frame = WebSocketCodec::DecodeFrame(rb.data(), rb.size(), consumed);
                if (!frame) break;

                rb.erase(rb.begin(), rb.begin() + consumed);

                switch (frame->opcode) {
                case WsOpcode::kPing: {
                    auto pong = WebSocketCodec::EncodePongFrame(frame->payload);
                    ::write(fd_list[i], pong.data(), pong.size());
                    break;
                }
                case WsOpcode::kClose:
                    to_remove.push_back(fd_list[i]);
                    break;
                case WsOpcode::kText:
                    HandleTextMessage(fd_list[i], frame->payload);
                    break;
                default:
                    break;
                }
            }
        }

        for (int fd : to_remove)
            RemoveConnection(fd);
    }

    void HandleTextMessage(int fd, const std::string& msg) {
        if (msg.find("subscribe:") == 0) {
            std::string new_key = msg.substr(10);
            std::lock_guard<std::mutex> lk(mu_);
            auto it = connections_.find(fd);
            if (it == connections_.end()) return;
            auto& old_key = it->second.pipeline_key;
            subscriptions_[old_key].erase(fd);
            old_key = new_key;
            subscriptions_[new_key].insert(fd);
            IL_DEBUG("WebSocket: fd=%d re-subscribed to %s", fd, new_key.c_str());
        }
    }

    void BroadcastData() {
        std::lock_guard<std::mutex> lk(mu_);
        std::unordered_set<std::string> keys;
        for (auto& [key, _] : subscriptions_)
            if (!_.empty()) keys.insert(key);

        for (auto& key : keys) {
            auto batch = WebSocketSinkStore::Instance().Latest(key);
            if (!batch) continue;

            std::string json;
            if (serializer_)
                json = serializer_(key, batch);
            else
                json = "{\"pipeline\":\"" + key + "\",\"ts\":" +
                       std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now().time_since_epoch()).count()) +
                       "}";

            auto frame = WebSocketCodec::EncodeFrame(json);

            auto sit = subscriptions_.find(key);
            if (sit == subscriptions_.end()) continue;

            std::vector<int> dead;
            for (int fd : sit->second) {
                ssize_t w = ::write(fd, frame.data(), frame.size());
                if (w <= 0) dead.push_back(fd);
            }
            for (int fd : dead)
                RemoveConnectionLocked(fd);
        }
    }

    void RemoveConnection(int fd) {
        std::lock_guard<std::mutex> lk(mu_);
        RemoveConnectionLocked(fd);
    }

    void RemoveConnectionLocked(int fd) {
        auto it = connections_.find(fd);
        if (it != connections_.end()) {
            auto& key = it->second.pipeline_key;
            subscriptions_[key].erase(fd);
            connections_.erase(it);
        }
        auto close_frame = WebSocketCodec::EncodeCloseFrame(1000);
        ::write(fd, close_frame.data(), close_frame.size());
        ::close(fd);
        IL_DEBUG("WebSocket: removed connection fd=%d", fd);
    }

    static std::string PathToPipelineKey(const std::string& path) {
        if (path.size() > 4 && path.substr(0, 4) == "/ws/")
            return path.substr(4);
        return "default";
    }

    mutable std::mutex mu_;
    std::unordered_map<int, ConnInfo> connections_;
    std::unordered_map<std::string, std::unordered_set<int>> subscriptions_;

    std::atomic<bool> running_{false};
    std::thread thread_;
    int broadcast_interval_ms_ = 1000;
    WsBroadcastSerializer serializer_;
};

}  // namespace illuminator
