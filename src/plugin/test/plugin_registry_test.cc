#include "gtest/gtest.h"
#include "plugin/manager/plugin_registry.h"

namespace illuminator {
namespace {

class MockSource : public SourcePlugin {
public:
    const char* Name() const override { return "mock_source"; }
    const char* Version() const override { return "1.0.0"; }
    Status Init(const ConfigValue&) override { return Status::Ok(); }
    StatusOr<DataBatchPtr> Collect() override {
        auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
        auto& rec = batch->AddRecord();
        rec.SetField(batch->InternString("value"), uint64_t{42});
        return batch;
    }
};

class MockProcessor : public ProcessorPlugin {
public:
    const char* Name() const override { return "mock_processor"; }
    const char* Version() const override { return "1.0.0"; }
    Status Init(const ConfigValue&) override { return Status::Ok(); }
    StatusOr<DataBatchPtr> Process(DataBatchPtr batch) override { return batch; }
};

class MockSink : public SinkPlugin {
public:
    const char* Name() const override { return "mock_sink"; }
    const char* Version() const override { return "1.0.0"; }
    Status Init(const ConfigValue&) override { return Status::Ok(); }
    Status Write(DataBatchPtr) override {
        written_++;
        return Status::Ok();
    }
    int written_ = 0;
};

TEST(PluginRegistryTest, RegisterAndCreateSource) {
    auto& reg = PluginRegistry::Instance();
    reg.RegisterSource("test_mock_source", []() {
        return std::make_unique<MockSource>();
    });

    auto source = reg.CreateSource("test_mock_source");
    ASSERT_NE(source, nullptr);
    EXPECT_STREQ(source->Name(), "mock_source");
    EXPECT_STREQ(source->Version(), "1.0.0");
}

TEST(PluginRegistryTest, CreateSourceCollectsData) {
    auto& reg = PluginRegistry::Instance();
    auto source = reg.CreateSource("test_mock_source");
    ASSERT_NE(source, nullptr);

    ConfigValue cfg;
    EXPECT_TRUE(source->Init(cfg).ok());

    auto result = source->Collect();
    ASSERT_TRUE(result.ok());
    EXPECT_FALSE(result.value()->records().empty());
}

TEST(PluginRegistryTest, RegisterAndCreateProcessor) {
    auto& reg = PluginRegistry::Instance();
    reg.RegisterProcessor("test_mock_proc", []() {
        return std::make_unique<MockProcessor>();
    });

    auto proc = reg.CreateProcessor("test_mock_proc");
    ASSERT_NE(proc, nullptr);
    EXPECT_STREQ(proc->Name(), "mock_processor");
}

TEST(PluginRegistryTest, RegisterAndCreateSink) {
    auto& reg = PluginRegistry::Instance();
    reg.RegisterSink("test_mock_sink", []() {
        return std::make_unique<MockSink>();
    });

    auto sink = reg.CreateSink("test_mock_sink");
    ASSERT_NE(sink, nullptr);

    auto batch = std::make_shared<DataBatch>(DataBatch::Type::kMetrics);
    EXPECT_TRUE(sink->Write(batch).ok());
}

TEST(PluginRegistryTest, CreateNonExistentReturnsNull) {
    auto& reg = PluginRegistry::Instance();
    EXPECT_EQ(reg.CreateSource("non_existent_xyz"), nullptr);
    EXPECT_EQ(reg.CreateProcessor("non_existent_xyz"), nullptr);
    EXPECT_EQ(reg.CreateSink("non_existent_xyz"), nullptr);
}

TEST(PluginRegistryTest, ListPluginsIncludesRegistered) {
    auto& reg = PluginRegistry::Instance();
    auto sources = reg.ListSources();
    bool found = false;
    for (auto& name : sources) {
        if (name == "test_mock_source") found = true;
    }
    EXPECT_TRUE(found);
}

}  // namespace
}  // namespace illuminator
