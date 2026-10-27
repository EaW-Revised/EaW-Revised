#include "loadcheck.hpp"
#include "model_methods.hpp"
#include "eawr/mods/profile.hpp"
#include "eawr/mods/fingerprint.hpp"
#include "eawr/core/json_text.hpp"
#include "eawr/core/sha256.hpp"
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <stdexcept>
#include <algorithm>
#include <charconv>
#include <array>
#include <windows.h>

namespace eawr::mods_service {
namespace {
namespace fs = std::filesystem;
using J = mods::steam::JsonValue;
using core::json_quote;
std::string encode(const J& v) {
    switch (v.kind) {
    case J::Kind::null: return "null";
    case J::Kind::boolean: return v.boolean ? "true" : "false";
    case J::Kind::number: {
        std::array<char, 64> buffer{};
        const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), v.number);
        if (error != std::errc{}) throw std::runtime_error("cannot encode JSON number");
        return std::string(buffer.data(), end);
    }
    case J::Kind::string: return json_quote(v.string);
    case J::Kind::array: {
        std::string out = "[";
        for (const auto& item : v.array) { if (out.size() > 1) out += ','; out += encode(item); }
        return out + ']';
    }
    case J::Kind::object: {
        std::string out = "{";
        for (std::size_t i = 0; i < v.keys.size(); ++i) {
            if (i) out += ',';
            out += json_quote(v.keys[i]) + ':' + encode(v.values[i]);
        }
        return out + '}';
    }
    }
    return "null";
}
// Every directory we write must resolve beneath the chosen user-data root without redirects.
fs::path safe_folder(const fs::path& root, std::initializer_list<const char*> parts) {
    std::error_code ec;
    auto folder = fs::weakly_canonical(root, ec);
    if (ec || root.empty()) throw std::runtime_error("user data unavailable");
    for (const auto* part : parts) {
        folder /= part;
        if (fs::weakly_canonical(folder, ec) != folder || ec) throw std::runtime_error("unsafe user data folder");
    }
    fs::create_directories(folder);
    return folder;
}
std::string fingerprint(const mods::Profile& p, const fs::path& game, std::string_view engine) {
    std::string material(engine);
    const auto append = [&](const fs::path& root) {
        const auto tree = mods::tree_fingerprint(root);
        if (tree.capped || tree.incomplete || tree.tree.empty()) throw std::runtime_error("chain fingerprint unavailable");
        material += '\n' + mods::steam::path_to_utf8(root) + '\n' + tree.tree;
    };
    for (const auto& layer : p.layers) { material += '\n' + layer.key; append(mods::steam::path_from_utf8(layer.root)); }
    append(game / "corruption/Data"); append(game / "GameData/Data");
    return core::sha256_hex(std::span(reinterpret_cast<const std::uint8_t*>(material.data()), material.size()));
}
struct Input { mods::Profile profile; fs::path cache; };
std::optional<Input> input(Service& s, const J& params, Reply& error) {
    const auto* id = params.string_member("profile");
    if (!id) { error = Reply::fail(kInvalidParams, "profile must be an id"); return {}; }
    // Reuse the profile method's path guards and key re-resolution.
    J args; args.kind = J::Kind::object; args.keys = {"id"};
    J value; value.kind = J::Kind::string; value.string = *id; args.values = {value};
    auto loaded = method_profile_load(s, args);
    if (!loaded.error_code.empty()) { error = std::move(loaded); return {}; }
    auto doc = mods::steam::parse_json(loaded.result);
    const auto* p = doc ? doc->member("profile") : nullptr;
    auto parsed = p ? mods::profile_from_json(encode(*p)) : mods::ProfileResult{};
    if (!parsed.profile) { error = Reply::fail(kInvalidParams, "profile unavailable"); return {}; }
    Input out;
    out.profile = std::move(*parsed.profile);
    out.cache = safe_folder(s.model.user_data, {"mods", "cache", "loadcheck"});
    return out;
}
std::wstring quote(const fs::path& path) {
    // Windows argv escaping, including backslashes before a closing quote.
    std::wstring out = L"\""; unsigned slashes{};
    for (const auto c : path.native()) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'"') out.append(slashes * 2 + 1, L'\\'); else out.append(slashes, L'\\');
        slashes = 0; out += c;
    }
    out.append(slashes * 2, L'\\'); return out + L'"';
}
fs::path helper_path() {
    std::wstring path(32768, L'\0');
    const auto count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(count);
    return fs::path(path).parent_path() / "eawr_loadcheck.exe";
}
std::string error_line(std::uint64_t id, std::string_view code, std::string_view message) {
    return "{\"id\":" + std::to_string(id) + ",\"error\":{\"code\":" + json_quote(code) + ",\"message\":" + json_quote(message)
        + ",\"data\":{\"error_count\":1,\"warning_count\":0,\"diagnostics\":[{\"code\":" + json_quote(code) + ",\"severity\":\"error\",\"count\":1,\"examples\":[" + json_quote(message) + "]}]}}}";
}
bool valid_report(const J& report) {
    const auto* ok = report.member("ok");
    const auto* rows = report.member("diagnostics");
    const auto* requirements = report.member("requirements");
    const auto errors = json_count(report.member("error_count"));
    const auto warnings = json_count(report.member("warning_count"));
    if (!report.string_member("stage") || !report.string_member("content_code") || !ok || ok->kind != J::Kind::boolean
        || !rows || rows->kind != J::Kind::array || !requirements || requirements->kind != J::Kind::array || !errors || !warnings) return false;
    std::uint64_t error_count{}, warning_count{};
    for (const auto& row : rows->array) {
        const auto* examples = row.member("examples");
        const auto* severity = row.string_member("severity");
        const auto count = json_count(row.member("count"));
        if (!row.string_member("code") || !count || !severity || !examples || examples->kind != J::Kind::array || examples->array.size() > 3) return false;
        if (*severity == "error") { if (*count > *errors - error_count) return false; error_count += *count; }
        else if (*severity == "warning") { if (*count > *warnings - warning_count) return false; warning_count += *count; }
        else if (*severity != "info") return false;
        for (const auto& example : examples->array) if (example.kind != J::Kind::string) return false;
    }
    return error_count == *errors && warning_count == *warnings;
}
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
struct Attributes {
    std::vector<unsigned char> bytes;
    LPPROC_THREAD_ATTRIBUTE_LIST list{};
    ~Attributes() { if (list) DeleteProcThreadAttributeList(list); }
};
}
struct LoadCheck::State {
    std::thread worker;
    std::atomic<bool> running{false}, cancelled{false};
    std::uint64_t request{};
    std::mutex mutex;
    HANDLE job{};
};
LoadCheck::LoadCheck(Service& s) : state_(std::make_unique<State>()), service_(s) {}
LoadCheck::~LoadCheck() { stop(); }
void LoadCheck::stop() {
    { std::lock_guard lock(state_->mutex); if (state_->running) state_->cancelled = true; if (state_->job) TerminateJobObject(state_->job, 9); }
    if (state_->worker.joinable()) state_->worker.join();
}
std::optional<Reply> LoadCheck::start(std::uint64_t id, const J& params) {
    if (state_->running) return Reply::fail(kBusy, "a load check is already running");
    if (!emit) return Reply::fail(kInternalError, "load check output unavailable");
    if (state_->worker.joinable()) state_->worker.join();
    Reply error;
    auto prepared = input(service_, params, error);
    if (!prepared) return error;
    const auto game = service_.model.game_root;
    const auto user = service_.model.user_data;
    const auto build = service_.info().engine_build;
    state_->cancelled = false; state_->request = id; state_->running = true;
    state_->worker = std::thread([this, id, prepared = std::move(*prepared), game, user, build] {
        std::string response;
        fs::path scratch;
        try {
            const auto key = fingerprint(prepared.profile, game, build);
            // The profile snapshot contains the same resolved roots the service showed.
            scratch = prepared.cache / ("running-" + std::to_string(GetCurrentProcessId()) + ".json");
            const auto report_path = fs::path(scratch.native() + L".report");
            if (fs::exists(scratch) || fs::exists(report_path)) throw std::runtime_error("load check scratch already exists");
            { std::ofstream snapshot(scratch, std::ios::binary); snapshot << mods::profile_to_json(prepared.profile); if (!snapshot) throw std::runtime_error("cannot write profile snapshot"); }
            Handle read, write, sink, job, process, thread;
            SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
            if (!CreatePipe(&read.value, &write.value, &security, 0) || !SetHandleInformation(read.value, HANDLE_FLAG_INHERIT, 0)) throw std::runtime_error("cannot create helper pipe");
            sink.value = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (sink.value == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open helper log sink");
            job.value = CreateJobObjectW(nullptr, nullptr);
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) throw std::runtime_error("cannot create helper job");
            Attributes attributes;
            SIZE_T attribute_bytes{};
            InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
            attributes.bytes.resize(attribute_bytes);
            const auto list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.bytes.data());
            if (!InitializeProcThreadAttributeList(list, 1, 0, &attribute_bytes)) throw std::runtime_error("cannot initialize helper handles");
            attributes.list = list;
            std::array<HANDLE, 2> inherited{write.value, sink.value}; // Only stdout's write end and the NUL sink.
            if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(), sizeof(inherited), nullptr, nullptr)) throw std::runtime_error("cannot restrict helper handles");
            STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            startup.StartupInfo.hStdOutput = write.value; startup.StartupInfo.hStdError = sink.value; startup.StartupInfo.hStdInput = sink.value;
            startup.lpAttributeList = list;
            PROCESS_INFORMATION child{};
            const auto helper = executable.empty() ? helper_path() : executable;
            auto command = quote(helper) + L" " + quote(scratch) + L" " + quote(user) + L" " + quote(report_path) + L" " + quote(game);
            if (!CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &child)) throw std::runtime_error("cannot start load check helper");
            process.value = child.hProcess; thread.value = child.hThread;
            if (!AssignProcessToJobObject(job.value, process.value)) { TerminateProcess(process.value, 3); throw std::runtime_error("cannot contain load check helper"); }
            { std::lock_guard lock(state_->mutex);
                if (!DuplicateHandle(GetCurrentProcess(), job.value, GetCurrentProcess(), &state_->job, 0, FALSE, DUPLICATE_SAME_ACCESS)) throw std::runtime_error("cannot retain helper job");
                if (state_->cancelled) TerminateJobObject(job.value, 9); else ResumeThread(thread.value);
            }
            CloseHandle(write.value); write.value = nullptr;
            std::string line, report;
            char bytes[4096]; DWORD got{};
            bool invalid = false;
            while (ReadFile(read.value, bytes, sizeof(bytes), &got, nullptr) && got) {
                for (DWORD i = 0; i < got; ++i) {
                    if (bytes[i] != '\n') { if (line.size() < kLineLimit) line += bytes[i]; else invalid = true; continue; }
                    const auto message = mods::steam::parse_json(line);
                    if (!message || message->kind != J::Kind::object) invalid = true;
                    else if (const auto* final = message->member("report"); final && final->kind == J::Kind::object && valid_report(*final) && report.empty()) report = encode(*final);
                    else if (message->string_member("stage") && json_count(message->member("done")) && json_count(message->member("total"))) {
                        const auto event = "{\"event\":\"loadcheck.progress\",\"id\":" + std::to_string(id) + ",\"data\":" + encode(*message) + '}';
                        if (event.size() > kLineLimit) invalid = true;
                        else if (!state_->cancelled) emit(event);
                    } else invalid = true;
                    line.clear();
                }
                if (invalid) { TerminateJobObject(job.value, 3); break; }
            }
            WaitForSingleObject(process.value, INFINITE);
            DWORD exit{}; GetExitCodeProcess(process.value, &exit);
            { std::lock_guard lock(state_->mutex); if (state_->job) CloseHandle(state_->job); state_->job = nullptr; }
            if (state_->cancelled) response = error_line(id, kCancelled, "load check cancelled");
            else if (exit != 0 || invalid || !line.empty() || report.empty()) throw std::runtime_error("load check helper failed or returned an invalid report");
            else {
                if (report.size() > kLineLimit - 64) throw std::runtime_error("load check report exceeded the line limit");
                const auto final_key = fingerprint(prepared.profile, game, build);
                if (key != final_key) throw std::runtime_error("chain changed during load check");
                std::lock_guard commit(state_->mutex);
                if (state_->cancelled) throw std::runtime_error("load check cancelled");
                const auto cache = prepared.cache / (key + ".json");
                if (fs::is_symlink(fs::symlink_status(cache))) throw std::runtime_error("unsafe cache file");
                // The helper's report file is private scratch; install the validated stdout report atomically.
                { std::ofstream file(report_path, std::ios::binary | std::ios::trunc); file << report; if (!file) throw std::runtime_error("cannot cache report"); }
                if (!MoveFileExW(report_path.c_str(), cache.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("cannot install cached report");
                response = "{\"id\":" + std::to_string(id) + ",\"result\":{\"report\":" + report + "}}";
                state_->running = false;
            }
            std::error_code ec; fs::remove(report_path, ec);
        } catch (const std::exception& e) {
            { std::lock_guard lock(state_->mutex); if (state_->job) CloseHandle(state_->job); state_->job = nullptr; }
            response = error_line(id, state_->cancelled ? kCancelled : kInternalError, state_->cancelled ? "load check cancelled" : e.what());
        }
        if (!scratch.empty()) { std::error_code ec; fs::remove(scratch, ec); fs::remove(fs::path(scratch.native() + L".report"), ec); }
        { std::lock_guard lock(state_->mutex);
            if (state_->cancelled) response = error_line(id, kCancelled, "load check cancelled");
            state_->running = false;
        }
        emit(std::move(response));
    });
    return {};
}
Reply LoadCheck::cancel(const J& params) {
    const auto request = json_count(params.member("request"));
    if (!request) return Reply::fail(kInvalidParams, "request must be an id");
    std::lock_guard lock(state_->mutex);
    const bool active = state_->running && state_->request == *request;
    if (active) {
        state_->cancelled = true;
        if (state_->job) TerminateJobObject(state_->job, 9);
    }
    return Reply::ok(active ? "{\"cancelled\":true}" : "{\"cancelled\":false}");
}
Reply LoadCheck::result(const J& params) {
    Reply error; auto prepared = input(service_, params, error); if (!prepared) return error;
    const auto key = fingerprint(prepared->profile, service_.model.game_root, service_.info().engine_build);
    const auto file = prepared->cache / (key + ".json");
    if (fs::is_symlink(fs::symlink_status(file))) return Reply::fail(kInvalidParams, "unsafe cache file");
    std::ifstream stream(file, std::ios::binary | std::ios::ate);
    if (!stream) return Reply::ok("{\"report\":null,\"next_cursor\":null}");
    if (stream.tellg() < 0 || stream.tellg() > static_cast<std::streamoff>(kLineLimit)) return Reply::fail(kInternalError, "invalid cached report");
    stream.seekg(0); std::string text((std::istreambuf_iterator<char>(stream)), {});
    auto report = mods::steam::parse_json(text);
    auto* diagnostics = report ? const_cast<J*>(report->member("diagnostics")) : nullptr;
    if (!report || report->kind != J::Kind::object || !valid_report(*report) || !diagnostics) return Reply::fail(kInternalError, "invalid cached report");
    std::size_t offset{};
    if (const auto* cursor = params.member("cursor"); cursor && cursor->kind != J::Kind::null) {
        if (cursor->kind != J::Kind::string) return Reply::fail(kInvalidParams, "invalid cursor");
        const auto found = service_.model.pages.find(cursor->string);
        if (found == service_.model.pages.end() || found->second.method != "loadcheck.result:" + key) return Reply::fail(kInvalidParams, "stale cursor");
        offset = found->second.offset;
    }
    std::size_t limit = 100;
    if (const auto* v = params.member("limit")) { const auto count = json_count(v); if (!count || *count < 1 || *count > 500) return Reply::fail(kInvalidParams, "limit must be 1..500"); limit = static_cast<std::size_t>(*count); }
    if (offset > diagnostics->array.size()) return Reply::fail(kInvalidParams, "stale cursor");
    const auto end = std::min(offset + limit, diagnostics->array.size());
    std::string next = "null";
    if (end < diagnostics->array.size()) {
        const auto token = "lc-" + std::to_string(++service_.model.sequence);
        service_.model.pages[token] = {"loadcheck.result:" + key, {}, end}; next = json_quote(token);
    }
    diagnostics->array = std::vector<J>(diagnostics->array.begin() + static_cast<std::ptrdiff_t>(offset), diagnostics->array.begin() + static_cast<std::ptrdiff_t>(end));
    return Reply::ok("{\"report\":" + encode(*report) + ",\"next_cursor\":" + next + '}');
}
Reply method_loadcheck_start(Service&, const J&) { return Reply::fail(kInternalError, "start dispatch unavailable"); }
Reply method_loadcheck_cancel(Service& s, const J& p) { return s.loadcheck().cancel(p); }
Reply method_loadcheck_result(Service& s, const J& p) { return s.loadcheck().result(p); }
}
