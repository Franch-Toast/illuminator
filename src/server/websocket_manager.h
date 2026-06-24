// ============================================================================
// WebSocketManager — WebSocket 连接管理与数据广播
// ============================================================================
//
// 【架构定位】
// WebSocketManager 是 Illuminator 实时数据推送的核心组件。它位于 Server 层，
// 从 StreamSinkStore 拉取最新数据，序列化后广播给所有订阅的 WebSocket 客户端。
//
// 数据流：
//   Pipeline → StreamSink → StreamSinkStore → WebSocketManager::BroadcastLoop()
//                                                   │
//                                                   ▼
//                                          序列化 (JSON) → 写入所有订阅的 fd
//
// 【职责】
//   1. 管理 WebSocket 连接（接受新连接、维护连接池、处理断开）
//   2. 按管道订阅分组（一个 WebSocket 连接订阅一个 pipeline_key）
//   3. 定期广播：从 StreamSinkStore 拉取最新数据，推送到所有订阅客户端
//   4. 支持两种模式：独立端口监听（Legacy）和 HTTP 同端口升级（推荐）
//   5. 处理 WebSocket 控制帧（Ping/Pong/Close）
//   6. 认证验证（Bearer Token 或 query 参数）
//
// 【线程模型】
//   ws-broadcast 线程：定期广播 + 处理入站消息（poll + 非阻塞）
//     - BroadcastLoop(): 主循环，每个周期执行 ProcessIncoming + BroadcastData
//     - ProcessIncoming(): 使用 poll(POLLIN) 非阻塞检查所有连接的可读状态
//     - BroadcastData(): 从 StreamSinkStore 拉取数据 → 去重 → 序列化 → 写入 fd
//   ws-accept 线程：接受新连接（仅独立端口模式，当前未使用）
//
// 【广播去重机制】
//   如果数据与上次广播相同（shared_ptr 比较），跳过广播。
//   这避免了推送重复数据，节省序列化和网络开销。
//
//   Pipeline 采集数据无变化 → DataBatch 的 shared_ptr 不变
//   → Latest() 返回相同的 shared_ptr → 与 last_broadcast_[key] 比较
//   → 相同 → 跳过广播
//
// 【连接管理】
//   连接通过 HandleUpgrade() 从 HTTP 服务器同端口注入。
//   HTTP 服务器检测到 WebSocket 升级请求（Upgrade: websocket 头），
//   调用 HandleUpgrade() 完成握手并添加连接。
//
//   连接生命周期：
//   HTTP 升级请求 → HandleUpgrade() → AddConnection() → 广播循环
//   客户端断开 → ProcessIncoming() 检测到 read <= 0 → RemoveConnection()
//   服务端停止 → Stop() 发送 Close 帧 → 关闭所有 fd
// ============================================================================

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

// ============================================================================
// WsBroadcastSerializer — WebSocket 广播序列化器类型
// ============================================================================
// 将 pipeline_key + DataBatch 序列化为 JSON 字符串的回调函数。
// 由前端设置，控制序列化格式（例如包含哪些字段、如何格式化）。
//
// 参数：
//   pipeline_key: Feature 名称（如 "cpu_utilization"）
//   batch:        要序列化的 DataBatch
//
// 返回：
//   JSON 字符串（用于 WebSocket 文本帧的 payload）
using WsBroadcastSerializer =
    std::function<std::string(const std::string& pipeline_key, DataBatchPtr batch)>;

class WebSocketManager {
public:
    // ========================================================================
    // 配置方法
    // ========================================================================

    // 设置广播间隔（毫秒），默认 1000ms
    // 间隔越短，前端数据更新越实时，但 CPU 开销越大
    void SetBroadcastInterval(int ms) { broadcast_interval_ms_ = ms; }

    // 设置序列化器（控制广播数据的 JSON 格式）
    void SetSerializer(WsBroadcastSerializer fn) { serializer_ = std::move(fn); }

