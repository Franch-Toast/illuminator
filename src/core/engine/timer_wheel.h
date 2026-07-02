// ============================================================================
// Illuminator TimerWheel — 基于 timerfd + epoll 的全局定时事件调度器
// ============================================================================
//
// TimerWheel 是 Illuminator v3 事件驱动架构的"心跳"组件，负责管理所有
// 周期性事件的触发时机。它不执行任何实际工作，只做调度决策。
//
// 核心设计理念：
// ===============
// "调度与执行分离"（Separation of Scheduling and Execution）
// TimerWheel 只决定"何时触发什么"，回调函数只做轻量级 dispatch
// （提交任务到线程池或注入 Sentinel 到 channel），保证纳秒级完成，
// TimerWheel 线程永不阻塞。
//
// 内部三大组件：
// ===============
// 1. timerfd（Linux 内核定时器）
//    - 基于 CLOCK_MONOTONIC 单调时钟，不受系统时间调整影响
//    - 文件描述符形式的定时器，可被 epoll 监听
//    - 动态 arm 到最小堆顶的最近触发时间
//
// 2. eventfd（线程间唤醒信号）
//    - 轻量级事件通知机制，基于内核计数器
//    - 外部线程注册新定时器时，写入 eventfd 唤醒 epoll 循环
//    - 避免 TimerWheel 线程在 sleep 中错过新注册的定时器
//
// 3. priority_queue（最小堆，定时器条目排序）
//    - 堆顶始终是最近要触发的定时器
//    - O(log n) 插入，O(1) 获取堆顶
//    - 使用 std::greater<TimerEntry> 实现最小堆
//
// 定时器回调只做 dispatch：
// =========================
// - Pull Source 采集事件 → 提交 Source::Collect() 到 CollectPool
// - Aggregator Flush 事件 → 调用 Pipeline::InjectFlush() 注入 Sentinel
// - Metrics 同步事件  → 提交指标采集到 CollectPool
// - 存储清理事件      → 提交 Prune 到 SinkPool
//
// 关键设计决策：
// ==============
// | 决策 | 选择 | 理由 |
// |------|------|------|
// | 实现方式 | timerfd + epoll + eventfd | Linux 内核精度高、无忙等待、支持外部唤醒 |
// | 回调执行 | 释放锁后执行回调 | 防止回调中注册新定时器导致死锁 |
// | 线程数 | 固定 1 线程 | 回调只做 Submit/Enqueue，纳秒级完成 |
// | 取消策略 | 懒惰删除（Lazy Deletion） | O(1) 取消，下次触发时统一清理 |
// | 重复定时器 drift | next_fire += interval | 避免回调执行时间累积导致的漂移 |
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

    // ---- TimerEntry: 定时器条目 ----
    // 每个条目代表一个注册的定时器，包含触发时间、间隔、回调等信息。
    // operator> 用于 priority_queue 的最小堆排序：next_fire 越早越靠前。
    struct TimerEntry {
        uint32_t id;                                       // 定时器唯一 ID
        std::chrono::steady_clock::time_point next_fire;   // 下次触发时间点（单调时钟）
        std::chrono::milliseconds interval;                 // 重复间隔（一次性定时器为 0）
        Callback callback;                                  // 触发时执行的回调函数
        bool repeating;                                     // 是否周期性重复

        // 最小堆比较：next_fire 越早的优先级越高（堆顶 = 最近触发）
        bool operator>(const TimerEntry& o) const { return next_fire > o.next_fire; }
    };

    // ---- 构造：创建 timerfd、eventfd、epoll 实例 ----
    // - timerfd 使用 CLOCK_MONOTONIC（单调时钟，不受系统时间调整影响）
    // - TFD_NONBLOCK：非阻塞模式，读取时不会阻塞线程
    // - EFD_CLOEXEC / EPOLL_CLOEXEC：exec 时自动关闭 fd，防止泄漏到子进程
    TimerWheel() {
        epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
        timer_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        event_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

        if (epoll_fd_ < 0 || timer_fd_ < 0 || event_fd_ < 0) {
            IL_ERROR("TimerWheel: failed to create fds (epoll={}, timer={}, event={})",
                     epoll_fd_, timer_fd_, event_fd_);
            return;
        }

        // 将 timerfd 和 eventfd 注册到 epoll 监听
        struct epoll_event ev{};
        ev.events = EPOLLIN;   // 监听可读事件（timerfd 到期 = 可读，eventfd 被写 = 可读）
        ev.data.fd = timer_fd_;
        epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, timer_fd_, &ev);
        ev.data.fd = event_fd_;
        epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, event_fd_, &ev);
    }

    // ---- 析构：停止调度线程，关闭所有 fd ----
    ~TimerWheel() {
        Stop();
        if (timer_fd_ >= 0) close(timer_fd_);
        if (event_fd_ >= 0) close(event_fd_);
        if (epoll_fd_ >= 0) close(epoll_fd_);
    }

    TimerWheel(const TimerWheel&) = delete;
    TimerWheel& operator=(const TimerWheel&) = delete;

    // ---- 注册周期性重复定时器 ----
    // interval: 触发间隔
    // cb:       触发时执行的回调（应只做轻量 dispatch，不阻塞）
    // 返回:     定时器 ID，用于后续 Cancel 操作
    uint32_t AddRepeating(std::chrono::milliseconds interval, Callback cb) {
        std::lock_guard<std::mutex> lock(mutex_);
        uint32_t id = next_id_++;
        heap_.push({id, std::chrono::steady_clock::now() + interval,
                    interval, std::move(cb), true});
        RearmTimerfdLocked();  // 新定时器可能比堆顶更早触发，需要重新 arm timerfd
        Wakeup();              // 唤醒 epoll 循环，让它重新检查堆顶
        return id;
    }

    // ---- 注册一次性定时器 ----
    // delay: 延迟时间
    // cb:    触发时执行的回调
    // 返回:  定时器 ID
    uint32_t AddOnce(std::chrono::milliseconds delay, Callback cb) {
        std::lock_guard<std::mutex> lock(mutex_);
        uint32_t id = next_id_++;
        heap_.push({id, std::chrono::steady_clock::now() + delay,
                    std::chrono::milliseconds(0), std::move(cb), false});
        RearmTimerfdLocked();
        Wakeup();
        return id;
    }

    // ---- 取消定时器（懒惰删除） ----
    // 只记录 ID 到 cancelled_ 列表，不立即从堆中删除。
    // 这样 Cancel 是 O(1) 操作，真正的删除推迟到下次 ProcessFired 时统一清理。
    void Cancel(uint32_t timer_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        cancelled_.push_back(timer_id);
    }

    // ---- 启动调度线程 ----
    void Start() {
        if (epoll_fd_ < 0) return;
        if (running_.load(std::memory_order_acquire)) return;
        running_.store(true, std::memory_order_release);
        thread_ = std::thread([this] { Run(); });
        IL_INFO("TimerWheel started");
    }

    // ---- 停止调度线程 ----
    // 1. 设置 running_ = false
    // 2. 通过 eventfd 唤醒 Run() 中的 epoll_wait
    // 3. 等待线程退出
    void Stop() {
        if (!running_.exchange(false)) return;  // exchange 保证只执行一次
        Wakeup();                                // 唤醒可能正在 epoll_wait 的线程
        if (thread_.joinable()) thread_.join();
        IL_INFO("TimerWheel stopped ({} timers fired total)",
                fires_total_.load(std::memory_order_relaxed));
    }

    // ---- 查询当前活跃的定时器数量 ----
    size_t ActiveTimers() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return heap_.size();
    }

    // ---- 查询累计触发次数 ----
    uint64_t FiresTotal() const { return fires_total_.load(std::memory_order_relaxed); }

