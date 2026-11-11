#include "eawr/core/host_probe.hpp"

#include <chrono>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>
#endif

namespace eawr::core::host {

std::int64_t monotonic_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::uint64_t physical_memory_bytes() noexcept {
#ifdef _WIN32
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) return memory.ullTotalPhys;
#endif
    return 0;
}

std::optional<bool> volume_seek_penalty(const wchar_t drive_letter) noexcept {
    std::optional<bool> answer; // empty unless the query answered
#ifdef _WIN32
    try {
        const std::wstring device = std::wstring(L"\\\\.\\") + drive_letter + L':';
        const HANDLE handle = CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            STORAGE_PROPERTY_QUERY query{};
            query.PropertyId = StorageDeviceSeekPenaltyProperty;
            query.QueryType = PropertyStandardQuery;
            DEVICE_SEEK_PENALTY_DESCRIPTOR descriptor{};
            DWORD returned = 0;
            const BOOL ok = DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), &descriptor,
                sizeof(descriptor), &returned, nullptr);
            CloseHandle(handle);
            if (ok && returned >= sizeof(descriptor)) answer = descriptor.IncursSeekPenalty != FALSE;
        }
    } catch (...) {
        answer.reset(); // the device name allocation failed: unknown
    }
#else
    (void)drive_letter;
#endif
    return answer;
}

} // namespace eawr::core::host
