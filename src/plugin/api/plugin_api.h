#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <memory>

#include "core/common/config.h"
#include "core/common/status.h"
#include "core/engine/data_batch.h"

namespace illuminator {

// Plugin type enumeration
enum class PluginType {
    kSource,
    kProcessor,
    kAggregator,
    kSink,
};

inline const char* PluginTypeToString(PluginType type) {
    switch (type) {
        case PluginType::kSource:     return "source";
        case PluginType::kProcessor:  return "processor";
        case PluginType::kAggregator: return "aggregator";
        case PluginType::kSink:       return "sink";
    }
    return "unknown";
}

// Base class for all plugins
class Plugin {
public:
    virtual ~Plugin() = default;

    virtual const char* Name() const = 0;
    virtual const char* Version() const = 0;
    virtual PluginType Type() const = 0;

    virtual Status Init(const ConfigValue& config) { return Status::Ok(); }
    virtual Status Start() { return Status::Ok(); }
    virtual Status Stop() { return Status::Ok(); }
};

}  // namespace illuminator

// ---- Stable C ABI for shared-object plugins ----
extern "C" {

struct IlPluginDescriptor {
    const char* name;
    const char* version;
    uint32_t type;  // illuminator::PluginType as uint32_t
    void* (*create)(const char* config_json);
    int   (*process)(void* ctx, void* input, void* output);
    void  (*destroy)(void* ctx);
};

// Every .so plugin must export this symbol
typedef const IlPluginDescriptor* (*IlPluginDescribeFn)();

#define IL_PLUGIN_EXPORT_NAME "illuminator_plugin_describe"

}  // extern "C"
