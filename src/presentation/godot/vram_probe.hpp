#pragma once

// #3027 GM-04: how much device-local memory the GPU the engine renders on has. On Vulkan the
// VK_EXT_memory_budget budget of the device-local heaps (what this process may use now,
// other programs' use excluded), else those heaps' sizes. Any other driver, or a loader that
// cannot be reached, reports nothing, and nothing is then constrained. Only the public
// Vulkan API is used, through the loader the engine itself already has loaded.

#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOGDI
#define NOGDI
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <dlfcn.h>
#endif

namespace eawr::presentation::godot_backend::detail {

struct DeviceMemory final {
    std::uint64_t bytes{};
    std::string source;
};

namespace vulkan_probe {
// The few Vulkan declarations needed (vulkan_core.h layouts).
using Function = void (*)();
using GetInstanceProcAddr = Function (*)(void* instance, const char* name);
struct MemoryType final { std::uint32_t flags, heap; };
struct MemoryHeap final { std::uint64_t size; std::uint32_t flags; };
struct MemoryProperties final {
    std::uint32_t type_count;
    MemoryType types[32];
    std::uint32_t heap_count;
    MemoryHeap heaps[16];
};
struct MemoryBudget final {
    std::uint32_t type;
    void* next;
    std::uint64_t budget[16];
    std::uint64_t usage[16];
};
struct MemoryProperties2 final {
    std::uint32_t type;
    void* next;
    MemoryProperties properties;
};
struct ExtensionProperties final {
    char name[256];
    std::uint32_t version;
};
constexpr std::uint32_t memory_properties_2_type = 1000059006U;
constexpr std::uint32_t memory_budget_type = 1000237000U;
constexpr std::uint32_t device_local_heap = 1U;
using GetMemoryProperties = void (*)(void* device, MemoryProperties* properties);
using GetMemoryProperties2 = void (*)(void* device, MemoryProperties2* properties);
using EnumerateDeviceExtensions = std::int32_t (*)(void* device, const char* layer, std::uint32_t* count,
                                                    ExtensionProperties* properties);

[[nodiscard]] inline GetInstanceProcAddr loader() {
#if defined(_WIN32)
    HMODULE module = GetModuleHandleW(L"vulkan-1.dll");
    if (module == nullptr) return nullptr;
    return reinterpret_cast<GetInstanceProcAddr>(
        reinterpret_cast<void*>(GetProcAddress(module, "vkGetInstanceProcAddr")));
#elif defined(__linux__)
    void* module = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_NOLOAD);
    if (module == nullptr) return nullptr;
    auto* function = dlsym(module, "vkGetInstanceProcAddr");
    dlclose(module);
    return reinterpret_cast<GetInstanceProcAddr>(function);
#else
    return nullptr;
#endif
}
} // namespace vulkan_probe

[[nodiscard]] inline DeviceMemory probe_device_memory() {
    using namespace vulkan_probe;
    godot::RenderingServer* rendering = godot::RenderingServer::get_singleton();
    if (rendering == nullptr) return {0, "no rendering server"};
    const std::string driver = rendering->get_current_rendering_driver_name().utf8().get_data();
    godot::RenderingDevice* device = rendering->get_rendering_device();
    if (driver != "vulkan" || device == nullptr) return {0, "unsupported driver " + driver};
    auto* instance = reinterpret_cast<void*>(static_cast<std::uintptr_t>(
        device->get_driver_resource(godot::RenderingDevice::DRIVER_RESOURCE_TOPMOST_OBJECT, godot::RID(), 0)));
    auto* physical = reinterpret_cast<void*>(static_cast<std::uintptr_t>(
        device->get_driver_resource(godot::RenderingDevice::DRIVER_RESOURCE_PHYSICAL_DEVICE, godot::RID(), 0)));
    const GetInstanceProcAddr get = loader();
    if (instance == nullptr || physical == nullptr || get == nullptr) return {0, "vulkan loader unavailable"};

    bool budget_supported = false;
    if (auto enumerate = reinterpret_cast<EnumerateDeviceExtensions>(get(instance, "vkEnumerateDeviceExtensionProperties"))) {
        std::uint32_t count = 0;
        if (enumerate(physical, nullptr, &count, nullptr) == 0 && count != 0) {
            std::vector<ExtensionProperties> extensions(count);
            if (enumerate(physical, nullptr, &count, extensions.data()) >= 0) {
                for (std::uint32_t index = 0; index < count && index < extensions.size(); ++index) {
                    budget_supported = budget_supported
                        || std::strncmp(extensions[index].name, "VK_EXT_memory_budget", 256) == 0;
                }
            }
        }
    }
    const auto device_local = [](const MemoryProperties& properties, const std::uint64_t* per_heap) {
        std::uint64_t total = 0;
        for (std::uint32_t heap = 0; heap < properties.heap_count && heap < 16; ++heap) {
            if ((properties.heaps[heap].flags & device_local_heap) != 0) total += per_heap[heap];
        }
        return total;
    };
    if (auto get2 = reinterpret_cast<GetMemoryProperties2>(get(instance, "vkGetPhysicalDeviceMemoryProperties2"))) {
        MemoryBudget budget{};
        budget.type = memory_budget_type;
        MemoryProperties2 properties{};
        properties.type = memory_properties_2_type;
        properties.next = budget_supported ? &budget : nullptr;
        get2(physical, &properties);
        if (budget_supported) {
            const std::uint64_t bytes = device_local(properties.properties, budget.budget);
            if (bytes != 0) return {bytes, "vulkan memory budget"};
        }
    }
    if (auto get1 = reinterpret_cast<GetMemoryProperties>(get(instance, "vkGetPhysicalDeviceMemoryProperties"))) {
        MemoryProperties properties{};
        get1(physical, &properties);
        std::uint64_t sizes[16]{};
        for (std::uint32_t heap = 0; heap < properties.heap_count && heap < 16; ++heap) {
            sizes[heap] = properties.heaps[heap].size;
        }
        const std::uint64_t bytes = device_local(properties, sizes);
        if (bytes != 0) return {bytes, "vulkan device-local heaps"};
    }
    return {0, "vulkan memory properties unavailable"};
}

} // namespace eawr::presentation::godot_backend::detail
