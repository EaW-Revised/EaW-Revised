#pragma once

#include "eawr/assets/asset_degrade.hpp"
#include "eawr/presentation/animation/animation.hpp"

#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// ASD-01 (docs/behaviour/asset-degrade.md): the one place a start-path caller turns a refused clip into "no clip plus one
// load note". A clip the loaders refuse (empty, no root group, cut short) and a clip that parses but does not fit its model
// both leave the object in its bind pose; neither fails the start. Notes are kept once per (file, asset kind, stage), so a
// clip shared by many objects or surfaces costs one note. The owner publishes `drain()` where its load notes go.
namespace eawr::presentation::animation {

class DegradedClips final {
public:
    // The clip at `path`, or no clip and one note.
    [[nodiscard]] std::optional<assets::Animation> load(const vfs::Vfs& filesystem, const std::string_view path) {
        auto result = assets::load_animation_or_note(filesystem, path);
        if (result.note) { add(std::move(*result.note)); return std::nullopt; }
        return std::move(result.clip);
    }

    // A player over `model` and `clip`, or no player and one note (the clip does not fit the model).
    [[nodiscard]] std::optional<Player> bind(const vfs::Vfs& filesystem, const assets::Model& model,
                                             const assets::Animation& clip, const std::string_view path) {
        auto player = Player::create(model, &clip);
        if (player) return std::move(player.value());
        add(assets::asset_degraded_note(filesystem, path, assets::DegradedAsset::animation, assets::DegradedStage::bind,
                                        player.error()));
        return std::nullopt;
    }

    // Records `note` unless the same file, kind and stage was already recorded. Returns whether it was kept.
    bool add(core::LoadNote note) {
        if (!seen_.insert(note.object + '|' + note.tag + '|' + note.diagnostic_code).second) return false;
        notes_.push_back(std::move(note));
        return true;
    }

    // The notes kept so far; the helper starts empty again (the dedupe memory stays). Named `drain`, not `take`, on purpose:
    // the live-session source guard counts `.take(` calls in the viewer's session code to pin the scheduler's ownership
    // (one scheduler, taken on the simulation thread), and a note drain is not a scheduler take.
    [[nodiscard]] std::vector<core::LoadNote> drain() { return std::exchange(notes_, {}); }
    [[nodiscard]] bool empty() const noexcept { return notes_.empty(); }

private:
    std::set<std::string> seen_;
    std::vector<core::LoadNote> notes_;
};

} // namespace eawr::presentation::animation
