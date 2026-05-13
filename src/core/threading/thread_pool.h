// ============================================================================
// Illuminator 线程池 — 通用任务执行器
// ============================================================================
//
// 标准生产者-消费者模式线程池，用于并行处理数据批次的各个阶段。
// 线程数默认为 std::thread::hardware_concurrency()（逻辑 CPU 核数）。
//
// 核心方法：
// ==========
// - Submit(F, args...): 提交一个可调用对象到任务队列，返回 std::future
//                        用于获取异步执行结果
// - PendingTasks(): 返回当前排队中的任务数量
//
// 使用示例：
// ==========
//   ThreadPool pool(4);
//   auto future = pool.Submit([](int x) { return x * 2; }, 21);
//   int result = future.get();  // 阻塞等待，result = 42
//
// 析构时自动通知所有线程退出并 join 等待完成。
// ============================================================================

#pragma once

#include <cstddef>
#include <functional>        // std::function
#include <future>            // std::future, std::packaged_task
#include <memory>            // std::make_shared
#include <mutex>             // std::mutex, std::lock_guard
#include <queue>             // std::queue
#include <thread>            // std::thread
#include <vector>
#include <condition_variable>
#include <type_traits>

namespace illuminator {

class ThreadPool {
public:
    // 构造函数：创建指定数量的工作线程，每个工作线程进入 WorkerLoop 等待任务
    explicit ThreadPool(size_t num_threads = std::thread::hardware_concurrency()) {
        for (size_t i = 0; i < num_threads; ++i) {
            workers_.emplace_back([this] { WorkerLoop(); });
        }
    }

    // 析构函数：设置停止标志，通知所有线程，等待它们退出
    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();  // 唤醒所有阻塞的线程
        for (auto& w : workers_) {
            if (w.joinable()) w.join();
        }
    }

    // 禁止拷贝
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // ---- 提交任务 ----
    // F: 可调用对象，Args: 可调用对象的参数
    // 返回 std::future，可通过 future.get() 获取结果（阻塞等待）
    template <typename F, typename... Args>
    auto Submit(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>> {
        using ReturnType = std::invoke_result_t<F, Args...>;

        // 使用 packaged_task 包装任务，使其可通过 future 获取结果
        auto task = std::make_shared<std::packaged_task<ReturnType()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));

        auto future = task->get_future();  // 获取与任务关联的 future

        {
            std::lock_guard<std::mutex> lock(mutex_);
            // 用一个 lambda 包装任务（捕获 shared_ptr 确保任务存活）
            tasks_.emplace([task]() { (*task)(); });
        }
        cv_.notify_one();  // 通知一个等待线程
        return future;
    }

    // ---- 查询 ----
    // 当前排队的任务数
    size_t PendingTasks() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return tasks_.size();
    }

    // 工作线程总数
    size_t NumThreads() const { return workers_.size(); }

private:
    // ---- 工作线程主循环 ----
    // 每个工作线程在此循环中等待任务，取出后执行。
    // 当 stop_ 为 true 且队列为空时退出。
    void WorkerLoop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                // 条件变量等待：直到有任务可执行或需要停止
                cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });

                // 停止条件：已发停止信号且队列为空
                if (stop_ && tasks_.empty()) return;

                // 取出队首任务
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            // 执行任务（锁已释放，不阻塞其他线程取任务）
            task();
        }
    }

    std::vector<std::thread> workers_;       // 工作线程列表
    std::queue<std::function<void()>> tasks_; // 任务队列
    mutable std::mutex mutex_;               // 保护 tasks_ 和 stop_ 的互斥锁
    std::condition_variable cv_;             // 条件变量（任务通知 + 停止通知）
    bool stop_ = false;                      // 停止标志
};

}  // namespace illuminator
