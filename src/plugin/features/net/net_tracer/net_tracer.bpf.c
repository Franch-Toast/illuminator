#include "../include/common.bpf.h"

// ============================================================================
// 文件：illuminator/src/ebpf_common/probes/net_tracer.bpf.c
// ============================================================================
//
// 【作用】
// 网络连接追踪 eBPF 探针。通过 inet_sock_set_state tracepoint 监控
// TCP 连接的状态变迁（ESTABLISHED → 连接建立, CLOSE → 连接断开）。
// 用于网络拓扑可视化、连接异常检测和流量分析。
//
// 【工作原理】
// 1. inet_sock_set_state tracepoint：
//    在内核 IP 层套接字状态变化时触发，携带完整的四元组信息
//    （src IP, dst IP, src port, dst port）和协议类型。
//
// 2. 状态过滤：
//    newstate == TCP_ESTABLISHED(1) → event_type=0，连接建立
//    newstate == TCP_CLOSE(7)       → event_type=2，连接关闭
//    其他中间状态（SYN_SENT, SYN_RECV, FIN_WAIT1 等）被丢弃。
//
// 3. IPv4 限定：
//    当前仅支持 AF_INET（IPv4），AF_INET6（IPv6）被过滤。
//    IPv6 支持需要在 il_net_event 中增加 128 位地址字段。
//
// 4. 字节统计预留：
//    il_net_event 中的 bytes_sent/bytes_recv 字段在此探针中
//    始终为 0。真实的流量字节统计需要在更细粒度的 tracepoint
//    （如 tcp_retransmit_skb）上实现，属于 Phase 3 规划。
//
// 5. 地址拷贝：
//    通过 __builtin_memcpy 从 tracepoint 上下文中拷贝 IP 地址。
//    使用 memcpy 而非指针复制是因为 ctx->saddr 在 ctx 中是数组字段，
//    但 BPF verifier 需要通过 size 参数确定拷贝的安全性。

// ============================================================================
// BPF Maps 定义
// ============================================================================

// net_events：网络事件 ring buffer
// 每个事件约 72 字节，256KB 可缓冲约 3600 个事件
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, IL_RINGBUF_SIZE);
} net_events SEC(".maps");

DECLARE_COLLECTION_GATE();
DECLARE_META_STATS();

// ============================================================================
// trace_inet_sock_set_state：TCP 连接状态变迁处理函数
//
// 功能：监控 TCP 套接字状态变化，记录连接建立和关闭事件。
//
// 参数：ctx - inet_sock_set_state tracepoint 上下文
//       ctx->family     = 地址族（AF_INET=2, AF_INET6=10）
//       ctx->protocol   = 传输层协议号（TCP=6, UDP=17）
//       ctx->saddr/daddr = 源/目标 IPv4 地址（32 位）
//       ctx->sport/dport = 源/目标端口号（主机字节序）
//       ctx->oldstate/newstate = TCP 状态变迁前后的状态值
//
// 处理流程：
// 1. 仅处理 IPv4（family==2），忽略 IPv6
// 2. 仅处理 TCP_ESTABLISHED 和 TCP_CLOSE 状态
// 3. 构造 il_net_event，填充四元组和进程信息
// 4. 通过 ring buffer 推送事件
//
// TCP 状态常量（来自内核 include/net/tcp_states.h）：
//   1=TCP_ESTABLISHED, 7=TCP_CLOSE
// ============================================================================
SEC("tracepoint/sock/inet_sock_set_state")
int trace_inet_sock_set_state(struct trace_event_raw_inet_sock_set_state *ctx) {
    CHECK_GATE();
    __u16 family = ctx->family;
    if (family != 2 /* AF_INET */) return 0;

    // 预留 ring buffer 空间
    INC_STAT(STAT_TOTAL_EVENTS);
    struct il_net_event *e = bpf_ringbuf_reserve(&net_events, sizeof(*e), 0);
    if (!e) {
        INC_STAT(STAT_BUFFER_FULL);
        return 0;
    }

    // ---------------------------------------------------------------
    // 填充进程和网络标识信息
    // ---------------------------------------------------------------
    __u64 pid_tgid = bpf_get_current_pid_tgid();
    e->timestamp_ns = bpf_ktime_get_ns();
    e->pid = pid_tgid >> 32;     // 进程 ID（tgid）
    e->tid = (__u32)pid_tgid;    // 线程 ID（pid）
    e->protocol = ctx->protocol; // 协议类型（6=TCP）
    e->sport = ctx->sport;       // 源端口（主机字节序）
    e->dport = ctx->dport;       // 目标端口（主机字节序）
    bpf_get_current_comm(&e->comm, sizeof(e->comm));

    // ---------------------------------------------------------------
    // 拷贝 IPv4 地址（4 字节 = 32 位）
    // 使用 __builtin_memcpy 是 BPF 编译器推荐的安全拷贝方式
    // BPF verifier 会检查 memcpy 的 size 参数是否在合理范围内
    // ---------------------------------------------------------------
    __builtin_memcpy(&e->saddr, ctx->saddr, 4);
    __builtin_memcpy(&e->daddr, ctx->daddr, 4);

    // ---------------------------------------------------------------
    // 状态判断和事件类型分类
    // ---------------------------------------------------------------
    int newstate = ctx->newstate;
    int oldstate = ctx->oldstate;

    // TCP_ESTABLISHED(1)：三次握手完成，连接建立
    if (newstate == 1 /* TCP_ESTABLISHED */) {
        e->event_type = 0;  // 连接建立/接受
    }
    // TCP_CLOSE(7)：连接关闭（主动关闭 FIN 或被动 RST）
    else if (newstate == 7 /* TCP_CLOSE */) {
        e->event_type = 2;  // 连接关闭
    }
    // 其他中间状态（SYN_SENT, SYN_RECV, FIN_WAIT 等）→ 丢弃
    else {
        INC_STAT(STAT_DROPPED);
        bpf_ringbuf_discard(e, 0);
        return 0;
    }

    // ---------------------------------------------------------------
    // 字节统计（当前阶段为预留字段，始终为 0）
    // 后续 Phase 3 将通过 tcp_probe tracepoint 或 TC hook 实现
    // ---------------------------------------------------------------
    e->bytes_sent = 0;
    e->bytes_recv = 0;

    // 提交事件：将此连接状态变化推送给用户态分析引擎
    bpf_ringbuf_submit(e, 0);
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
