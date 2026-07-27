// ============================================================================
// SSE Handler — Server-Sent Events 订阅管理器
// ============================================================================
//
// 实现 SSE 数据推送，替代 WebSocket 实时数据通道。
//
// 协议流程：
//   1. POST /api/v1/events/subscribe → 注册订阅，返回 subscription_id
//   2. GET  /api/v1/events/{id}      → 建立 SSE 长连接
//   3. POST /api/v1/events/{id}/update → 动态更新订阅列表
//
// 线程模型：
//   - SSE 连接由 httplib 的 HTTP 线程维护（每个连接占一个线程）
//   - SinkPool 线程通过 SseSink::Write() 将数据推入 per-subscription 队列
//   - HTTP 线程从队列取数据，执行实际的 write(fd) 推送
//   - SinkPool 不会被慢客户端阻塞（只做 queue push + notify）
//
// 分帧机制（大数据包）：
//   超过 64KB 的数据自动分帧传输，前端 DataBus 负责重组。
// ============================================================================

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "httplib.h"
#include "nlohmann/json.hpp"

#include "core/common/logging.h"
#include "core/common/status.h"

namespace illuminator {

static constexpr size_t kSseFrameMaxBytes = 64 * 1024;
static constexpr size_t kSseQueueMaxSize = 256;
static constexpr size_t kSseRecentCacheSize = 256;

struct SseSubscription {
    std::string id;
    std::unordered_set<std::string> features;
    std::mutex mu;
    std::condition_variable cv;
    std::queue<std::string> outbox;
    std::deque<std::pair<uint64_t, std::string>> recent_messages;
    std::atomic<bool> active{true};
    std::atomic<bool> replay_done{false};
    std::atomic<uint64_t> messages_dropped{0};
};

class SseHandler {
public:
    static SseHandler& Instance() {
        static SseHandler handler;
        return handler;
    }

    // 创建订阅，返回 subscription_id
    std::string Subscribe(const std::vector<std::string>& features) {
        auto sub = std::make_shared<SseSubscription>();
        sub->id = GenerateId();
        sub->features.insert(features.begin(), features.end());

        std::lock_guard lock(mu_);
        subscriptions_[sub->id] = sub;
        IL_INFO("SSE: new subscription '{}' for {} features", sub->id, features.size());
        return sub->id;
    }

    // 更新订阅列表
    Status UpdateSubscription(const std::string& id,
                              const std::vector<std::string>& add,
                              const std::vector<std::string>& remove) {
        auto sub = GetSubscription(id);
        if (!sub) {
            return Status::Error(StatusCode::kNotFound, "subscription not found: " + id);
        }

        std::lock_guard lock(sub->mu);
        for (auto& f : add) sub->features.insert(f);
        for (auto& f : remove) sub->features.erase(f);
        return Status::Ok();
    }

    // 移除订阅
    void Unsubscribe(const std::string& id) {
        std::lock_guard lock(mu_);
        auto it = subscriptions_.find(id);
        if (it != subscriptions_.end()) {
            it->second->active.store(false);
            it->second->cv.notify_all();
            subscriptions_.erase(it);
        }
    }

    // 关闭所有 SSE 连接（用于优雅退出）
    void ShutdownAll() {
        std::lock_guard lock(mu_);
        for (auto& [id, sub] : subscriptions_) {
            sub->active.store(false);
            sub->cv.notify_all();
        }
        subscriptions_.clear();
    }

    // 向订阅了指定 feature 的所有连接推送数据（由 SseSink 从 SinkPool 线程调用）
    void Publish(const std::string& feature, const std::string& json_data) {
        std::lock_guard lock(mu_);
        for (auto& [id, sub] : subscriptions_) {
            if (!sub->active.load()) continue;

            bool subscribed = false;
            {
                std::lock_guard sub_lock(sub->mu);
                subscribed = sub->features.count(feature) > 0;
            }

            if (subscribed) {
                EnqueueMessage(sub, feature, json_data);
            }
        }
    }

