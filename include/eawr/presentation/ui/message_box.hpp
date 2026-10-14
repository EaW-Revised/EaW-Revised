#pragma once

// Message and confirm boxes (MNU-42, docs/ui/menu-screens.md): the modal boxes the out-of-game
// screens raise for a quit confirmation, a delete confirmation, a validation message or a
// "saving" notice. Engine-free: a caller describes the box (caption, up to three user lines, up
// to three buttons with result ids); the model picks the retail dialog and the gadget each piece
// goes to, and maps a pressed button or Esc to a result. The viewer's EawrMessageBox draws it.
//
// Texts are text-database keys or literals: the viewer resolves each through the text lookup,
// which shows a missing key as written (UI-T3).

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace eawr::presentation::ui {

inline constexpr std::size_t message_box_max_buttons = 3;
inline constexpr std::size_t message_box_lines = 3;

// Which retail dialog family the box belongs to.
enum class BoxKind : std::uint8_t {
    standard,      // IDD_DIALOG_BOX, _BOX0, _BOX1, _BOX2 and their _CENTERED variants, by button count
    small_message, // IDD_DIALOG_BOX_SMALL: a caption only, for the saving and loading notices
    quit_confirm,  // IDD_DIALOG_BOX_QUIT_CONFIRM: a caption, three lines and two buttons
};

struct BoxButton {
    std::string text; // a key (TEXT_BUTTON_YES) or a literal
    int result{};
    // Esc presses this button. A box with no cancel button ignores Esc.
    bool cancel{};
};

struct MessageBoxRequest {
    BoxKind kind{BoxKind::standard};
    std::string caption;
    std::array<std::string, message_box_lines> lines;
    std::vector<BoxButton> buttons; // left to right
    // Centred text variants of the standard dialogs; the small and quit-confirm dialogs have none.
    bool centred{};
    // User-entered names and system messages are UTF-8 literals, never database keys.
    std::array<bool, message_box_lines> literal_lines{};
};

// One button as the dialog places it.
struct PlannedBoxButton {
    std::string control; // the dialog gadget id (IDC_DIALOG_BOX_BUTTON_LEFT, _CENTER or _RIGHT)
    std::string text;
    int result{};
    bool cancel{};
};

struct MessageBoxPlan {
    std::string dialog;
    std::string caption_control{"IDC_CAPTION"};
    std::string caption;
    // The user-string gadgets that carry text, in order (IDC_USER_TEXT_STRING_1..3); a dialog
    // without line gadgets (the small box) takes none.
    std::vector<std::pair<std::string, std::string>> lines;
    std::vector<std::string> literal_line_controls;
    std::vector<PlannedBoxButton> buttons;
    // Empty when the request is valid; the box is not built otherwise.
    std::string problem;

    [[nodiscard]] bool valid() const noexcept { return problem.empty(); }
    // The result Esc gives, if the box has a cancel button.
    [[nodiscard]] std::optional<int> escape_result() const;
    // The result of the button that owns this gadget id, if it is one of the box's buttons.
    [[nodiscard]] std::optional<int> result_for_control(std::string_view control) const;
};

// MNU-42: the standard dialog follows the button count (0 to three buttons: BOX0, BOX1, BOX2,
// BOX); two buttons use the left and right slots, one the left, three left, centre and right.
[[nodiscard]] MessageBoxPlan plan_message_box(const MessageBoxRequest& request);

// The dialog name for a button count of a standard box.
[[nodiscard]] std::string_view standard_box_dialog(std::size_t buttons, bool centred);

} // namespace eawr::presentation::ui
