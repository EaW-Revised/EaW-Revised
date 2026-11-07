#include "instance.hpp"

#include "text_formats.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <limits>
#include <system_error>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace eawr::launcher {

namespace {

constexpr std::size_t kPlayerNameMaxChars = 32;   // player_name_max_chars in the viewer's player profile

std::string read_small(const fs::path& path, std::size_t limit = 1u << 20) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {};
    std::string text(limit + 1, '\0');
    stream.read(text.data(), static_cast<std::streamsize>(text.size()));
    const auto size = static_cast<std::size_t>(stream.gcount());
    if (size > limit) return {};
    text.resize(size);
    return text;
}

// The content of a claim file; nullopt when it cannot be opened right now (missing, or briefly locked by a rename or a
// scanner). "Cannot read" is not "stale": only a file that was read and is empty or malformed is judged stale.
std::optional<std::string> read_claim_file(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return std::nullopt;
    std::string text(4097, '\0');
    stream.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (stream.bad()) return std::nullopt;
    text.resize(static_cast<std::size_t>(stream.gcount()));
    if (text.size() > 4096) return std::string(4097, 'x');   // oversized: malformed, not unreadable
    return text;
}

bool write_plain(const fs::path& path, const std::string& text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << text;
    stream.close();
    return static_cast<bool>(stream);
}

std::string claim_text(const ProcessIdentity& self) {
    std::string text = "{\"pid\":" + std::to_string(self.pid);
    if (!self.start_time.empty()) text += ",\"process_start_time\":\"" + self.start_time + "\"";
    return text + "}\n";
}

// Moves `from` to `to` only if `to` does not exist (atomic); false when `to` exists or `from` is gone.
bool move_no_replace(const fs::path& from, const fs::path& to) {
#ifdef _WIN32
    return MoveFileExW(from.c_str(), to.c_str(), 0) != 0;
#else
    std::error_code error;
    fs::create_hard_link(from, to, error);
    if (error) return false;
    fs::remove(from, error);
    return true;
#endif
}

// Publishes `text` as `claim` only if `claim` does not exist; the content is complete when the file appears.
bool publish_exclusive(const fs::path& claim, const std::string& text, unsigned pid) {
    fs::path temporary = claim;
    temporary += "." + std::to_string(pid) + ".tmp";
    if (!write_plain(temporary, text)) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return false;
    }
    const bool published = move_no_replace(temporary, claim);
    if (!published) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
    }
    return published;
}

struct ClaimFile {
    bool valid = false;
    unsigned pid = 0;
    std::optional<std::string> start_time;
};

ClaimFile parse_claim(const std::string& text) {
    ClaimFile claim;
    const auto json = parse_json(text);
    if (!json || json->kind != JsonValue::Kind::object) return claim;
    const auto* pid = json->member("pid");
    if (!pid || pid->kind != JsonValue::Kind::number || !std::isfinite(pid->number) || pid->number < 1
        || pid->number > (std::numeric_limits<unsigned>::max)() || std::floor(pid->number) != pid->number) return claim;
    claim.pid = static_cast<unsigned>(pid->number);
    if (const auto* start = json->member("process_start_time")) {
        if (start->kind != JsonValue::Kind::string || start->string.empty()
            || !std::all_of(start->string.begin(), start->string.end(), [](char c) { return c >= '0' && c <= '9'; })) return claim;
        claim.start_time = start->string;
    }
    claim.valid = true;
    return claim;
}

enum class Takeover { removed, changed, busy };

// The takeover lock is an OS-level exclusive hold on `.claim.takeover`, never a file whose presence means "locked":
// Windows opens it with share mode 0 (and delete-on-close), other systems flock() it. The operating system releases
// it when the holder dies, so there is no stale lock to clear and nothing to delete by name after reading it.
class TakeoverLock {
public:
    explicit TakeoverLock(const fs::path& path) {
#ifdef _WIN32
        handle_ = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
#else
        descriptor_ = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (descriptor_ >= 0 && ::flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
            ::close(descriptor_);
            descriptor_ = -1;
        }
#endif
    }
    ~TakeoverLock() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#else
        if (descriptor_ >= 0) ::close(descriptor_);
#endif
    }
    TakeoverLock(const TakeoverLock&) = delete;
    TakeoverLock& operator=(const TakeoverLock&) = delete;
    [[nodiscard]] bool held() const {
#ifdef _WIN32
        return handle_ != INVALID_HANDLE_VALUE;
#else
        return descriptor_ >= 0;
#endif
    }
