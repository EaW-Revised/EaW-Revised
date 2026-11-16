#pragma once

#include "eawr/platform/load_notes.hpp"
#include "eawr/core/degradation_ledger.hpp"
#include "eawr/core/build_identity.hpp"
#include "eawr/mods/fingerprint.hpp"
#include "viewer_path.hpp"
#include "viewer_profile_arguments.hpp"
#include "startup_window.hpp"

#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace eawr::presentation::godot_backend {
struct ViewerLoadNoteContext {
    mods::Profile profile;
    core::LoadNoteHeader header;
    core::LoadNoteCollector collected;
    std::vector<std::string> layers;
};
inline ViewerLoadNoteContext& viewer_load_note_context(const bool startup = false) {
    static auto context = [startup] {
        ViewerLoadNoteContext result;
        const auto& selected = viewer_profile_options().profile;
        if (selected) { result.profile = *selected; result.header.profile_id = selected->id; }
        std::filesystem::path game;
        const auto args = viewer_profile_arguments();
        std::vector<std::filesystem::path> roots;
        for (int64_t i = 0; i + 1 < args.size(); ++i) {
            const auto flag = args[i];
            if (flag == "--eawr-game-root") game = ViewerPath{std::string(args[i + 1].utf8().get_data())}.native();
            if (!selected && flag == "--eawr-mod-root") {
                const std::string chain = args[i + 1].utf8().get_data();
                for (std::size_t start = 0; start < chain.size();) {
                    auto end = chain.find(';', start);
                    if (end == std::string::npos) end = chain.size();
                    roots.push_back(ViewerPath{chain.substr(start, end - start)}.native());
                    start = end + 1;
                }
            }
        }
        if (!selected && !roots.empty()) {
            mods::DiscoveryInput input; input.manual_folders = roots;
            const auto discovered = mods::discover(input);
            for (const auto& root : roots) {
                const auto key = mods::manual_key(root);
                for (const auto& record : discovered.records) if (record.key == key) {
                    result.profile.layers.push_back({record.key, record.root}); break;
                }
            }
        }
        if (!game.empty()) {
            auto fingerprint = [&] {
                return mods::loadcheck_chain_fingerprint(result.profile, game, core::build_identity());
            };
            result.header.chain_fingerprint = startup ? read_with_startup_messages(fingerprint) : fingerprint();
        }
        return result;
    }();
    return context;
}
// Also called during gameplay. Only the dedicated startup caller supplies a
// message check; ordinary publication must never dispatch into half-updated modes.
inline void publish_viewer_load_notes(std::vector<core::LoadNote> notes,
                                      const std::vector<std::string>& layers, bool append = false,
                                      void (*startup_message_check)() = nullptr) {
    auto& context = viewer_load_note_context();
    if (!append) { context.collected.reset(); context.layers = layers; }
    std::vector<std::string> raw_mods;
    for (const auto& layer : context.layers) if (layer != "base" && layer != "expansion") raw_mods.push_back(layer);
    const std::vector<std::string> stock{"base", "expansion"};
    core::bind_load_note_layers(notes, raw_mods, stock);
    for (auto& note : notes) {
        if (note.layer.chain_position && *note.layer.chain_position < context.profile.layers.size())
            note.layer.mod_id = context.profile.layers[static_cast<std::size_t>(*note.layer.chain_position)].key;
        godot::UtilityFunctions::print(godot::String::utf8(core::format_diagnostic(core::load_note_diagnostic(note)).c_str()));
        if (startup_message_check) startup_message_check();
        // The ledger takes the note with its final (profile key) layer id, so a mod is one source however it was bound.
        core::degradation::record_load_notes(std::span<const core::LoadNote>(&note, 1));
        context.collected.append(std::move(note));
    }
    {
        // The session summary names the chain it ran on (the context's fingerprint is computed once, here).
        core::degradation::SessionInfo info;
        info.chain_fingerprint = context.header.chain_fingerprint;
        // The notes carry the layer key; the summary names a mod by its stable id (a Workshop folder given by path is
        // `workshop:<id>`, like the headless runs).
        for (const auto& layer : context.profile.layers) {
            info.mod_layers.push_back(layer.key);
            info.mods.push_back(layer.key.starts_with("path:") && !layer.root.empty()
                                    ? mods::folder_mod_id(mods::steam::path_from_utf8(layer.root)) : layer.key);
        }
        core::degradation::describe_session(info);
    }
    auto* settings = godot::ProjectSettings::get_singleton();
    if (settings == nullptr) {
        godot::UtilityFunctions::printerr(godot::String("load notes: not written (the project settings are not available)"));
        return;
    }
    const auto root = settings->globalize_path("user://").utf8();
    const auto user = ViewerPath{std::string(root.get_data(), static_cast<std::size_t>(root.length()))}.native();
    const auto result = platform::write_load_notes(user, context.header, context.collected.snapshot());
    if (result.failure)
        godot::UtilityFunctions::printerr(godot::String::utf8(("load notes: " + result.failure->message).c_str()));
    // A per-session copy that could not be written (or any file a publish left behind) is a warning, never a failure.
    for (const auto& leftover : result.leftovers)
        godot::UtilityFunctions::printerr(godot::String::utf8(("load notes (warning): " + leftover.message).c_str()));
}
} // namespace eawr::presentation::godot_backend
