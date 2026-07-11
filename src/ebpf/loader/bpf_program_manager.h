#pragma once

// ============================================================================
// 文件：illuminator/src/ebpf/loader/bpf_program_manager.h
// ============================================================================
//
// 【作用】
// BPF 程序生命周期管理器。负责 eBPF 程序的打开（open）、加载（load）、
// 附加（attach）和分离（detach）全生命周期管理。对外提供统一的接口，
// 隔离底层 libbpf 库的复杂 API，让上层 pipeline 只需调用简洁的方法。
//
// 【工作原理】
// 1. 对象管理（LoadObject）：
//    打开并加载一个 BPF 对象文件（*.bpf.o），保存到 objects_ 映射表中。
//    底层调用 bpf_object__open() 解析 ELF 格式的 BPF 目标文件，
//    然后调用 bpf_object__load() 将 BPF 程序加载到内核。
//
// 2. 程序附加（AttachProgram）：
//    通过程序名找到已加载的 BPF 程序，调用 bpf_program__attach()
//    将其附加到对应的内核钩子点（tracepoint/kprobe/uprobe 等）。
//    返回的 bpf_link 指针保存到 links_ 向量中，用于分离管理。
//
// 3. 资源查询（GetMapFd/GetProgFd）：
//    提供按对象名和程序名查找 BPF map 文件描述符和程序文件描述符的能力。
//    这些 fd 用于用户态与 BPF 程序交互（读写 map、配置 ring buffer）。
//
// 4. Ring Buffer 创建（CreateRingBuffer）：
//    封装 ring_buffer__new() 调用，为用户态创建一个可以从 BPF ring buffer
//    map 中异步消费事件的 ring buffer 实例。
//
// 5. 特性探测：
//    构造时自动调用 ProbeKernelFeatures() 获取内核特性信息，
//    上层可通过 Features() 方法查询（如是否支持 BTF、Ring Buffer）。
//
// 【设计约束】
// - 使用 RAII：析构函数自动调用 DetachAll() 并关闭所有 BPF 对象
// - 处理失败情况：每个可能失败的操作都返回 Status，上层可判断成功与否
// - 支持批量附加：AttachPrograms() 接收名称列表，逐个附加并记录警告
// ============================================================================

// 前向声明旧内核可能未定义的 BPF 链接类型枚举
// 这确保了在旧内核头文件环境下编译不受影响
#include <linux/bpf.h>
#include "ebpf/include/bpf_compat.h"

#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/common/logging.h"
#include "core/common/status.h"
#include "ebpf/loader/feature_probe.h"  // 内核特性探测

namespace illuminator {

// ============================================================================
// BpfProgramManager：BPF 程序加载管理器
//
// 统一的 eBPF 程序生命周期管理入口。封装 libbpf 底层 API，
// 提供面向对象的 BPF 程序加载、附加、查询和销毁接口。
// ============================================================================
class BpfProgramManager {
public:
    // ------------------------------------------------------------------------
    // 构造函数：自动执行内核特性探测
    // 在对象创建时一次性探测内核的 BTF/ring buffer/BPF tracing 等特性，
    // 结果保存在 features_ 成员中，后续可通过 Features() 查询。
    // ------------------------------------------------------------------------
    BpfProgramManager() {
        features_ = ProbeKernelFeatures();
    }

    // ------------------------------------------------------------------------
    // 析构函数：RAII 自动资源清理
    // 1. DetachAll() 销毁所有 bpf_link，将程序从内核钩子点分离
    // 2. 遍历并调用 bpf_object__close() 卸载所有 BPF 对象，释放内核资源
    // ------------------------------------------------------------------------
    ~BpfProgramManager() {
        DetachAll();
        for (auto& [name, obj] : objects_) {
            if (obj && skeleton_owned_.find(name) == skeleton_owned_.end())
                bpf_object__close(obj);
        }
    }

    // ------------------------------------------------------------------------
    // Features：返回探测到的内核特性信息
    // 调用方可通过返回值判断：是否支持 BTF（CO-RE 兼容性）、ring buffer、
    // BPF tracing 等。用于在运行时决定启用/禁用某些功能。
    // ------------------------------------------------------------------------
    const KernelFeatures& Features() const { return features_; }