    // 设置认证 Token（用于 WebSocket 连接认证）
    // 空字符串表示不需要认证
    void SetAuthToken(const std::string& token) { auth_token_ = token; }

    // ========================================================================
    // AddConnection — 添加 WebSocket 连接（HTTP 升级后调用）
    // ========================================================================
    //
    // 从 URL 路径中提取 pipeline_key（如 /ws/cpu_util → "cpu_util"），
    // 建立 fd → pipeline_key 的映射，记录到订阅表。
    //
    // 数据流：
    //   HTTP 服务器检测到 WebSocket 升级请求
    //   → 验证认证 → 完成 WebSocket 握手
    //   → 调用 AddConnection(fd, "/ws/cpu_utilization")
    //   → PathToPipelineKey("/ws/cpu_utilization") → "cpu_utilization"
    //   → connections_[fd] = {pipeline_key: "cpu_utilization"}
    //   → subscriptions_["cpu_utilization"].insert(fd)
    //
    // 参数：
    //   fd:             连接的 socket 文件描述符
    //   subscribe_path: WebSocket URL 路径（如 "/ws/cpu_utilization"）
    void AddConnection(int fd, const std::string& subscribe_path) {
        std::string key = PathToPipelineKey(subscribe_path);
        IL_INFO("WebSocket: new connection fd={} subscribe={}", fd, key);

        std::lock_guard<std::mutex> lk(mu_);
        connections_[fd] = ConnInfo{key, {}};
        subscriptions_[key].insert(fd);
    }

    // ========================================================================
    // HandleUpgrade — 处理 HTTP WebSocket 升级请求（同端口模式）
    // ========================================================================
    //
    // 从 HTTP 请求中提取升级请求，验证认证，执行 WebSocket 握手，添加连接。
    //
    // 流程：
    //   1. 验证认证（Bearer Token 或 ?token= 查询参数）
    //   2. 执行 WebSocket 握手（RFC 6455 协议）
    //   3. 提取 URL 路径中的 pipeline_key
    //   4. 调用 AddConnection 添加连接
    //
    // 参数：
    //   fd:         连接的 socket 文件描述符
    //   raw_request: 原始 HTTP 请求（用于提取认证信息和升级路径）
    //
    // 返回：
    //   true:  升级成功，调用者不能关闭 fd（fd 由 WebSocketManager 管理）
    //   false: 升级失败，调用者应关闭 fd
    bool HandleUpgrade(int fd, const std::string& raw_request) {
        // Step 1: 验证认证
        if (!ValidateAuth(raw_request)) {
            const char* resp = "HTTP/1.1 401 Unauthorized\r\n\r\n";
            ::write(fd, resp, strlen(resp));
            return false;
        }

        // Step 2: 执行 WebSocket 握手
        if (!WebSocketCodec::PerformHandshake(fd, raw_request)) {
            return false;
        }

        // Step 3: 提取路径并添加连接
        auto path = WebSocketCodec::GetUpgradePath(raw_request);
        AddConnection(fd, path);
        return true;
    }

    // ========================================================================
    // Start — 启动 WebSocket 广播线程
    // ========================================================================
    //
    // 启动 ws-broadcast 线程，开始定期广播循环。
    // 连接通过 HandleUpgrade() 从 HTTP 服务器同端口注入。
    void Start() {
        running_.store(true);
        thread_ = std::thread([this] {
            SetThreadName("ws-broadcast");
            BroadcastLoop();
        });
        IL_INFO("WebSocketManager started (interval={}ms)", broadcast_interval_ms_);
    }

