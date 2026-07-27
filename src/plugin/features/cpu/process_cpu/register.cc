// ProcessCpuDriver 静态注册（从 header 移至编译单元以确保 alwayslink 生效）

#include "plugin/features/cpu/process_cpu/process_cpu_driver.h"

static bool _reg_process_cpu = [] {
    ::illuminator::PluginRegistry::Instance().AddDriver(
        [] { return std::make_unique<illuminator::ProcessCpuDriver>(); });
    return true;
}();
