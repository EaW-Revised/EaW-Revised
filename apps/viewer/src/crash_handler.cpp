#include "crash_handler.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <strsafe.h>

namespace eawr::presentation::godot_backend {
namespace {
constexpr DWORD path_capacity = 32768;
struct CrashState {
    wchar_t log[path_capacity]{}, directory[path_capacity]{}, dump[path_capacity]{}, report[path_capacity]{};
    char build[256]{}, home[4096]{}, tail[16384]{}, text[32768]{};
    DWORD home_size{}, thread_id{};
    HANDLE requested{}, finished{}, worker{};
    volatile LONG claimed{}, stopping{};
    EXCEPTION_RECORD record{};
    CONTEXT context{};
    EXCEPTION_POINTERS pointers{&record, &context};
    LPTOP_LEVEL_EXCEPTION_FILTER previous{};
} state;

// Fixed-size byte writer: no formatting library, locale, heap, or Godot calls.
struct Text {
    char* data;
    DWORD size{}, capacity;
    void byte(char value) noexcept { if (size < capacity) data[size++] = value; }
    void literal(const char* value) noexcept { for (; *value; ++value) byte(*value); }
    void hex(ULONG64 value) noexcept {
        literal("0x");
        bool started = false;
        for (int shift = 60; shift >= 0; shift -= 4) {
            const auto digit = static_cast<unsigned>((value >> shift) & 15);
            if (digit || started || !shift) { byte("0123456789abcdef"[digit]); started = true; }
        }
    }
};

char folded(char value) noexcept {
    if (value == '\\') return '/';
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}
void redacted(Text& out, const char* input, DWORD count) noexcept {
    for (DWORD at = 0; at < count;) {
        bool match = state.home_size != 0;
        bool partial = false;
        DWORD end = at;
        for (DWORD index = 0; match && index < state.home_size; ++index) {
            // A concurrent log write can leave the tail ending inside a home
            // path. Hold back that prefix just as we redact a complete path.
            if (end == count) { partial = index != 0; match = false; break; }
            match = folded(input[end]) == folded(state.home[index]);
            if (match) {
                ++end;
                // JSON logs may escape each backslash. Consume either spelling.
                if (state.home[index] == '\\' && end < count && input[end - 1] == '\\' && input[end] == '\\') ++end;
            }
        }
        if (match || partial) { out.literal("<home>"); at = end; }
        else { const char value = input[at++]; out.byte(value ? value : '?'); }
    }
}
void write_bytes(HANDLE file, const char* bytes, DWORD count) noexcept {
    while (file != INVALID_HANDLE_VALUE && count) {
        DWORD written{};
        if (!WriteFile(file, bytes, count, &written, nullptr) || !written) break;
        bytes += written; count -= written;
    }
}
void frame(Text& out, DWORD64 address) noexcept {
    MEMORY_BASIC_INFORMATION memory{};
    wchar_t module[path_capacity]{};
    out.hex(address); out.literal(" ");
    if (VirtualQuery(reinterpret_cast<void*>(address), &memory, sizeof(memory))
        && GetModuleFileNameW(static_cast<HMODULE>(memory.AllocationBase), module, path_capacity)) {
        const wchar_t* name = module;
        for (const wchar_t* at = module; *at; ++at) if (*at == '\\' || *at == '/') name = at + 1;
        for (; *name; ++name) out.byte(*name < 128 ? static_cast<char>(*name) : '?');
        out.literal("+"); out.hex(address - reinterpret_cast<DWORD64>(memory.AllocationBase));
    } else out.literal("<unknown module>");
    out.literal("\r\n");
}

void save_crash() noexcept {
    // Persist the original failure before entering DbgHelp. Even if the OS dump
    // writer cannot finish in a damaged process, this evidence survives.
    Text first{state.text, 0, static_cast<DWORD>(sizeof(state.text))};
    first.literal("EAWR crash report\r\nBuild: "); first.literal(state.build);
    first.literal("\r\nException: "); first.hex(state.record.ExceptionCode);
    first.literal("\r\nAddress: "); first.hex(reinterpret_cast<ULONG64>(state.record.ExceptionAddress));
    first.literal("\r\nDump writer started; files remain local.\r\n");
    HANDLE initial = CreateFileW(state.report, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    write_bytes(initial, first.data, first.size);
    if (initial != INVALID_HANDLE_VALUE) { FlushFileBuffers(initial); CloseHandle(initial); }
    SYSTEMTIME time{}; GetSystemTime(&time);
    // The filename includes the build identity and a UTC file-time plus PID.
    FILETIME stamp{}; SystemTimeToFileTime(&time, &stamp);
    Text name{state.text, 0, 1024};
    name.literal("crash-");
    // Keep the filename component within 255 characters even for a maximally
    // sized identity; the report retains the complete bounded build identity.
    for (DWORD index = 0; index < 215 && state.build[index]; ++index) name.byte(state.build[index]);
    name.literal("-");
    name.hex((static_cast<ULONG64>(stamp.dwHighDateTime) << 32) | stamp.dwLowDateTime);
    name.literal("-"); name.hex(GetCurrentProcessId()); name.literal(".dmp"); name.byte(0);
    if (FAILED(StringCchCopyW(state.dump, path_capacity, state.directory))) return;
    const int prefix = lstrlenW(state.dump);
    MultiByteToWideChar(CP_UTF8, 0, state.text, -1, state.dump + prefix, static_cast<int>(path_capacity) - prefix);
    HANDLE dump = CreateFileW(state.dump, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD dump_error{};
    if (dump != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION exception{state.thread_id, &state.pointers, FALSE};
        if (!MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump,
                MiniDumpNormal, &exception, nullptr, nullptr)) dump_error = GetLastError();
        FlushFileBuffers(dump); CloseHandle(dump);
    } else dump_error = GetLastError();

    Text out{state.text, 0, static_cast<DWORD>(sizeof(state.text))};
    out.literal("EAWR crash report\r\nBuild: "); out.literal(state.build);
    out.literal("\r\nException: "); out.hex(state.record.ExceptionCode);
    out.literal("\r\nAddress: "); out.hex(reinterpret_cast<ULONG64>(state.record.ExceptionAddress));
    out.literal("\r\nDump error (0 = saved): "); out.hex(dump_error);
    out.literal("\r\nFiles stay on this computer; share only if you choose.\r\nFrames (module+offset):\r\n");
    frame(out, reinterpret_cast<DWORD64>(state.record.ExceptionAddress));
    // Modules can have loaded since healthy-startup SymInitialize.
    SymRefreshModuleList(GetCurrentProcess());
#if defined(_M_X64) || defined(__x86_64__)
    STACKFRAME64 stack{};
    CONTEXT context = state.context;
    stack.AddrPC = {context.Rip, 0, AddrModeFlat};
    stack.AddrStack = {context.Rsp, 0, AddrModeFlat};
    stack.AddrFrame = {context.Rbp, 0, AddrModeFlat};
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, state.thread_id);
    if (thread) {
        for (unsigned index = 0; index < 31; ++index) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), thread, &stack,
                    &context, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || !stack.AddrPC.Offset) break;
            frame(out, stack.AddrPC.Offset);
        }
        CloseHandle(thread);
    }
