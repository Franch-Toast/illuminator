// ============================================================================
// Illuminator AsyncChannel — 支持事件多态的管道异步通道
// ============================================================================
//
// AsyncChannel 是生产者和消费者之间的"有界无锁管道"，封装了 LockFreeQueue，
// 传输 variant<DataBatchPtr, FlushSentinel>，使 ProcessThread 成为纯事件处理器。
//
// 核心设计理念：
// ===============
// 1. 事件多态（Event Polymorphism）
//    Channel 中传输两种消息，通过 std::variant 统一封装：
//      - DataBatchPtr: 正常数据批次（Source 采集或 Processor 处理后）
//      - FlushSentinel: Aggregator 刷盘触发信号（由 TimerWheel 回调注入）
//    消费者通过 std::visit 分发到不同的处理分支，无需 if-else 类型判断。
//
// 2. 生产者-消费者解耦
//    - 生产者（CollectPool 线程、eBPF 回调）只管入队，不关心消费者何时处理
//    - 消费者（ProcessThread）只管出队，不关心数据何时到达
//    - 双方通过无锁队列通信，无锁竞争，性能极高
//
// 3. 反压（Backpressure）机制
//    当消费者处理速度跟不上生产者时，队列逐渐填满。AsyncChannel 通过水位线
//    （high/low watermark）和丢包策略（DropPolicy）来控制流量：
//      - 超过 high watermark（默认 80%）→ 触发反压，通知 Source 减速
//      - 低于 low watermark（默认 20%）→ 解除反压，Source 恢复正常速率
//      这种"滞后"（hysteresis）设计避免了在阈值附近反复震荡。
//
// 4. 三级自适应退避（Adaptive Backoff）
//    Dequeue 时消费者不在空队列上空转，而是逐步降低 CPU 开销：
//      Phase 1 — Spin（16 次忙等）：适合数据密集场景，延迟最低（纳秒级响应）
//      Phase 2 — Yield（8 次让步）：让出 CPU 给其他线程，仍有数据则快速返回
//      Phase 3 — Sleep（1ms 间隔）：数据稀疏时的节能模式，CPU 占用趋近于零
//    这种设计在"高吞吐"和"低 CPU"之间取得了平衡。
//
// 通道传输的两种消息：
// =====================
// - DataBatchPtr: 正常数据批次（指标记录、堆栈采样等）
//   由 Source::Collect() 或 eBPF 回调生产，ProcessThread 消费
// - FlushSentinel: 空结构体，占 1 字节，仅作为信号
//   由 TimerWheel 定时回调调用 InjectFlush() 注入
//   ProcessThread 收到后触发 Aggregator::Flush() 刷出聚合数据
//
// 设计要点：
// ==========
//   1. 数据入队 TryEnqueue: 无锁 CAS，纳秒级
//   2. Sentinel 注入 InjectFlush: 优先级高于普通数据（队列满时驱逐旧数据）
//   3. 消费端 Dequeue: spin → yield → sleep 三级自适应退避
//   4. 水位线反压: 超过 high watermark 时触发 backpressured
//   5. 全量统计: enqueued / dequeued / dropped / flush_injected / backpressure_events
//   6. 丢包策略: kDropNewest（拒绝新数据）或 kDropOldest（驱逐旧数据）
// ============================================================================

#pragma once

#include <atomic>
#include <chrono>
#include <optional>
#include <thread>
#include <variant>

#include "core/engine/data_batch.h"
#include "core/memory/lock_free_queue.h"

