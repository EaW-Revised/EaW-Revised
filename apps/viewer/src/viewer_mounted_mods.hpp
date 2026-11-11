#pragma once
// The description of the mods this process mounted, for the galactic save header (GalacticMode::Options::mounted_mods).
// Records are discovered once (prime), the content digests are requested then so they are ready by save time, and every
// save/load operation copies the current cached description. Nothing here waits for hashing: a digest that is not ready stays null.
#include "eawr/mods/content_digest.hpp"
#include "eawr/mods/save_description.hpp"
#include "eawr/mods/steam/library_folders.hpp"
#include "eawr/mods/steam/paths.hpp"
#include "viewer_load_notes.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace eawr::presentation::godot_backend {
namespace mounted_mods_detail {
struct State {
    std::mutex mutex; // guards the fields below; never held across discovery or hashing
    bool primed = false;
    bool discovered = false; // records are filled (the background discovery finished)
    mods::Profile profile; // immutable mounted layers, read without Godot on service threads
    std::thread worker;
    std::vector<mods::ModRecord> records;
    std::shared_ptr<mods::ContentDigests> digests;
    std::optional<mods::ModDescription> final_description; // set once every digest is settled
};
inline State& state() { static State s; return s; }
// Reads Godot settings: call on the main thread only.
inline mods::ContentDigestOptions digest_options() {
    mods::ContentDigestOptions options;
    const auto root = godot::ProjectSettings::get_singleton()->globalize_path("user://").utf8();
    const auto user = ViewerPath{std::string(root.get_data(), static_cast<std::size_t>(root.length()))}.native();
    if (!user.empty()) {
        auto folder = user / "mods" / "cache" / "content";
        const auto args = godot::OS::get_singleton()->get_cmdline_user_args();
        for (int i = 0; i + 1 < args.size(); ++i) {
            if (args[i] == godot::String("--eawr-digest-cache")) {
                const auto value = godot::String(args[i + 1]).utf8();
                folder = ViewerPath{std::string(value.get_data(), static_cast<std::size_t>(value.length()))}.native();
                break;
            }
        }
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        if (!ec) options.cache_folder = std::move(folder);
    }
    return options;
}
// Runs on the worker thread without touching Godot; the lock is taken only to commit the result.
inline void discover_mounted(const std::filesystem::path& game_root, const mods::Profile& profile,
                             mods::ContentDigestOptions digest_opts) {
    std::vector<mods::ModRecord> found;
    std::shared_ptr<mods::ContentDigests> digests;
    if (!profile.layers.empty()) {
        mods::DiscoveryInput input;
        if (!game_root.empty()) {
            input.mods_folder = game_root / "corruption" / "Mods";
            for (auto parent = game_root; !parent.empty() && parent != parent.parent_path(); parent = parent.parent_path())
                if (mods::steam::lower_ascii(mods::steam::path_to_utf8(parent.filename())) == "steamapps") {
                    input.steam_roots.push_back(parent.parent_path()); break;
                }
        }
        for (const auto& root : mods::native_profile_steam_reader().roots()) mods::steam::add_library(input.steam_roots, root);
        for (const auto& layer : profile.layers) input.manual_folders.push_back(mods::steam::path_from_utf8(layer.root));
        auto catalog = mods::discover(input);
        mods::read_mod_metadata(catalog);
        digests = std::make_shared<mods::ContentDigests>(std::move(digest_opts));
        for (const auto& layer : profile.layers) {
            // Only the mounted layers are hashed; apply(request=true) queues the background job and returns at once.
            if (const auto* record = mods::find_chain_record(catalog.records, layer.key)) {
                auto copy = *record;
                digests->apply(copy, true);
                found.push_back(std::move(copy));
            }
        }
    }
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.records = std::move(found);
    s.digests = std::move(digests);
    s.discovered = true; // also after a failed or empty discovery: the description is then explicitly incomplete
}
} // namespace mounted_mods_detail

// Starts the discovery of the mounted layers' records and requests their content digests on a background thread so
// the menu load does not wait for the folder walk. Call once on the main thread after mount resolution; later calls
// do nothing.
inline void prime_mounted_mods(const std::filesystem::path& game_root) {
    auto& s = mounted_mods_detail::state();
    std::lock_guard lock(s.mutex);
    if (s.primed) return;
    s.primed = true;
    mods::Profile profile = viewer_load_note_context().profile; // Godot state is read here, not on the worker
    s.profile = profile;
    auto options = mounted_mods_detail::digest_options();
    s.worker = std::thread([game_root, profile = std::move(profile), options = std::move(options)]() mutable {
        mounted_mods_detail::discover_mounted(game_root, profile, std::move(options));
    });
}

// Joins the discovery thread and stops the digest jobs. Call from the viewer host's destructor, before Godot teardown.
inline void shutdown_mounted_mods() {
    auto& s = mounted_mods_detail::state();
    std::thread worker;
    std::shared_ptr<mods::ContentDigests> digests;
    {
        std::lock_guard lock(s.mutex);
        worker = std::move(s.worker);
    }
    if (worker.joinable()) worker.join();
    {
        std::lock_guard lock(s.mutex);
        digests = std::move(s.digests);
    }
    digests.reset(); // ~ContentDigests cancels the running job and joins its thread
}

// One function for every GalacticMode::Options construction site. Cheap: re-reads digest state without touching files,
// and never waits for the discovery thread (not finished yet -> an explicitly incomplete description).
[[nodiscard]] inline std::optional<mods::ModDescription> mounted_mod_description() {
    auto& s = mounted_mods_detail::state();
    std::lock_guard lock(s.mutex);
    const auto& profile = s.profile;
    if (s.final_description) return s.final_description;
    if (!s.discovered) return mods::describe_mounted_profile(profile, {});
    bool settled = true;
    if (s.digests) for (auto& record : s.records) {
        if (record.content.state == mods::ContentDigestState::ready) continue;
        record.content = s.digests->lookup(record, false);
        if (record.content.state == mods::ContentDigestState::pending) settled = false;
    }
    auto description = mods::describe_mounted_profile(profile, s.records);
    if (settled) s.final_description = description;
    return description;
}
// Recheck asynchronous digests on the main thread without waiting for disk I/O.
[[nodiscard]] inline bool mounted_mods_ready() {
    static_cast<void>(mounted_mod_description());
    auto& s = mounted_mods_detail::state();
    std::lock_guard lock(s.mutex);
    return s.final_description.has_value();
}
// How far the content digests of the mounted layers are: files hashed so far of the files in all the trees (0, 0 while the
// discovery thread has not finished). Cheap like mounted_mods_ready(): memory only, no mod file is read.
struct MountedModProgress { std::uint64_t files_done = 0, files_total = 0; };
[[nodiscard]] inline MountedModProgress mounted_mods_progress() {
    auto& s = mounted_mods_detail::state();
    std::lock_guard lock(s.mutex);
    MountedModProgress total;
    if (!s.discovered || !s.digests) return total;
    for (const auto& record : s.records) {
        total.files_total += record.fingerprint.tree_files;
        const auto done = record.content.state == mods::ContentDigestState::ready
            ? record.fingerprint.tree_files : s.digests->progress(record.key).files;
        total.files_done += std::min(done, record.fingerprint.tree_files);
    }
    return total;
}
} // namespace eawr::presentation::godot_backend