    // ------------------------------------------------------------------------
    // LoadObject：加载 BPF 目标文件
    //
    // 参数：
    //   name - 程序逻辑名称（作为对象的标识键，用于后续查找）
    //   path - BPF .o 文件的文件系统路径
    //
    // 流程：
    //   1. bpf_object__open(path)  → 解析 ELF 文件，读取 maps/programs 元数据
    //   2. bpf_object__load(obj)   → 加载到内核，创建 maps，验证 BPF 程序
    //   3. 保存到 objects_[name]   → 建立名称到对象的映射
    //
    // 返回：
    //   Status::Ok()               → 成功
    //   Status::Error(kInternal)   → 打开或加载失败，附带错误描述
    // ------------------------------------------------------------------------
    Status LoadObject(const std::string& name, const std::string& path) {
        struct bpf_object *obj = bpf_object__open(path.c_str());
        if (!obj) {
            return Status::Error(StatusCode::kInternal,
                "Failed to open BPF object: " + path);
        }

        int err = bpf_object__load(obj);
        if (err) {
            bpf_object__close(obj);
            return Status::Error(StatusCode::kInternal,
                "Failed to load BPF object: " + path +
                " (err=" + std::to_string(err) + ")");
        }

        objects_[name] = obj;
        IL_INFO("Loaded BPF object: {} from {}", name, path);
        return Status::Ok();
    }

    // ------------------------------------------------------------------------
    // RegisterSkeletonObject：注册由 skeleton 管理的 BPF 对象
    //
    // 将 skeleton 的 bpf_object* 注册到管理器，使 GetMapFd/GetProgFd/
    // AttachProgram 等方法可以正常使用。对象生命周期由 skeleton 管理（
    // 通过 xxx_bpf__destroy），管理器析构时不会 close 这些对象。
    //
    // 参数：
    //   name - 逻辑对象名（与 GetMapFd 等调用时使用的名称一致）
    //   obj  - skeleton 的 bpf_object 指针（skel->obj）
    // ------------------------------------------------------------------------
    void RegisterSkeletonObject(const std::string& name, struct bpf_object* obj) {
        objects_[name] = obj;
        skeleton_owned_.insert(name);
        IL_INFO("Registered skeleton BPF object: {}", name);
    }

    // ------------------------------------------------------------------------
    // GetMapFd：获取 BPF map 的文件描述符
    //
    // 参数：
    //   obj_name - BPF 对象名称（即 LoadObject 时传入的 name）
    //   map_name - BPF map 名称（在 BPF C 代码中 .maps section 定义的名称）
    //
    // 返回值：
    //   有效的 fd（>=0）  → 找到 map，可用于 ring_buffer__new 或 map 操作
    //   -1               → 对象不存在或 map 不存在
    //
    // 用途：用户态需要通过 fd 操作 BPF maps（读取聚合统计、配置参数等）
    // ------------------------------------------------------------------------
    int GetMapFd(const std::string& obj_name, const std::string& map_name) {
        auto it = objects_.find(obj_name);
        if (it == objects_.end()) return -1;
        struct bpf_map *map = bpf_object__find_map_by_name(it->second, map_name.c_str());
        if (!map) return -1;
        return bpf_map__fd(map);
    }

    // ------------------------------------------------------------------------
    // GetProgFd：获取 BPF 程序的 fd
    //
    // 参数：
    //   obj_name  - BPF 对象名称
    //   prog_name - BPF 程序名称（在 BPF C 代码中 SEC 定义的函数名）
    //
    // 返回值：
    //   有效的 fd（>=0）  → 找到程序
    //   -1               → 对象不存在或程序不存在
    //
    // 用途：通过 prog fd 可执行 BPF_PROG_TEST_RUN 等高级操作
    // ------------------------------------------------------------------------
    int GetProgFd(const std::string& obj_name, const std::string& prog_name) {
        auto it = objects_.find(obj_name);
        if (it == objects_.end()) return -1;
        struct bpf_program *prog = bpf_object__find_program_by_name(
            it->second, prog_name.c_str());
        if (!prog) return -1;
        return bpf_program__fd(prog);
    }

