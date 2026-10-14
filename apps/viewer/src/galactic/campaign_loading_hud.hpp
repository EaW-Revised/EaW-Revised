#pragma once

#include "loading_screen.hpp"
#include "scene_content.hpp"

namespace eawr::presentation::godot_backend {

// WGP-161/162: retain copied loading facts, never observer sensor intelligence.
// Runtime clone phase and absent map identities stay explicit provider seams.
inline EawrLoadingScreen::Options campaign_loading_options(
    std::shared_ptr<const platform::CampaignLoadingView> view,
    std::shared_ptr<const BattleContent> mounted,
    const viewer::galactic::SceneContent* scene, std::function<bool()> topmost) {
    EawrLoadingScreen::Options options;
    options.content = std::move(mounted);
    const auto& descriptor = view->descriptor;
    options.campaign_space = true;
    options.battle = descriptor.layer == campaign::BattleLayer::space
        ? presentation::ui::LoadingBattle::space : presentation::ui::LoadingBattle::land;
    switch (descriptor.kind) {
    case platform::CampaignLoadingKind::ordinary: options.mission = presentation::ui::LoadingMission::ordinary; break;
    case platform::CampaignLoadingKind::tutorial: options.mission = presentation::ui::LoadingMission::tutorial; break;
    case platform::CampaignLoadingKind::story: options.mission = presentation::ui::LoadingMission::story; break;
    case platform::CampaignLoadingKind::corruption: options.mission = presentation::ui::LoadingMission::corruption; break;
    }
    options.mission_map = descriptor.dialog_map;
    options.topmost = std::move(topmost);
    options.context_name = descriptor.current_type.value_or(descriptor.location_type);
    // The selected child map is not the retained dialog/thumbnail map.
    options.map_path = descriptor.dialog_map.value_or(std::string());
    options.map_name = descriptor.location_type;
    if (descriptor.basic_income) options.basic_income = static_cast<double>(descriptor.basic_income->raw())
        / static_cast<double>(sim::math::Fixed::scale);
    if (scene && descriptor.actual_owner) if (const auto* owner = viewer::galactic::affiliation_for_owner(*scene, descriptor.actual_owner)) {
        options.owner_name = owner->faction;
        options.owner_colour = {owner->secondary.r, owner->secondary.g, owner->secondary.b};
    }
    options.campaign_preparation = std::move(view);
    return options;
}

inline std::string campaign_loading_error(const platform::CampaignLoadingDescriptor& descriptor,
    const std::string& diagnostic) {
    // WGP-167's fixed message is used only with a retained, known layer base.
    if (!descriptor.base_level) return "Cannot prepare map '" + descriptor.selected_map + "': " + diagnostic;
    return "Error: Could not find/load map named '" + descriptor.selected_map + "' for "
        + std::to_string(*descriptor.base_level) + " level base.";
}
} // namespace eawr::presentation::godot_backend
