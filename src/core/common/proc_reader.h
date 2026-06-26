// ============================================================================
// proc_reader — /proc 文件系统解析工具库
// ============================================================================
//
// 【定位】
//   Illuminator 的底层数据采集基础库。所有 CPU、内存、进程相关的 Source
//   都依赖本文件的函数从 /proc 文件系统读取原始数据。
//
// 【文件结构】
//   1. 数据结构：CoreJiffies, CpuSnapshot, LoadAvg, ProcessStat
//   2. CPU 相关：ReadCpuSnapshot(), ReadTotalCpuJiffies(), CountOnlineCpus()
//   3. 系统负载：ReadLoadAvg()
//   4. CPU 频率：ReadCpuFrequenciesMhz()
//   5. 进程相关：ParseProcStat(), ReadCtxtSwitches(), ScanProcesses()
//
// 【设计原则】
//   - 全部为 inline 函数，零额外依赖，可直接 include 使用
//   - 文件 I/O 失败时静默返回空/零值，不抛异常（监控场景下容错优先）
//   - 所有函数在 proc 命名空间下，避免与其他模块冲突
//
// 【为什么用 /proc 而不是 eBPF？】
//   /proc 是 Linux 内核的标准伪文件系统，无需额外权限，兼容性最好。
//   对于定时轮询的指标采集（1s 间隔），/proc 的开销完全可以接受。
//   eBPF 仅在需要事件驱动（如 IO 延迟追踪）或高精度采样时才使用。
// ============================================================================

#pragma once

#include <cstdint>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace illuminator::proc {

// ============================================================================
// CoreJiffies — 单个 CPU 核心的时间统计（jiffies 单位）
// ============================================================================
//
// 对应 /proc/stat 中每行 "cpu<N> <user> <nice> <system> ..." 的字段。
// 所有值都是自系统启动以来的累计值（monotonic counter），
// 需要通过前后两次快照的差值来计算利用率百分比。
//
// 字段说明：
//   user:   用户态 CPU 时间（含 nice 优先级调整后的时间）
//   nice:   nice 值大于 0 的用户态 CPU 时间（低优先级进程）
//   system: 内核态 CPU 时间
//   idle:   空闲时间
//   iowait: 等待 I/O 完成的时间（CPU 空闲但有未完成的磁盘 I/O）
//   irq:    处理硬中断的时间
//   softirq: 处理软中断的时间
//   steal:  虚拟化环境中被 hypervisor"偷走"的时间
//
// 辅助方法：
//   Total(): 所有时间的总和（用于计算时间差 dt）
//   Busy():  实际工作时间 = Total - idle - iowait
struct CoreJiffies {
    std::string name;
    uint64_t user = 0, nice = 0, system = 0, idle = 0;
    uint64_t iowait = 0, irq = 0, softirq = 0, steal = 0;

    uint64_t Total() const {
        return user + nice + system + idle + iowait + irq + softirq + steal;
    }
    uint64_t Busy() const { return Total() - idle - iowait; }
};

// ============================================================================
// CpuSnapshot — 系统级 CPU 快照
// ============================================================================
//
// 一次 ReadCpuSnapshot() 调用返回的完整 CPU 状态快照。
// 包含所有核心的时间统计 + 系统级计数器 + 运行队列状态。
//
// 字段说明：
//   cores:         所有 CPU 核心的时间统计（cores[0] 是 "cpu" 总计行）
//   ctxt:          系统累上下文切换次数
//   intr:          系统累计中断次数
//   procs_running: 当前正在运行的进程数（从 /proc/stat 的 procs_running 字段）
//   procs_blocked: 当前阻塞等待 I/O 的进程数（从 /proc/stat 的 procs_blocked 字段）
struct CpuSnapshot {
    std::vector<CoreJiffies> cores;
    uint64_t ctxt = 0;
    uint64_t intr = 0;
    uint32_t procs_running = 0;
    uint32_t procs_blocked = 0;
};