    // ------------------------------------------------------------------------
    // AttachProgram：将单个 BPF 程序附加到内核钩子点
    //
    // 参数：
    //   obj_name  - BPF 对象名称
    //   prog_name - BPF 程序名称（函数名）
    //
    // 流程：
    //   1. 在 objects_ 中查找对象
    //   2. 在对象中按名称查找程序
    //   3. bpf_program__attach(prog) → 自动根据程序类型附加到对应钩子
    //      （perf_event→CPU perf, tracepoint→tracepoint, uprobe→uprobe 等）
    //   4. 将返回的 bpf_link 存入 links_ 向量
    //
    // 错误码：
    //   kNotFound → 对象或程序名称不匹配
    //   kInternal → 附加失败（权限不足或钩子不可用）
    // ------------------------------------------------------------------------
    Status AttachProgram(const std::string& obj_name, const std::string& prog_name) {
        auto it = objects_.find(obj_name);
        if (it == objects_.end()) {
            return Status::Error(StatusCode::kNotFound,
                "BPF object not found: " + obj_name);
        }

        struct bpf_program *prog = bpf_object__find_program_by_name(
            it->second, prog_name.c_str());
        if (!prog) {
            return Status::Error(StatusCode::kNotFound,
                "BPF program not found: " + prog_name);
        }

        struct bpf_link *link = bpf_program__attach(prog);
        if (!link) {
            return Status::Error(StatusCode::kInternal,
                "Failed to attach: " + prog_name);
        }

        links_.push_back(link);
        IL_INFO("Attached BPF program: {}", prog_name);
        return Status::Ok();
    }

    // ------------------------------------------------------------------------
    // AttachPrograms：批量附加多个程序
    //
    // 参数：
    //   obj_name   - BPF 对象名称
    //   prog_names - 要附加的程序名称列表
    //
    // 行为：逐个调用 AttachProgram()。单个程序附加失败只记录警告（IL_WARN），
    // 不会中断后续程序的附加。这是因为部分探针可能依赖可选的 tracepoint。
    // 若列表非空且全部附加失败，返回 kInternal，否则返回 Ok（含部分成功）。
    // ------------------------------------------------------------------------
    Status AttachPrograms(const std::string& obj_name,
                          const std::vector<std::string>& prog_names) {
        int failures = 0;
        for (auto& name : prog_names) {
            auto status = AttachProgram(obj_name, name);
            if (!status.ok()) {
                IL_WARN("Failed to attach {}: {}", name,
                        status.message());
                ++failures;
            }
        }
        if (!prog_names.empty() &&
            failures == static_cast<int>(prog_names.size())) {
            return Status::Error(StatusCode::kInternal,
                "Failed to attach all " + std::to_string(failures) +
                " BPF program(s) for object: " + obj_name);
        }
        return Status::Ok();
    }

    // ------------------------------------------------------------------------
    // DetachAll：分离所有已附加的 BPF 程序
    //
    // 操作：遍历 links_ 向量，对每个 bpf_link 调用 bpf_link__destroy()。
    // 销毁 link 后 BPF 程序将不再被触发，但 BPF 对象仍然在内核中（maps 可用）。
    // 调用完成后清空 links_。
    // ------------------------------------------------------------------------
    void DetachAll() {
        for (auto* link : links_) {
            if (link) bpf_link__destroy(link);
        }
        links_.clear();
    }

    // ------------------------------------------------------------------------
    // CreateRingBuffer：为指定的 BPF ring buffer map 创建用户态 ring buffer
    //
    // 参数：
    //   map_fd   - ring buffer 类型 BPF map 的 fd（通过 GetMapFd 获取）
    //   callback - 事件回调函数：每当 ring buffer 有新事件时被调用
    //   ctx      - 回调函数的上下文指针（透传给 callback）
    //
    // 返回值：绑定了 map_fd 的 ring_buffer 指针，用于 polling 消费事件。
    //
    // 原理：ring buffer 是 BPF 程序和用户态之间的无锁共享内存环形队列。
    // BPF 侧通过 bpf_ringbuf_reserve/submit 写入事件，
    // 用户态此组件通过 epoll 机制异步读取。
    // ------------------------------------------------------------------------
    struct ring_buffer* CreateRingBuffer(
        int map_fd,
        ring_buffer_sample_fn callback,
        void* ctx) {
        return ring_buffer__new(map_fd, callback, ctx, nullptr);
    }

private:
    KernelFeatures features_;
    std::unordered_map<std::string, struct bpf_object*> objects_;
    std::unordered_set<std::string> skeleton_owned_;  // skeleton 管理的对象名，析构时跳过
    std::vector<struct bpf_link*> links_;
};

}  // namespace illuminator
