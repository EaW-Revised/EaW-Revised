#pragma once

// The runtime message and confirm box (MNU-42, docs/ui/menu-screens.md): a modal CanvasLayer that
// builds one of the retail box dialogs through the dialog builder with the retail skin, fills the
// caption, lines and buttons from a MessageBoxRequest and reports the chosen result through the
// `result_chosen` signal. It blocks pointer input to everything under it with the kit's modal
// layer (UI-I1) and the host drops keys while it is visible; Esc gives the request's cancel result.

#include "ui/dialog_builder.hpp"
#include "ui/input_routing.hpp"

#include "eawr/data/ui/dialog_catalog.hpp"
#include "eawr/data/ui/text_database.hpp"
#include "eawr/presentation/ui/message_box.hpp"
#include "eawr/presentation/ui/theme.hpp"

#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/theme.hpp>

#include <string>
#include <vector>

namespace eawr::presentation::godot_backend {

// What a box needs from the screen that raises it. The pointers outlive every open box.
struct MessageBoxContext final {
    const data::ui::DialogCatalog* catalog{};
    const presentation::ui::ThemeModel* theme_model{};
    data::ui::TextLookup* text{}; // null shows the keys and literals as written
    godot::Ref<godot::Theme> theme;
    godot::Vector2 viewport_size;
};

class EawrMessageBox final : public godot::CanvasLayer {
    GDCLASS(EawrMessageBox, godot::CanvasLayer)

public:
    static constexpr int layer_index = 100;

    EawrMessageBox() = default;

    // Builds the box and adds it under `parent`. A false return leaves it unopened with the reason
    // in failure(); the caller frees it.
    [[nodiscard]] bool open(godot::Node& parent, const MessageBoxContext& context,
                            const presentation::ui::MessageBoxRequest& request);

    [[nodiscard]] const std::string& failure() const noexcept { return failure_; }
    [[nodiscard]] const presentation::ui::MessageBoxPlan& plan() const noexcept { return plan_; }
    [[nodiscard]] godot::Control* button(const std::string& control) const;
    [[nodiscard]] godot::String line_text(const std::string& control) const;
    [[nodiscard]] godot::Control* modal() const noexcept { return modal_; }
    [[nodiscard]] godot::Control* frame() const noexcept { return built_.frame; }

    // Chooses a result as a click on its button does: emits `result_chosen`, then frees the box.
    void choose(int64_t result);

    void _input(const godot::Ref<godot::InputEvent>& event) override;

protected:
    static void _bind_methods();

private:
    presentation::ui::MessageBoxPlan plan_;
    BuiltDialog built_;
    EawrUiModalLayer* modal_{};
    std::string failure_;
    bool chosen_{};
};

void register_message_box_classes();

} // namespace eawr::presentation::godot_backend
