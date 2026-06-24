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
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <cstring>

#include "core/common/logging.h"
#include "core/threading/thread_util.h"
#include "server/websocket_server.h"
#include "sinks/stream_sink/stream_sink.h"

namespace illuminator {

// WebSocket 广播序列化器类型：将 pipeline_key + DataBatch 序列化为 JSON 字符串
using WsBroadcastSerializer =
    std::function<std::string(const std::string& pipeline_key, DataBatchPtr batch)>;

// ============================================================================
// WebSocketManager — WebSocket 连接管理与数据广播
// ============================================================================
// 职责：
//   1. 管理 WebSocket 连接（接受新连接、维护连接池、处理断开）
//   2. 按管道订阅分组（一个 WebSocket 连接订阅一个 pipeline_key）
//   3. 定期广播：从 StreamSinkStore 拉取最新数据，推送到所有订阅客户端
//   4. 支持两种模式：独立端口监听（Legacy）和 HTTP 同端口升级（推荐）
//   5. 处理 WebSocket 控制帧（Ping/Pong/Close）
//
// 线程模型：
//   - ws-broadcast 线程：定期广播 + 处理入站消息（poll + 非阻塞）
//   - ws-accept 线程：接受新连接（仅独立端口模式）
//
// 广播去重：如果数据与上次广播相同，跳过（避免推送重复数据）
// ============================================================================
class WebSocketManager {
public:
    // ---- 配置 ----
    void SetBroadcastInterval(int ms) { broadcast_interval_ms_ = ms; }
    void SetSerializer(WsBroadcastSerializer fn) { serializer_ = std::move(fn); }
    void SetAuthToken(const std::string& token) { auth_token_ = token; }

    // ---- AddConnection — 添加 WebSocket 连接（HTTP 升级后调用） ----
    // 从 URL 路径中提取 pipeline_key（如 /ws/cpu_util → "cpu_util"），
    // 建立 fd → pipeline_key 的映射，记录到订阅表。
    void AddConnection(int fd, const std::string& subscribe_path) {
        std::string key = PathToPipelineKey(subscribe_path);
        IL_INFO("WebSocket: new connection fd={} subscribe={}", fd, key);

        std::lock_guard<std::mutex> lk(mu_);
        connections_[fd] = ConnInfo{key, {}};
        subscriptions_[key].insert(fd);
    }

    // ---- HandleUpgrade — 处理 HTTP WebSocket 升级请求（同端口模式） ----
    // 从 HTTP 请求中提取升级请求，验证认证，执行 WebSocket 握手，添加连接。
    // 返回 true 表示升级成功（调用者不能关闭 fd），false 表示失败（调用者应关闭 fd）。
    // Handle a WebSocket upgrade from the HTTP server (same-port mode).
    // Returns true if the connection was accepted (caller must NOT close fd).
    bool HandleUpgrade(int fd, const std::string& raw_request) {
        if (!ValidateAuth(raw_request)) {
            const char* resp = "HTTP/1.1 401 Unauthorized\r\n\r\n";
            ::write(fd, resp, strlen(resp));
            return false;
        }
        if (!WebSocketCodec::PerformHandshake(fd, raw_request)) {
            return false;
        }
        auto path = WebSocketCodec::GetUpgradePath(raw_request);
        AddConnection(fd, path);
        return true;
    }

    // ---- Start — 启动 WebSocket 广播线程 ----
    // 连接通过 HandleUpgrade() 从 WsAwareServer 同端口注入。
    void Start() {
        running_.store(true);
        thread_ = std::thread([this] {
            SetThreadName("ws-broadcast");
            BroadcastLoop();
        });
        IL_INFO("WebSocketManager started (interval={}ms)", broadcast_interval_ms_);
    }

    // ---- Stop — 停止 WebSocket 服务 ----
    // 停止广播线程，发送 Close 帧并关闭所有连接。
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
    // ConnInfo — 每个 WebSocket 连接的状态信息
    struct ConnInfo {
        std::string pipeline_key;         // 订阅的管道标识
        std::vector<uint8_t> recv_buf;    // 接收缓冲区（WebSocket 帧可能分片到达）
    };

    // ---- BroadcastLoop — 广播主循环（ws-broadcast 线程） ----
    // 每个广播周期执行：
    //   1. ProcessIncoming() — 处理客户端入站消息（ping/pong/close/subscribe）
    //   2. BroadcastData() — 从 StreamSinkStore 拉取最新数据，推送到所有订阅客户端
    //   3. sleep(broadcast_interval_ms_) — 等待下一个周期
    void BroadcastLoop() {
        while (running_.load()) {
            ProcessIncoming();
            BroadcastData();

            std::this_thread::sleep_for(
                std::chrono::milliseconds(broadcast_interval_ms_));
        }
    }

    // ---- ProcessIncoming — 处理客户端入站消息 ----
    // 使用 poll(POLLIN) 非阻塞检查所有连接的可读状态。
    // 支持的 WebSocket 帧类型：
    //   - Ping  → 回复 Pong
    //   - Close  → 移除连接
    //   - Text  → 处理订阅消息（subscribe:xxx）
    // 连接断开时（read 返回 <= 0），自动移除。
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