    // 处理 SSE GET 请求（阻塞式，由 httplib 线程执行）
    void HandleSseConnection(const httplib::Request& req,
                             const std::string& subscription_id,
                             httplib::Response& res) {
        auto sub = GetSubscription(subscription_id);
        if (!sub) {
            res.status = 404;
            res.set_content(R"({"error":"subscription not found"})", "application/json");
            return;
        }

        // Parse Last-Event-ID from header (standard SSE) or query param (manual reconnect).
        uint64_t resume_after_id = 0;
        bool has_resume_id = false;
        auto last_event_it = req.headers.find("Last-Event-ID");
        if (last_event_it != req.headers.end()) {
            try {
                resume_after_id = std::stoull(last_event_it->second);
                has_resume_id = true;
            } catch (...) {
                // Ignore malformed header.
            }
        } else if (req.has_param("lastEventId")) {
            try {
                resume_after_id = std::stoull(req.get_param_value("lastEventId"));
                has_resume_id = true;
            } catch (...) {
                // Ignore malformed param.
            }
        }

        res.set_header("Content-Type", "text/event-stream");
        res.set_header("Cache-Control", "no-cache");
        res.set_header("Connection", "keep-alive");
        res.set_header("X-Accel-Buffering", "no");

        res.set_chunked_content_provider(
            "text/event-stream",
            [sub, has_resume_id, resume_after_id](size_t /*offset*/, httplib::DataSink& sink) -> bool {
                // Replay cached messages that the client missed since Last-Event-ID.
                if (!sub->replay_done.load() && has_resume_id) {
                    sub->replay_done.store(true);
                    std::vector<std::string> replay;
                    {
                        std::lock_guard lock(sub->mu);
                        for (const auto& [event_id, msg] : sub->recent_messages) {
                            if (event_id > resume_after_id) {
                                replay.push_back(msg);
                            }
                        }
                    }
                    for (const auto& msg : replay) {
                        if (!sink.write(msg.c_str(), msg.size())) {
                            return false;
                        }
                    }
                }

                while (sub->active.load()) {
                    std::string message;
                    {
                        std::unique_lock lock(sub->mu);
                        sub->cv.wait_for(lock, std::chrono::seconds(15), [&sub] {
                            return !sub->outbox.empty() || !sub->active.load();
                        });

                        if (!sub->active.load()) return false;

                        if (sub->outbox.empty()) {
                            // Send heartbeat comment every 15s to keep the connection alive
                            std::string heartbeat = ":heartbeat\n\n";
                            sink.write(heartbeat.c_str(), heartbeat.size());
                            continue;
                        }

                        message = std::move(sub->outbox.front());
                        sub->outbox.pop();
                    }

                    if (!sink.write(message.c_str(), message.size())) {
                        return false;
                    }
                }
                return false;
            },
            [sub](bool /*success*/) {
                sub->active.store(false);
                IL_INFO("SSE: connection closed for subscription '{}'", sub->id);
            });
    }

    // 注册 SSE 路由到 httplib Server
    void RegisterRoutes(httplib::Server& server) {
        server.Post("/api/v1/events/subscribe",
            [this](const httplib::Request& req, httplib::Response& res) {
                HandleSubscribe(req, res);
            });

        server.Get(R"(/api/v1/events/([a-zA-Z0-9_-]+))",
            [this](const httplib::Request& req, httplib::Response& res) {
                auto id = req.matches[1].str();
                HandleSseConnection(req, id, res);
            });

        server.Post(R"(/api/v1/events/([a-zA-Z0-9_-]+)/update)",
            [this](const httplib::Request& req, httplib::Response& res) {
                HandleUpdateSubscription(req, res);
            });

        IL_INFO("SSE routes registered");
    }

    size_t ActiveSubscriptions() const {
        std::lock_guard lock(mu_);
        return subscriptions_.size();
    }

    std::shared_ptr<SseSubscription> GetSubscription(const std::string& id) {
        std::lock_guard lock(mu_);
        auto it = subscriptions_.find(id);
        return (it != subscriptions_.end()) ? it->second : nullptr;
    }

private:
    SseHandler() = default;

