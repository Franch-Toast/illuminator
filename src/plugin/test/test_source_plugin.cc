// Real shared-library fixture: validates the existing C++ SDK bridge end to end.
#include "plugin/api/ebpf_source_base.h"

namespace {
class TestSource : public illuminator::SourcePlugin {
public:
    const char* Name() const override { return "test_so_source"; }
    const char* Version() const override { return "1"; }
    uint32_t IntervalMs() const override { return 25; }
    illuminator::StatusOr<illuminator::DataBatchPtr> Collect() override {
        auto batch = std::make_shared<illuminator::DataBatch>();
        auto& record = batch->AddRecord();
        record.timestamp = illuminator::NowTimestamp();
        record.SetField(batch->InternString("so_value"), int64_t{42});
        return batch;
    }
};
void* Create(const char*) { return new TestSource; }
void Destroy(void* instance) { delete static_cast<TestSource*>(instance); }
}

extern "C" const IlPluginDescriptor* illuminator_plugin_describe() {
    static const IlPluginDescriptor descriptor{
        IL_PLUGIN_API_VERSION, "test_so_source", "1", 0, Create, nullptr, Destroy};
    return &descriptor;
}
