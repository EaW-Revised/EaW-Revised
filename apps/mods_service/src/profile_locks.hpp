#pragma once
// Which saved profiles a running game instance holds (#5619, P4). Derived only from the launcher's state files
// (`launcher/game-running.json` of instance 1, `instances/<n>/launcher/game-running.json` of the others) and
// the liveness of the process each names; never from anything the caller says.
#include "eawr/mods/steam/paths.hpp"

#include <string>
#include <vector>

namespace eawr::mods_service {

struct ProfileLock {
    std::string profile;
    std::vector<int> instances;   // ascending; the same profile may run in several instances
};

struct ProfileLocks {
    std::vector<ProfileLock> locks;   // sorted by profile id
    std::vector<int> unreadable;      // instances whose state file exists but could not be read or understood (ascending)
};

// A missing state file, a state without a profile id (a vanilla instance), or one whose process is gone (dead PID,
// or a reused PID with another creation time) holds nothing. A state file that EXISTS but cannot be opened, is
// oversized or malformed is not "free": which profile it names is unknown, so its instance is reported in
// `unreadable` and callers fail closed (the same rule as for an unreadable launcher claim).
[[nodiscard]] ProfileLocks read_profile_locks(const mods::steam::fs::path& user_data);

// What stops a change of `profile`: the instances running it and the instances with an unreadable state.
struct ProfileBlock {
    std::vector<int> instances;
    std::vector<int> unreadable;
    [[nodiscard]] bool blocked() const { return !instances.empty() || !unreadable.empty(); }
};
[[nodiscard]] ProfileBlock profile_block_for(const mods::steam::fs::path& user_data, const std::string& profile);

} // namespace eawr::mods_service
