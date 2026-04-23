#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>

#include "core/common/logging.h"
#include "core/common/status.h"
#include "core/engine/data_batch.h"
#include "plugin/api/processor_plugin.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {

// WASM plugin runtime abstraction.
// Loads WASM modules and executes them as Processor plugins.
// Actual WASM VM integration (WAMR/Wasmtime) would be added as a
// build dependency; this provides the framework and interface.
class WasmRuntime {
public:
    static WasmRuntime& Instance() {
        static WasmRuntime inst;
        return inst;
    }

    Status Init() {
        IL_INFO("WASM runtime initialized (stub)");
        initialized_ = true;
        return Status::Ok();
    }

    bool IsAvailable() const { return initialized_; }

    Status LoadModule(const std::string& name, const std::string& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            return Status::Error(StatusCode::kNotFound,
                "WASM module not found: " + path);
        }

        std::vector<uint8_t> bytes(
            std::istreambuf_iterator<char>(file), {});

        if (bytes.size() < 4 || bytes[0] != 0x00 || bytes[1] != 0x61 ||
            bytes[2] != 0x73 || bytes[3] != 0x6d) {
            return Status::Error(StatusCode::kInvalidArgument,
                "Not a valid WASM file: " + path);
        }

        modules_[name] = std::move(bytes);
        IL_INFO("Loaded WASM module: %s (%zu bytes)", name.c_str(),
                modules_[name].size());
        return Status::Ok();
    }

    const std::vector<uint8_t>* GetModule(const std::string& name) const {
        auto it = modules_.find(name);
        return it != modules_.end() ? &it->second : nullptr;
    }

    std::vector<std::string> ListModules() const {
        std::vector<std::string> names;
        for (auto& [k, _] : modules_) names.push_back(k);
        return names;
    }

private:
    WasmRuntime() = default;
    bool initialized_ = false;
    std::unordered_map<std::string, std::vector<uint8_t>> modules_;
};

// WASM-based processor plugin placeholder.
// Delegates processing to a WASM module function.
class WasmProcessorPlugin : public ProcessorPlugin {
public:
    explicit WasmProcessorPlugin(const std::string& module_name)
        : module_name_(module_name) {}

    const char* Name() const override { return module_name_.c_str(); }
    const char* Version() const override { return "wasm-0.1.0"; }

    Status Init(const ConfigValue& config) override {
        if (!WasmRuntime::Instance().IsAvailable()) {
            return Status::Error(StatusCode::kUnavailable,
                "WASM runtime not initialized");
        }
        auto* mod = WasmRuntime::Instance().GetModule(module_name_);
        if (!mod) {
            return Status::Error(StatusCode::kNotFound,
                "WASM module not loaded: " + module_name_);
        }
        IL_INFO("WASM processor '%s' initialized", module_name_.c_str());
        return Status::Ok();
    }

    StatusOr<DataBatchPtr> Process(DataBatchPtr input) override {
        // In a full implementation, this would:
        // 1. Serialize input to a WASM-readable format
        // 2. Call the WASM module's process() function
        // 3. Deserialize the output
        // For now, pass through unchanged
        return input;
    }

private:
    std::string module_name_;
};

}  // namespace illuminator