    // ---- HandleTextMessage — 处理文本消息（订阅切换） ----
    // 消息格式：subscribe:<pipeline_key>
    // 支持运行时切换订阅，先取消旧订阅，再建立新订阅。
    // REQUIRES: mu_ already held by caller (ProcessIncoming)
    void HandleTextMessage(int fd, const std::string& msg) {
        if (msg.find("subscribe:") == 0) {
            std::string new_key = msg.substr(10);
            auto it = connections_.find(fd);
            if (it == connections_.end()) return;
            auto& old_key = it->second.pipeline_key;
            subscriptions_[old_key].erase(fd);
            old_key = new_key;
            subscriptions_[new_key].insert(fd);
            IL_DEBUG("WebSocket: fd={} re-subscribed to {}", fd, new_key);
        }
    }

    // ---- BroadcastData — 数据广播（三阶段） ----
    // Phase 1（加锁）：快照当前订阅表，复制 fd 列表
    // Phase 2（无锁）：遍历每个 pipeline_key，从 StreamSinkStore 获取最新数据
    //                → 去重（与上次广播的数据相同则跳过）
    //                → 序列化 → 写入所有订阅的 fd
    //                → 记录写入失败的 fd（死连接）
    // Phase 3（加锁）：清理死连接
    void BroadcastData() {
        // Phase 1 (locked): snapshot current subscriptions
        std::unordered_map<std::string, std::vector<int>> snapshot;
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (auto& [key, fds] : subscriptions_) {
                if (!fds.empty())
                    snapshot[key] = std::vector<int>(fds.begin(), fds.end());
            }
        }

        // Phase 2 (lock-free): serialize + send (with dedup)
        for (auto& [key, fds] : snapshot) {
            auto batch = StreamSinkStore::Instance().GetBuffer(key).Latest();
            if (!batch) continue;

            // Dedup: skip if data hasn't changed since last broadcast
            if (last_broadcast_[key] == batch) continue;
            last_broadcast_[key] = batch;

            std::string json;
            if (serializer_)
                json = serializer_(key, batch);
            else
                json = "{\"pipeline\":\"" + key + "\",\"ts\":" +
                       std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now().time_since_epoch()).count()) +
                       "}";

            auto frame = WebSocketCodec::EncodeFrame(json);

            std::vector<int> dead;
            for (int fd : fds) {
                ssize_t w = ::write(fd, frame.data(), frame.size());
                if (w <= 0) dead.push_back(fd);
            }

            // Phase 3 (locked): clean up dead connections
            if (!dead.empty()) {
                std::lock_guard<std::mutex> lk(mu_);
                for (int fd : dead)
                    RemoveConnectionLocked(fd);
            }
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
        IL_DEBUG("WebSocket: removed connection fd={}", fd);
    }

    // ---- PathToPipelineKey — 从 URL 路径提取 pipeline_key ----
    // 如 /ws/cpu_utilization → "cpu_utilization"，/ws/ → "default"
    static std::string PathToPipelineKey(const std::string& path) {
        if (path.size() > 4 && path.substr(0, 4) == "/ws/")
            return path.substr(4);
        return "default";
    }

    // ---- ValidateAuth — 验证 WebSocket 连接的认证 ----
    // 支持两种方式：Authorization: Bearer <token> 头，或 ?token=<token> 查询参数
    bool ValidateAuth(const std::string& request) const {
        if (auth_token_.empty()) return true;

        auto auth_header = detail::ExtractHeader(request, "Authorization");
        if (auth_header == "Bearer " + auth_token_) return true;

        auto token_param = ExtractQueryParam(request, "token");
        if (!token_param.empty() && token_param == auth_token_) return true;

        return false;
    }

    // ---- ExtractQueryParam — 从 HTTP 请求行中提取查询参数 ----
    // 解析 GET /ws/cpu?token=xxx HTTP/1.1 中的 token 参数
    static std::string ExtractQueryParam(const std::string& request,
                                          const std::string& param) {
        auto sp1 = request.find(' ');
        if (sp1 == std::string::npos) return "";
        auto sp2 = request.find(' ', sp1 + 1);
        if (sp2 == std::string::npos) return "";
        auto uri = request.substr(sp1 + 1, sp2 - sp1 - 1);
        auto q = uri.find('?');
        if (q == std::string::npos) return "";
        auto query = uri.substr(q + 1);

        std::string key = param + "=";
        auto pos = query.find(key);
        if (pos == std::string::npos) return "";
        pos += key.size();
        auto end = query.find('&', pos);
        return (end == std::string::npos) ? query.substr(pos) : query.substr(pos, end - pos);
    }

    mutable std::mutex mu_;
    std::unordered_map<int, ConnInfo> connections_;
    std::unordered_map<std::string, std::unordered_set<int>> subscriptions_;

    std::atomic<bool> running_{false};
    std::thread thread_;
    int broadcast_interval_ms_ = 1000;
    WsBroadcastSerializer serializer_;
    std::string auth_token_;
    std::unordered_map<std::string, DataBatchPtr> last_broadcast_;
};

}  // namespace illuminator