    void HandleSubscribe(const httplib::Request& req, httplib::Response& res) {
        try {
            auto body = nlohmann::json::parse(req.body);
            std::vector<std::string> features;
            if (body.contains("features") && body["features"].is_array()) {
                for (auto& f : body["features"]) {
                    features.push_back(f.get<std::string>());
                }
            }

            auto id = Subscribe(features);
            nlohmann::json response = {
                {"subscription_id", id},
                {"url", "/api/v1/events/" + id}
            };
            res.set_content(response.dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            nlohmann::json err = {{"error", e.what()}};
            res.set_content(err.dump(), "application/json");
        }
    }

    void HandleUpdateSubscription(const httplib::Request& req, httplib::Response& res) {
        auto id = req.matches[1].str();
        try {
            auto body = nlohmann::json::parse(req.body);
            std::vector<std::string> add_features, remove_features;

            if (body.contains("add") && body["add"].is_array()) {
                for (auto& f : body["add"]) add_features.push_back(f.get<std::string>());
            }
            if (body.contains("remove") && body["remove"].is_array()) {
                for (auto& f : body["remove"]) remove_features.push_back(f.get<std::string>());
            }

            auto status = UpdateSubscription(id, add_features, remove_features);
            if (!status.ok()) {
                res.status = 404;
                nlohmann::json err = {{"error", status.message()}};
                res.set_content(err.dump(), "application/json");
                return;
            }

            res.set_content(R"({"ok":true})", "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            nlohmann::json err = {{"error", e.what()}};
            res.set_content(err.dump(), "application/json");
        }
    }

    void EnqueueMessage(std::shared_ptr<SseSubscription>& sub,
                        const std::string& feature,
                        const std::string& json_data) {
        // Assign a monotonic event id so EventSource can send Last-Event-ID on reconnect.
        uint64_t event_id = seq_counter_.fetch_add(1);
        if (json_data.size() <= kSseFrameMaxBytes) {
            std::string msg = "id: " + std::to_string(event_id) + "\nevent: data\ndata: " + json_data + "\n\n";
            std::lock_guard lock(sub->mu);
            if (sub->outbox.size() < kSseQueueMaxSize) {
                sub->outbox.push(msg);
                CacheRecentMessage(sub, event_id, msg);
                sub->cv.notify_one();
            }
        } else {
            // Split into frames
            size_t total_frames = (json_data.size() + kSseFrameMaxBytes - 1) / kSseFrameMaxBytes;
            uint64_t seq = event_id;
            std::vector<std::string> frames;
            frames.reserve(total_frames);

            for (size_t i = 0; i < total_frames; ++i) {
                size_t offset = i * kSseFrameMaxBytes;
                size_t len = std::min(kSseFrameMaxBytes, json_data.size() - offset);
                std::string chunk = json_data.substr(offset, len);

                nlohmann::json frame = {
                    {"feature", feature},
                    {"seq", seq},
                    {"frame_idx", i},
                    {"frame_total", total_frames},
                    {"payload", chunk}
                };

                std::string msg = "id: " + std::to_string(event_id) + "\nevent: frame\ndata: " + frame.dump() + "\n\n";
                frames.push_back(std::move(msg));
            }

            std::lock_guard lock(sub->mu);
            if (sub->outbox.size() + frames.size() <= kSseQueueMaxSize) {
                for (auto& msg : frames) {
                    sub->outbox.push(msg);
                    CacheRecentMessage(sub, event_id, msg);
                }
                sub->cv.notify_one();
            } else {
                sub->messages_dropped.fetch_add(1);
            }
        }
    }

    void CacheRecentMessage(std::shared_ptr<SseSubscription>& sub,
                            uint64_t event_id,
                            const std::string& msg) {
        sub->recent_messages.emplace_back(event_id, msg);
        if (sub->recent_messages.size() > kSseRecentCacheSize) {
            sub->recent_messages.pop_front();
        }
    }

    std::string GenerateId() {
        static std::atomic<uint64_t> counter{0};
        uint64_t id = counter.fetch_add(1);
        char buf[32];
        snprintf(buf, sizeof(buf), "sub_%lu", id);
        return buf;
    }

    mutable std::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<SseSubscription>> subscriptions_;
    std::atomic<uint64_t> seq_counter_{0};
};

}  // namespace illuminator
