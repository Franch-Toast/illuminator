#!/usr/bin/env bash
# ===========================================================================
# Illuminator 环境检测脚本
# 用途：检测本地编译环境、运行环境是否满足项目需求
# 使用：bash scripts/check_env.sh [--build | --runtime | --all]
# ===========================================================================

set -uo pipefail

# --- 颜色定义 ---
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
BOLD='\033[1m'
NC='\033[0m'

# --- 最低版本要求 ---
MIN_BAZEL_MAJOR=7
MIN_BAZEL_MINOR=0
MIN_GCC_MAJOR=11
MIN_CLANG_MAJOR=10
MIN_NODE_MAJOR=18
MIN_KERNEL_MAJOR=5
MIN_KERNEL_MINOR=8
MIN_LIBBPF_MAJOR=0
MIN_LIBBPF_MINOR=5
MIN_SQLITE_MAJOR=3
MIN_SQLITE_MINOR=31

# --- 计数器 ---
PASS=0
WARN=0
FAIL=0
SUGGESTIONS=()

# --- 辅助函数 ---
print_header() {
    echo -e "\n${BOLD}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${BOLD}  $1${NC}"
    echo -e "${BOLD}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
}

check_pass() {
    echo -e "  ${GREEN}✓${NC} $1"
    ((PASS++))
}

check_warn() {
    echo -e "  ${YELLOW}⚠${NC} $1"
    ((WARN++))
}

check_fail() {
    echo -e "  ${RED}✗${NC} $1"
    ((FAIL++))
}

add_suggestion() {
    SUGGESTIONS+=("$1")
}

version_ge() {
    # 比较版本: version_ge "actual" "required"
    printf '%s\n%s' "$2" "$1" | sort -V -C
}

extract_version() {
    echo "$1" | grep -oP '\d+\.\d+(\.\d+)?' | head -1
}

