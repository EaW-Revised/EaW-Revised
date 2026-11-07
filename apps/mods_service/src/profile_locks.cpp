#include "profile_locks.hpp"

#include "instance.hpp"

#include "eawr/mods/steam/text_formats.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <system_error>

namespace eawr::mods_service {
namespace {

namespace fs = mods::steam::fs;
using mods::steam::JsonValue;

constexpr std::size_t kStateLimit = 64 * 1024;   // the launcher's own bound for these files

enum class State { free, unreadable, locked };
struct StateRead {
    State state = State::free;
    std::string profile;
};

// {"profile":"id","pid":N,"process_start_time":"ticks"}. A missing file is free. A state without the creation time
// (older launchers) is judged by its PID alone.
StateRead read_state(const fs::path& file) {
    std::error_code error;
    if (!fs::exists(file, error)) return {};
    const StateRead unreadable{State::unreadable, {}};
    std::ifstream stream(file, std::ios::binary);
    if (!stream) return unreadable;
    std::string text(kStateLimit + 1, '\0');
    stream.read(text.data(), static_cast<std::streamsize>(text.size()));
    const auto size = static_cast<std::size_t>(stream.gcount());
    if (stream.bad() || size > kStateLimit) return unreadable;
    text.resize(size);
    const auto json = mods::steam::parse_json(text);
    if (!json || json->kind != JsonValue::Kind::object) return unreadable;
    const std::string* profile = json->string_member("profile");
    if (profile == nullptr) return unreadable;
    if (profile->empty()) return {};   // a vanilla instance holds no profile
    const JsonValue* pid = json->member("pid");
    if (pid == nullptr || pid->kind != JsonValue::Kind::number || !std::isfinite(pid->number) || pid->number < 1
        || pid->number > (std::numeric_limits<unsigned>::max)() || std::floor(pid->number) != pid->number) return unreadable;
    std::optional<std::string> start;
    if (const JsonValue* value = json->member("process_start_time")) {
        if (value->kind != JsonValue::Kind::string || value->string.empty()
            || !std::all_of(value->string.begin(), value->string.end(), [](char c) { return c >= '0' && c <= '9'; })) return unreadable;
        start = value->string;
    }
    // Only a provably gone process releases the profile.
    if (launcher::process_gone(static_cast<unsigned>(pid->number), start)) return {};
    return {State::locked, *profile};
}

} // namespace

ProfileLocks read_profile_locks(const fs::path& user_data) {
    ProfileLocks result;
    if (user_data.empty()) return result;
    std::map<std::string, std::set<int>> held;
    const auto note = [&](const fs::path& file, int instance) {
        const StateRead read = read_state(file);
        if (read.state == State::locked) held[read.profile].insert(instance);
        else if (read.state == State::unreadable) result.unreadable.push_back(instance);
    };
    note(launcher::instance_launcher_dir(user_data, 1) / "game-running.json", 1);
    std::vector<int> found;
    std::error_code error;
    for (fs::directory_iterator it(user_data / "instances", error), end; !error && it != end; it.increment(error)) {
        const auto instance = launcher::parse_instance_id(it->path().filename().wstring());
        if (instance && *instance >= 2) found.push_back(*instance);
    }
    std::sort(found.begin(), found.end());
    for (const int instance : found) note(launcher::instance_launcher_dir(user_data, instance) / "game-running.json", instance);
    for (const auto& [profile, instances] : held) result.locks.push_back({profile, {instances.begin(), instances.end()}});
    return result;
}

ProfileBlock profile_block_for(const fs::path& user_data, const std::string& profile) {
    ProfileLocks all = read_profile_locks(user_data);
    ProfileBlock block;
    block.unreadable = std::move(all.unreadable);
    for (ProfileLock& lock : all.locks)
        if (lock.profile == profile) block.instances = std::move(lock.instances);
    return block;
}

} // namespace eawr::mods_service
