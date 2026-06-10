#pragma once

#include <cstdint>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace illuminator::proc {

struct CoreJiffies {
    std::string name;
    uint64_t user = 0, nice = 0, system = 0, idle = 0;
    uint64_t iowait = 0, irq = 0, softirq = 0, steal = 0;

    uint64_t Total() const {
        return user + nice + system + idle + iowait + irq + softirq + steal;
    }
    uint64_t Busy() const { return Total() - idle - iowait; }
};

struct CpuSnapshot {
    std::vector<CoreJiffies> cores;
    uint64_t ctxt = 0;
    uint64_t intr = 0;
    uint32_t procs_running = 0;
    uint32_t procs_blocked = 0;
};

struct LoadAvg {
    double load1 = 0, load5 = 0, load15 = 0;
};

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

inline CpuSnapshot ReadCpuSnapshot() {
    CpuSnapshot snap;
    std::ifstream file("/proc/stat");
    if (!file.is_open()) return snap;

    std::string line;
    while (std::getline(file, line)) {
        if (line.compare(0, 3, "cpu") == 0 &&
            (line[3] == ' ' || (line[3] >= '0' && line[3] <= '9'))) {
            CoreJiffies j;
            std::istringstream iss(line);
            iss >> j.name >> j.user >> j.nice >> j.system >> j.idle
                >> j.iowait >> j.irq >> j.softirq >> j.steal;
            snap.cores.push_back(j);
        } else if (line.compare(0, 5, "ctxt ") == 0) {
            std::istringstream iss(line.substr(5));
            iss >> snap.ctxt;
        } else if (line.compare(0, 5, "intr ") == 0) {
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

inline uint64_t ReadTotalCpuJiffies() {
    std::ifstream file("/proc/stat");
    if (!file.is_open()) return 0;
    std::string line;
    if (!std::getline(file, line)) return 0;
    if (line.compare(0, 4, "cpu ") != 0) return 0;
    std::istringstream iss(line);
    std::string tag;
    iss >> tag;
    uint64_t total = 0, v;
    while (iss >> v) total += v;
    return total;
}

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

inline LoadAvg ReadLoadAvg() {
    LoadAvg la;
    std::ifstream file("/proc/loadavg");
    if (file.is_open())
        file >> la.load1 >> la.load5 >> la.load15;
    return la;
}

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

inline bool ParseProcStat(const std::string& path, ProcessStat& out) {
    std::ifstream file(path);
    if (!file.is_open()) return false;
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    auto open = content.find('(');
    auto close = content.rfind(')');
    if (open == std::string::npos || close == std::string::npos) return false;
    out.comm = content.substr(open + 1, close - open - 1);
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

inline std::vector<ProcessStat> ScanProcesses() {
    std::vector<ProcessStat> out;
    DIR* dir = opendir("/proc");
    if (!dir) return out;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type != DT_DIR) continue;
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
