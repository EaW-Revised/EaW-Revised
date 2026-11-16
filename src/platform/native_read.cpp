#include "eawr/platform/native_read.hpp"

#include <fstream>
#include <string>
#include <system_error>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#endif

namespace eawr::platform {

NativeReadResult read_native_chunks(const std::filesystem::path& file, const std::span<std::byte> buffer,
    const std::function<bool(std::span<const std::byte>)>& consume) {
    NativeReadResult result;
    if (buffer.empty() || !consume) return result;
    std::ifstream input(file, std::ios::binary);
    if (!input) return result;
    input.rdbuf()->pubsetbuf(nullptr, 0);   // the caller reads in its own chunks
    while (true) {
        input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const auto got = static_cast<std::size_t>(input.gcount());
        if (got > 0) {
            result.bytes += got;
            if (!consume(buffer.first(got))) {
                result.ok = true;
                return result;
            }
        }
        if (input.eof()) {
            result.ok = true;
            return result;
        }
        if (!input) return result;
    }
}

StorageMedia storage_media_of(const std::filesystem::path& path) {
#ifdef _WIN32
    // The volume the path string names (GetVolumePathNameW works on the string: a root that is itself a junction answers for the
    // junction's own volume, a folder mount point is not a drive letter and stays unknown; the digest's tree walk never enters
    // junctions or mount points, so every file it lists sits on its root's volume), then the seek-penalty property of that
    // device: IncursSeekPenalty is true for a spinning disk and false for flash. Any failure is unknown.
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error);
    if (error) return StorageMedia::unknown;
    wchar_t volume[MAX_PATH + 1] = {};
    if (!GetVolumePathNameW(absolute.c_str(), volume, MAX_PATH)) return StorageMedia::unknown;
    std::wstring device = volume;
    while (!device.empty() && (device.back() == L'\\' || device.back() == L'/')) device.pop_back();
    if (device.size() != 2 || device[1] != L':') return StorageMedia::unknown;   // a drive letter; UNC and other forms stay unknown
    device = L"\\\\.\\" + device;
    const HANDLE handle = CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return StorageMedia::unknown;
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    query.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR descriptor{};
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), &descriptor, sizeof(descriptor), &returned, nullptr);
    CloseHandle(handle);
    if (!ok || returned < sizeof(descriptor)) return StorageMedia::unknown;
    return descriptor.IncursSeekPenalty ? StorageMedia::rotational : StorageMedia::solid_state;
#else
    static_cast<void>(path);
    return StorageMedia::unknown;
#endif
}

std::uint64_t first_physical_offset(const std::filesystem::path& file) {
#ifdef _WIN32
    const HANDLE handle = CreateFileW(file.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return 0;
    STARTING_VCN_INPUT_BUFFER start{};   // VCN 0: the first extent
    alignas(8) unsigned char out[256] = {};
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(handle, FSCTL_GET_RETRIEVAL_POINTERS, &start, sizeof(start), out, sizeof(out), &returned, nullptr);
    const DWORD error = ok ? 0 : GetLastError();
    CloseHandle(handle);
    if (!ok && error != ERROR_MORE_DATA) return 0;   // more data: the first extents are still filled in
    const auto* buffer = reinterpret_cast<const RETRIEVAL_POINTERS_BUFFER*>(out);
    if (buffer->ExtentCount == 0 || buffer->Extents[0].Lcn.QuadPart < 0) return 0;   // -1: not allocated on disk
    return static_cast<std::uint64_t>(buffer->Extents[0].Lcn.QuadPart);
#else
    static_cast<void>(file);
    return 0;
#endif
}

} // namespace eawr::platform
