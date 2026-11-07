#pragma once

#include "eawr/presentation/ui/selection_config.hpp"

#include <utility>

namespace eawr::presentation::godot_backend {

// Keep the no-player-settings case local to the battle, including its XML defaults.
template <typename Active>
void prepare_battle_selection_settings(ui::PlayerSettings& fallback, const vfs::Vfs& filesystem) {
    if (const auto* active = Active::get()) {
        auto settings = *active;
        ui::apply_selection_config(settings, filesystem);
        Active::set(std::move(settings));
    } else {
        fallback = ui::PlayerSettings{};
        ui::apply_selection_config(fallback, filesystem);
    }
}

} // namespace eawr::presentation::godot_backend
