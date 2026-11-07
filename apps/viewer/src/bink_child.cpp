#include "bink_child.hpp"
#include "ui/bink_protocol.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <array>
#include <chrono>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace eawr::presentation::godot_backend {
namespace {
class Handle {
public:
    HANDLE value{};
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
class Attributes {
public:
    std::vector<unsigned char> storage;
    LPPROC_THREAD_ATTRIBUTE_LIST list{};
    ~Attributes() { if (list) DeleteProcThreadAttributeList(list); }
};
class Child final : public BinkDecoder {
public:
    explicit Child(std::shared_ptr<std::atomic_bool> cancelled) : cancelled_(std::move(cancelled)) {
        wchar_t value[2]{};
        trace_ = GetEnvironmentVariableW(L"EAWR_BINK_TRACE", value, 2) == 1 && value[0] == L'1';
    }
    ~Child() override {
        if (job_.value) TerminateJobObject(job_.value, 1);
        if (process_.value) WaitForSingleObject(process_.value, 3000);
        if (exchange_) UnmapViewOfFile(exchange_);
    }
    bool start(const std::filesystem::path& helper, const std::filesystem::path& root,
               const std::vector<std::byte>& source, const BinkMetadata& metadata) {
        if (source.size() > std::numeric_limits<std::uint32_t>::max()) return false;
        std::error_code ec;
        const auto game = std::filesystem::canonical(root, ec);
        if (ec || !game.is_absolute()) return false;
        std::filesystem::path dll;
        for (const auto& candidate : {game / L"corruption" / L"bink2w64.dll", game / L"GameData" / L"bink2w64.dll"}) {
            auto resolved = std::filesystem::canonical(candidate, ec);
            if (ec) { ec.clear(); continue; }
            auto rel = resolved.lexically_relative(game);
            if (rel.empty() || *rel.begin() == L".." || rel.is_absolute()) continue;
            dll = std::move(resolved); break;
        }
        if (dll.empty() || !helper.is_absolute() || !std::filesystem::is_regular_file(helper, ec)) return false;
        const auto output = static_cast<std::uint64_t>(metadata.width) * metadata.height * 4;
        const auto length = sizeof(BinkExchange) + static_cast<std::uint64_t>(source.size()) + output;
        if (length > bink_memory_budget + sizeof(BinkExchange) || length > std::numeric_limits<DWORD>::max()) return false;
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        mapping_.value = CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0, static_cast<DWORD>(length), nullptr);
        request_.value = CreateEventW(&security, FALSE, FALSE, nullptr);
        response_.value = CreateEventW(&security, FALSE, FALSE, nullptr);
        if (!mapping_.value || !request_.value || !response_.value) return false;
        exchange_ = static_cast<BinkExchange*>(MapViewOfFile(mapping_.value, FILE_MAP_ALL_ACCESS, 0, 0, 0));
        if (!exchange_) return false;
        *exchange_ = {}; exchange_->source_size = static_cast<std::uint32_t>(source.size());
        std::memcpy(exchange_ + 1, source.data(), source.size());
        job_.value = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job_.value || !SetInformationJobObject(job_.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) return false;
        Attributes attributes;
        SIZE_T count{};
        InitializeProcThreadAttributeList(nullptr, 1, 0, &count);
        attributes.storage.resize(count);
        attributes.list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.storage.data());
        if (!InitializeProcThreadAttributeList(attributes.list, 1, 0, &count)) { attributes.list = nullptr; return false; }
        std::array<HANDLE, 3> inherited{mapping_.value, request_.value, response_.value};
        if (!UpdateProcThreadAttribute(attributes.list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                inherited.data(), sizeof(inherited), nullptr, nullptr)) return false;
        STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.lpAttributeList = attributes.list;
        PROCESS_INFORMATION child{};
        const auto numeric = [](HANDLE h) { return std::to_wstring(reinterpret_cast<std::uintptr_t>(h)); };
        // Windows paths cannot contain a quote; pass an explicit executable separately.
        auto command = L"\"" + helper.native() + L"\" " + numeric(mapping_.value) + L" " + numeric(request_.value)
            + L" " + numeric(response_.value) + L" \"" + dll.native() + L"\"";
        if (!CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, TRUE,
                CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr,
                &startup.StartupInfo, &child)) return false;
        process_.value = child.hProcess;
        Handle thread; thread.value = child.hThread;
        if (!AssignProcessToJobObject(job_.value, process_.value)) { TerminateProcess(process_.value, 1); return false; }
        if (ResumeThread(thread.value) == static_cast<DWORD>(-1)) return false;
        if (!wait() || exchange_->status != 1 || exchange_->width != metadata.width || exchange_->height != metadata.height
            || exchange_->frames != metadata.frames || exchange_->rate_numerator != metadata.rate_numerator
            || exchange_->rate_denominator != metadata.rate_denominator || exchange_->output_size != output) return false;
        metadata_ = metadata; source_size_ = source.size();
        return true;
    }
    bool next(BinkFrame& frame) override {
        exchange_->status = 0; exchange_->command = 1;
        if (!SetEvent(request_.value) || !wait() || exchange_->status != 1
            || exchange_->output_size != static_cast<std::uint64_t>(metadata_.width) * metadata_.height * 4
            || exchange_->width != metadata_.width || exchange_->height != metadata_.height
            || exchange_->rate_numerator != metadata_.rate_numerator
            || exchange_->rate_denominator != metadata_.rate_denominator) return false;
        frame.width = metadata_.width; frame.height = metadata_.height;
        frame.rate_numerator = metadata_.rate_numerator; frame.rate_denominator = metadata_.rate_denominator;
        if (trace_) {
            frame.selected_frame = exchange_->selected_frame; frame.total_frames = metadata_.frames;
            for (std::size_t i = 0; i < 3; ++i) frame.trace_first[i] = exchange_->trace_first[i];
            frame.trace_last = exchange_->trace_last;
        }
        const auto* begin = reinterpret_cast<const std::uint8_t*>(exchange_ + 1) + source_size_;
        frame.rgba.assign(begin, begin + exchange_->output_size);
        return true;
    }
private:
    bool wait() const {
        const std::array<HANDLE, 2> handles{response_.value, process_.value};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!cancelled_->load() && std::chrono::steady_clock::now() < deadline) {
            const auto result = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, 25);
            if (result == WAIT_OBJECT_0) return true;
            if (result != WAIT_TIMEOUT) return false;
        }
        return false;
    }
    Handle mapping_, request_, response_, job_, process_;
    BinkExchange* exchange_{};
    BinkMetadata metadata_{};
    std::size_t source_size_{};
    bool trace_{};
    std::shared_ptr<std::atomic_bool> cancelled_;
};
}
std::unique_ptr<BinkDecoder> open_bink_child(const std::filesystem::path& helper,
    const std::filesystem::path& game_root, std::vector<std::byte> source,
    std::shared_ptr<std::atomic_bool> cancelled) {
    BinkMetadata metadata;
    if (!admit_bink(source, bink_memory_budget, metadata)) return {};
    auto child = std::make_unique<Child>(std::move(cancelled));
    if (!child->start(helper, game_root, source, metadata)) return {};
    return child;
}
} // namespace eawr::presentation::godot_backend
#else
namespace eawr::presentation::godot_backend {
std::unique_ptr<BinkDecoder> open_bink_child(const std::filesystem::path&,
    const std::filesystem::path&, std::vector<std::byte>, std::shared_ptr<std::atomic_bool>) { return {}; }
}
#endif
