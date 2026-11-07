// Read-only engine pass. No session, VM, battle or script execution is created.
#include "report.hpp"
#include "records.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/mods/profile.hpp"
#include "eawr/mods/file_index.hpp"
#include "eawr/mods/chain.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/units/simulation_model.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs.hpp"
#include "eawr/core/json_text.hpp"
#include "eawr/core/load_profile_report.hpp"
#include "eawr/platform/load_notes.hpp"
#include "eawr/core/build_identity.hpp"
#include "eawr/mods/fingerprint.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#include <winioctl.h>
#include <shellapi.h>
#endif
namespace {
namespace fs = std::filesystem;
using eawr::core::json_quote;
using eawr::core::Severity;
// Working set of this process, for the memory samples of a load profile (zero off Windows).
struct MemorySample { std::uint64_t working_set{}, peak{}; };
MemorySample process_memory() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        return {counters.WorkingSetSize, counters.PeakWorkingSetSize};
#endif
    return {};
}
// Clears the thread's recorder pointer on every exit path, including an exception out of the load.
struct RecorderGuard {
    ~RecorderGuard() { eawr::core::load_profile::active = nullptr; }
};
// The value of EAWR_LOAD_PROFILE, empty when unset or empty. Asks for the needed size first, so any length works.
fs::path profile_path_from_environment() {
#ifdef _WIN32
    std::size_t length{};
    if (_wgetenv_s(&length, nullptr, 0, L"EAWR_LOAD_PROFILE") != 0 || length <= 1) return {};
    std::vector<wchar_t> value(length);
    if (_wgetenv_s(&length, value.data(), value.size(), L"EAWR_LOAD_PROFILE") != 0 || length <= 1) return {};
    return fs::path(std::wstring(value.data()));
#else
    if (const char* env = std::getenv("EAWR_LOAD_PROFILE"); env && *env) return fs::path(env);
    return {};
#endif
}
// Whether the volume of `path` is a spinning disk or solid state; unknown if the path or the OS will not say.
eawr::units::DiskKind volume_disk_kind(const fs::path& path) {
    std::error_code ec;
    const auto absolute = fs::absolute(path, ec);
    return ec ? eawr::units::DiskKind::unknown : eawr::units::volume_disk_kind(absolute.root_name().wstring());
}

void emit(const std::string& text) {
#ifdef _WIN32
    auto line = text + '\n';
    while (!line.empty()) {
        DWORD written{};
        if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line.data(), static_cast<DWORD>(line.size()), &written, nullptr) || !written) return;
        line.erase(0, written);
    }
#else
    std::cout << text << std::endl;
