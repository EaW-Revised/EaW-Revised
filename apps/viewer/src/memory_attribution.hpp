#pragma once

#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/performance.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <mutex>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef NOGDI
#define NOGDI
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace eawr::presentation::godot_backend {

// Diagnostic only: device counters overlap engine monitors and CPU backing.
// They must never be added to private commit as disjoint owned bytes.
class MemoryAttribution final {
public:
    static bool enabled() {
        auto* os = godot::OS::get_singleton();
        return os && os->get_cmdline_user_args().has("--eawr-memory-attribution");
    }
    // Load-phase timeline: one log line with the process private bytes. Census runs only.
    static void load_mark(const char* label, const std::uint64_t cache_retained_bytes = 0) {
        if (!enabled()) return;
        std::uint64_t private_bytes = 0;
#if defined(_WIN32)
        PROCESS_MEMORY_COUNTERS_EX counters{};
        counters.cb = sizeof(counters);
        if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
            private_bytes = static_cast<std::uint64_t>(counters.PrivateUsage);
#endif
        godot::UtilityFunctions::print("EAWR load_mark ", label, " private_bytes=", static_cast<std::int64_t>(private_bytes),
            " cache_retained_bytes=", static_cast<std::int64_t>(cache_retained_bytes),
            " unix_us=", static_cast<std::int64_t>(unix_us()));
    }
    void capture() {
        if (!enabled()) return;
        auto* performance = godot::Performance::get_singleton();
        if (!performance) return;
        captured_ = true;
        unix_us_ = unix_us();
        engine_debug_ = godot::OS::get_singleton()->is_debug_build();
        engine_editor_ = godot::OS::get_singleton()->has_feature("editor");
        // Engine diagnostic only: observe the option; never enable it here.
        extra_gpu_tracking_ = godot::OS::get_singleton()->get_cmdline_args().has("--extra-gpu-memory-tracking");
        static_bytes_ = static_cast<std::uint64_t>(performance->get_monitor(godot::Performance::MEMORY_STATIC));
        video_bytes_ = static_cast<std::uint64_t>(performance->get_monitor(godot::Performance::RENDER_VIDEO_MEM_USED));
        texture_bytes_ = static_cast<std::uint64_t>(performance->get_monitor(godot::Performance::RENDER_TEXTURE_MEM_USED));
        buffer_bytes_ = static_cast<std::uint64_t>(performance->get_monitor(godot::Performance::RENDER_BUFFER_MEM_USED));
        capture_heaps();
        if (auto* rendering = godot::RenderingServer::get_singleton()) {
            {
                std::lock_guard lock(device_mutex_);
                if (next_slot_ == devices_.size()) return;
                slot_ = static_cast<std::int64_t>(next_slot_++);
            }
            // No wait on the main thread; the report distinguishes pending
            // from unsupported and zero. Slots outlive every queued callback.
            rendering->call_on_render_thread(callable_mp_static(&capture_device).bind(slot_));
        }
    }
    void write(std::ostream& output) const {
        if (!captured_) { output << "null"; return; }
        output << "{\"units\":\"bytes\",\"scope\":\"overlapping_monitors_not_private_ownership\""
            << ",\"unix_us\":" << unix_us_
            << ",\"engine_debug\":" << (engine_debug_ ? "true" : "false")
            << ",\"engine_editor\":" << (engine_editor_ ? "true" : "false")
            << ",\"extra_gpu_tracking\":" << (extra_gpu_tracking_ ? "true" : "false")
            << ",\"extra_gpu_tracking_scope\":\"diagnostic_only\""
            << ",\"memory_static_release_may_be_unavailable\":" << static_bytes_
            << ",\"render_video\":" << video_bytes_ << ",\"render_texture\":" << texture_bytes_
            << ",\"render_buffer\":" << buffer_bytes_ << ",\"rd\":";
        std::lock_guard lock(device_mutex_);
        if (slot_ < 0) output << "{\"status\":\"not_requested\"}";
        else {
            const auto& device = devices_[static_cast<std::size_t>(slot_)];
            output << "{\"status\":\"" << (device.ready ? (device.available ? "complete" : "unsupported") : "pending")
                << "\",\"texture\":" << device.texture << ",\"buffer\":" << device.buffer
                << ",\"total\":" << device.total << ",\"unix_us\":" << device.unix_us
                << ",\"driver_host_bytes\":" << device.driver_host << ",\"driver_host_allocations\":" << device.driver_allocations
                << ",\"device_report_bytes\":" << device.device_report << ",\"device_report_allocations\":" << device.device_allocations
                << ",\"tracked_types\":" << device.type_count << ",\"objects\":[";
            for (std::size_t index = 0; index < device.objects.size(); ++index) {
                const auto& item = device.objects[index];
                output << (index ? "," : "") << "{\"name\":" << std::quoted(item.name)
                    << ",\"driver_host_bytes\":" << item.driver_host << ",\"device_report_bytes\":" << item.device << '}';
            }
            output << "]}";
        }
        output << ",\"heaps\":{\"enumerated\":" << heap_count_ << ",\"captured\":" << heaps_captured_
            << ",\"summary_supported\":" << (heap_summary_supported_ ? "true" : "false")
            << ",\"rows\":[";
        for (std::size_t index = 0; index < heaps_captured_; ++index) {
            const auto& heap = heaps_[index];
            output << (index ? "," : "") << "{\"handle\":" << heap.handle
                << ",\"allocation_base\":" << heap.base << ",\"type\":" << heap.type
                << ",\"summary_ok\":" << (heap.summary_ok ? "true" : "false")
                << ",\"summary_error\":" << heap.error << ",\"allocated\":" << heap.allocated
                << ",\"committed\":" << heap.committed << ",\"reserved\":" << heap.reserved << '}';
        }
        output << "]}}";
    }
private:
    struct Object final {
        std::string name;
        std::uint64_t driver_host{}, device{};
    };
    struct Device final {
        std::uint64_t texture, buffer, total, unix_us;
        std::uint64_t driver_host, driver_allocations, device_report, device_allocations, type_count;
        std::vector<Object> objects;
        bool ready, available;
    };
    static void capture_device(const std::int64_t slot) {
        Device sample{};
        sample.unix_us = unix_us();
        if (auto* rendering = godot::RenderingServer::get_singleton()) {
            if (auto* device = rendering->get_rendering_device()) {
                sample.texture = device->get_memory_usage(godot::RenderingDevice::MEMORY_TEXTURES);
                sample.buffer = device->get_memory_usage(godot::RenderingDevice::MEMORY_BUFFERS);
                sample.total = device->get_memory_usage(godot::RenderingDevice::MEMORY_TOTAL);
                sample.driver_host = device->get_driver_total_memory();
                sample.driver_allocations = device->get_driver_allocation_count();
                sample.device_report = device->get_device_total_memory();
                sample.device_allocations = device->get_device_allocation_count();
                sample.type_count = device->get_tracked_object_type_count();
                for (std::uint32_t index = 0; index < sample.type_count && index < 64; ++index) {
                    const auto name = device->get_tracked_object_name(index).utf8();
                    sample.objects.push_back({name.get_data(), device->get_driver_memory_by_object_type(index),
                        device->get_device_memory_by_object_type(index)});
                }
                sample.available = true;
            }
        }
        sample.ready = true;
        std::lock_guard lock(device_mutex_);
        devices_[static_cast<std::size_t>(slot)] = std::move(sample);
    }
    inline static std::mutex device_mutex_;
    inline static std::array<Device, 48> devices_{};
    inline static std::size_t next_slot_{};
    struct Heap final {
        std::uint64_t handle{}, base{}, allocated{}, committed{}, reserved{};
        std::uint32_t type{}, error{};
        bool summary_ok{};
    };
    void capture_heaps() {
#if defined(_WIN32)
        std::array<HANDLE, 128> handles{};
        heap_count_ = GetProcessHeaps(static_cast<DWORD>(handles.size()), handles.data());
        if (heap_count_ > handles.size()) return;
        // Windows versions without HeapSummary still report handles/type/base.
        const auto module = GetModuleHandleW(L"kernel32.dll");
        using Summary = BOOL (WINAPI*)(HANDLE, DWORD, LPHEAP_SUMMARY);
        const auto summary = reinterpret_cast<Summary>(reinterpret_cast<void*>(GetProcAddress(module, "HeapSummary")));
        heap_summary_supported_ = summary != nullptr;
        const auto began = std::chrono::steady_clock::now();
        for (std::size_t index = 0; index < heap_count_ && index < handles.size(); ++index) {
            if (std::chrono::steady_clock::now() - began > std::chrono::seconds(1)) break;
            auto& heap = heaps_[heaps_captured_++];
            heap.handle = reinterpret_cast<std::uintptr_t>(handles[index]);
            MEMORY_BASIC_INFORMATION region{};
            if (VirtualQuery(handles[index], &region, sizeof(region)))
                heap.base = reinterpret_cast<std::uintptr_t>(region.AllocationBase);
            ULONG type{};
            if (HeapQueryInformation(handles[index], HeapCompatibilityInformation, &type, sizeof(type), nullptr))
                heap.type = type;
            HEAP_SUMMARY value{};
            value.cb = static_cast<DWORD>(sizeof(value));
            if (summary && summary(handles[index], 0, &value)) {
                heap.summary_ok = true;
                heap.allocated = value.cbAllocated;
                heap.committed = value.cbCommitted;
                heap.reserved = value.cbReserved;
            } else heap.error = summary ? GetLastError() : ERROR_CALL_NOT_IMPLEMENTED;
        }
#endif
    }
    static std::uint64_t unix_us() {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    }
    std::uint64_t static_bytes_{}, video_bytes_{}, texture_bytes_{}, buffer_bytes_{}, unix_us_{};
    std::array<Heap, 128> heaps_{};
    std::size_t heap_count_{}, heaps_captured_{};
    bool heap_summary_supported_{};
    bool engine_debug_{}, engine_editor_{}, extra_gpu_tracking_{};
    std::int64_t slot_{-1};
    bool captured_{};
};

} // namespace eawr::presentation::godot_backend
