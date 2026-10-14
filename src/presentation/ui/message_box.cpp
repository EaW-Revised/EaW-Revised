#include "eawr/presentation/ui/message_box.hpp"

#include <utility>

namespace eawr::presentation::ui {
namespace {

constexpr std::string_view left_button = "IDC_DIALOG_BOX_BUTTON_LEFT";
constexpr std::string_view centre_button = "IDC_DIALOG_BOX_BUTTON_CENTER";
constexpr std::string_view right_button = "IDC_DIALOG_BOX_BUTTON_RIGHT";
constexpr std::array<std::string_view, message_box_lines> line_controls = {
    "IDC_USER_TEXT_STRING_1", "IDC_USER_TEXT_STRING_2", "IDC_USER_TEXT_STRING_3"};

// The gadget slots a button count uses, left to right.
[[nodiscard]] std::vector<std::string_view> slots(const std::size_t count) {
    switch (count) {
    case 1: return {left_button};
    case 2: return {left_button, right_button};
    case 3: return {left_button, centre_button, right_button};
    default: return {};
    }
}

} // namespace

std::string_view standard_box_dialog(const std::size_t buttons, const bool centred) {
    switch (buttons) {
    case 0: return centred ? "IDD_DIALOG_BOX0_CENTERED" : "IDD_DIALOG_BOX0";
    case 1: return centred ? "IDD_DIALOG_BOX1_CENTERED" : "IDD_DIALOG_BOX1";
    case 2: return centred ? "IDD_DIALOG_BOX2_CENTERED" : "IDD_DIALOG_BOX2";
    default: return centred ? "IDD_DIALOG_BOX_CENTERED" : "IDD_DIALOG_BOX";
    }
}

std::optional<int> MessageBoxPlan::escape_result() const {
    for (const PlannedBoxButton& button : buttons) {
        if (button.cancel) return button.result;
    }
    return std::nullopt;
}

std::optional<int> MessageBoxPlan::result_for_control(const std::string_view control) const {
    for (const PlannedBoxButton& button : buttons) {
        if (button.control == control) return button.result;
    }
    return std::nullopt;
}

MessageBoxPlan plan_message_box(const MessageBoxRequest& request) {
    MessageBoxPlan plan;
    plan.caption = request.caption;
    const std::size_t count = request.buttons.size();
    switch (request.kind) {
    case BoxKind::standard:
        if (count > message_box_max_buttons) {
            plan.problem = "a message box has at most three buttons";
            return plan;
        }
        plan.dialog = std::string(standard_box_dialog(count, request.centred));
        break;
    case BoxKind::small_message:
        if (count != 0) {
            plan.problem = "the small message box has no buttons";
            return plan;
        }
        plan.dialog = "IDD_DIALOG_BOX_SMALL";
        break;
    case BoxKind::quit_confirm:
        if (count != 2) {
            plan.problem = "the quit confirmation has exactly two buttons";
            return plan;
        }
        plan.dialog = "IDD_DIALOG_BOX_QUIT_CONFIRM";
        break;
    }
    if (request.kind != BoxKind::small_message) {
        for (std::size_t index = 0; index < message_box_lines; ++index) {
            if (!request.lines[index].empty()) {
                plan.lines.emplace_back(std::string(line_controls[index]), request.lines[index]);
                if (request.literal_lines[index]) plan.literal_line_controls.emplace_back(line_controls[index]);
            }
        }
    }
    const std::vector<std::string_view> placed = slots(count);
    for (std::size_t index = 0; index < placed.size(); ++index) {
        const BoxButton& button = request.buttons[index];
        plan.buttons.push_back({std::string(placed[index]), button.text, button.result, button.cancel});
    }
    return plan;
}

} // namespace eawr::presentation::ui