    // ========================================================================
    // Stop — 停止 WebSocket 服务
    // ========================================================================
    //
    // 停止广播线程，发送 Close 帧（状态码 1001: Going Away）并关闭所有连接。
    //
    // 停止流程：
    //   1. 设置 running_ = false（通知 BroadcastLoop 退出）
    //   2. 等待广播线程退出（join）
    //   3. 遍历所有连接：发送 Close 帧 → 关闭 fd
    //   4. 清空连接表和订阅表
    void Stop() {
        running_.store(false);
        if (thread_.joinable()) thread_.join();

        std::lock_guard<std::mutex> lk(mu_);
        for (auto& [fd, _] : connections_) {
            // 发送 Close 帧（状态码 1001: 服务端正在关闭）
            auto close_frame = WebSocketCodec::EncodeCloseFrame(1001);
            ::write(fd, close_frame.data(), close_frame.size());
            ::close(fd);
        }
        connections_.clear();
        subscriptions_.clear();
        IL_INFO("WebSocketManager stopped");
    }

    // 查询当前连接数
    size_t ConnectionCount() const {
        std::lock_guard<std::mutex> lk(mu_);
        return connections_.size();
    }

private:
    // ========================================================================
    // ConnInfo — 每个 WebSocket 连接的状态信息
    // ========================================================================
    struct ConnInfo {
        std::string pipeline_key;         // 订阅的管道标识（如 "cpu_utilization"）
        std::vector<uint8_t> recv_buf;    // 接收缓冲区（WebSocket 帧可能分片到达，需要累积）
    };

    // ========================================================================
    // BroadcastLoop — 广播主循环（ws-broadcast 线程）
    // ========================================================================
    //
    // 每个广播周期执行：
    //   1. ProcessIncoming() — 处理客户端入站消息（ping/pong/close/subscribe）
    //   2. BroadcastData()  — 从 StreamSinkStore 拉取最新数据，推送到所有订阅客户端
    //   3. sleep(broadcast_interval_ms_) — 等待下一个周期
    //
    // 广播间隔建议：
    //   - 1000ms: 默认，适合大多数场景（CPU 利用率等低频指标）
    //   - 500ms:  适合需要较实时更新的场景
    //   - 200ms:  高频场景，注意 CPU 开销
    void BroadcastLoop() {
        while (running_.load()) {
            ProcessIncoming();
            BroadcastData();

            std::this_thread::sleep_for(
                std::chrono::milliseconds(broadcast_interval_ms_));
        }
    }

    // ========================================================================
    // ProcessIncoming — 处理客户端入站消息
    // ========================================================================
    //
    // 使用 poll(POLLIN) 非阻塞检查所有连接的可读状态。
    // 支持的 WebSocket 帧类型：
    //   - Ping  → 回复 Pong（保持连接活跃）
    //   - Close → 移除连接
    //   - Text  → 处理订阅消息（subscribe:xxx）
    //
    // 处理流程：
    //   1. 加锁快照所有连接 fd 列表
    //   2. poll() 非阻塞检查可读状态
    //   3. 对每个可读的 fd：read 数据 → 累积到 recv_buf → 解码 WebSocket 帧
    //   4. 根据 opcode 分发处理
    //   5. 记录需要移除的 fd（read 返回 <= 0 或收到 Close 帧）
    //   6. 批量移除死连接
    void ProcessIncoming() {
        std::vector<struct pollfd> pfds;
        std::vector<int> fd_list;

        // Step 1: 加锁快照所有连接 fd 列表
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (auto& [fd, _] : connections_) {
                struct pollfd pfd{};
                pfd.fd = fd;
                pfd.events = POLLIN;  // 只监听可读事件
                pfds.push_back(pfd);
                fd_list.push_back(fd);
            }
        }

        if (pfds.empty()) return;

        // Step 2: poll() 非阻塞检查（timeout=0）
        int ret = poll(pfds.data(), pfds.size(), 0);
        if (ret <= 0) return;  // 无事件或错误

        std::vector<int> to_remove;

