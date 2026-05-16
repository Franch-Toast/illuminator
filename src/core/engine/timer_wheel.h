// ============================================================================
// Illuminator TimerWheel — 基于 timerfd + epoll 的全局定时事件调度器
// ============================================================================
//
// 管理所有周期性事件的触发时机，不执行任何实际工作。
// 定时器回调只做 dispatch（提交任务到线程池或注入 Sentinel 到 channel），
// 保证纳秒级完成，TimerWheel 线程永不阻塞。
//
// 内部结构:
//   - 1 个 timerfd (CLOCK_MONOTONIC): 动态 arm 到堆顶最近触发时间
//   - 1 个 eventfd: 用于外部唤醒 (注册新定时器 / 停止信号)
//   - epoll 同时监听 timerfd 和 eventfd
//   - priority_queue 管理定时器条目
// ============================================================================

#pragma once

#include <sys/timerfd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "core/common/logging.h"
#include "core/threading/thread_util.h"

namespace illuminator {

class TimerWheel {
public:
    using Callback = std::function<void()>;

    struct TimerEntry {
        uint32_t id;
        std::chrono::steady_clock::time_point next_fire;
        std::chrono::milliseconds interval;
        Callback callback;
        bool repeating;

        bool operator>(const TimerEntry& o) const { return next_fire > o.next_fire; }
    };

    TimerWheel() {
        epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
        timer_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        event_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

        if (epoll_fd_ < 0 || timer_fd_ < 0 || event_fd_ < 0) {
            IL_ERROR("TimerWheel: failed to create fds (epoll={}, timer={}, event={})",
                     epoll_fd_, timer_fd_, event_fd_);
            return;
        }

        struct epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = timer_fd_;
        epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, timer_fd_, &ev);
        ev.data.fd = event_fd_;
        epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, event_fd_, &ev);
    }

    ~TimerWheel() {
        Stop();
        if (timer_fd_ >= 0) close(timer_fd_);
        if (event_fd_ >= 0) close(event_fd_);
        if (epoll_fd_ >= 0) close(epoll_fd_);
    }

    TimerWheel(const TimerWheel&) = delete;
    TimerWheel& operator=(const TimerWheel&) = delete;

    uint32_t AddRepeating(std::chrono::milliseconds interval, Callback cb) {
        std::lock_guard<std::mutex> lock(mutex_);
        uint32_t id = next_id_++;
        heap_.push({id, std::chrono::steady_clock::now() + interval,
                    interval, std::move(cb), true});
        RearmTimerfdLocked();
        Wakeup();
        return id;
    }

    uint32_t AddOnce(std::chrono::milliseconds delay, Callback cb) {
        std::lock_guard<std::mutex> lock(mutex_);
        uint32_t id = next_id_++;
        heap_.push({id, std::chrono::steady_clock::now() + delay,
                    std::chrono::milliseconds(0), std::move(cb), false});
        RearmTimerfdLocked();
        Wakeup();
        return id;
    }

    void Cancel(uint32_t timer_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        cancelled_.push_back(timer_id);
    }

    void Start() {
        if (epoll_fd_ < 0) return;
        running_.store(true, std::memory_order_release);
        thread_ = std::thread([this] { Run(); });
        IL_INFO("TimerWheel started");
    }

    void Stop() {
        if (!running_.exchange(false)) return;
        Wakeup();
        if (thread_.joinable()) thread_.join();
        IL_INFO("TimerWheel stopped ({} timers fired total)",
                fires_total_.load(std::memory_order_relaxed));
    }

    size_t ActiveTimers() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return heap_.size();
    }

    uint64_t FiresTotal() const { return fires_total_.load(std::memory_order_relaxed); }

private:
    void Run() {
        SetThreadName("timer-wheel");
        struct epoll_event events[2];

        while (running_.load(std::memory_order_acquire)) {
            int nfds = epoll_wait(epoll_fd_, events, 2, 1000);

            for (int i = 0; i < nfds; ++i) {
                if (events[i].data.fd == timer_fd_) {
                    uint64_t expirations;
                    [[maybe_unused]] auto r = read(timer_fd_, &expirations, sizeof(expirations));
                } else if (events[i].data.fd == event_fd_) {
                    uint64_t val;
                    [[maybe_unused]] auto r = read(event_fd_, &val, sizeof(val));
                }
            }

            ProcessFired();
        }
    }

    void ProcessFired() {
        auto now = std::chrono::steady_clock::now();
        std::vector<TimerEntry> fired;

        {
            std::lock_guard<std::mutex> lock(mutex_);

            PurgeCancelledLocked();

            while (!heap_.empty() && heap_.top().next_fire <= now) {
                fired.push_back(heap_.top());
                heap_.pop();
            }
        }

        for (auto& entry : fired) {
            entry.callback();
            fires_total_.fetch_add(1, std::memory_order_relaxed);

            if (entry.repeating && running_.load(std::memory_order_relaxed)) {
                std::lock_guard<std::mutex> lock(mutex_);
                entry.next_fire += entry.interval;
                heap_.push(std::move(entry));
            }
        }

        std::lock_guard<std::mutex> lock(mutex_);
        RearmTimerfdLocked();
    }

    void PurgeCancelledLocked() {
        if (cancelled_.empty()) return;

        std::vector<TimerEntry> remaining;
        while (!heap_.empty()) {
            auto entry = heap_.top();
            heap_.pop();
            bool is_cancelled = false;
            for (auto cid : cancelled_) {
                if (entry.id == cid) { is_cancelled = true; break; }
            }
            if (!is_cancelled) remaining.push_back(std::move(entry));
        }
        for (auto& e : remaining) heap_.push(std::move(e));
        cancelled_.clear();
    }

    void RearmTimerfdLocked() {
        struct itimerspec its{};

        if (!heap_.empty()) {
            auto dur = heap_.top().next_fire - std::chrono::steady_clock::now();
            if (dur.count() <= 0) {
                its.it_value.tv_nsec = 1;
            } else {
                auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(dur);
                its.it_value.tv_sec = static_cast<time_t>(ns.count() / 1'000'000'000);
                its.it_value.tv_nsec = static_cast<long>(ns.count() % 1'000'000'000);
            }
        }
        timerfd_settime(timer_fd_, 0, &its, nullptr);
    }

    void Wakeup() {
        uint64_t val = 1;
        [[maybe_unused]] auto r = write(event_fd_, &val, sizeof(val));
    }

    using Heap = std::priority_queue<TimerEntry, std::vector<TimerEntry>,
                                     std::greater<TimerEntry>>;
    Heap heap_;
    mutable std::mutex mutex_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    uint32_t next_id_{0};
    std::atomic<uint64_t> fires_total_{0};
    std::vector<uint32_t> cancelled_;

    int epoll_fd_ = -1;
    int timer_fd_ = -1;
    int event_fd_ = -1;
};

}  // namespace illuminator