#endif
    out.literal("\r\nLast log bytes (home redacted):\r\n");
    HANDLE log = state.home_size ? CreateFileW(state.log, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) : INVALID_HANDLE_VALUE;
    if (log != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size{}, offset{};
        if (GetFileSizeEx(log, &size)) {
            const auto capacity = static_cast<LONGLONG>(sizeof(state.tail));
            offset.QuadPart = size.QuadPart > capacity ? size.QuadPart - capacity : 0;
            if (SetFilePointerEx(log, offset, nullptr, FILE_BEGIN)) {
                DWORD read{};
                if (ReadFile(log, state.tail, sizeof(state.tail), &read, nullptr)) {
                    // Drop the partial first line: a truncated home prefix cannot leak.
                    DWORD begin{};
                    if (offset.QuadPart) { while (begin < read && state.tail[begin++] != '\n') {} }
                    redacted(out, state.tail + begin, read - begin);
                }
            }
        }
        CloseHandle(log);
    }
    HANDLE report = CreateFileW(state.report, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    write_bytes(report, out.data, out.size);
    if (report != INVALID_HANDLE_VALUE) { FlushFileBuffers(report); CloseHandle(report); }

    Text notice{state.text, 0, static_cast<DWORD>(sizeof(state.text))};
    notice.literal("\r\nEAWR crash files (local only): ");
    const int count = WideCharToMultiByte(CP_UTF8, 0, state.dump, -1, state.tail,
        sizeof(state.tail), nullptr, nullptr);
    if (count > 0 && state.home_size) redacted(notice, state.tail, static_cast<DWORD>(count - 1));
    else notice.literal("beside the active Godot log");
    notice.literal("; crash-report.txt beside the dump\r\n");
    log = CreateFileW(state.log, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    write_bytes(log, notice.data, notice.size);
    if (log != INVALID_HANDLE_VALUE) { FlushFileBuffers(log); CloseHandle(log); }
}
DWORD WINAPI dump_worker(void*) noexcept {
    WaitForSingleObject(state.requested, INFINITE);
    if (!state.stopping) save_crash();
    SetEvent(state.finished);
    return 0;
}
LONG WINAPI crash_filter(EXCEPTION_POINTERS* pointers) noexcept {
    if (InterlockedCompareExchange(&state.claimed, 1, 0)) return EXCEPTION_EXECUTE_HANDLER;
    state.thread_id = GetCurrentThreadId();
    state.record = *pointers->ExceptionRecord;
    state.record.ExceptionRecord = nullptr;
    state.context = *pointers->ContextRecord;
    SetEvent(state.requested);
    WaitForSingleObject(state.finished, 20000);
    return EXCEPTION_EXECUTE_HANDLER;
}
}