namespace illuminator {

// ============================================================================
// Overloaded — std::visit 的辅助工具（C++17 标准技巧）
// ============================================================================
// std::visit 需要传入一个"可调用对象"，该对象必须对 variant 的每一种类型
// 都有对应的 operator() 重载。Overloaded 通过多重继承将多个 lambda 的
// operator() 合并到一个对象中，实现"模式匹配"式的代码。
//
// 用法：
//   std::visit(Overloaded{
//       [](DataBatchPtr& batch) { /* 处理数据 */ },
//       [](FlushSentinel&)     { /* 处理刷盘信号 */ },
//   }, variant_item);
//
// 原理：Overloaded<Lambda1, Lambda2> 同时继承 Lambda1 和 Lambda2，
//   通过 using Ts::operator()... 将两个 lambda 的 operator() 都引入作用域，
//   std::visit 会根据 variant 的实际类型调用对应的 operator()。
template <class... Ts> struct Overloaded : Ts... { using Ts::operator()...; };
// C++17 类模板参数推导指引（CTAD）：让编译器自动推导模板参数
template <class... Ts> Overloaded(Ts...) -> Overloaded<Ts...>;

// ============================================================================
// FlushSentinel — 刷盘信号（空载荷标记）
// ============================================================================
// 一个空结构体，仅作为"信号"存在，不携带任何数据。占 1 字节（C++ 规定空类型
// 至少占 1 字节以保证不同对象有不同地址）。在 variant 中通过类型标签区分，
// 不需要额外的 bool 或 enum 字段。
//
// 由 TimerWheel 的定时回调通过 InjectFlush() 注入到 AsyncChannel 中，
// ProcessThread 收到后触发 Aggregator::Flush() 将累积的数据刷出。
struct FlushSentinel {};

// ============================================================================
// ChannelItem — 通道中传输的消息类型
// ============================================================================
// std::variant 是 C++17 引入的类型安全的联合体（union）。
// 与 C 语言的 union 不同，variant 会记住当前存储的是哪个类型，
// 通过 std::visit 可以安全地访问当前值。
//
// sizeof(ChannelItem) = sizeof(variant<shared_ptr, FlushSentinel>)
//                     = 16B（shared_ptr）+ 1B（FlushSentinel）+ 7B（padding）+ 8B（discriminant）
//                     ≈ 24 字节（具体取决于编译器实现）
using ChannelItem = std::variant<DataBatchPtr, FlushSentinel>;

// ============================================================================
// DropPolicy — 队列满时的丢包策略
// ============================================================================
// kDropNewest: 拒绝新数据（丢弃即将入队的数据）
//   适合"数据新鲜度"优先的场景 — 宁可丢新数据，也要保留历史数据
// kDropOldest: 驱逐旧数据（丢弃队列中最旧的数据，腾出空间给新数据）
//   适合"最新数据优先"的场景 — 宁可丢旧数据，也要保证最新数据被处理
enum class DropPolicy : uint8_t {
    kDropNewest,
    kDropOldest,
};

// ============================================================================
// AsyncChannel — 异步有界通道
// ============================================================================
class AsyncChannel {
public:
    // ---- 统计信息（全部使用 atomic，保证多线程安全读取） ----
    struct Stats {
        std::atomic<uint64_t> enqueued{0};            // 成功入队次数
        std::atomic<uint64_t> dropped{0};             // 丢弃次数（队列满 + 丢包策略）
        std::atomic<uint64_t> dequeued{0};            // 成功出队次数
        std::atomic<uint64_t> flush_injected{0};      // FlushSentinel 注入次数
        std::atomic<uint64_t> backpressure_events{0}; // 反压触发次数（进入反压时 +1）
    };

    // ---- 构造 ----
    // capacity: 队列容量（默认 4096，会被 RoundUpPow2 对齐到 2 的幂）
    // policy:   丢包策略
    // high_wm:  高水位线比例（默认 0.8 = 80%），超过此比例触发反压
    // low_wm:   低水位线比例（默认 0.2 = 20%），低于此比例解除反压
    explicit AsyncChannel(size_t capacity = 4096,
                          DropPolicy policy = DropPolicy::kDropNewest,
                          double high_wm = 0.8, double low_wm = 0.2)
        : queue_(capacity), drop_policy_(policy),
          high_wm_(high_wm), low_wm_(low_wm) {}