private:
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int descriptor_ = -1;
#endif
};

// Removes a claim judged stale. Judge-then-remove runs while holding the takeover lock, so two takers never act on
// the same judgement and the claim is never moved or hidden (a launcher publishing meanwhile still finds it).
// Under the lock the claim is read again and removed only if it still is what was judged; a competitor's live claim
// is left alone and so is a claim that could not be removed (`changed`: the caller looks again). `busy`: another
// taker holds the lock right now.
Takeover take_stale_claim(const fs::path& claim, const std::string& judged) {
    fs::path lock_path = claim;
    lock_path += ".takeover";
    const TakeoverLock lock(lock_path);
    if (!lock.held()) return Takeover::busy;
    const auto now = read_claim_file(claim);
    if (!now || *now != judged) return Takeover::changed;   // gone, replaced, or unreadable: look again
    std::error_code error;
    const bool removed = fs::remove(claim, error);
    return removed && !error ? Takeover::removed : Takeover::changed;
}

std::wstring lower_wide(std::wstring_view text) {
    std::wstring out(text);
    for (wchar_t& c : out) c = static_cast<wchar_t>(std::towlower(c));
    return out;
}

std::vector<std::string> split_lines(std::string_view text) {
    std::vector<std::string> lines;
    std::size_t position = 0;
    while (position < text.size()) {
        std::size_t end = text.find('\n', position);
        if (end == std::string_view::npos) end = text.size();
        std::string line(text.substr(position, end - position));
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        position = end + 1;
    }
    return lines;
}

std::string join_lines(const std::vector<std::string>& lines) {
    std::string text;
    for (const std::string& line : lines) text += line + "\n";
    return text;
}

// Replaces the "key=..." line or appends one.
void set_key(std::vector<std::string>& lines, const std::string& key, const std::string& value) {
    const std::string prefix = key + "=";
    for (std::string& line : lines) {
        if (line.rfind(prefix, 0) == 0) {
            line = prefix + value;
            return;
        }
    }
    lines.push_back(prefix + value);
}

std::string get_key(const std::vector<std::string>& lines, const std::string& key) {
    const std::string prefix = key + "=";
    for (const std::string& line : lines)
        if (line.rfind(prefix, 0) == 0) return line.substr(prefix.size());
    return {};
}

std::string trim_spaces(const std::string& text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

std::string truncate_chars(const std::string& text, std::size_t max_chars) {
    std::size_t chars = 0;
    std::size_t index = 0;
    for (; index < text.size(); ++index) {
        if ((static_cast<unsigned char>(text[index]) & 0xC0) != 0x80) {
            if (chars == max_chars) break;
            ++chars;
        }
    }
    return text.substr(0, index);
}

bool has_header(std::string_view text, std::string_view prefix) { return text.substr(0, prefix.size()) == prefix; }

} // namespace

std::optional<int> parse_instance_id(std::wstring_view text) {
    if (text.empty() || text.size() > 3) return std::nullopt;
    int value = 0;
    for (const wchar_t c : text) {
        if (c < L'0' || c > L'9') return std::nullopt;
        value = value * 10 + (c - L'0');
    }
    if (value < 1 || value > kMaxInstanceId) return std::nullopt;
    return value;
}

unsigned instance_host_port(int instance) {
    return kFirstHostPort + 2u * static_cast<unsigned>(std::max(instance, 1) - 1);
}

fs::path instance_root(const fs::path& user_data, int instance) {
    return instance <= 1 ? user_data : user_data / "instances" / std::to_string(instance);
}

