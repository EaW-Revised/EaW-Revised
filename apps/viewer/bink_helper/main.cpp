#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "../../../src/presentation/godot/ui/bink_movie.hpp"
#include "../../../src/presentation/godot/ui/bink_protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>

using namespace eawr::presentation::godot_backend;
namespace {
using Open = void* (*)(const void*, std::uint32_t);
using Error = const char* (*)();
using Decode = std::uint32_t (*)(void*);
using Copy = std::uint32_t (*)(void*, void*, std::int32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t);
using Next = void (*)(void*);
using Close = void (*)(void*);
// Verified seven-word handle prefix: word 3 is the one-based selected frame.
constexpr std::size_t selected_frame_offset = 3 * sizeof(std::uint32_t);
template<class T> T lookup(HMODULE module, const char* name) {
    const FARPROC raw = GetProcAddress(module, name);
    static_assert(sizeof(T) == sizeof(raw));
    T result{};
    std::memcpy(&result, &raw, sizeof(result));
    return result;
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 5) return 2;
    const auto mapping = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(_wcstoui64(argv[1], nullptr, 10)));
    const auto request = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(_wcstoui64(argv[2], nullptr, 10)));
    const auto response = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(_wcstoui64(argv[3], nullptr, 10)));
    auto* exchange = static_cast<BinkExchange*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    if (!exchange) return 3;
    // The parent creates the mapping, but treat every field as untrusted anyway.
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(exchange, &region, sizeof(region)) || region.RegionSize < sizeof(BinkExchange)
        || exchange->version != 1 || exchange->source_size > bink_memory_budget
        || static_cast<std::uint64_t>(sizeof(BinkExchange)) + exchange->source_size > region.RegionSize) {
        UnmapViewOfFile(exchange); return 4;
    }
    auto* source = reinterpret_cast<std::byte*>(exchange + 1);
    BinkMetadata metadata;
    if (!admit_bink(std::span<const std::byte>(source, exchange->source_size), bink_memory_budget, metadata)) {
        exchange->status = 2; SetEvent(response); UnmapViewOfFile(exchange); return 5;
    }
    const std::uint64_t output_size = static_cast<std::uint64_t>(metadata.width) * metadata.height * 4;
    if (sizeof(BinkExchange) + static_cast<std::uint64_t>(exchange->source_size) + output_size > region.RegionSize
        || output_size > std::numeric_limits<std::uint32_t>::max()) {
        exchange->status = 2; SetEvent(response); UnmapViewOfFile(exchange); return 6;
    }
    HMODULE module = LoadLibraryExW(argv[4], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) { exchange->status = 2; SetEvent(response); UnmapViewOfFile(exchange); return 7; }
    const auto open = lookup<Open>(module, "BinkOpen");
    const auto error = lookup<Error>(module, "BinkGetError");
    const auto decode = lookup<Decode>(module, "BinkDoFrame");
    const auto copy = lookup<Copy>(module, "BinkCopyToBuffer");
    const auto next = lookup<Next>(module, "BinkNextFrame");
    const auto close = lookup<Close>(module, "BinkClose");
    if (!open || !error || !decode || !copy || !next || !close) {
        exchange->status = 2; SetEvent(response); FreeLibrary(module); UnmapViewOfFile(exchange); return 8;
    }
    void* movie = open(source, 0x04100000U);
    if (!movie) { exchange->status = 2; SetEvent(response); FreeLibrary(module); UnmapViewOfFile(exchange); return 9; }
    std::uint32_t native_header[7]{};
    std::memcpy(native_header, movie, sizeof(native_header));
    if (native_header[0] != metadata.width || native_header[1] != metadata.height || native_header[2] != metadata.frames
        || native_header[5] != metadata.rate_numerator || native_header[6] != metadata.rate_denominator) {
        exchange->status = 2; SetEvent(response); close(movie); FreeLibrary(module); UnmapViewOfFile(exchange); return 10;
    }
    exchange->width = metadata.width; exchange->height = metadata.height; exchange->frames = metadata.frames;
    exchange->rate_numerator = metadata.rate_numerator; exchange->rate_denominator = metadata.rate_denominator;
    exchange->output_size = static_cast<std::uint32_t>(output_size);
    auto* output = source + exchange->source_size;
    wchar_t trace_value[2]{};
    const bool trace = GetEnvironmentVariableW(L"EAWR_BINK_TRACE", trace_value, 2) == 1 && trace_value[0] == L'1';
    bool first = true;
    exchange->status = 1; SetEvent(response);
    while (WaitForSingleObject(request, INFINITE) == WAIT_OBJECT_0) {
        if (exchange->command == 2) break;
        if (exchange->command != 1) { exchange->status = 2; SetEvent(response); break; }
        if (!first) next(movie);
        first = false;
        // Check the selected frame after decode; the verified prefix is one-based.
        const auto decode_result = decode(movie);
        std::uint32_t selected{};
        std::memcpy(&selected, static_cast<const std::byte*>(movie) + selected_frame_offset, sizeof(selected));
        if (selected == 0 || selected > metadata.frames || decode_result != 0
            || copy(movie, output, static_cast<std::int32_t>(metadata.width * 4U), metadata.height, 0, 0, 4) != 0) {
            exchange->status = 2; SetEvent(response); break;
        }
        if (trace) {
            exchange->selected_frame = selected;
            if (selected <= 3) exchange->trace_first[selected - 1] = selected;
            if (selected == metadata.frames) exchange->trace_last = selected;
        }
        exchange->status = 1; SetEvent(response);
    }
    close(movie); FreeLibrary(module); UnmapViewOfFile(exchange);
    return 0;
}
