#include "plugin/infra/builtin_plugins.h"
#include "plugin/infra/plugin_registry.h"
#include "plugin/sinks/recording_sink/recording_sink.h"

namespace illuminator {

void RegisterBuiltinPlugins() {
    static const bool registered = [] {
        PluginRegistry::Instance().RegisterSink("recording", [] {
            return std::make_unique<RecordingSink>();
        });
        return true;
    }();
    (void)registered;
}

}  // namespace illuminator
