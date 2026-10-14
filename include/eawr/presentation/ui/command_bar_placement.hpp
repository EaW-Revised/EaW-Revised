#pragma once

#include "eawr/presentation/ui/layout.hpp"

namespace eawr::presentation::ui {

enum class ShellOrigin : std::uint8_t { centre, lower_left };

// GHUDL-01..04: shell translation is independent of its authored mesh bounds.
// The main HUD explicitly supplies lower_left; auxiliary manual shells retain
// their centred identity transform unless their caller supplies a translation.
struct CommandBarPlacement {
    bool configured{};
    bool model_offset_x{};
    bool model_offset_y{};
    bool manual_offset{};
    ShellOrigin manual_origin{ShellOrigin::lower_left};
    ReferencePoint manual_translation;
};

[[nodiscard]] CommandBarPlacement command_bar_placement(
    const data::ui::CommandBarComponent* component,
    ShellOrigin manual_origin = ShellOrigin::lower_left) noexcept;

// Automatic shells receive the live command-bar offset. Manual shells retain
// their caller's translation instead. Both translations are in model units,
// with y up; the viewport projection applies the common UI-L1 scale once.
[[nodiscard]] ShellPlacement place_command_bar(const ReferenceSpace& space, double shell_width,
    const CommandBarPlacement& policy, ReferencePoint automatic_translation = {}) noexcept;

} // namespace eawr::presentation::ui
