#include "eawr/presentation/ui/command_bar_placement.hpp"

namespace eawr::presentation::ui {

CommandBarPlacement command_bar_placement(const data::ui::CommandBarComponent* component,
    const ShellOrigin manual_origin) noexcept {
    CommandBarPlacement out;
    out.manual_origin = manual_origin;
    if (!component || component->type != data::ui::ComponentType::shell) return out;
    using data::ui::Field;
    out.configured = component->find(Field::model_offset_x) || component->find(Field::model_offset_y)
        || component->find(Field::manual_offset);
    // GHUDL-02: unset axis/manual flags default to false in the debug build.
    out.model_offset_x = component->flag(Field::model_offset_x);
    out.model_offset_y = component->flag(Field::model_offset_y);
    out.manual_offset = component->flag(Field::manual_offset);
    return out;
}

ShellPlacement place_command_bar(const ReferenceSpace& space, const double shell_width,
    const CommandBarPlacement& policy, const ReferencePoint automatic_translation) noexcept {
    const auto legacy = place_shell(space, shell_width);
    if (!policy.configured) return legacy; // Existing shells without these tags are byte-identical.
    const auto translation = snap_shell_offset(policy.manual_offset
        ? policy.manual_translation : automatic_translation);
    ShellPlacement out{space.viewport.width / 2.0, space.viewport.height / 2.0, space.scale};
    // GHUDL-03/04: only an explicit main-shell caller supplies lower-left.
    // Manual_Offset does not itself imply lower-left anchoring or centring.
    if (policy.manual_offset && policy.manual_origin == ShellOrigin::lower_left) {
        if (policy.model_offset_x) out.left = legacy.left;
        if (policy.model_offset_y) out.bottom = legacy.bottom;
    }
    if (policy.model_offset_x) out.left += translation.x * space.scale;
    if (policy.model_offset_y) out.bottom -= translation.y * space.scale;
    return out;
}

} // namespace eawr::presentation::ui
