#pragma once

#include <cstdint>
#include <string>

namespace eawr::presentation::godot_backend {

// The exact rendering device's local heaps and current process budget. Sampled on the
// render thread. First use or refresh requests a sample; non-refresh calls poll the completed
// cache. The main thread never waits or makes a Vulkan call. Refresh before live allocation changes.
struct AdapterMemory final {
    std::uint64_t device_local_bytes{};
    std::uint64_t budget_bytes{};
    std::string source{"pending"};
    bool pending{true};
};

[[nodiscard]] AdapterMemory query_adapter_memory(bool refresh = false);

} // namespace eawr::presentation::godot_backend