        // Step 3-5: 处理每个可读的 fd
        for (size_t i = 0; i < pfds.size(); ++i) {
            if (!(pfds[i].revents & POLLIN)) continue;

            uint8_t buf[4096];
            ssize_t n = ::read(fd_list[i], buf, sizeof(buf));
            if (n <= 0) {
                // 连接断开（read 返回 0 或 -1）
                to_remove.push_back(fd_list[i]);
                continue;
            }

            std::lock_guard<std::mutex> lk(mu_);
            auto it = connections_.find(fd_list[i]);
            if (it == connections_.end()) continue;

            // 累积数据到接收缓冲区（WebSocket 帧可能分片到达）
            auto& rb = it->second.recv_buf;
            rb.insert(rb.end(), buf, buf + n);

            // 循环解码 WebSocket 帧（一个 TCP 包可能包含多个帧）
            while (!rb.empty()) {
                size_t consumed = 0;
                auto frame = WebSocketCodec::DecodeFrame(rb.data(), rb.size(), consumed);
                if (!frame) break;  // 帧不完整，等待更多数据

                // 移除已解码的数据
                rb.erase(rb.begin(), rb.begin() + consumed);

                // 根据 opcode 分发处理
                switch (frame->opcode) {
                case WsOpcode::kPing: {
                    // 回复 Pong 帧（保持连接活跃）
                    auto pong = WebSocketCodec::EncodePongFrame(frame->payload);
                    ::write(fd_list[i], pong.data(), pong.size());
                    break;
                }
                case WsOpcode::kClose:
                    // 客户端主动关闭连接
                    to_remove.push_back(fd_list[i]);
                    break;
                case WsOpcode::kText:
                    // 文本消息：处理订阅切换
                    HandleTextMessage(fd_list[i], frame->payload);
                    break;
                default:
                    break;
                }
            }
        }

