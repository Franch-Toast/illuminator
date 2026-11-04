#!/usr/bin/env bash
# Core host requirements. eBPF compilation/loading is an optional SDK capability.
set -uo pipefail
failures=0
check() {
    if command -v "$1" >/dev/null 2>&1; then
        printf 'OK   %s\n' "$1"
    else
        printf 'FAIL %s (%s)\n' "$1" "$2"
        failures=$((failures + 1))
    fi
}
mode=${1:---all}
case "$mode" in
    --build|--runtime|--all) ;;
    *) printf 'Usage: %s [--build|--runtime|--all]\n' "$0"; exit 2 ;;
esac
if [[ $(uname -s) != Linux ]]; then
    echo 'FAIL Linux is required by TimerWheel (timerfd/epoll/eventfd)'
    failures=$((failures + 1))
fi
if [[ "$mode" != --runtime ]]; then
    check bazel 'Bazel 7.6.1 or Bazelisk'
    check g++ 'C++20 compiler'
    check python3 'CLI smoke tests'
    if command -v g++ >/dev/null 2>&1; then
        if ! printf '#include <variant>\nstruct C { int value; };\nint main() { C c{.value=1}; std::variant<int> v{c.value}; }\n' | g++ -std=c++2a -x c++ - -o /dev/null; then
            failures=$((failures + 1))
        fi
        for header in sqlite3.h bpf/bpf.h; do
            if printf '#include <%s>\n' "$header" | g++ -x c++ -fsyntax-only - >/dev/null 2>&1; then
                printf 'OK   %s\n' "$header"
            else
                printf 'FAIL %s (install libsqlite3-dev / libbpf-dev)\n' "$header"
                failures=$((failures + 1))
            fi
        done
    fi
    echo 'Optional eBPF development: clang, bpftool, libbpf-dev, libelf-dev, zlib1g-dev.'
fi
if [[ "$mode" != --build ]]; then
    if command -v ldconfig >/dev/null 2>&1 && ldconfig -p 2>/dev/null | grep 'libsqlite3.so' >/dev/null; then
        echo 'OK   SQLite runtime'
    else
        echo 'FAIL SQLite runtime (install libsqlite3-0)'
        failures=$((failures + 1))
    fi
    echo 'Core demo runs without root or network ports.'
fi
exit "$((failures > 0))"
