#pragma once

// Forward-declare enums that libbpf.h may reference
// but older kernel headers may not define
#include <linux/bpf.h>
#ifndef BPF_LINK_TYPE_UNSPEC
enum bpf_link_type { BPF_LINK_TYPE_UNSPEC = 0 };
#endif
#include <bpf/libbpf.h>
#include <bpf/bpf.h>

#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/common/logging.h"
#include "core/common/status.h"
#include "ebpf/loader/feature_probe.h"

namespace illuminator {

class BpfProgramManager {
public:
    BpfProgramManager() {
        features_ = ProbeKernelFeatures();
    }

    ~BpfProgramManager() {
        DetachAll();
        for (auto& [name, obj] : objects_) {
            if (obj) bpf_object__close(obj);
        }
    }

    const KernelFeatures& Features() const { return features_; }

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
        IL_INFO("Loaded BPF object: %s from %s", name.c_str(), path.c_str());
        return Status::Ok();
    }

    int GetMapFd(const std::string& obj_name, const std::string& map_name) {
        auto it = objects_.find(obj_name);
        if (it == objects_.end()) return -1;
        struct bpf_map *map = bpf_object__find_map_by_name(it->second, map_name.c_str());
        if (!map) return -1;
        return bpf_map__fd(map);
    }

    int GetProgFd(const std::string& obj_name, const std::string& prog_name) {
        auto it = objects_.find(obj_name);
        if (it == objects_.end()) return -1;
        struct bpf_program *prog = bpf_object__find_program_by_name(
            it->second, prog_name.c_str());
        if (!prog) return -1;
        return bpf_program__fd(prog);
    }

    Status AttachAll(const std::string& obj_name) {
        // No-op: use AttachProgram for each specific program instead
        (void)obj_name;
        return Status::Ok();
    }

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
        IL_INFO("Attached BPF program: %s", prog_name.c_str());
        return Status::Ok();
    }

    // Attach multiple programs by name
    Status AttachPrograms(const std::string& obj_name,
                          const std::vector<std::string>& prog_names) {
        for (auto& name : prog_names) {
            auto status = AttachProgram(obj_name, name);
            if (!status.ok()) {
                IL_WARN("Failed to attach %s: %s", name.c_str(),
                        status.message().c_str());
            }
        }
        return Status::Ok();
    }

    void DetachAll() {
        for (auto* link : links_) {
            if (link) bpf_link__destroy(link);
        }
        links_.clear();
    }

    struct ring_buffer* CreateRingBuffer(
        int map_fd,
        ring_buffer_sample_fn callback,
        void* ctx) {
        return ring_buffer__new(map_fd, callback, ctx, nullptr);
    }

private:
    KernelFeatures features_;
    std::unordered_map<std::string, struct bpf_object*> objects_;
    std::vector<struct bpf_link*> links_;
};

}  // namespace illuminator