    // ====================================================================
    // TryEnqueue — 数据入队（生产端）
    // ====================================================================
    // 将 DataBatchPtr 推入无锁队列。如果队列满，根据丢包策略决定：
    //   - kDropNewest: 直接丢弃新数据，返回 false
    //   - kDropOldest: 先弹出最旧数据腾出空间，再推入新数据
    //
    // 成功入队后更新 enqueued 计数并检查反压状态。
    // 失败时更新 dropped 计数。
    //
    // 返回: true = 成功入队，false = 数据被丢弃
    bool TryEnqueue(DataBatchPtr batch) {
        ChannelItem item(std::move(batch));
        if (queue_.TryPush(std::move(item))) {
            stats_.enqueued.fetch_add(1, std::memory_order_relaxed);
            UpdateBackpressure();  // 队列变满 → 可能触发反压
            return true;
        }

        // 队列满 + kDropOldest 策略：弹出最旧数据，为新数据腾空间
        if (drop_policy_ == DropPolicy::kDropOldest) {
            ChannelItem discarded;
            if (queue_.TryPop(discarded)) {
                stats_.dropped.fetch_add(1, std::memory_order_relaxed);
            }
            if (queue_.TryPush(std::move(item))) {
                stats_.enqueued.fetch_add(1, std::memory_order_relaxed);
                UpdateBackpressure();
                return true;
            }
        }

        // 队列满 + kDropNewest 策略（或 kDropOldest 但腾空间失败）
        stats_.dropped.fetch_add(1, std::memory_order_relaxed);
        UpdateBackpressure();
        return false;
    }

    // ====================================================================
    // InjectFlush — 注入 FlushSentinel（TimerWheel 回调专用）
    // ====================================================================
    // 将 FlushSentinel 注入队列。与 TryEnqueue 不同：
    //   1. Sentinel 不受丢包策略影响 — 这是控制信号，不是数据
    //   2. 队列满时，驱逐一个旧数据（可能是 DataBatch 或 另一个 Sentinel）
    //      为 Sentinel 腾出空间，保证 flush 信号一定能送达
    //   3. 两次尝试入队：第一次直接推，失败则弹出旧数据再推
    //
    // 这种优先级设计的原因：如果 flush 信号被丢弃，Aggregator 中的
    // 数据可能永远不会被刷出，导致数据积压和内存泄漏。
    //
    // 返回: true = 成功注入，false = 极端情况失败（几乎不可能）

