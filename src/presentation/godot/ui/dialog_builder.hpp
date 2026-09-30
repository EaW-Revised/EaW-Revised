#pragma once

// Builds one catalogue dialog as kit Controls (ticket UI-06 #229): the frame
// placed by UI-L4 in the model's safe area, every gadget at its UI-L3 rect,
// each with the theme variation of its dialog and control
// (ThemeModel::variation), and captions through the text database (UI-T3
// renders a missing key as written). It builds the look only; behaviour and
// the dialogs' screen presets belong to the screens that use them.

#include "ui/kit.hpp"

#include "eawr/data/ui/dialog_catalog.hpp"
#include "eawr/data/ui/text_database.hpp"
#include "eawr/presentation/ui/layout.hpp"
#include "eawr/presentation/ui/theme.hpp"

#include <godot_cpp/classes/control.hpp>

#include <string>
#include <vector>

namespace eawr::presentation::godot_backend {

struct BuiltGadget final {
    std::string id;
    std::string statement;
    std::string class_name;
    std::string kind; // button, check, radio, label, group, edit, combo, list, slider, bar; empty when skipped
    std::string variation;
    std::string caption;
    godot::Control* control{}; // null when skipped
    presentation::ui::PixelRect rect; // on screen
};

struct BuiltDialog final {
    EawrUiFrame* frame{};
    presentation::ui::DialogLayout layout;
    std::vector<BuiltGadget> gadgets;
};

// The frame is not yet in a tree; the caller adds it where it belongs.
// Images (PETROGLYPH_DIALOG_IMAGE) are set at run time and are skipped.
[[nodiscard]] BuiltDialog build_dialog(const data::ui::Dialog& dialog, const presentation::ui::ThemeModel& theme,
                                       data::ui::TextLookup* text, presentation::ui::Placement placement);

} // namespace eawr::presentation::godot_backend
