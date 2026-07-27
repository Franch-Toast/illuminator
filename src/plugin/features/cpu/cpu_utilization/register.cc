// CpuUtilizationDriver 静态注册（从 header 移至编译单元以确保 alwayslink 生效）

#include "plugin/features/cpu/cpu_utilization/cpu_utilization_driver.h"

static bool _reg_cpu_util = [] {
    ::illuminator::PluginRegistry::Instance().AddDriver(
        [] { return std::make_unique<illuminator::CpuUtilizationDriver>(); });
    return true;
}();
