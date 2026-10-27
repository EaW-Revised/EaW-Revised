// Read-only engine pass. No session, VM, battle or script execution is created.
#include "report.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/mods/profile.hpp"
#include "eawr/mods/chain.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs.hpp"
#include "eawr/core/json_text.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif
namespace {
namespace fs = std::filesystem;
using eawr::core::json_quote;
using eawr::core::Severity;
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
int run(const std::vector<fs::path>& args) {
    if (args.size() != 4) return 2; // profile, user data, report, game root
    const auto user = fs::weakly_canonical(args[1]);
    const auto destination = fs::weakly_canonical(args[2]);
    const auto relative = destination.lexically_relative(user);
    if (relative.empty() || *relative.begin() == ".." || fs::is_symlink(fs::symlink_status(args[2]))) return 2;
    auto profile = eawr::mods::load_profile(args[0]);
    if (!profile.profile) return 2;
    eawr::loadcheck::Groups diagnostics;
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
                const auto& parent = edge.parent;
                const bool present = std::any_of(profile.profile->layers.begin(), profile.profile->layers.end(),
                    [&](const auto& candidate) { return candidate.key == parent; });
                requirements += ",{\"kind\":\"parent\",\"key\":" + json_quote(parent) + ",\"satisfied\":" + (present ? "true}" : "false}");
                if (!present) { ok = false; eawr::loadcheck::add(diagnostics, "EAWR-MODS-0003", "Needs parent " + parent, Severity::error); }
        }
    }
    const auto progress = [&](std::string next, unsigned done) {
        stage = std::move(next);
        emit("{\"stage\":" + json_quote(stage) + ",\"done\":" + std::to_string(done) + ",\"total\":5}");
    };
    const auto failure = [&](const eawr::core::Diagnostic& d) {
        eawr::loadcheck::add(diagnostics, d.code, d.message, d.severity); ok = false;
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
        progress("catalog", 1);
        auto catalog = eawr::data::load_catalog(filesystem.value(), profile.profile->layers.empty()
            ? eawr::data::Profile::foc : eawr::data::Profile::remake);
        if (!catalog) { failure(catalog.error()); return; }
        for (const auto& d : catalog.value().diagnostics) eawr::loadcheck::add(diagnostics, d.code, d.message, d.severity);
        if (catalog.value().health.failed()) { ok = false; return; }
        progress("types", 2);
        eawr::scene::VfsAssetCache assets(filesystem.value());
        const auto access = assets.access();
        eawr::units::LoadInput input;
        input.catalog = &catalog.value().catalog; input.filesystem = &filesystem.value();
        input.model = access.model; input.digest = access.sha256;
        for (const auto& d : catalog.value().catalog.definitions())
            if (d.namespace_winner && d.category == eawr::data::Category::game_object) input.types.push_back(d.id);
        // An empty catalog must never fall back to the pinned test fleet.
        if (input.types.empty()) { ok = false; eawr::loadcheck::add(diagnostics, "EAWR-MODS-SVC-0011", "Catalog has no game object types", Severity::error); return; }
        auto tables = eawr::units::load_unit_tables(input);
        if (!tables) { failure(tables.error()); return; }
        for (const auto& row : tables.value().unresolved)
            eawr::loadcheck::add(diagnostics, "EAWR-UNITS-0001", row.owner + ": " + row.field + ": " + row.reason, Severity::warning);
        progress("units", 3);
        std::vector<eawr::core::Diagnostic> skipped;
        const auto content = eawr::skirmish::session_content(tables.value(), {}, {}, eawr::units::ContentErrors::skip_object, &skipped);
        for (const auto& d : skipped) eawr::loadcheck::add(diagnostics, d.code, d.message, d.severity);
        if (!content) { failure(content.error()); return; }
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
    const auto report = "{\"scope\":\"space-skirmish\",\"stage\":" + json_quote(stage) + ",\"ok\":" + (ok ? "true" : "false")
        + ",\"content_code\":" + json_quote(code) + ",\"requirements\":" + requirements + "]"
        + ",\"error_count\":" + std::to_string(eawr::loadcheck::count(diagnostics, Severity::error))
        + ",\"warning_count\":" + std::to_string(eawr::loadcheck::count(diagnostics, Severity::warning))
        + ",\"diagnostics\":" + eawr::loadcheck::groups_json(diagnostics) + '}';
    std::ofstream file(args[2], std::ios::binary | std::ios::trunc);
    file << report;
    if (!file) return 3;
    emit("{\"report\":" + report + '}');
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