// ============================================================================
// LoadAvg — 系统负载均值
// ============================================================================
//
// 对应 /proc/loadavg 的内容。load1/5/15 分别表示过去 1/5/15 分钟的平均负载。
// 负载值大致等于"可运行 + 不可中断睡眠"的进程数。
// 值 > CPU 核心数时表示系统过载。
struct LoadAvg {
    double load1 = 0, load5 = 0, load15 = 0;
};

// ============================================================================
// ProcessStat — 进程状态快照
// ============================================================================
//
// 从 /proc/[pid]/stat 和 /proc/[pid]/status 中提取的进程指标。
// 用于 cpu_processes 和进程详情页面的数据源。
//
// 字段说明：
//   pid:          进程 ID
//   comm:         进程名（括号内的内容，最多 15 字符）
//   state:        进程状态（R=运行, S=睡眠, D=不可中断睡眠, Z=僵尸, T=停止）
//   utime:        用户态 CPU 时间（jiffies）
//   stime:        内核态 CPU 时间（jiffies）
//   starttime:    进程启动时间（jiffies，从系统启动算起）
//   num_threads:  线程数
//   vsize:        虚拟内存大小（字节）
//   rss_pages:    常驻内存页数（每页通常 4KB）
//   voluntary_ctxt_switches:      主动上下文切换次数
//   nonvoluntary_ctxt_switches:   被动上下文切换次数（时间片用完）
struct ProcessStat {
    uint32_t pid = 0;
    std::string comm;
    char state = '?';
    uint64_t utime = 0, stime = 0, starttime = 0;
    uint32_t num_threads = 0;
    uint64_t vsize = 0;
    int64_t rss_pages = 0;
    uint64_t voluntary_ctxt_switches = 0;
    uint64_t nonvoluntary_ctxt_switches = 0;
};

// ============================================================================
// ReadCpuSnapshot — 读取系统 CPU 快照
// ============================================================================
//
// 解析 /proc/stat 文件，提取所有 CPU 核心的时间统计 + 系统级计数器。
//
// /proc/stat 格式示例：
//   cpu  123456 7890 234567 8901234 5678 123 45 0 0 0
//   cpu0 12345 678 23456 890123 567 12 4 0 0 0
//   cpu1 12345 678 23456 890123 567 12 4 0 0 0
//   ctxt 987654321
//   intr 123456789
//   procs_running 3
//   procs_blocked 0
//
// 返回值：CpuSnapshot（文件打开失败时返回空快照，所有字段为 0）
inline CpuSnapshot ReadCpuSnapshot() {
    CpuSnapshot snap;
    std::ifstream file("/proc/stat");
    if (!file.is_open()) return snap;

    std::string line;
    while (std::getline(file, line)) {
        // 匹配 "cpu" 或 "cpu0", "cpu1", ... 行，跳过 "cpu " 行之外的其他前缀
        if (line.compare(0, 3, "cpu") == 0 &&
            (line[3] == ' ' || (line[3] >= '0' && line[3] <= '9'))) {
            CoreJiffies j;
            std::istringstream iss(line);
            iss >> j.name >> j.user >> j.nice >> j.system >> j.idle
                >> j.iowait >> j.irq >> j.softirq >> j.steal;
            snap.cores.push_back(j);
        } else if (line.compare(0, 5, "ctxt ") == 0) {
            // 上下文切换累计次数
            std::istringstream iss(line.substr(5));
            iss >> snap.ctxt;
        } else if (line.compare(0, 5, "intr ") == 0) {
            // 中断累计次数（行首是 intr，后面第一个数字是总中断数）
            std::istringstream iss(line.substr(5));
            iss >> snap.intr;
        } else if (line.compare(0, 14, "procs_running ") == 0) {
            std::istringstream iss(line.substr(14));
            iss >> snap.procs_running;
        } else if (line.compare(0, 14, "procs_blocked ") == 0) {
            std::istringstream iss(line.substr(14));
            iss >> snap.procs_blocked;
        }
    }
    return snap;
}