#endif
}
int run(std::vector<fs::path> args) {
    if (!args.empty() && args.front() == "--records") {
        args.erase(args.begin());
        return eawr::loadcheck::records(args);
    }
    bool notes_requested = false, notes_json = false;
    std::erase_if(args, [&](const fs::path& arg) {
        if (arg == "--notes") { notes_requested = true; return true; }
        if (arg == "--json") { notes_json = true; return true; }
        return false;
    });
    if (notes_json && !notes_requested) return 2;
    // Opt-in measurement (--load-profile <out.json> or EAWR_LOAD_PROFILE): a recording of the load plus a type cap that
    // lets a run be repeated at growing sizes. Neither changes what is loaded when absent.
    fs::path profile_out;
    std::size_t type_limit = 0;
    bool type_limit_given = false;
    for (std::size_t i = 0; i < args.size();) {
        const bool out_flag = args[i] == "--load-profile", limit_flag = args[i] == "--load-profile-type-limit";
        if (!out_flag && !limit_flag) { ++i; continue; }
        if (i + 1 >= args.size()) return 2;
        if (out_flag) profile_out = args[i + 1];
        else {
            const auto text = args[i + 1].string();
            std::size_t used = 0;
            try { type_limit = std::stoull(text, &used); } catch (...) { return 2; }
            if (used != text.size() || type_limit == 0) return 2;
            type_limit_given = true;
        }
        args.erase(args.begin() + static_cast<std::ptrdiff_t>(i), args.begin() + static_cast<std::ptrdiff_t>(i) + 2);
    }
    if (profile_out.empty()) profile_out = profile_path_from_environment();
    if (type_limit_given && profile_out.empty()) {
        // A capped load must never be mistaken for a full one: the cap only exists inside a measurement run.
        std::cerr << "--load-profile-type-limit needs --load-profile <out.json> or EAWR_LOAD_PROFILE" << '\n';
        return 2;
    }
    eawr::core::load_profile::Recording recording;
    const RecorderGuard recorder_guard;
    if (!profile_out.empty()) eawr::core::load_profile::active = &recording;
    std::size_t types_available = 0;
    std::string memory_samples, model_cache;
    // The process memory when a stage begins (so, at the end of the one before) and at the end of the run.
    const auto sample_memory = [&](const std::string& stage_name) {
        if (profile_out.empty()) return;
        const auto memory = process_memory();
        memory_samples += std::string(memory_samples.empty() ? "" : ",") + "{\"before\":" + json_quote(stage_name)
            + ",\"working_set\":" + std::to_string(memory.working_set) + ",\"peak_working_set\":"
            + std::to_string(memory.peak) + '}';
    };
    if (args.size() != 4 && args.size() != 6) return 2; // profile, user data, report, game root, optional key/index
    const auto user = fs::weakly_canonical(args[1]);
    const auto destination = fs::weakly_canonical(args[2]);
    const auto relative = destination.lexically_relative(user);
    if (relative.empty() || *relative.begin() == ".." || fs::is_symlink(fs::symlink_status(args[2]))) return 2;
    if (args.size() == 6) {
        const auto index_relative = fs::weakly_canonical(args[5]).lexically_relative(user);
        if (index_relative.empty() || *index_relative.begin() == ".." || fs::is_symlink(fs::symlink_status(args[5]))) return 2;
    }
    auto profile = eawr::mods::load_profile(args[0]);
    if (!profile.profile) return 2;
    eawr::loadcheck::Groups diagnostics;
    eawr::core::LoadNoteCollector notes;
    std::string stage = "requirements", code;
    bool ok = true;
    std::string requirements = "[{\"kind\":\"foc\",\"key\":\"foc\",\"satisfied\":";
    requirements += fs::is_directory(args[3] / "corruption/Data") ? "true}" : "false}";
    eawr::mods::DiscoveryInput discovery;
    for (const auto& layer : profile.profile->layers) discovery.manual_folders.push_back(eawr::mods::steam::path_from_utf8(layer.root));
    auto records = eawr::mods::discover(discovery);
    eawr::mods::read_mod_metadata(records);
    for (auto& record : records.records)
        for (const auto& layer : profile.profile->layers)
            if (record.root == layer.root) {
                const auto* variant = record.metadata.string_member("variant_file");
                const auto marker = layer.key.find("#variant:");
                if ((!variant && marker == std::string::npos)
                    || (variant && marker != std::string::npos && layer.key.substr(marker + 9) == *variant)) record.key = layer.key;
            }
    for (const auto& layer : profile.profile->layers) {
        const auto chain = eawr::mods::resolve_chain(layer.key, records.records, {});
        for (const auto& edge : chain.edges) {
                // A launch-script MODPATH only hints at a parent (often the mod's own folder name) and needs player
                // confirmation: it is never a requirement. The shipped known chains reach the service per request,
                // not this helper, so modinfo/player edges stay the only hard requirements here.
                if (edge.why == "launch-script") continue;
                const auto& parent = edge.parent;
                const bool present = std::any_of(profile.profile->layers.begin(), profile.profile->layers.end(),
                    [&](const auto& candidate) { return candidate.key == parent; });
                requirements += ",{\"kind\":\"parent\",\"key\":" + json_quote(parent) + ",\"satisfied\":" + (present ? "true}" : "false}");
                if (!present) { ok = false; eawr::loadcheck::add(diagnostics, "EAWR-MODS-0003", "Needs parent " + parent, Severity::error); }
        }
    }
    const auto progress = [&](std::string next, unsigned done) {
        sample_memory(next);
        stage = std::move(next);
        emit("{\"stage\":" + json_quote(stage) + ",\"done\":" + std::to_string(done) + ",\"total\":5}");
    };
    const auto failure = [&](const eawr::core::Diagnostic& d) {
        eawr::loadcheck::add(diagnostics, d); ok = false;
    };
    const auto pass = [&] {
        progress("mount", 0);
        std::vector<eawr::vfs::LayerRoot> roots;
        for (const auto& layer : profile.profile->layers) {
            roots.emplace_back(layer.key, eawr::vfs::mod_data_root(eawr::mods::steam::path_from_utf8(layer.root)));
        }
        roots.emplace_back("expansion", args[3] / "corruption/Data");
        roots.emplace_back("base", args[3] / "GameData/Data");
        auto chain = eawr::vfs::resolve_manifest_chain(roots);
        if (!chain) { failure(chain.error()); return; }
        std::vector<eawr::vfs::MountSpec> mounts;
        for (auto& manifest : chain.value()) mounts.push_back(std::move(manifest.mount));
        auto filesystem = eawr::vfs::Vfs::mount(mounts);
        if (!filesystem) { failure(filesystem.error()); return; }
        if (args.size() == 6) {
            progress("files", 0);
            std::vector<std::string> layers;
            for (const auto& mount : mounts) layers.push_back(mount.layer_id);
            // Conflict inspection is optional: it cannot invalidate playable content.
            const auto index_warning = [&](std::string message) {
                std::error_code ec;
                fs::remove(args[5], ec);
                eawr::loadcheck::add(diagnostics, "EAWR-MODS-SVC-0013", std::move(message), Severity::warning);
            };
            try {
                auto index = eawr::mods::build_file_index(filesystem.value(), layers, eawr::mods::steam::path_to_utf8(args[4]));
                if (!index) index_warning("File index unavailable: " + index.error().message);
                else {
                    std::ofstream cache(args[5], std::ios::binary | std::ios::trunc);
                    const bool written = eawr::mods::write_file_index(cache, index.value());
                    cache.close(); // Include delayed flush/close failures before publishing.
                    if (!written || !cache) index_warning("Could not write the file index cache");
                }
            } catch (const std::exception& error) {
                index_warning("File index unavailable: " + std::string(error.what()));
            }
        }
        progress("catalog", 1);
        auto catalog = eawr::data::load_catalog(filesystem.value(), profile.profile->layers.empty()
            ? eawr::data::Profile::foc : eawr::data::Profile::remake);
        if (!catalog) { failure(catalog.error()); return; }
        std::vector<std::string> mod_ids;
        for (const auto& layer : profile.profile->layers) mod_ids.push_back(layer.key);
        const std::vector<std::string> stock_ids{"expansion", "base"};
        eawr::core::bind_load_note_layers(catalog.value().notes, mod_ids, stock_ids);
        for (auto& note : catalog.value().notes) notes.append(std::move(note));
        for (const auto& d : catalog.value().diagnostics) eawr::loadcheck::add(diagnostics, d);
        if (catalog.value().health.failed()) { ok = false; return; }
        progress("types", 2);
        // The load check reads each model once and keeps only the simulation projection: no VfsAssetCache pins.
        eawr::units::SimulationModelStore models(filesystem.value());
        eawr::units::LoadInput input;
        input.catalog = &catalog.value().catalog; input.filesystem = &filesystem.value();
        input.model = models.model_lookup(); input.digest = models.digest_lookup();
        {
            eawr::core::load_profile::Scope select_scope(eawr::core::load_profile::Phase::type_select);
            for (const auto& d : catalog.value().catalog.definitions())
                if (d.namespace_winner && d.category == eawr::data::Category::game_object) input.types.push_back(d.id);
            types_available = input.types.size();
            // Catalog order is the registry order: the first N entries are the same on every run.
            if (type_limit && input.types.size() > type_limit) input.types.resize(type_limit);
            if (type_limit)
                eawr::loadcheck::add(diagnostics, "EAWR-MODS-LC-0001", eawr::core::load_profile::partial_load_message(input.types.size(), types_available, type_limit),
                    Severity::warning);
        }
        // An empty catalog must never fall back to the pinned test fleet.
        if (input.types.empty()) { ok = false; eawr::loadcheck::add(diagnostics, "EAWR-MODS-SVC-0011", "Catalog has no game object types", Severity::error); return; }
        {
            // Decode the planned models on worker threads ahead of the loader, which still takes them in its own order.
            eawr::units::PrefetchOptions prefetch;
            // Models come from the game install and from every mod layer: the slowest volume sets the reader depth.
            std::vector<eawr::units::DiskKind> kinds{volume_disk_kind(args[3])};
            for (const auto& layer : profile.profile->layers)
                kinds.push_back(volume_disk_kind(eawr::mods::steam::path_from_utf8(layer.root)));
            prefetch.disk = eawr::units::slowest_disk(kinds);
            models.prefetch(eawr::units::planned_model_paths(input), prefetch);
        }
        auto tables = eawr::units::load_unit_tables(input);
        if (!profile_out.empty()) {
            const auto stats = models.stats();
            model_cache = "{\"calls\":" + std::to_string(stats.requests) + ",\"unique_loads\":" + std::to_string(stats.decoded)
                + ",\"entries\":" + std::to_string(stats.retained) + ",\"read_bytes\":" + std::to_string(stats.read_bytes)
                + ",\"hashed_bytes\":" + std::to_string(stats.hashed_bytes) + ",\"live_full_models\":"
                + std::to_string(stats.live_full) + ",\"peak_full_models\":" + std::to_string(stats.peak_full)
                + ",\"prefetch_planned\":" + std::to_string(stats.prefetch_planned) + ",\"served_ready\":"
                + std::to_string(stats.served_ready) + ",\"served_waited\":" + std::to_string(stats.served_waited)
                + ",\"served_claimed\":" + std::to_string(stats.served_claimed) + ",\"wait_us\":" + std::to_string(stats.wait_us)
                + ",\"readers_peak\":" + std::to_string(stats.readers_peak) + ",\"readers_limit_final\":"
                + std::to_string(stats.readers_limit_final) + ",\"window_bytes\":" + std::to_string(stats.window_bytes)
                + ",\"disk\":" + (stats.disk == 2 ? "\"spinning\"" : stats.disk == 1 ? "\"solid_state\"" : "\"unknown\"") + '}';
        }
        if (!tables) { failure(tables.error()); return; }
        for (const auto& row : tables.value().unresolved)
            eawr::loadcheck::add(diagnostics, "EAWR-UNITS-0001", row.owner + ": " + row.field + ": " + row.reason, Severity::warning);
        progress("units", 3);
        std::vector<eawr::core::Diagnostic> skipped;
        const auto content = eawr::skirmish::session_content(tables.value(), {}, {}, eawr::units::ContentErrors::skip_object, &skipped);
        for (const auto& d : skipped) eawr::loadcheck::add(diagnostics, d);
        if (!content) { failure(content.error()); return; }
        for (const auto& note : content.value().load_notes) notes.append(note);
        if (!skipped.empty())
            eawr::loadcheck::add(diagnostics, "EAWR-UNITS-0007", eawr::skirmish::content_load_summary(skipped), Severity::info);
        progress("identity", 4);
        auto data = eawr::skirmish::read_data_identity(filesystem.value());
        if (!data) { failure(data.error()); return; }
        eawr::skirmish::StartInputs identity;
        identity.tables = &tables.value();
        identity.script_manifest_digest = data.value().script_manifest_digest;
        identity.ai_data_digest = data.value().ai_data_digest;
        const auto skipped_ids = eawr::skirmish::skipped_type_ids(skipped);
        code = eawr::loadcheck::content_code(eawr::skirmish::match_identity(identity, skipped_ids));
        progress("complete", 5);
    };
    pass();
    sample_memory("end");
    eawr::core::load_profile::active = nullptr;
    if (!profile_out.empty()) {
        std::ofstream profile_file(profile_out, std::ios::binary | std::ios::trunc);
        eawr::core::load_profile::write_report(profile_file, recording, 50,
            "\"types_available\":" + std::to_string(types_available) + ",\"type_limit\":" + std::to_string(type_limit)
            + ",\"stage\":" + json_quote(stage) + ",\"memory\":[" + memory_samples + "],\"model_cache\":"
            + (model_cache.empty() ? std::string("null") : model_cache));
        if (!profile_file) return 3;
    }
    const auto snapshot = notes.snapshot();
    eawr::core::LoadNoteHeader header;
    header.profile_id = profile.profile->id;
    header.chain_fingerprint = args.size() == 6 ? eawr::mods::steam::path_to_utf8(args[4])
        : eawr::mods::loadcheck_chain_fingerprint(*profile.profile, args[3], eawr::core::build_identity());
    header = eawr::platform::stamp_load_note_header(std::move(header));
    const auto saved_notes = eawr::platform::write_load_notes(user, header, snapshot);
    if (saved_notes.failure)
        eawr::loadcheck::add(diagnostics, "EAWR-LOAD-0002", saved_notes.failure->message, Severity::warning);
    for (const auto& note : snapshot)
        std::cerr << eawr::core::format_diagnostic(eawr::core::load_note_diagnostic(note)) << '\n';
    const auto report = "{\"scope\":\"space-skirmish\",\"stage\":" + json_quote(stage) + ",\"ok\":" + (ok ? "true" : "false")
        + ",\"content_code\":" + json_quote(code) + ",\"requirements\":" + requirements + "]"
        + ",\"error_count\":" + std::to_string(eawr::loadcheck::count(diagnostics, Severity::error))
        + ",\"warning_count\":" + std::to_string(eawr::loadcheck::count(diagnostics, Severity::warning))
        + ",\"diagnostics\":" + eawr::loadcheck::groups_json(diagnostics) + '}';
    std::ofstream file(args[2], std::ios::binary | std::ios::trunc);
    file << report;
    if (!file) return 3;
    emit("{\"report\":" + report + '}');
    if (notes_requested) {
        auto listing = notes_json ? eawr::core::load_notes_json(snapshot, header) : eawr::core::load_notes_text(snapshot);
        if (!listing.empty() && listing.back() == '\n') listing.pop_back();
        emit(listing);
    }
    return 0; // Reported loader failures are successful transport, ok=false.
}
}
#ifdef _WIN32
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int count{}; auto argv = CommandLineToArgvW(GetCommandLineW(), &count);
    std::vector<fs::path> args;
    for (int i = 1; argv && i < count; ++i) args.emplace_back(argv[i]);
    if (argv) LocalFree(argv);
    try { return run(args); } catch (...) { return 3; }
}
#else
int main(int argc, char** argv) {
    std::vector<fs::path> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    try { return run(args); } catch (...) { return 3; }
}
#endif