# ===========================================================================
# 编译环境检测
# ===========================================================================
check_build_env() {
    print_header "编译环境检测 (Build Environment)"

    # --- Bazel / Bazelisk ---
    echo -e "\n${BLUE}[Bazel 构建系统]${NC}"
    if command -v bazel &>/dev/null; then
        local bazel_ver
        bazel_ver=$(bazel --version 2>/dev/null | grep -oP '\d+\.\d+\.\d+' | head -1)
        if [ -n "$bazel_ver" ]; then
            local major minor
            major=$(echo "$bazel_ver" | cut -d. -f1)
            minor=$(echo "$bazel_ver" | cut -d. -f2)
            if [ "$major" -ge "$MIN_BAZEL_MAJOR" ]; then
                check_pass "Bazel $bazel_ver (需要 >= ${MIN_BAZEL_MAJOR}.${MIN_BAZEL_MINOR})"
            else
                check_fail "Bazel $bazel_ver 版本过低 (需要 >= ${MIN_BAZEL_MAJOR}.${MIN_BAZEL_MINOR})"
                add_suggestion "安装 Bazelisk (自动管理 Bazel 版本):\n    curl -fsSL https://github.com/bazelbuild/bazelisk/releases/download/v1.25.0/bazelisk-linux-amd64 -o /usr/local/bin/bazel && chmod +x /usr/local/bin/bazel"
            fi
        fi

        if command -v bazelisk &>/dev/null || (bazel --version 2>/dev/null | grep -qi bazelisk); then
            check_pass "Bazelisk 已安装 (自动读取 .bazelversion)"
        else
            check_warn "未使用 Bazelisk — 建议安装以自动切换 Bazel 版本"
        fi
    else
        check_fail "Bazel 未安装"
        add_suggestion "安装 Bazelisk:\n    curl -fsSL https://github.com/bazelbuild/bazelisk/releases/download/v1.25.0/bazelisk-linux-amd64 -o /usr/local/bin/bazel && chmod +x /usr/local/bin/bazel"
    fi

    # --- .bazelversion ---
    if [ -f ".bazelversion" ]; then
        check_pass ".bazelversion 文件存在: $(cat .bazelversion | tr -d '\n')"
    else
        check_warn ".bazelversion 文件缺失 — CI 可能使用不匹配的 Bazel 版本"
    fi

    # --- GCC ---
    echo -e "\n${BLUE}[C++ 编译器]${NC}"
    if command -v g++ &>/dev/null; then
        local gcc_ver
        gcc_ver=$(g++ --version 2>/dev/null | grep -oP '\d+\.\d+\.\d+' | head -1)
        local gcc_major
        gcc_major=$(echo "$gcc_ver" | cut -d. -f1)
        if [ "$gcc_major" -ge "$MIN_GCC_MAJOR" ]; then
            check_pass "GCC $gcc_ver (需要 >= ${MIN_GCC_MAJOR}.0, 支持 C++20)"
        else
            check_fail "GCC $gcc_ver — C++20 支持不完整 (需要 >= ${MIN_GCC_MAJOR}.0)"
            add_suggestion "安装 GCC ${MIN_GCC_MAJOR}:\n    sudo apt-get install -y gcc-${MIN_GCC_MAJOR} g++-${MIN_GCC_MAJOR}\n    sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-${MIN_GCC_MAJOR} 100\n    sudo update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-${MIN_GCC_MAJOR} 100"
        fi
    else
        check_fail "GCC 未安装"
        add_suggestion "安装 GCC ${MIN_GCC_MAJOR}:\n    sudo apt-get install -y gcc-${MIN_GCC_MAJOR} g++-${MIN_GCC_MAJOR}"
    fi

    # --- C++20 标准支持测试 ---
    if command -v g++ &>/dev/null; then
        local cpp20_test
        cpp20_test=$(echo '#include <concepts>
int main() { return 0; }' | g++ -std=c++20 -x c++ - -o /dev/null 2>&1 && echo "ok" || echo "fail")
        if [ "$cpp20_test" = "ok" ]; then
            check_pass "C++20 编译测试通过 (concepts, ranges 可用)"
        else
            check_warn "C++20 编译测试失败 — 部分特性可能不可用"
        fi
    fi

    # --- Clang (BPF 编译) ---
    echo -e "\n${BLUE}[Clang (eBPF 探针编译)]${NC}"
    if command -v clang &>/dev/null; then
        local clang_ver
        clang_ver=$(clang --version 2>/dev/null | grep -oP '\d+\.\d+\.\d+' | head -1)
        local clang_major
        clang_major=$(echo "$clang_ver" | cut -d. -f1)
        if [ "$clang_major" -ge "$MIN_CLANG_MAJOR" ]; then
            check_pass "Clang $clang_ver (需要 >= ${MIN_CLANG_MAJOR}.0, BPF target 支持)"
        else
            check_fail "Clang $clang_ver 版本过低 (需要 >= ${MIN_CLANG_MAJOR}.0)"
            add_suggestion "安装 Clang:\n    sudo apt-get install -y clang"
        fi

        # BPF target 测试
        if clang --print-targets 2>/dev/null | grep -q "bpf"; then
            check_pass "Clang BPF target 已支持"
        else
            check_warn "Clang 未报告 BPF target — 编译探针可能失败"
        fi
    else
        check_fail "Clang 未安装 (eBPF 探针编译必需)"
        add_suggestion "安装 Clang:\n    sudo apt-get install -y clang"
    fi

    # --- 系统库 ---
    echo -e "\n${BLUE}[系统开发库]${NC}"

    # libbpf-dev
    if pkg-config --exists libbpf 2>/dev/null; then
        local libbpf_ver
        libbpf_ver=$(pkg-config --modversion libbpf 2>/dev/null || echo "unknown")
        check_pass "libbpf-dev $libbpf_ver"
    elif [ -f /usr/include/bpf/libbpf.h ]; then
        local libbpf_ver
        libbpf_ver=$(dpkg -l libbpf-dev 2>/dev/null | awk '/^ii/{print $3}' | head -1)
        check_pass "libbpf-dev ${libbpf_ver:-installed}"
    else
        check_fail "libbpf-dev 未安装 (eBPF 编译必需)"
        add_suggestion "安装 libbpf-dev:\n    sudo apt-get install -y libbpf-dev"
    fi

    # libelf-dev
    if [ -f /usr/include/libelf.h ] || [ -f /usr/include/gelf.h ]; then
        check_pass "libelf-dev 已安装"
    else
        check_fail "libelf-dev 未安装"
        add_suggestion "安装 libelf-dev:\n    sudo apt-get install -y libelf-dev"
    fi

    # zlib
    if [ -f /usr/include/zlib.h ]; then
        check_pass "zlib1g-dev 已安装"
    else
        check_fail "zlib1g-dev 未安装"
        add_suggestion "安装 zlib1g-dev:\n    sudo apt-get install -y zlib1g-dev"
    fi

    # sqlite3
    if [ -f /usr/include/sqlite3.h ]; then
        local sqlite_ver
        sqlite_ver=$(sqlite3 --version 2>/dev/null | awk '{print $1}' || echo "")
        if [ -n "$sqlite_ver" ]; then
            check_pass "libsqlite3-dev $sqlite_ver (需要 >= ${MIN_SQLITE_MAJOR}.${MIN_SQLITE_MINOR})"
        else
            check_pass "libsqlite3-dev 已安装"
        fi
    else
        check_fail "libsqlite3-dev 未安装"
        add_suggestion "安装 libsqlite3-dev:\n    sudo apt-get install -y libsqlite3-dev"
    fi

    # --- Node.js (前端) ---
    echo -e "\n${BLUE}[Node.js (前端构建)]${NC}"
    if command -v node &>/dev/null; then
        local node_ver
        node_ver=$(node --version 2>/dev/null | grep -oP '\d+\.\d+\.\d+')
        local node_major
        node_major=$(echo "$node_ver" | cut -d. -f1)
        if [ "$node_major" -ge "$MIN_NODE_MAJOR" ]; then
            check_pass "Node.js $node_ver (需要 >= ${MIN_NODE_MAJOR}.0)"
        else
            check_fail "Node.js $node_ver 版本过低 (需要 >= ${MIN_NODE_MAJOR}.0, Vite 5.x 要求)"
            add_suggestion "安装 Node.js 20 LTS:\n    curl -fsSL https://deb.nodesource.com/setup_20.x | sudo -E bash -\n    sudo apt-get install -y nodejs"
        fi
    else
        check_warn "Node.js 未安装 (仅前端构建需要)"
        add_suggestion "安装 Node.js 20 LTS:\n    curl -fsSL https://deb.nodesource.com/setup_20.x | sudo -E bash -\n    sudo apt-get install -y nodejs"
    fi

    if command -v npm &>/dev/null; then
        check_pass "npm $(npm --version 2>/dev/null)"
    fi

    # --- Python ---
    echo -e "\n${BLUE}[Python (Bazel 依赖)]${NC}"
    if command -v python3 &>/dev/null; then
        local py_ver
        py_ver=$(python3 --version 2>/dev/null | grep -oP '\d+\.\d+\.\d+')
        check_pass "Python $py_ver"
    else
        check_fail "Python3 未安装 (Bazel 运行必需)"
        add_suggestion "安装 Python3:\n    sudo apt-get install -y python3"
    fi
}

# ===========================================================================
# 运行环境检测
# ===========================================================================
check_runtime_env() {
    print_header "运行环境检测 (Runtime Environment)"

    # --- 操作系统 ---
    echo -e "\n${BLUE}[操作系统]${NC}"
    if [ -f /etc/os-release ]; then
        local os_name os_ver
        os_name=$(. /etc/os-release && echo "$NAME")
        os_ver=$(. /etc/os-release && echo "$VERSION_ID")
        check_pass "$os_name $os_ver"
    fi
    echo -e "  架构: $(uname -m)"

    # --- 内核版本 ---
    echo -e "\n${BLUE}[Linux 内核]${NC}"
    local kernel_ver
    kernel_ver=$(uname -r)
    local kernel_major kernel_minor
    kernel_major=$(echo "$kernel_ver" | cut -d. -f1)
    kernel_minor=$(echo "$kernel_ver" | cut -d. -f2)

    if [ "$kernel_major" -gt "$MIN_KERNEL_MAJOR" ] || \
       { [ "$kernel_major" -eq "$MIN_KERNEL_MAJOR" ] && [ "$kernel_minor" -ge "$MIN_KERNEL_MINOR" ]; }; then
        check_pass "内核 $kernel_ver (需要 >= ${MIN_KERNEL_MAJOR}.${MIN_KERNEL_MINOR} 支持 BPF ring buffer)"
    else
        check_fail "内核 $kernel_ver 版本过低 (需要 >= ${MIN_KERNEL_MAJOR}.${MIN_KERNEL_MINOR})"
        add_suggestion "升级内核到 5.8+:\n    sudo apt-get install -y linux-generic-hwe-$(lsb_release -rs) (Ubuntu HWE kernel)"
    fi

    # --- BPF 子系统 ---
    echo -e "\n${BLUE}[eBPF 运行条件]${NC}"

    # /sys/kernel/btf/vmlinux (CO-RE 需要)
    if [ -f /sys/kernel/btf/vmlinux ]; then
        check_pass "BTF vmlinux 可用 (CO-RE 跨版本兼容)"
    else
        check_fail "BTF vmlinux 不可用 — eBPF CO-RE 程序无法运行"
        add_suggestion "启用 BTF:\n    安装带 CONFIG_DEBUG_INFO_BTF=y 的内核（Ubuntu 20.10+ 默认开启）"
    fi

    # bpf() syscall 可用性
    if [ -e /proc/sys/kernel/unprivileged_bpf_disabled ]; then
        local bpf_disabled
        bpf_disabled=$(cat /proc/sys/kernel/unprivileged_bpf_disabled 2>/dev/null || echo "2")
        case "$bpf_disabled" in
            0) check_pass "非特权 BPF 已启用 (unprivileged_bpf_disabled=0)" ;;
            1) check_warn "非特权 BPF 已禁用 — 需要 root 或 CAP_BPF 运行" ;;
            2) check_warn "非特权 BPF 受限 — 需要 CAP_BPF + CAP_PERFMON" ;;
        esac
    fi

    # CAP_BPF 权限检查
    if [ "$(id -u)" -eq 0 ]; then
        check_pass "当前以 root 运行 (完整 BPF 权限)"
    else
        local caps
        caps=$(cat /proc/self/status 2>/dev/null | grep CapEff | awk '{print $2}')
        if [ -n "$caps" ] && [ "$caps" != "0000000000000000" ]; then
            check_warn "非 root 用户 — 运行 illuminator 需要: sudo 或 setcap"
            add_suggestion "授予 eBPF 运行权限:\n    sudo setcap cap_bpf,cap_perfmon=+ep ./illuminator\n    # 或使用 Docker:\n    docker run --cap-add CAP_BPF --cap-add CAP_PERFMON ..."
        else
            check_warn "非 root 用户 — 运行 eBPF 探针需要额外权限"
            add_suggestion "授予 eBPF 运行权限:\n    sudo setcap cap_bpf,cap_perfmon=+ep ./illuminator\n    # 或使用 Docker:\n    docker run --cap-add CAP_BPF --cap-add CAP_PERFMON ..."
        fi
    fi

    # perf_event 支持
    if [ -e /proc/sys/kernel/perf_event_paranoid ]; then
        local paranoid
        paranoid=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo "4")
        case "$paranoid" in
            -1|0|1) check_pass "perf_event_paranoid=$paranoid (允许 CPU profiling)" ;;
            2)      check_warn "perf_event_paranoid=2 (内核级 profiling 受限，需 root)" ;;
            *)      check_warn "perf_event_paranoid=$paranoid (可能限制 profiling)" ;;
        esac
    fi

    # --- 运行时库 ---
    echo -e "\n${BLUE}[运行时动态库]${NC}"

    declare -A libs=(
        ["libsqlite3"]="libsqlite3-0"
        ["libbpf"]="libbpf0"
        ["libelf"]="libelf1"
        ["libz"]="zlib1g"
    )
    for lib in "${!libs[@]}"; do
        local found
        found=$(ldconfig -p 2>/dev/null | grep "${lib}.so" | awk '{print $NF}' | head -1)
        if [ -n "$found" ]; then
            check_pass "${lib}.so → $found"
        else
            check_fail "${lib}.so 未找到"
            add_suggestion "安装运行时库:\n    sudo apt-get install -y ${libs[$lib]}"
        fi
    done

    # --- 网络端口 ---
    echo -e "\n${BLUE}[端口可用性]${NC}"
    for port in 9527 9528; do
        if ! ss -tlnp 2>/dev/null | grep -q ":${port} " && \
           ! netstat -tlnp 2>/dev/null | grep -q ":${port} "; then
            check_pass "端口 $port 可用"
        else
            check_warn "端口 $port 已被占用"
        fi
    done

    # --- 磁盘空间 ---
    echo -e "\n${BLUE}[磁盘空间]${NC}"
    local avail_gb
    avail_gb=$(df -BG . 2>/dev/null | awk 'NR==2{gsub("G","",$4); print $4}')
    if [ -n "$avail_gb" ] && [ "$avail_gb" -ge 2 ]; then
        check_pass "可用空间 ${avail_gb}GB (最低需要 2GB)"
    elif [ -n "$avail_gb" ]; then
        check_warn "可用空间仅 ${avail_gb}GB (建议 >= 2GB)"
    fi
}