// ============================================================================
// ReadTotalCpuJiffies — 读取 CPU 总计 jiffies
// ============================================================================
//
// 只解析 /proc/stat 的第一行（"cpu " 总计行），将所有字段求和返回。
// 用于需要快速获取 CPU 总时间的场景（如 /healthz 健康检查）。
//
// 返回值：CPU 总计 jiffies（文件打开失败返回 0）
inline uint64_t ReadTotalCpuJiffies() {
    std::ifstream file("/proc/stat");
    if (!file.is_open()) return 0;
    std::string line;
    if (!std::getline(file, line)) return 0;
    if (line.compare(0, 4, "cpu ") != 0) return 0;
    std::istringstream iss(line);
    std::string tag;
    iss >> tag;  // 跳过 "cpu" 标签
    uint64_t total = 0, v;
    while (iss >> v) total += v;
    return total;
}

// ============================================================================
// CountOnlineCpus — 统计在线 CPU 核心数
// ============================================================================
//
// 通过解析 /proc/stat 中 "cpu0", "cpu1", ... 行数来统计在线 CPU 核心数。
// 注意：这统计的是 "cpu<N>" 的行数，不包括 "cpu " 总计行。
//
// 返回值：在线 CPU 核心数（至少返回 1，防止除零错误）
inline int CountOnlineCpus() {
    std::ifstream file("/proc/stat");
    if (!file.is_open()) return 1;
    int count = 0;
    std::string line;
    while (std::getline(file, line)) {
        if (line.compare(0, 3, "cpu") == 0 && line.size() > 3 &&
            line[3] >= '0' && line[3] <= '9')
            ++count;
    }
    return count > 0 ? count : 1;
}

// ============================================================================
// ReadLoadAvg — 读取系统负载均值
// ============================================================================
//
// 解析 /proc/loadavg 文件：
//   <load_1m> <load_5m> <load_15m> <running>/<total> <last_pid>
//
// 我们只取前三个负载值，忽略后面的字段。
//
// 返回值：LoadAvg（文件打开失败时返回全零）
inline LoadAvg ReadLoadAvg() {
    LoadAvg la;
    std::ifstream file("/proc/loadavg");
    if (file.is_open())
        file >> la.load1 >> la.load5 >> la.load15;
    return la;
}

// ============================================================================
// ReadCpuFrequenciesMhz — 读取各核心 CPU 当前频率
// ============================================================================
//
// 遍历 /sys/devices/system/cpu/cpu<N>/cpufreq/scaling_cur_freq，
// 读取每个核心的当前运行频率（kHz），转换为 MHz 后返回。
//
// 注意：
//   - 需要 root 权限或合适的 sysfs 权限
//   - 无权限时文件打开失败，返回空 vector
//   - 从 cpu0 开始尝试，直到某个核心的路径不存在为止
//
// 返回值：各核心的 CPU 频率列表（MHz），单位已从 kHz 转换
inline std::vector<double> ReadCpuFrequenciesMhz() {
    std::vector<double> freqs;
    int idx = 0;
    while (true) {
        std::string path = "/sys/devices/system/cpu/cpu" +
                           std::to_string(idx) + "/cpufreq/scaling_cur_freq";
        std::ifstream file(path);
        if (!file.is_open()) break;
        uint64_t khz;
        file >> khz;
        freqs.push_back(static_cast<double>(khz) / 1000.0);
        ++idx;
    }
    return freqs;
}

