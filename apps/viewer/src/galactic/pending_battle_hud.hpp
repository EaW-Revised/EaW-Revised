#pragma once
#include "ui/dialog_builder.hpp"
#include "ui/font_provider.hpp"
#include "eawr/data/ui/command_bar.hpp"
#include "eawr/presentation/ui/hud_shell.hpp"
#include "eawr/vfs/vfs.hpp"

namespace eawr::presentation::godot_backend {
class EawrTacticalHud;
struct BuiltPendingBattleHud {
    godot::Control* frame{};
    EawrTacticalHud* surface{};
    std::vector<BuiltGadget> gadgets;
    std::string shell;
    std::string animation;
    bool animated_pose{};
};
// WGM-175: mounted command-bar models, never an extra centered modal.
[[nodiscard]] BuiltPendingBattleHud build_pending_battle_hud(const vfs::Vfs&,
    const data::ui::CommandBarCatalog&, FontProvider&, presentation::ui::HudFaction faction,
    godot::Vector2 size, bool single,
    const std::function<godot::Ref<godot::Texture2D>(std::string_view)>& texture,
    const std::function<std::string(std::string_view)>& text,
    const std::function<void(const std::string&)>& sound = {});
}
