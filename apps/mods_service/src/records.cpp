#include "records.hpp"
#include "json_value.hpp"
#include "eawr/mods/fingerprint.hpp"
#include "eawr/mods/chain.hpp"
#include <windows.h>
#include <atomic>
#include <map>
#include <mutex>
#include <thread>
#include <fstream>
#include <array>
#include <stdexcept>
#include <set>
#include <chrono>

namespace eawr::mods_service {
namespace {
namespace fs = std::filesystem;
constexpr auto kFingerprintReuse = std::chrono::seconds(2);
using J = mods::steam::JsonValue;
using mods::steam::json_quote;
struct Handle {
    HANDLE value{};
    ~Handle() { reset(); }
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    void reset(HANDLE replacement = nullptr) {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
        value = replacement;
    }
};
struct Helper {
    Handle job, process, read, write;
    fs::path snapshot;
    std::string pending, profile;
    ~Helper() {
        // job is private to this helper. Never locate/terminate a process by name.
        if (job.value) TerminateJobObject(job.value, 9);
        if (process.value) WaitForSingleObject(process.value, INFINITE);
        std::error_code ec; if (!snapshot.empty()) fs::remove(snapshot, ec);
    }
};
std::wstring quote(const fs::path& path) {
    std::wstring out = L"\""; unsigned slashes{};
    for (const auto c : path.native()) {
        if (c == L'\\') { ++slashes; continue; }
        out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0; out += c;
    }
    out.append(slashes * 2, L'\\'); return out + L'"';
}
fs::path helper_path() {
    std::wstring path(32768, L'\0');
    path.resize(GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())));
    return fs::path(path).parent_path() / "eawr_loadcheck.exe";
}
std::string error(std::uint64_t id, std::string_view code, std::string_view message) {
    return "{\"id\":" + std::to_string(id) + ",\"error\":{\"code\":" + json_quote(code) + ",\"message\":" + json_quote(message) + "}}";
}
std::optional<Reply> validate(std::string_view method, const J& params) {
    const auto invalid = [] { return Reply::fail(kInvalidParams, "invalid records parameters"); };
    if (const auto* fresh = params.member("fresh"); fresh && fresh->kind != J::Kind::boolean) return invalid();
    if (method == "records.get") {
        if (!params.string_member("id") || !params.string_member("category")) return invalid();
        const auto& category = *params.string_member("category");
        if (category != "game_objects" && category != "hardpoints" && category != "abilities"
            && category != "factions" && category != "campaigns" && category != "sfx") return invalid();
        if (const auto* offset = params.member("tag_offset"); offset && !json_count(offset)) return invalid();
    } else {
        if (const auto* limit = params.member("limit")) {
            const auto count = json_count(limit);
            if (!count || *count < 1 || *count > 500) return invalid();
        }
        if (const auto* cursor = params.member("cursor"); cursor && cursor->kind != J::Kind::null && cursor->kind != J::Kind::string) return invalid();
        if (const auto* filters = params.member("filters")) {
            if (filters->kind != J::Kind::object) return invalid();
            for (const auto* name : {"object", "tag", "changed_by_layer"})
                if (const auto* v = filters->member(name); v && v->kind != J::Kind::string) return invalid();
            for (const auto* name : {"several_mods_same_tag", "dropped"})
                if (const auto* v = filters->member(name); v && v->kind != J::Kind::boolean) return invalid();
        }
    }
    return {};
}
void validate_chain(const mods::Profile& profile) {
    // Metadata may have changed since the UI's last discovery. Re-read only the selected
    // roots on the worker before a new fingerprint is cached, without walking Steam trees.
    mods::DiscoveryInput discovery; discovery.walk_trees = false;
    std::vector<std::string> keys;
    for (const auto& layer : profile.layers) { discovery.manual_folders.push_back(mods::steam::path_from_utf8(layer.root)); keys.push_back(layer.key); }
    auto catalog = mods::discover(discovery);
    mods::read_mod_metadata(catalog);
    for (auto& record : catalog.records) for (const auto& layer : profile.layers) {
        std::error_code ec;
        if (!fs::equivalent(mods::steam::path_from_utf8(record.root), mods::steam::path_from_utf8(layer.root), ec) || ec) continue;
        const auto* variant = record.metadata.string_member("variant_file");
        const auto marker = layer.key.find("#variant:");
        if ((!variant && marker == std::string::npos)
            || (variant && marker != std::string::npos && layer.key.substr(marker + 9) == *variant)) record.key = layer.key;
    }
    const auto checked = mods::check_order(keys, catalog.records, {});
    if (!checked.launchable || !checked.warnings.empty() || !checked.diagnostics.empty())
        throw std::invalid_argument("profile chain is unclean; resolve its diagnostics first");
}
// The snapshot is private, below the guarded user root, and deleted with its helper.
fs::path snapshot_path(const fs::path& root, std::size_t sequence) {
    std::error_code ec;
    auto folder = fs::weakly_canonical(root, ec);
    if (root.empty() || ec) throw std::runtime_error("user data unavailable");
    for (const auto* part : {"mods", "cache", "records"}) {
        folder /= part;
        if (fs::weakly_canonical(folder, ec) != folder || ec) throw std::runtime_error("unsafe records scratch folder");
    }
    fs::create_directories(folder);
    return folder / ("running-" + std::to_string(GetCurrentProcessId()) + '-' + std::to_string(sequence) + ".json");
}
void launch(Helper& helper, const fs::path& executable, const fs::path& game, std::string_view key) {
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Handle child_out, child_in, sink, thread;
    if (!CreatePipe(&helper.read.value, &child_out.value, &security, 0)
        || !SetHandleInformation(helper.read.value, HANDLE_FLAG_INHERIT, 0)
        || !CreatePipe(&child_in.value, &helper.write.value, &security, 0)
        || !SetHandleInformation(helper.write.value, HANDLE_FLAG_INHERIT, 0)) throw std::runtime_error("cannot create records pipes");
    sink.value = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (sink.value == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open records log sink");
    helper.job.value = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!helper.job.value || !SetInformationJobObject(helper.job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) throw std::runtime_error("cannot create records job");
    SIZE_T size{}; InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<unsigned char> bytes(size);
    const auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(bytes.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size)) throw std::runtime_error("cannot initialize records handles");
    struct Cleanup { LPPROC_THREAD_ATTRIBUTE_LIST p; ~Cleanup() { DeleteProcThreadAttributeList(p); } } cleanup{attributes};
    HANDLE inherited[]{child_in.value, child_out.value, sink.value};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr)) throw std::runtime_error("cannot restrict records handles");
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = child_in.value; startup.StartupInfo.hStdOutput = child_out.value; startup.StartupInfo.hStdError = sink.value;
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION child{};
    auto command = quote(executable) + L" --records " + quote(helper.snapshot) + L" " + quote(game) + L" " + quote(mods::steam::path_from_utf8(key));
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
        nullptr, nullptr, &startup.StartupInfo, &child)) throw std::runtime_error("cannot start records helper");
    helper.process.value = child.hProcess; thread.value = child.hThread;
    if (!AssignProcessToJobObject(helper.job.value, helper.process.value)) {
        TerminateProcess(helper.process.value, 3); throw std::runtime_error("cannot contain records helper");
    }
    if (ResumeThread(thread.value) == static_cast<DWORD>(-1)) throw std::runtime_error("cannot resume records helper");
}
void write(Helper& helper, std::string text) {
    text += '\n'; std::string_view bytes(text);
    while (!bytes.empty()) {
        DWORD written{};
        if (!WriteFile(helper.write.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) || !written) throw std::runtime_error("records helper input closed");
        bytes.remove_prefix(written);
    }
}
std::string read(Helper& helper) {
    for (;;) {
        const auto end = helper.pending.find('\n');
        if (end != std::string::npos) {
            if (end > kLineLimit) throw std::runtime_error("records helper line exceeded the limit");
            auto line = helper.pending.substr(0, end); helper.pending.erase(0, end + 1); return line;
        }
        if (helper.pending.size() > kLineLimit) throw std::runtime_error("records helper line exceeded the limit");
        char bytes[4096]; DWORD got{};
        if (!ReadFile(helper.read.value, bytes, sizeof(bytes), &got, nullptr) || !got) throw std::runtime_error("records helper failed");
        helper.pending.append(bytes, got);
    }
}
}
struct Records::State {
    std::thread worker;
    std::mutex mutex;
    bool running{}, cancelled{};
    std::uint64_t request{};
    std::size_t sequence{};
    std::string profile;
    HANDLE active_job{}; // borrowed while worker owns the Helper
    std::map<std::string, std::unique_ptr<Helper>> helpers;
    std::set<std::string> invalidated;
    // Chain fingerprint reuse window (protocol 1.13): per profile id, the key computed at `at` for these layers.
    struct Reuse { std::string key; std::chrono::steady_clock::time_point at; std::vector<mods::ProfileLayer> layers; };
    std::map<std::string, Reuse> reuse;
    // Bumped by every invalidation; a walk that started before a bump must not publish its key.
    std::uint64_t reuse_generation{};
};
void Records::invalidate_reuse() { std::lock_guard lock(state_->mutex); state_->reuse.clear(); ++state_->reuse_generation; }
Records::Records(Service& service) : state_(std::make_unique<State>()), service_(service) {}
Records::~Records() { stop(); }
void Records::stop() {
    {
        std::lock_guard lock(state_->mutex);
        state_->cancelled = true;
        if (state_->active_job) TerminateJobObject(state_->active_job, 9);
    }
    if (state_->worker.joinable()) state_->worker.join();
    state_->helpers.clear();
}
void Records::forget_profile(std::string_view profile) {
    bool active;
    { std::lock_guard lock(state_->mutex); state_->reuse.erase(std::string(profile)); ++state_->reuse_generation; }
    { std::lock_guard lock(state_->mutex); active = state_->running && state_->profile == profile; }
    if (active) {
        { std::lock_guard lock(state_->mutex); state_->cancelled = true; if (state_->active_job) TerminateJobObject(state_->active_job, 9); }
        if (state_->worker.joinable()) state_->worker.join();
    } else {
        std::lock_guard lock(state_->mutex);
        if (state_->running) { state_->invalidated.emplace(profile); return; }
    }
    for (auto it = state_->helpers.begin(); it != state_->helpers.end();) {
        if (it->second->profile == profile) it = state_->helpers.erase(it); else ++it;
    }
}
std::optional<Reply> Records::start(std::uint64_t id, std::string method, const J& params) {
    { std::lock_guard lock(state_->mutex); if (state_->running) return Reply::fail(kBusy, "a records request is already running"); }
    if (!emit) return Reply::fail(kInternalError, "records output unavailable");
    if (auto invalid = validate(method, params)) return invalid;
    Reply failure; auto profile = records_profile(service_, params, failure); if (!profile) return failure;
    if (state_->worker.joinable()) state_->worker.join();
    const auto game = service_.model.game_root, user = service_.model.user_data;
    const auto build = service_.info().engine_build;
    const auto helper_exe = executable.empty() ? helper_path() : executable;
    { std::lock_guard lock(state_->mutex); state_->cancelled = false; state_->running = true; state_->request = id; state_->profile = profile->id; }
    state_->worker = std::thread([this, id, method = std::move(method), params, profile = std::move(*profile), game, user, build, helper_exe] {
        std::string response, identity;
        try {
            // Reuse a fingerprint taken within kFingerprintReuse for the same profile layers unless the client
            // asked `fresh`; mutating service methods clear the entries (invalidate_reuse / forget_profile).
            std::string key;
            const auto* fresh = params.member("fresh");
            if (!(fresh && fresh->boolean)) {
                std::lock_guard lock(state_->mutex);
                const auto found_reuse = state_->reuse.find(profile.id);
                if (found_reuse != state_->reuse.end() && std::chrono::steady_clock::now() - found_reuse->second.at < kFingerprintReuse
                    && found_reuse->second.layers == profile.layers && state_->helpers.contains(profile.id + ':' + found_reuse->second.key))
                    key = found_reuse->second.key;
            }
            const bool reused = !key.empty();
            if (!reused) {
                const auto taken = std::chrono::steady_clock::now();
                std::uint64_t generation; { std::lock_guard lock(state_->mutex); generation = state_->reuse_generation; }
                key = mods::loadcheck_chain_fingerprint(profile, game, build);
                if (!key.empty()) { std::lock_guard lock(state_->mutex); if (state_->reuse_generation == generation) state_->reuse[profile.id] = {key, taken, profile.layers}; }
            }
            if (key.empty()) throw std::runtime_error("chain fingerprint unavailable");
            identity = profile.id + ':' + key;
            auto found = state_->helpers.find(identity);
            const bool warm = found != state_->helpers.end();
            if (found == state_->helpers.end()) {
                validate_chain(profile);
                auto helper = std::make_unique<Helper>(); helper->profile = profile.id;
                helper->snapshot = snapshot_path(user, ++state_->sequence);
                if (fs::exists(helper->snapshot) || fs::is_symlink(fs::symlink_status(helper->snapshot))) throw std::runtime_error("records scratch already exists");
                { std::ofstream file(helper->snapshot, std::ios::binary); file << mods::profile_to_json(profile); if (!file) throw std::runtime_error("cannot write records snapshot"); }
                // Hold the mutex until the new owned job is visible to Cancel.
                { std::lock_guard lock(state_->mutex);
                    if (state_->cancelled) throw std::runtime_error("records cancelled");
                    launch(*helper, helper_exe, game, key); state_->active_job = helper->job.value;
                }
                found = state_->helpers.emplace(identity, std::move(helper)).first;
            } else {
                std::lock_guard lock(state_->mutex); state_->active_job = found->second->job.value;
                if (state_->cancelled) TerminateJobObject(state_->active_job, 9);
            }
            auto& helper = *found->second;
            write(helper, "{\"method\":" + json_quote(method) + ",\"params\":" + encode_json(params) + '}');
            for (;;) {
                const auto line = read(helper);
                const auto message = mods::steam::parse_json(line);
                if (!message || message->kind != J::Kind::object) throw std::runtime_error("invalid records helper response");
                if (const auto* result = message->member("result"); result && result->kind == J::Kind::object) {
                    const auto* fingerprint = result->string_member("chain_fingerprint");
                    if (!fingerprint || *fingerprint != key) throw std::runtime_error("invalid records helper identity");
                    response = "{\"id\":" + std::to_string(id) + ",\"result\":" + encode_json(*result) + '}'; break;
                }
                if (const auto* value = message->member("error"); value && value->kind == J::Kind::object) {
                    const auto* code = value->string_member("code"), *text = value->string_member("message");
                    if (!code || !text || (*code != kInvalidParams && *code != kInternalError)) throw std::runtime_error("invalid records helper error");
                    response = error(id, *code, *text); break;
                }
                const auto done = json_count(message->member("done")), total = json_count(message->member("total"));
                if (!message->string_member("stage") || !done || !total || *done > *total) throw std::runtime_error("invalid records helper progress");
                const auto event = "{\"event\":\"records.progress\",\"id\":" + std::to_string(id) + ",\"data\":" + encode_json(*message) + '}';
                if (event.size() > kLineLimit) throw std::runtime_error("records progress exceeded the line limit");
                { std::lock_guard lock(state_->mutex); if (!state_->cancelled) emit(event); }
            }
            // A request served by a finished helper takes milliseconds, so the second walk over every chain file
            // (the dominant cost of a warm page) only guards requests that build the index; the check before it stays.
            if (!warm && !reused && key != mods::loadcheck_chain_fingerprint(profile, game, build)) throw std::runtime_error("chain changed during records request");
            const auto saved = mods::load_profile(user / "mods" / "profiles" / (profile.id + ".json"));
            if (!saved.profile || saved.profile->layers != profile.layers) throw std::runtime_error("profile changed during records request");
            if (response.size() > kLineLimit) throw std::runtime_error("records answer exceeded the line limit");
        } catch (const std::exception& e) {
            response = error(id, dynamic_cast<const std::invalid_argument*>(&e) ? kInvalidParams : kInternalError, e.what());
            { std::lock_guard lock(state_->mutex); state_->active_job = nullptr; }
            state_->helpers.erase(identity);
        }
        {
            std::lock_guard lock(state_->mutex);
            state_->active_job = nullptr;
            if (state_->cancelled) { response = error(id, kCancelled, "records request cancelled"); state_->helpers.erase(identity); }
            for (auto it = state_->helpers.begin(); it != state_->helpers.end();) {
                if (state_->invalidated.contains(it->second->profile)) it = state_->helpers.erase(it); else ++it;
            }
            state_->invalidated.clear();
            // Publish the terminal response before another request can start.
            emit(std::move(response)); state_->running = false;
        }
    });
    return {};
}
Reply Records::cancel(const J& params) {
    const auto request = json_count(params.member("request"));
    if (!request) return Reply::fail(kInvalidParams, "request must be an id");
    std::lock_guard lock(state_->mutex);
    const bool active = state_->running && state_->request == *request;
    if (active) { state_->cancelled = true; if (state_->active_job) TerminateJobObject(state_->active_job, 9); }
    return Reply::ok(active ? "{\"cancelled\":true}" : "{\"cancelled\":false}");
}
Reply method_records_index(Service&, const J&) { return Reply::fail(kInternalError, "records dispatch unavailable"); }
Reply method_records_get(Service&, const J&) { return Reply::fail(kInternalError, "records dispatch unavailable"); }
Reply method_records_cancel(Service& s, const J& params) { return s.records().cancel(params); }
}
