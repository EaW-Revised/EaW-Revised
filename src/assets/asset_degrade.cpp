#include "eawr/assets/asset_degrade.hpp"

#include "eawr/vfs/vfs.hpp"

#include <utility>

namespace eawr::assets {
namespace {
constexpr std::string_view kind_name(const DegradedAsset kind) noexcept {
    return kind == DegradedAsset::animation ? "animation" : "model";
}
} // namespace

core::LoadNote asset_degraded_note(const vfs::Vfs& filesystem, const std::string_view path, const DegradedAsset kind,
                                   const DegradedStage stage, const core::Diagnostic& why) {
    core::LoadNote note;
    note.code = std::string(asset_degraded_code);
    note.classification = core::LoadCategory::unknown_degrade;
    note.file = std::string(path);
    note.layer.mod_id = std::string(core::LoadLayer{}.mod_id);
    if (const auto record = filesystem.stat(path)) {
        // The logical name the layer holds the file under, and the layer that supplied it: the ledger reads the mod from here.
        if (!record.value().canonical_path.empty()) note.file = record.value().canonical_path;
        if (!record.value().layer_id.empty()) note.layer.mod_id = record.value().layer_id;
    }
    note.object = std::string(path);
    note.tag = std::string(kind_name(kind));
    note.authored_value = std::string(path);
    // Both stages happen while the scene loads, so the note is a load-phase note.
    note.action = kind == DegradedAsset::animation ? core::LoadAction::default_used : core::LoadAction::skipped_object;
    note.phase = core::LoadPhase::load;
    note.effective_value = kind == DegradedAsset::animation ? "no animation: the model keeps its bind pose"
                                                            : "no model: the object is drawn without it";
    note.diagnostic_code = why.code;
    note.message = std::string(kind_name(kind)) + (stage == DegradedStage::load ? " could not be loaded: " : " does not bind to its model: ")
                   + why.message;
    return note;
}

TolerantAnimation load_animation_or_note(const vfs::Vfs& filesystem, const std::string_view path) {
    TolerantAnimation result;
    auto loaded = load_animation(filesystem, path);
    if (loaded) {
        result.clip.emplace(std::move(loaded.value()));
        return result;
    }
    result.note = asset_degraded_note(filesystem, path, DegradedAsset::animation, DegradedStage::load, loaded.error());
    return result;
}

} // namespace eawr::assets
