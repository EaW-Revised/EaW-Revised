#include "eawr/mods/profile.hpp"
#include "eawr/mods/steam/library_folders.hpp"
#include <chrono>
#include <fstream>
#include <iostream>

using namespace eawr::mods;
namespace fs = eawr::mods::steam::fs;
namespace {
int failures = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { ++failures; std::cerr << __LINE__ << ": " #__VA_ARGS__ "\n"; } } while (false)
void write(const fs::path& file, const std::string& bytes) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << bytes;
}
bool code(const ProfileResult& result, std::string_view expected) {
    return !result.profile && result.error && result.error->code == expected;
}
class FakeSteamReader final : public ProfileSteamReader {
public:
    std::vector<fs::path> locations;
    std::vector<fs::path> roots() const override { return locations; }
};
}
int main(int argc, char* argv[]) {
    const auto scratch = fs::temp_directory_path() / ("eawr-profile-tests-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(scratch);
    if (argc == 2 && std::string_view(argv[1]) == "--symlink-only") {
        fs::create_directories(scratch / "outside");
        fs::create_directories(scratch / "user");
        std::error_code error;
        fs::create_directory_symlink(scratch / "outside", scratch / "user/mods", error);
        if (error) {
            std::cerr << "SKIP: host cannot create directory symlinks: " << error.message() << '\n';
            fs::remove_all(scratch);
            return 77;
        }
        Profile safe;
        safe.id = "safe"; safe.name = "Safe";
        const auto refusal = save_profile(scratch / "user", safe);
        CHECK(refusal && refusal->code == codes::profile_io);
        CHECK(fs::is_empty(scratch / "outside"));
        fs::remove(scratch / "user/mods");
        fs::create_directories(scratch / "user/mods");
        fs::create_directory_symlink(scratch / "outside", scratch / "user/mods/profiles");
        const auto nested_refusal = save_profile(scratch / "user", safe);
        CHECK(nested_refusal && nested_refusal->code == codes::profile_io);
        CHECK(fs::is_empty(scratch / "outside"));
        fs::remove(scratch / "user/mods/profiles");
        fs::remove_all(scratch);
        return failures ? 1 : 0;
    }
    FakeSteamReader steam_reader;
    Profile profile;
    profile.id = "sample";
    profile.name = "A profile \"with quotes\"";
    profile.order_source = "player";
    profile.layers = {{"workshop:123456", steam::path_to_utf8(scratch / "leaf")},
                      {"mods:parent", steam::path_to_utf8(scratch / "parent")}};
    profile.separators = {{0, "Top"}, {2, "End"}};
    profile.launch.render_thread = "separate";
    profile.launch.frame_rate_limit = -1;
    profile.accepted = {"fingerprint", "ok-with-warnings"};
    for (const auto& layer : profile.layers) fs::create_directories(steam::path_from_utf8(layer.root));
    const auto roundtrip = profile_from_json(profile_to_json(profile));
    CHECK(roundtrip.profile && *roundtrip.profile == profile);
    const auto create = profile_from_json(R"({"id":"fresh","name":"Fresh","layers":[]})");
    CHECK(create.profile && create.profile->launch == ProfileLaunch{} && create.profile->order_source == "chain");
    CHECK(create.profile && !create.profile->launch.frame_rate_limit);
    const auto inherit = profile_from_json(R"({"id":"x","name":"x","layers":[],"launch":{"frame_rate_limit":null}})");
    CHECK(inherit.profile && !inherit.profile->launch.frame_rate_limit);
    for (int cap : {-1, 0, 144}) {
        auto explicit_cap = *create.profile;
        explicit_cap.launch.frame_rate_limit = cap;
        const auto cap_roundtrip = profile_from_json(profile_to_json(explicit_cap));
        CHECK(cap_roundtrip.profile && cap_roundtrip.profile->launch.frame_rate_limit == cap);
        CHECK(create.profile->launch.frame_rate_limit.value_or(cap) == cap);
    }
    CHECK(code(profile_from_json("{"), codes::profile_json));
    CHECK(code(profile_from_json(R"({"version":2})"), codes::profile_version));
    CHECK(code(profile_from_json(R"({"id":"../escape","name":"x","layers":[]})"), codes::profile_id));
    CHECK(code(profile_from_json(R"({"id":"CON","name":"x","layers":[]})"), codes::profile_id));
    CHECK(code(profile_from_json(R"({"id":"x","name":"x","layers":[],"order_source":1})"), codes::profile_fields));
    CHECK(code(profile_from_json(R"({"id":"x","name":"x","layers":[{"key":"k","root":"relative"}]})"), codes::profile_layer));
    CHECK(code(profile_from_json(R"({"id":"x","name":"x","layers":[],"launch":{"frame_rate_limit":1.5}})"), codes::profile_launch));
    CHECK(code(profile_from_json(R"({"id":"x","name":"x","layers":[],"separators":[{"before":1,"label":"bad"}]})"), codes::profile_fields));
    auto duplicate = profile;
    duplicate.layers.push_back(duplicate.layers.front());
    CHECK(validate_profile(duplicate)->code == codes::profile_layer);
    const auto user = scratch / "user";
    CHECK(!save_profile(user, profile));
    const auto file = user / "mods/profiles/sample.json";
    CHECK(load_profile(file).profile == roundtrip.profile);
    profile.name = "Replacement";
    CHECK(!save_profile(user, profile));
    CHECK(load_profile(file).profile->name == "Replacement");
#ifdef _WIN32
    // A real replace failure at the same id preserves every previously committed byte.
    const auto committed = steam::read_text_file(file);
    fs::permissions(file, fs::perms::owner_read, fs::perm_options::replace);
    auto rejected = profile; rejected.name = "Cannot replace read-only target";
    const auto replace_failure = save_profile(user, rejected);
    fs::permissions(file, fs::perms::owner_all, fs::perm_options::add);
    CHECK(replace_failure && replace_failure->code == codes::profile_io);
    CHECK(steam::read_text_file(file) == committed);
#endif
    // A failed replacement preserves the last complete file and leaves no temporary directories.
    auto invalid = profile;
    invalid.id = "../escape";
    CHECK(save_profile(user, invalid)->code == codes::profile_id);
    CHECK(load_profile(file).profile->name == "Replacement");
    fs::create_directory(file.parent_path() / "blocked.json");
    auto blocked = profile; blocked.id = "blocked";
    CHECK(save_profile(user, blocked)->code == codes::profile_io);
    for (const auto& entry : fs::directory_iterator(file.parent_path())) CHECK(!entry.path().filename().string().starts_with(".profile-"));
    write(scratch / "unversioned.json", R"({"id":"x","name":"x","layers":[]})");
    CHECK(code(load_profile(scratch / "unversioned.json"), codes::profile_version));
    CHECK(code(load_profile(scratch / "absent.json"), codes::profile_io));

    // Steam library moved: keep key and explicit order, rediscover its new absolute root.
    DiscoveryInput discovery;
    discovery.steam_libraries = {scratch / "new-library"};
    const auto moved_root = (scratch / "new-library/steamapps/workshop/content/32470/123456").lexically_normal();
    fs::create_directories(moved_root / "Data");
    auto moved = profile;
    moved.layers.front().root = steam::path_to_utf8(scratch / "old-library/missing");
    const auto resolved = resolve_profile(moved, discovery);
    CHECK(resolved.profile && resolved.profile->layers.front().root == steam::path_to_utf8(moved_root));
    CHECK(resolved.profile && resolved.profile->layers.back() == profile.layers.back());
    // The platform adapter's injected root discovers moved libraries through libraryfolders.vdf.
    steam_reader.locations = {scratch / "steam-client"};
    const auto library_text = steam::path_to_utf8(discovery.steam_libraries.front());
    std::string escaped;
    for (char c : library_text) { if (c == '\\') escaped += '\\'; escaped += c; }
    write(steam_reader.locations.front() / "steamapps/libraryfolders.vdf",
        "\"libraryfolders\" { \"1\" { \"path\" \"" + escaped + "\" } }");
    CHECK(!save_profile(user, moved));
    const auto moved_cli = expand_profile_arguments({"--eawr-mod-profile", steam::path_to_utf8(file)}, {},
        "--eawr-mod-root", &steam_reader);
    CHECK(!moved_cli.error && moved_cli.arguments.back() == steam::path_to_utf8(moved_root) + ';' + profile.layers.back().root);
    CHECK(!save_profile(user, profile));
    moved.layers.front().key = "workshop:999999";
    const auto missing = resolve_profile(moved, discovery);
    CHECK(code(missing, codes::profile_missing_key));
    CHECK(missing.error->message.find("workshop:999999") != std::string::npos);

    // This exact shared pre-pass is used by the viewer and sim_headless, before mode dispatch.
    const auto roots = profile.layers[0].root + ';' + profile.layers[1].root;
    for (const auto* flag : {"--eawr-mod-root", "--mod-root"}) {
        const auto from_profile = expand_profile_arguments({"--eawr-mod-profile", steam::path_to_utf8(file)}, {}, flag, &steam_reader);
        const auto direct = expand_profile_arguments({flag, roots}, {}, flag);
        CHECK(!from_profile.error && from_profile.arguments == direct.arguments);
        CHECK(from_profile.launch && from_profile.launch->frame_rate_limit == -1);
        const auto override = expand_profile_arguments({"--eawr-mod-profile", "nonexistent.json", flag, roots}, {}, flag);
        CHECK(!override.error && override.arguments == direct.arguments && !override.report.empty());
    }
    CHECK(expand_profile_arguments({"--eawr-mod-profile"}).error->code == codes::profile_arguments);
    const std::vector<std::string> single_root{"--eawr-mod-root", profile.layers.front().root};
    CHECK(expand_profile_arguments(single_root).arguments == single_root);
    CHECK(expand_profile_arguments(single_root).report.empty());
    auto vanilla = *create.profile;
    CHECK(!save_profile(user, vanilla));
    const auto unmodded = expand_profile_arguments({"--eawr-mod-profile", steam::path_to_utf8(user / "mods/profiles/fresh.json")});
    CHECK(!unmodded.error && unmodded.arguments.empty());
    fs::remove_all(scratch);
    return failures ? 1 : 0;
}