bool install_crash_handler(const wchar_t* log_path, const char* build_id) noexcept {
    if (state.worker) return true;
    if (!log_path || !build_id) return false;
    size_t path_size{};
    if (FAILED(StringCchLengthW(log_path, path_capacity - 1200, &path_size))) return false;
    const DWORD full_size = GetFullPathNameW(log_path, path_capacity - 1200, state.directory, nullptr);
    if (!full_size || full_size >= path_capacity - 1200) return false;
    for (wchar_t* at = state.directory; *at; ++at) if (*at == L'/') *at = L'\\';
    // Every file operation uses an extended absolute path. The reserved 1200
    // characters cover its prefix and the longest bounded build/dump name,
    // independently of MAX_PATH and the process long-path manifest/policy.
    const bool extended = state.directory[0] == L'\\' && state.directory[1] == L'\\'
        && state.directory[2] == L'?' && state.directory[3] == L'\\';
    if (extended) {
        if (FAILED(StringCchCopyW(state.log, path_capacity, state.directory))) return false;
    } else {
        const bool unc = state.directory[0] == L'\\' && state.directory[1] == L'\\';
        if (FAILED(StringCchCopyW(state.log, path_capacity, unc ? L"\\\\?\\UNC\\" : L"\\\\?\\"))
            || FAILED(StringCchCatW(state.log, path_capacity, state.directory + (unc ? 2 : 0)))) return false;
    }
    if (FAILED(StringCchCopyW(state.directory, path_capacity, state.log))) return false;
    wchar_t* last{};
    for (wchar_t* at = state.directory; *at; ++at) if (*at == '\\' || *at == '/') last = at;
    if (!last) return false;
    last[1] = 0;
    // The log directory normally exists already; also support the crash test
    // before Godot writes its first line.
    CreateDirectoryW(state.directory, nullptr);
    if (FAILED(StringCchCopyW(state.report, path_capacity, state.directory))
        || FAILED(StringCchCatW(state.report, path_capacity, L"crash-report.txt"))) return false;
    unsigned count{};
    for (; build_id[count] && count < sizeof(state.build) - 1; ++count) {
        const char value = build_id[count];
        state.build[count] = ((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z')
            || (value >= '0' && value <= '9') || value == '.' || value == '+' || value == '-' || value == '_') ? value : '_';
    }
    state.build[count] = 0;
    wchar_t home[4096]{};
    const DWORD length = GetEnvironmentVariableW(L"USERPROFILE", home, 4096);
    if (length && length < 4096) {
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, home, -1, state.home, sizeof(state.home), nullptr, nullptr);
        if (bytes > 0) state.home_size = static_cast<DWORD>(bytes - 1);
    }
    state.claimed = 0; state.stopping = 0;
    state.requested = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    state.finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    // All DbgHelp calls are confined to this worker; initialize while healthy.
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
    SymInitialize(GetCurrentProcess(), "", TRUE);
    if (state.requested && state.finished) state.worker = CreateThread(nullptr, 1024 * 1024, dump_worker, nullptr, 0, nullptr);
    if (!state.worker) { uninstall_crash_handler(); return false; }
    state.previous = SetUnhandledExceptionFilter(crash_filter);
    return true;
}
void uninstall_crash_handler() noexcept {
    if (state.worker) {
        SetUnhandledExceptionFilter(state.previous);
        InterlockedExchange(&state.stopping, 1);
        SetEvent(state.requested);
        WaitForSingleObject(state.worker, INFINITE);
        CloseHandle(state.worker); state.worker = nullptr;
    }
    if (state.requested) { CloseHandle(state.requested); state.requested = nullptr; }
    if (state.finished) { CloseHandle(state.finished); state.finished = nullptr; }
    SymCleanup(GetCurrentProcess());
}
void deliberate_test_crash() noexcept { RaiseException(0xE0424157, EXCEPTION_NONCONTINUABLE, 0, nullptr); }
}
#else
namespace eawr::presentation::godot_backend {
// TODO: reserve a signal-safe handoff and platform dump writer for Linux/macOS.
bool install_crash_handler(const wchar_t*, const char*) noexcept { return false; }
void uninstall_crash_handler() noexcept {}
void deliberate_test_crash() noexcept {}
}
#endif