private:
    // ====================================================================
    // Run() — 主事件循环（在独立线程中运行）
    // ====================================================================
    // 使用 epoll 同时监听 timerfd 和 eventfd：
    //   - timerfd 到期 → 说明有定时器该触发了 → 调用 ProcessFired()
    //   - eventfd 被写 → 有新定时器注册或需要停止 → 调用 ProcessFired()
    //   - epoll_wait 超时（1秒）→ 兜底检查，防止边界情况下的遗漏
    //
    // 注意：epoll_wait 返回后需要读取 fd 中的数据，否则下次不会触发。
    //   timerfd 会累加过期次数，必须 read 读出；eventfd 必须 read 清零计数器。
    void Run() {
        SetThreadName("timer-wheel");
        struct epoll_event events[2];  // 最多同时监听 2 个事件

        while (running_.load(std::memory_order_acquire)) {
            // 阻塞等待事件，最多 1 秒超时（防止某些边界情况下的死等）
            int nfds = epoll_wait(epoll_fd_, events, 2, 1000);

            for (int i = 0; i < nfds; ++i) {
                if (events[i].data.fd == timer_fd_) {
                    // timerfd 到期：读掉过期次数，让 fd 恢复不可读状态
                    uint64_t expirations;
                    [[maybe_unused]] auto r = read(timer_fd_, &expirations, sizeof(expirations));
                } else if (events[i].data.fd == event_fd_) {
                    // eventfd 被唤醒：读掉计数器值，让 fd 恢复不可读状态
                    uint64_t val;
                    [[maybe_unused]] auto r = read(event_fd_, &val, sizeof(val));
                }
            }

            ProcessFired();  // 处理所有到期的定时器
        }
    }

    // ====================================================================
    // ProcessFired() — 处理所有到期的定时器
    // ====================================================================
    // 流程：
    //   1. 加锁，从堆中弹出所有 next_fire <= now 的条目
    //   2. 释放锁（关键！），逐个执行回调
    //   3. 重复定时器：next_fire += interval 后重新入堆
    //   4. 最后重新 arm timerfd 到新的堆顶时间
    //
    // 设计要点：
    //   - 回调在锁外执行：防止回调中注册/取消定时器导致死锁
    //   - next_fire += interval 而非 next_fire = now + interval：
    //     避免回调执行时间导致的"漂移"，保持精确的周期性
    void ProcessFired() {
        auto now = std::chrono::steady_clock::now();
        std::vector<TimerEntry> fired;

        {
            std::lock_guard<std::mutex> lock(mutex_);

            // 先清理被取消的定时器（懒惰删除）
            PurgeCancelledLocked();

            // 弹出所有到期的条目
            while (!heap_.empty() && heap_.top().next_fire <= now) {
                fired.push_back(heap_.top());
                heap_.pop();
            }
        }  // 锁释放 — 回调在锁外执行

        // 逐个执行回调（无锁！）
        for (auto& entry : fired) {
            entry.callback();
            fires_total_.fetch_add(1, std::memory_order_relaxed);

            // 周期性定时器重新入堆
            if (entry.repeating && running_.load(std::memory_order_relaxed)) {
                std::lock_guard<std::mutex> lock(mutex_);
                entry.next_fire += entry.interval;  // 保持精确周期，不累积 drift
                heap_.push(std::move(entry));
            }
        }

        // 重新设置 timerfd 到新的堆顶时间
        std::lock_guard<std::mutex> lock(mutex_);
        RearmTimerfdLocked();
    }

    // ====================================================================
    // PurgeCancelledLocked() — 懒惰删除：统一清理被取消的定时器
    // ====================================================================
    // 策略：Cancel 时只记录 ID，不立即从堆中删除。
    // 等到下次 ProcessFired 时，通过重建堆的方式统一清理。
    //
    // 复杂度：O(n log n)（重建堆），但分摊到每次触发，实际开销很小。
    // 如果 Cancel 很少发生（正常情况），这个函数几乎不执行任何操作。
    void PurgeCancelledLocked() {
        if (cancelled_.empty()) return;

        // 重建堆：过滤掉被取消的条目
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

    // ====================================================================
    // RearmTimerfdLocked() — 重新设置内核定时器
    // ====================================================================
    // 用堆顶的 next_fire 时间设置 timerfd 的下次触发时间。
    // 如果堆为空，设置 its.it_value = 0，表示"不触发"（无限等待）。
    // 如果堆顶已过期（dur <= 0），设置 1ns 后立即触发，避免死等。
    void RearmTimerfdLocked() {
        struct itimerspec its{};

        if (!heap_.empty()) {
            auto dur = heap_.top().next_fire - std::chrono::steady_clock::now();
            if (dur.count() <= 0) {
                // 已过期：设置 1ns 后立即触发
                its.it_value.tv_nsec = 1;
            } else {
                // 未过期：计算剩余纳秒数并设置到 timerfd
                auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(dur);
                its.it_value.tv_sec = static_cast<time_t>(ns.count() / 1'000'000'000);
                its.it_value.tv_nsec = static_cast<long>(ns.count() % 1'000'000'000);
            }
        }
        // its.it_value = {0, 0} 表示"不触发"，timerfd 将一直等待
        timerfd_settime(timer_fd_, 0, &its, nullptr);
    }

    // ---- 唤醒 epoll 循环 ----
    // 向 eventfd 写入 1，使 epoll_wait 立即返回。
    // 用于：注册新定时器后需要重新检查堆顶，或 Stop 时唤醒线程。
    void Wakeup() {
        uint64_t val = 1;
        [[maybe_unused]] auto r = write(event_fd_, &val, sizeof(val));
    }

    // ---- 最小堆类型别名 ----
    // std::greater<TimerEntry> 实现最小堆：堆顶是 next_fire 最小的条目
    using Heap = std::priority_queue<TimerEntry, std::vector<TimerEntry>,
                                     std::greater<TimerEntry>>;
    Heap heap_;                          // 定时器最小堆（按 next_fire 排序）
    mutable std::mutex mutex_;           // 保护堆操作和 cancelled_ 列表的互斥锁
    std::atomic<bool> running_{false};   // 调度线程是否在运行
    std::thread thread_;                 // 调度线程
    uint32_t next_id_{0};               // 下一个定时器 ID（单调递增）
    std::atomic<uint64_t> fires_total_{0};  // 累计触发次数（统计用）
    std::vector<uint32_t> cancelled_;   // 待取消的定时器 ID 列表（懒惰删除）

    // ---- Linux 文件描述符 ----
    int epoll_fd_ = -1;   // epoll 实例
    int timer_fd_ = -1;   // 内核定时器
    int event_fd_ = -1;   // 唤醒信号
};

}  // namespace illuminator