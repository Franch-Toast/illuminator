#pragma once

#include <dlfcn.h>
#include <string>
#include <vector>

#include "core/common/logging.h"
#include "core/common/status.h"
#include "plugin/api/plugin_api.h"

namespace illuminator {

// Loads shared-object (.so) plugins at runtime via dlopen/dlsym
class SoLoader {
public:
    ~SoLoader() {
        for (auto* handle : handles_) {
            if (handle) dlclose(handle);
        }
    }

    Status LoadPlugin(const std::string& path) {
        void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            return Status::Error(StatusCode::kInternal,
                std::string("dlopen failed: ") + dlerror());
        }

        auto describe_fn = reinterpret_cast<IlPluginDescribeFn>(
            dlsym(handle, IL_PLUGIN_EXPORT_NAME));
        if (!describe_fn) {
            dlclose(handle);
            return Status::Error(StatusCode::kInvalidArgument,
                std::string("Symbol not found: ") + IL_PLUGIN_EXPORT_NAME);
        }

        const IlPluginDescriptor* desc = describe_fn();
        if (!desc) {
            dlclose(handle);
            return Status::Error(StatusCode::kInternal,
                "Plugin describe() returned null");
        }

        IL_INFO("Loaded SO plugin: %s v%s (type=%u) from %s",
                desc->name, desc->version, desc->type, path.c_str());

        handles_.push_back(handle);
        descriptors_.push_back(desc);
        return Status::Ok();
    }

    const std::vector<const IlPluginDescriptor*>& Descriptors() const {
        return descriptors_;
    }

private:
    std::vector<void*> handles_;
    std::vector<const IlPluginDescriptor*> descriptors_;
};

}  // namespace illuminator
