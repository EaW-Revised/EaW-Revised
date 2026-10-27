#pragma once
// Sole owner of eawr-mod-profile v1. Layer zero is highest priority; separators never mount content.
#include "eawr/mods/discovery.hpp"
#include <string_view>

namespace eawr::mods {
namespace codes {
inline constexpr std::string_view profile_json = "EAWR-MODS-0100";
inline constexpr std::string_view profile_version = "EAWR-MODS-0101";
inline constexpr std::string_view profile_id = "EAWR-MODS-0102";
inline constexpr std::string_view profile_fields = "EAWR-MODS-0103";
inline constexpr std::string_view profile_layer = "EAWR-MODS-0104";
inline constexpr std::string_view profile_launch = "EAWR-MODS-0105";
inline constexpr std::string_view profile_io = "EAWR-MODS-0106";
inline constexpr std::string_view profile_missing_key = "EAWR-MODS-0107";
inline constexpr std::string_view profile_arguments = "EAWR-MODS-0108";
}
struct ProfileLayer {
    std::string key;
    std::string root;
    friend bool operator==(const ProfileLayer&, const ProfileLayer&) = default;
};
struct ProfileSeparator {
    std::size_t before{};
    std::string label;
    friend bool operator==(const ProfileSeparator&, const ProfileSeparator&) = default;
};
struct ProfileLaunch {
    std::string mode = "front-end";
    std::string render_thread = "safe";
    std::optional<int> frame_rate_limit; // absent/null inherits settings; -1 display, 0 off, positive cap
    friend bool operator==(const ProfileLaunch&, const ProfileLaunch&) = default;
};
struct ProfileAccepted {
    std::string chain_fingerprint;
    std::string load_check;
    friend bool operator==(const ProfileAccepted&, const ProfileAccepted&) = default;
};
struct Profile {
    std::string id;
    std::string name;
    std::vector<ProfileLayer> layers;
    std::string order_source = "chain";
    std::vector<ProfileSeparator> separators;
    ProfileLaunch launch;
    ProfileAccepted accepted;
    friend bool operator==(const Profile&, const Profile&) = default;
};
struct ProfileResult {
    std::optional<Profile> profile;
    std::optional<Warning> error;
};
[[nodiscard]] std::optional<Warning> validate_profile(const Profile& profile);
[[nodiscard]] std::string profile_to_json(const Profile& profile);
// Missing optional fields take the defaults above (also used for service create intent).
// Present format/version must match; required on disk (load_profile).
[[nodiscard]] ProfileResult profile_from_json(std::string_view text);
[[nodiscard]] ProfileResult load_profile(const steam::fs::path& file);
// Only writes <user_data>/mods/profiles/<id>.json. Rejects symlinked descendant folders;
// replacement is a single atomic rename, including on Windows. Never writes to mod roots.
[[nodiscard]] std::optional<Warning> save_profile(const steam::fs::path& user_data, const Profile& profile);
[[nodiscard]] ProfileResult resolve_profile(Profile profile, const DiscoveryInput& input);
// Engine adapters share this pre-pass. root_flag is --eawr-mod-root (viewer) or --mod-root (sim).
// Explicit roots win, even if the profile is unreadable; the report says so.
struct ProfileArguments {
    std::optional<Profile> profile;
    std::vector<std::string> arguments;
    std::optional<ProfileLaunch> launch;
    std::optional<Warning> error;
    std::vector<std::string> report;
};
// Native registry access is isolated in a Windows-only adapter; tests inject synthetic locations.
class ProfileSteamReader {
public:
    virtual ~ProfileSteamReader() = default;
    [[nodiscard]] virtual std::vector<steam::fs::path> roots() const = 0;
};
[[nodiscard]] const ProfileSteamReader& native_profile_steam_reader();
[[nodiscard]] ProfileArguments expand_profile_arguments(const std::vector<std::string>& arguments,
    DiscoveryInput input = {}, std::string_view root_flag = "--eawr-mod-root", const ProfileSteamReader* steam_reader = nullptr);
} // namespace eawr::mods