    // TODO: 目前这种设计在极端条件下可能导致Sentinel丢失，这对业务逻辑有影响，但是目前没有更好的方案，后续考虑优化
    bool InjectFlush() {
        ChannelItem item(FlushSentinel{});
        // 第一次尝试：直接推入
        if (queue_.TryPush(std::move(item))) {
            stats_.flush_injected.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        // 队列满：弹出最旧数据腾出空间
        ChannelItem discarded;
        if (queue_.TryPop(discarded)) {
            stats_.dropped.fetch_add(1, std::memory_order_relaxed);
        }
        // 第二次尝试：应该能成功了
        ChannelItem retry(FlushSentinel{});
        if (queue_.TryPush(std::move(retry))) {
            stats_.flush_injected.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        return false;  // 极端情况：两次尝试都失败
    }

    // ====================================================================
    // Dequeue — 阻塞出队（消费端，三级自适应退避）
    // ====================================================================
    // 从队列中取出一条消息。使用三级自适应退避策略，在延迟和 CPU 开销之间
    // 取得平衡：
    //
    // Phase 1 — Spin（16 次忙等，快速路径）
    //   连续 16 次尝试 TryPop，不阻塞、不睡眠。
    //   适合高频数据场景：如果队列中持续有数据，Phase 1 就能命中，
    //   延迟仅为 ~几十纳秒（TryPop 的 CAS 操作开销）。
    //   16 次是一个经验值，足够覆盖大多数高频场景的间隔。
    //
    // Phase 2 — Yield（8 次让出 CPU）
    //   16 次忙等失败后，说明数据到达频率较低或队列为空。
    //   调用 std::this_thread::yield() 让出 CPU 时间片给其他线程。
    //   如果数据在 yield 期间到达，可以快速返回，延迟 ~微秒级。
    //   8 次 yield 后仍未拿到数据，说明数据确实稀疏。
    //
    // Phase 3 — Sleep（1ms 间隔，超时退出）
    //   数据稀疏或队列为空，进入睡眠模式。
    //   每次 sleep 1ms，期间检查 TryPop，直到超时。
    //   CPU 占用趋近于零，适合低频场景。
    //
    // 返回: std::optional<ChannelItem> — 有数据时返回 item，超时时返回 nullopt
    std::optional<ChannelItem> Dequeue(std::chrono::milliseconds timeout) {
        ChannelItem result;

        // Phase 1: spin（快路径，低延迟场景）
        for (int i = 0; i < 16; ++i) {
            if (queue_.TryPop(result)) {
                stats_.dequeued.fetch_add(1, std::memory_order_relaxed);
                UpdateBackpressure();  // 队列变空 → 可能解除反压
                return result;
            }
        }

        // Phase 2: yield（让出 CPU 时间片）
        for (int i = 0; i < 8; ++i) {
            std::this_thread::yield();
            if (queue_.TryPop(result)) {
                stats_.dequeued.fetch_add(1, std::memory_order_relaxed);
                UpdateBackpressure();
                return result;
            }
        }

        // Phase 3: sleep（低频场景，节省 CPU）
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (queue_.TryPop(result)) {
                stats_.dequeued.fetch_add(1, std::memory_order_relaxed);
                UpdateBackpressure();
                return result;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        return std::nullopt;  // 超时，没有数据
    }

    // ---- 非阻塞出队（用于优雅停机的 Drain 流程） ----
    // 与 Dequeue 不同，TryDequeue 不等待、不阻塞，立即返回。
    // 在 ProcessThread 的 Drain() 阶段使用，快速排空 channel 中的剩余数据。
    std::optional<ChannelItem> TryDequeue() {
        ChannelItem result;
        if (queue_.TryPop(result)) {
            stats_.dequeued.fetch_add(1, std::memory_order_relaxed);
            UpdateBackpressure();
            return result;
        }
        return std::nullopt;
    }

    // ---- 反压状态查询 ----
    // 使用 acquire 语义读取，确保能看到其他线程对 backpressured_ 的最新修改。
    // Pipeline::Enqueue() 中检查此状态，触发 OnBackpressure() 通知 Source。
    bool IsBackpressured() const {
        return backpressured_.load(std::memory_order_acquire);
    }

    // ---- 容量查询 ----
    size_t SizeApprox() const { return queue_.SizeApprox(); }
    size_t capacity() const { return queue_.capacity(); }

    const Stats& stats() const { return stats_; }

private:
    // ====================================================================
    // UpdateBackpressure — 更新反压状态（滞后设计）
    // ====================================================================
    // 使用"滞后"（hysteresis）设计避免在阈值附近反复震荡：
    //   当前不在反压状态 → 队列超过 high watermark → 进入反压
    //   当前在反压状态   → 队列低于 low watermark  → 退出反压
    //
    // 为什么需要滞后？
    //   如果只有单一阈值（如 80%），当队列在 79%-81% 之间波动时，
    //   反压状态会反复切换，导致 Source 频繁加速/减速，引起系统抖动。
    //   滞后设计（high=80%, low=20%）使得状态切换需要"决定性"的队列变化。
    //
    // compare_exchange_strong 保证只有一个线程能成功改变状态，
    // 避免多个生产者同时触发反压导致重复计数。
    void UpdateBackpressure() {
        bool was = backpressured_.load(std::memory_order_relaxed);
        if (!was && queue_.AboveHighWatermark(high_wm_)) {
            // 从"正常"切换到"反压"：使用 CAS 保证只有一个线程成功
            if (backpressured_.compare_exchange_strong(was, true,
                    std::memory_order_release)) {
                stats_.backpressure_events.fetch_add(1, std::memory_order_relaxed);
            }
        } else if (was && queue_.BelowLowWatermark(low_wm_)) {
            // 从"反压"切换到"正常"：直接 store 即可（消费者是单线程）
            backpressured_.store(false, std::memory_order_release);
        }
    }

    // ---- 底层数据结构 ----
    LockFreeQueue<ChannelItem> queue_;       // 无锁有界环形缓冲区
    Stats stats_;                             // 统计信息（全 atomic）
    std::atomic<bool> backpressured_{false};  // 反压状态标志
    DropPolicy drop_policy_;                  // 丢包策略
    double high_wm_;                          // 高水位线比例（触发反压）
    double low_wm_;                           // 低水位线比例（解除反压）
};

}  // namespace illuminator