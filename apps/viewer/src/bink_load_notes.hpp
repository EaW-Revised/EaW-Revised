#pragma once
#include "viewer_load_notes.hpp"
#include <algorithm>
#include <vector>

namespace eawr::presentation::godot_backend {
inline std::vector<core::LoadNote>& bink_pending_notes() {
    static std::vector<core::LoadNote> notes;
    return notes;
}
inline void enqueue_bink_load_note(std::string file, std::string layer, std::string movie) {
    auto& notes = bink_pending_notes();
    const auto exists = std::any_of(notes.begin(), notes.end(), [&](const auto& n) {
        return n.code == "EAWR-LOAD-1237" && n.file == file && n.layer.mod_id == layer && n.object == movie;
    });
    if (exists) return;
    constexpr std::size_t cap = 64;
    if (notes.size() >= cap) notes.erase(notes.begin());
    core::LoadNote note;
    note.code = "EAWR-LOAD-1237";
    note.file = std::move(file); note.object = std::move(movie);
    note.layer.mod_id = std::move(layer);
    note.classification = core::LoadCategory::unknown_degrade;
    note.action = core::LoadAction::runtime_degraded;
    note.phase = core::LoadPhase::runtime;
    note.message = "HUD movie playback unavailable; speech and battle continue";
    notes.push_back(std::move(note));
}
inline void flush_bink_load_notes() {
    auto& notes = bink_pending_notes();
    if (notes.empty()) return;
    auto pending = std::move(notes);
    notes.clear();
    publish_viewer_load_notes(std::move(pending), {}, true);
}
} // namespace eawr::presentation::godot_backend