// ============================================================================
// ParseProcStat — 解析 /proc/[pid]/stat 文件
// ============================================================================
//
// /proc/[pid]/stat 格式特殊：进程名在括号内，可能包含空格和特殊字符。
// 因此不能简单用 istringstream 按空格分割，需要先定位括号边界。
//
// 解析策略：
//   1. 读整个文件内容到字符串
//   2. 找到第一个 '(' 和最后一个 ')' 的位置
//   3. 括号内的内容 = 进程名（comm）
//   4. 括号后的内容用 istringstream 按字段顺序解析
//
// 参数：
//   path: /proc/[pid]/stat 的完整路径
//   out:  输出参数，填充解析后的 ProcessStat
//
// 返回值：true 表示解析成功，false 表示文件不存在或格式异常
inline bool ParseProcStat(const std::string& path, ProcessStat& out) {
    std::ifstream file(path);
    if (!file.is_open()) return false;
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    // 定位括号：进程名在第一个 '(' 和最后一个 ')' 之间
    auto open = content.find('(');
    auto close = content.rfind(')');
    if (open == std::string::npos || close == std::string::npos) return false;
    out.comm = content.substr(open + 1, close - open - 1);
    // 括号后按空格分割，跳过前两个字段（state, ppid），
    // 直接读取我们需要的字段
    std::istringstream iss(content.substr(close + 2));
    std::string state_str;
    int64_t ppid, pgrp, session, tty_nr, tpgid;
    uint64_t flags, minflt, cminflt, majflt, cmajflt;
    uint64_t cutime, cstime;
    int64_t priority, nice_val, itrealvalue;
    iss >> state_str >> ppid >> pgrp >> session >> tty_nr >> tpgid
        >> flags >> minflt >> cminflt >> majflt >> cmajflt
        >> out.utime >> out.stime >> cutime >> cstime
        >> priority >> nice_val >> out.num_threads >> itrealvalue
        >> out.starttime >> out.vsize >> out.rss_pages;
    if (!state_str.empty()) out.state = state_str[0];
    return true;
}

// ============================================================================
// ReadCtxtSwitches — 读取进程的上下文切换统计
// ============================================================================
//
// 从 /proc/[pid]/status 中提取 voluntary_ctxt_switches 和
// nonvoluntary_ctxt_switches 字段。
//
// /proc/[pid]/status 格式示例：
//   ...
//   voluntary_ctxt_switches:	12345
//   nonvoluntary_ctxt_switches:	6789
//   ...
//
// 参数：
//   pid: 进程 ID
//   out: 输出参数，直接修改其 voluntary/nonvoluntary_ctxt_switches 字段
inline void ReadCtxtSwitches(uint32_t pid, ProcessStat& out) {
    std::ifstream file("/proc/" + std::to_string(pid) + "/status");
    if (!file.is_open()) return;
    std::string line;
    while (std::getline(file, line)) {
        if (line.compare(0, 25, "voluntary_ctxt_switches:\t") == 0) {
            std::istringstream iss(line.substr(25));
            iss >> out.voluntary_ctxt_switches;
        } else if (line.compare(0, 28, "nonvoluntary_ctxt_switches:\t") == 0) {
            std::istringstream iss(line.substr(28));
            iss >> out.nonvoluntary_ctxt_switches;
        }
    }
}

// ============================================================================
// ScanProcesses — 扫描所有进程
// ============================================================================
//
// 遍历 /proc 目录，对每个数字子目录（即进程 PID）调用 ParseProcStat()
// 解析 /proc/[pid]/stat，收集所有进程的状态快照。
//
// 实现细节：
//   1. 使用 opendir/readdir 遍历 /proc（比 std::filesystem 更轻量）
//   2. 只处理数字目录名（过滤掉 /proc/net、/proc/sys 等非进程目录）
//   3. 对每个进程调用 ParseProcStat()，解析失败则跳过（进程可能已退出）
//   4. 注意：此函数不包含上下文切换统计（需要额外调用 ReadCtxtSwitches）
//
// 返回值：所有有效进程的 ProcessStat 列表
inline std::vector<ProcessStat> ScanProcesses() {
    std::vector<ProcessStat> out;
    DIR* dir = opendir("/proc");
    if (!dir) return out;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        // 只处理目录（/proc/[pid] 是目录）
        if (entry->d_type != DT_DIR) continue;
        // 过滤非数字目录名（/proc/net、/proc/sys 等）
        char* endp;
        long pid = strtol(entry->d_name, &endp, 10);
        if (*endp != '\0' || pid <= 0) continue;
        ProcessStat snap;
        snap.pid = static_cast<uint32_t>(pid);
        std::string stat_path = "/proc/" + std::string(entry->d_name) + "/stat";
        if (!ParseProcStat(stat_path, snap)) continue;
        out.push_back(std::move(snap));
    }
    closedir(dir);
    return out;
}

}  // namespace illuminator::proc