fs::path instance_launcher_dir(const fs::path& user_data, int instance) {
    return instance_root(user_data, instance) / "launcher";
}

fs::path instance_appdata_dir(const fs::path& user_data, int instance) {
    return instance_root(user_data, instance) / "appdata";
}

fs::path instance_viewer_dir(const fs::path& user_data, int instance) {
    return instance_appdata_dir(user_data, instance) / "Godot" / "app_userdata" / "EAWR Viewer";
}

std::wstring build_environment_block(const std::vector<std::wstring>& entries, std::wstring_view name, std::wstring_view value) {
    const std::wstring wanted = lower_wide(name);
    std::wstring replacement(name);
    replacement += L'=';
    replacement += value;
    std::vector<std::wstring> result;
    bool replaced = false;
    for (const std::wstring& entry : entries) {
        const std::size_t equals = entry.find(L'=', 1);   // a leading '=' belongs to the name ("=C:=C:\...")
        const bool matches = equals != std::wstring::npos && equals == wanted.size() && lower_wide(entry.substr(0, equals)) == wanted;
        if (!matches) {
            result.push_back(entry);
        } else if (!replaced) {
            result.push_back(replacement);
            replaced = true;
        }   // a duplicate of the same name is dropped
    }
    if (!replaced) result.push_back(replacement);
    std::wstring block;
    for (const std::wstring& entry : result) {
        block += entry;
        block += L'\0';
    }
    block += L'\0';
    return block;
}

#ifdef _WIN32
namespace {
std::optional<std::string> start_time_of(HANDLE process) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return {};
    const auto ticks = (static_cast<unsigned long long>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    return std::to_string(ticks);
}
} // namespace

ProcessIdentity current_process_identity() {
    ProcessIdentity self;
    self.pid = GetCurrentProcessId();
    if (const auto start = start_time_of(GetCurrentProcess())) self.start_time = *start;
    return self;
}

std::optional<std::string> process_start_time(unsigned pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    const auto result = start_time_of(process);
    CloseHandle(process);
    return result;
}

bool process_gone(unsigned pid, const std::optional<std::string>& start_time) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!process) {
        const DWORD error = GetLastError();
        if (error == ERROR_INVALID_PARAMETER) return true;   // no such process
        if (error != ERROR_ACCESS_DENIED) return false;
        // A protected process cannot be waited on, but its creation time and exit state can still be read: a dead
        // claim whose PID a protected process now owns must not stay "alive" forever.
        process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process) return false;
        DWORD code = STILL_ACTIVE;
        bool gone = GetExitCodeProcess(process, &code) && code != STILL_ACTIVE;
        if (!gone && start_time) {
            const auto actual = start_time_of(process);
            if (actual && *actual != *start_time) gone = true;
        }
        CloseHandle(process);
        return gone;
    }
    bool dead = WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
    if (!dead && start_time) {
        const auto actual = start_time_of(process);
        if (actual && *actual != *start_time) dead = true;
    }
    CloseHandle(process);
    return dead;
}
#else
ProcessIdentity current_process_identity() { return {}; }
std::optional<std::string> process_start_time(unsigned) { return std::nullopt; }
bool process_gone(unsigned, const std::optional<std::string>&) { return false; }
#endif

ClaimResult claim_instance(const fs::path& user_data, int instance, const ProcessIdentity& self, ProcessGoneFn gone) {
    ClaimResult result;
    const fs::path root = instance_root(user_data, instance);
    std::error_code error;
    fs::create_directories(root, error);
    const fs::path claim = root / ".claim";
    bool after_stale = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (publish_exclusive(claim, claim_text(self), self.pid)) {
            result.status = after_stale ? ClaimStatus::claimed_after_stale : ClaimStatus::claimed;
            return result;
        }
        const auto content = read_claim_file(claim);
        if (!content) {   // missing (released or being taken over) or briefly unreadable: look again, never judge it stale
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        const std::string& text = *content;
        const ClaimFile existing = parse_claim(text);
        if (existing.valid && existing.pid == self.pid && (!existing.start_time || self.start_time.empty() || *existing.start_time == self.start_time)) {
            result.status = ClaimStatus::mine;
            return result;
        }
        if (existing.valid && !gone(existing.pid, existing.start_time)) {
            result.status = ClaimStatus::held;
            result.owner_pid = existing.pid;
            return result;
        }
        // Stale (dead owner) or unreadable. Take exactly what was judged; if someone replaced it meanwhile
        // (or released it), start over and look at the new state.
        switch (take_stale_claim(claim, text)) {
        case Takeover::removed: after_stale = true; break;
        case Takeover::changed: break;
        case Takeover::busy: std::this_thread::sleep_for(std::chrono::milliseconds(10)); break;
        }
    }
    result.status = ClaimStatus::held;   // owner_pid stays 0: another launcher is mid-takeover for too long
    return result;
}