        // Step 6: 批量移除死连接
        for (int fd : to_remove)
            RemoveConnection(fd);
    }

    // ========================================================================
    // HandleTextMessage — 处理文本消息（订阅切换）
    // ========================================================================
    //
    // 消息格式：subscribe:<pipeline_key>
    // 例如：subscribe:cpu_profiler
    //
    // 支持运行时切换订阅：先取消旧订阅，再建立新订阅。
    // 前端可以通过此机制在同一个 WebSocket 连接中切换监控的 Feature，
    // 无需断开重连。
    //
    // 前提条件：调用者必须持有 mu_ 锁（由 ProcessIncoming 保证）
    //
    // 参数：
    //   fd:  连接的 socket 文件描述符
    //   msg: 文本消息内容（格式：subscribe:<pipeline_key>）
    void HandleTextMessage(int fd, const std::string& msg) {
        if (msg.find("subscribe:") == 0) {
            std::string new_key = msg.substr(10);  // 提取 pipeline_key
            auto it = connections_.find(fd);
            if (it == connections_.end()) return;

            // 取消旧订阅
            auto& old_key = it->second.pipeline_key;
            subscriptions_[old_key].erase(fd);

            // 建立新订阅
            old_key = new_key;
            subscriptions_[new_key].insert(fd);
            IL_DEBUG("WebSocket: fd={} re-subscribed to {}", fd, new_key);
        }
    }

    // ========================================================================
    // BroadcastData — 数据广播（三阶段 + 去重）
    // ========================================================================
    //
    // Phase 1（加锁）：快照当前订阅表，复制 fd 列表
    // Phase 2（无锁）：遍历每个 pipeline_key，从 StreamSinkStore 获取最新数据
    //                → 去重（与上次广播的数据相同则跳过）
    //                → 序列化（调用 serializer_ 回调）
    //                → 编码为 WebSocket 文本帧
    //                → 写入所有订阅的 fd
    //                → 记录写入失败的 fd（死连接）
    // Phase 3（加锁）：清理死连接
    //
    // 为什么分三阶段？
    //   Phase 1 加锁快照 → Phase 2 无锁广播 → Phase 3 加锁清理
    //   这样设计避免了在整个广播过程中持锁，因为序列化和网络 I/O
    //   可能耗时较长（尤其是序列化大型 DataBatch 时），持锁会阻塞
    //   新连接的添加和断开。
    void BroadcastData() {
        // ---- Phase 1（加锁）：快照当前订阅表 ----
        // 复制所有订阅关系，避免在广播过程中持锁
        std::unordered_map<std::string, std::vector<int>> snapshot;
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (auto& [key, fds] : subscriptions_) {
                if (!fds.empty())
                    snapshot[key] = std::vector<int>(fds.begin(), fds.end());
            }
        }

        // ---- Phase 2（无锁）：序列化 + 发送（含去重） ----
        for (auto& [key, fds] : snapshot) {
            // 从 StreamSinkStore 获取最新数据
            auto batch = StreamSinkStore::Instance().GetBuffer(key).Latest();
            if (!batch) continue;  // 无数据，跳过

            // 去重：如果数据与上次广播相同，跳过
            // 使用 shared_ptr 直接比较（指针比较，不是内容比较），效率极高
            if (last_broadcast_[key] == batch) continue;
            last_broadcast_[key] = batch;

            // 序列化数据为 JSON 字符串
            std::string json;
            if (serializer_)
                json = serializer_(key, batch);  // 使用自定义序列化器
            else
                // 默认序列化器：只包含 pipeline 名称和时间戳
                json = "{\"pipeline\":\"" + key + "\",\"ts\":" +
                       std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now().time_since_epoch()).count()) +
                       "}";

            // 编码为 WebSocket 文本帧
            auto frame = WebSocketCodec::EncodeFrame(json);

            // 写入所有订阅的 fd，收集写入失败的 fd
            std::vector<int> dead;
            for (int fd : fds) {
                ssize_t w = ::write(fd, frame.data(), frame.size());
                if (w <= 0) dead.push_back(fd);  // 写入失败（连接已断开）
            }

            // ---- Phase 3（加锁）：清理死连接 ----
            if (!dead.empty()) {
                std::lock_guard<std::mutex> lk(mu_);
                for (int fd : dead)
                    RemoveConnectionLocked(fd);
            }
        }
    }

    // ---- RemoveConnection — 移除连接（外部调用，自动加锁） ----
    void RemoveConnection(int fd) {
        std::lock_guard<std::mutex> lk(mu_);
        RemoveConnectionLocked(fd);
    }

    // ---- RemoveConnectionLocked — 移除连接（内部调用，调用者已持锁） ----
    //
    // 移除流程：
    //   1. 从订阅表中移除（subscriptions_[key].erase(fd)）
    //   2. 从连接表中移除（connections_.erase(fd)）
    //   3. 发送 Close 帧（状态码 1000: Normal Closure）
    //   4. 关闭 fd
    void RemoveConnectionLocked(int fd) {
        auto it = connections_.find(fd);
        if (it != connections_.end()) {
            auto& key = it->second.pipeline_key;
            subscriptions_[key].erase(fd);  // 从订阅表移除
            connections_.erase(it);         // 从连接表移除
        }
        // 发送 Close 帧并关闭连接
        auto close_frame = WebSocketCodec::EncodeCloseFrame(1000);
        ::write(fd, close_frame.data(), close_frame.size());
        ::close(fd);
        IL_DEBUG("WebSocket: removed connection fd={}", fd);
    }

    // ========================================================================
    // PathToPipelineKey — 从 URL 路径提取 pipeline_key
    // ========================================================================
    //
    // 如 /ws/cpu_utilization → "cpu_utilization"
    //    /ws/                → "default"
    //
    // 参数：
    //   path: WebSocket URL 路径（如 "/ws/cpu_utilization"）
    //
    // 返回：
    //   pipeline_key（如 "cpu_utilization"）
    static std::string PathToPipelineKey(const std::string& path) {
        if (path.size() > 4 && path.substr(0, 4) == "/ws/")
            return path.substr(4);
        return "default";
    }

    // ========================================================================
    // ValidateAuth — 验证 WebSocket 连接的认证
    // ========================================================================
    //
    // 支持两种认证方式：
    //   1. Authorization: Bearer <token> 头（推荐）
    //   2. ?token=<token> 查询参数（兼容不支持自定义头的客户端）
    //
    // 如果 auth_token_ 为空，表示不需要认证，直接返回 true。
    //
    // 参数：
    //   request: 原始 HTTP 请求字符串
    //
    // 返回：
    //   true:  认证通过
    //   false: 认证失败
    bool ValidateAuth(const std::string& request) const {
        if (auth_token_.empty()) return true;  // 不需要认证

        // 方式 1：Authorization: Bearer <token> 头
        auto auth_header = detail::ExtractHeader(request, "Authorization");
        if (auth_header == "Bearer " + auth_token_) return true;

        // 方式 2：?token=<token> 查询参数
        auto token_param = ExtractQueryParam(request, "token");
        if (!token_param.empty() && token_param == auth_token_) return true;

        return false;
    }

    // ========================================================================
    // ExtractQueryParam — 从 HTTP 请求行中提取查询参数
    // ========================================================================
    //
    // 解析 GET /ws/cpu?token=xxx HTTP/1.1 中的 token 参数。
    //
    // 解析流程：
    //   1. 找到第一个空格（请求方法后）
    //   2. 找到第二个空格（URI 后）
    //   3. 提取 URI（如 "/ws/cpu?token=xxx"）
    //   4. 找到 '?' 分隔符
    //   5. 提取查询字符串（如 "token=xxx"）
    //   6. 查找参数名并提取值
    //
    // 参数：
    //   request: 原始 HTTP 请求字符串
    //   param:   要提取的参数名（如 "token"）
    //
    // 返回：
    //   参数值，未找到时返回空字符串
    static std::string ExtractQueryParam(const std::string& request,
                                          const std::string& param) {
        // 找到 URI 的起止位置
        auto sp1 = request.find(' ');
        if (sp1 == std::string::npos) return "";
        auto sp2 = request.find(' ', sp1 + 1);
        if (sp2 == std::string::npos) return "";
        auto uri = request.substr(sp1 + 1, sp2 - sp1 - 1);

        // 找到查询字符串
        auto q = uri.find('?');
        if (q == std::string::npos) return "";
        auto query = uri.substr(q + 1);

        // 查找参数值
        std::string key = param + "=";
        auto pos = query.find(key);
        if (pos == std::string::npos) return "";
        pos += key.size();
        auto end = query.find('&', pos);
        return (end == std::string::npos) ? query.substr(pos) : query.substr(pos, end - pos);
    }

    // ========================================================================
    // 成员变量
    // ========================================================================

    mutable std::mutex mu_;  // 互斥锁（保护 connections_ 和 subscriptions_）

    // 连接表：fd → ConnInfo（连接状态信息）
    std::unordered_map<int, ConnInfo> connections_;

    // 订阅表：pipeline_key → 订阅该管道的 fd 集合
    // 一个 pipeline_key 可能被多个 WebSocket 客户端订阅
    std::unordered_map<std::string, std::unordered_set<int>> subscriptions_;

    std::atomic<bool> running_{false};  // 运行状态标志
    std::thread thread_;               // 广播线程
    int broadcast_interval_ms_ = 1000;  // 广播间隔（毫秒），默认 1 秒

    WsBroadcastSerializer serializer_;  // 序列化器回调

    std::string auth_token_;  // 认证 Token（空字符串表示不需要认证）

    // 最近广播数据缓存（用于去重）
    // key: pipeline_key, value: 上次广播的 DataBatch 的 shared_ptr
    // 通过 shared_ptr 比较实现去重，效率极高（O(1) 指针比较）
    std::unordered_map<std::string, DataBatchPtr> last_broadcast_;
};

}  // namespace illuminator