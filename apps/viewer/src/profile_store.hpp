#pragma once

// #2935: the viewer's side of the player profile (docs/ui/player-profile.md). The record, its
// text form and the defaults are engine-free (eawr/presentation/ui/player_profile.hpp); this
// file chooses where the record lives: `user://eawr-profile.cfg`, beside the settings file.
// The profile is UI state (UI-C2): nothing here reaches the simulation, a replay or a hash.

#include "eawr/presentation/ui/player_profile.hpp"

#include <filesystem>
#include <string>
#include <utility>

namespace eawr::presentation::godot_backend {

class ProfileStore final {
public:
    ProfileStore() = default;
    // A test or a capture run names its own file.
    explicit ProfileStore(std::filesystem::path file) : file_(std::move(file)) {}

    // The file in use: the given path, else the user data folder's.
    [[nodiscard]] std::filesystem::path file() const;
    [[nodiscard]] bool exists() const;
    // A missing or unreadable file is the defaults (`defaulted`, no note for a missing one).
    [[nodiscard]] presentation::ui::ProfileLoad load() const;
    // Written to a sibling file and renamed over the target, so a crash keeps the old file.
    [[nodiscard]] bool save(const presentation::ui::PlayerProfile& profile, std::string& error) const;

private:
    std::filesystem::path file_;
};

} // namespace eawr::presentation::godot_backend