# ===========================================================================
# 总结报告
# ===========================================================================
print_summary() {
    print_header "检测总结"
    echo -e "  ${GREEN}通过: $PASS${NC}  ${YELLOW}警告: $WARN${NC}  ${RED}失败: $FAIL${NC}"
    echo ""

    if [ ${#SUGGESTIONS[@]} -gt 0 ]; then
        echo -e "${BOLD}修复建议:${NC}"
        echo ""
        local i=1
        for suggestion in "${SUGGESTIONS[@]}"; do
            echo -e "  ${YELLOW}[$i]${NC} $(echo -e "$suggestion")"
            echo ""
            ((i++))
        done

        # 一键修复命令
        echo -e "${BOLD}一键安装所有缺失依赖 (Ubuntu/Debian):${NC}"
        echo -e "  ${BLUE}sudo apt-get update && sudo apt-get install -y \\"
        echo -e "    build-essential gcc-11 g++-11 clang \\"
        echo -e "    libbpf-dev libelf-dev zlib1g-dev libsqlite3-dev \\"
        echo -e "    python3${NC}"
        echo ""
    fi

    if [ "$FAIL" -eq 0 ]; then
        echo -e "  ${GREEN}${BOLD}环境检测全部通过！可以开始构建和运行 Illuminator。${NC}"
        return 0
    else
        echo -e "  ${RED}${BOLD}存在 $FAIL 个问题需要修复，请参考上方建议。${NC}"
        return 1
    fi
}

# ===========================================================================
# 主函数
# ===========================================================================
main() {
    echo -e "${BOLD}Illuminator 环境检测工具 v1.0${NC}"
    echo -e "时间: $(date '+%Y-%m-%d %H:%M:%S')"
    echo -e "主机: $(hostname) ($(uname -m))"

    local mode="${1:---all}"

    case "$mode" in
        --build)
            check_build_env
            ;;
        --runtime)
            check_runtime_env
            ;;
        --all|*)
            check_build_env
            check_runtime_env
            ;;
    esac

    print_summary
}

# 确保在项目根目录运行
if [ ! -f "MODULE.bazel" ] && [ ! -f "WORKSPACE" ]; then
    echo -e "${YELLOW}警告: 未在 illuminator 项目根目录运行，部分检测可能不准确${NC}"
    echo -e "建议: cd /path/to/illuminator && bash scripts/check_env.sh"
    echo ""
fi

main "$@"
