#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "../../src/presentation/godot/ui/bink_protocol.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
using namespace eawr::presentation::godot_backend;
int wmain(int argc, wchar_t** argv) {
    if (argc != 5) return 2;
    auto mapping = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(_wcstoui64(argv[1], nullptr, 10)));
    auto request = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(_wcstoui64(argv[2], nullptr, 10)));
    auto response = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(_wcstoui64(argv[3], nullptr, 10)));
    auto* e = static_cast<BinkExchange*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    if (!e) return 3;
    auto* source = reinterpret_cast<std::uint8_t*>(e + 1);
    const auto mode = source[e->source_size - 1];
    if (mode == 1) {
        SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
        RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0, nullptr);
        return 4;
    }
    if (mode == 2) { Sleep(60000); return 5; } // simulated startup hang
    e->width = 2; e->height = 3; e->frames = 1; e->rate_numerator = 30; e->rate_denominator = 1;
    e->output_size = 24; e->status = 1;
    if (mode == 4) e->width = 3;
    SetEvent(response);
    while (WaitForSingleObject(request, INFINITE) == WAIT_OBJECT_0) {
        if (mode == 3) { Sleep(60000); break; } // simulated frame hang
        if (mode == 7) break; // truncated response: child exits without signalling
        std::memset(source + e->source_size, 42, 24);
        e->status = mode == 6 ? 2 : 1;
        if (mode == 5) e->output_size = 23;
        SetEvent(response);
    }
    UnmapViewOfFile(e);
    return 0;
}