void release_claim(const fs::path& user_data, int instance, const ProcessIdentity& self) {
    const fs::path claim = instance_root(user_data, instance) / ".claim";
    const ClaimFile existing = parse_claim(read_small(claim, 4096));
    if (!existing.valid || existing.pid != self.pid) return;
    std::error_code error;
    fs::remove(claim, error);
}

std::optional<int> claim_next_free(const fs::path& user_data, const ProcessIdentity& self, int max_instance,
                                   ClaimResult* out, ProcessGoneFn gone) {
    for (int instance = 2; instance <= std::min(max_instance, kMaxInstanceId); ++instance) {
        const ClaimResult result = claim_instance(user_data, instance, self, gone);
        if (result.ok()) {
            if (out) *out = result;
            return instance;
        }
    }
    return std::nullopt;
}

std::string seed_settings_text(std::string_view primary, bool primary_exists) {
    std::vector<std::string> lines;
    if (primary_exists && has_header(primary, "eawr-settings ")) lines = split_lines(primary);
    else lines = {"eawr-settings 1"};
    set_key(lines, "video.windowed", "1");   // two full-screen windows on one monitor would cover each other
    return join_lines(lines);
}

std::string seed_profile_text(std::string_view primary, bool primary_exists, int instance) {
    std::vector<std::string> lines;
    if (primary_exists && has_header(primary, "eawr-profile ")) lines = split_lines(primary);
    else lines = {"eawr-profile 1"};
    std::string base = trim_spaces(get_key(lines, "player.network_name"));
    if (base.empty()) base = trim_spaces(get_key(lines, "player.name"));
    if (base.empty()) base = "Player";   // the viewer's fallback_player_name
    const std::string suffix = " #" + std::to_string(instance);
    base = trim_spaces(truncate_chars(base, kPlayerNameMaxChars - suffix.size()));
    set_key(lines, "player.network_name", base + suffix);
    return join_lines(lines);
}

bool seed_instance_files(const fs::path& primary_viewer_dir, const fs::path& instance_viewer_dir, int instance, std::string& error) {
    std::error_code code;
    fs::create_directories(instance_viewer_dir, code);
    if (code) {
        error = "cannot create " + path_to_utf8(instance_viewer_dir) + ": " + code.message();
        return false;
    }
    struct Item {
        const char* file;
        bool profile;
    };
    for (const Item item : {Item{"eawr-settings.cfg", false}, Item{"eawr-profile.cfg", true}}) {
        const fs::path target = instance_viewer_dir / item.file;
        if (fs::exists(target, code)) continue;
        const fs::path source = primary_viewer_dir / item.file;
        const bool exists = fs::is_regular_file(source, code);
        const std::string text = read_small(source, 1u << 20);
        const std::string seeded = item.profile ? seed_profile_text(text, exists, instance) : seed_settings_text(text, exists);
        fs::path temporary = target;
        temporary += ".tmp";
        if (!write_plain(temporary, seeded)) {
            error = "cannot write " + path_to_utf8(temporary);
            return false;
        }
        fs::rename(temporary, target, code);
        if (code) {
            error = "cannot write " + path_to_utf8(target) + ": " + code.message();
            return false;
        }
    }
    return true;
}

} // namespace eawr::launcher
