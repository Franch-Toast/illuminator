#pragma once

namespace illuminator {

// Force-links all builtin plugins so they register with the PluginRegistry
// at static initialization time. Call this from main() to prevent
// the linker from stripping unused translation units.
void RegisterBuiltinPlugins();

}  // namespace illuminator
