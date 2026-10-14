// The message and confirm boxes' model (MNU-42, docs/ui/menu-screens.md): the retail dialog a
// request picks from its button count and kind, the gadget each button and line goes to, and
// what a pressed button or Esc gives back.

#include "eawr/presentation/ui/message_box.hpp"
#include "ui_test_support.hpp"

#include <iostream>
#include <string>

namespace {
namespace ui = eawr::presentation::ui;

void expect(const bool condition, const std::string& message) { eawr::test::ui::expect(condition, message.c_str()); }

ui::MessageBoxRequest request(const std::size_t buttons, const bool centred = false) {
    ui::MessageBoxRequest value;
    value.caption = "TEXT_CAPTION_KEY";
    value.centred = centred;
    for (std::size_t index = 0; index < buttons; ++index) {
        value.buttons.push_back({"TEXT_BUTTON_" + std::to_string(index), static_cast<int>(index) + 10, false});
    }
    return value;
}

void variants() {
    const char* const plain[] = {"IDD_DIALOG_BOX0", "IDD_DIALOG_BOX1", "IDD_DIALOG_BOX2", "IDD_DIALOG_BOX"};
    const char* const centred[] = {"IDD_DIALOG_BOX0_CENTERED", "IDD_DIALOG_BOX1_CENTERED", "IDD_DIALOG_BOX2_CENTERED",
                                   "IDD_DIALOG_BOX_CENTERED"};
    for (std::size_t count = 0; count <= 3; ++count) {
        const ui::MessageBoxPlan a = ui::plan_message_box(request(count));
        const ui::MessageBoxPlan b = ui::plan_message_box(request(count, true));
        expect(a.valid() && a.dialog == plain[count], "MNU-42: " + std::to_string(count) + " buttons use " + plain[count]);
        expect(b.valid() && b.dialog == centred[count], "MNU-42: centred, " + std::to_string(count) + " buttons use "
                                                            + centred[count]);
        expect(a.buttons.size() == count, "every button is placed");
    }
    ui::MessageBoxRequest four = request(4);
    expect(!ui::plan_message_box(four).valid(), "four buttons are refused");

    ui::MessageBoxRequest small;
    small.kind = ui::BoxKind::small_message;
    small.caption = "TEXT_SAVELOAD_LOADING_MSG";
    small.lines[0] = "ignored";
    const ui::MessageBoxPlan small_plan = ui::plan_message_box(small);
    expect(small_plan.valid() && small_plan.dialog == "IDD_DIALOG_BOX_SMALL" && small_plan.buttons.empty()
               && small_plan.lines.empty() && small_plan.caption == "TEXT_SAVELOAD_LOADING_MSG",
           "MNU-42: the saving and loading notice is the small box, caption only");
    small.buttons.push_back({"TEXT_BUTTON_OK", 1, false});
    expect(!ui::plan_message_box(small).valid(), "the small box takes no buttons");

    ui::MessageBoxRequest quit = request(2);
    quit.kind = ui::BoxKind::quit_confirm;
    quit.centred = true;
    expect(ui::plan_message_box(quit).dialog == "IDD_DIALOG_BOX_QUIT_CONFIRM",
           "MNU-42: the quit confirmation has its own dialog, centred or not");
    quit.buttons.pop_back();
    expect(!ui::plan_message_box(quit).valid(), "the quit confirmation needs two buttons");
}

void slots() {
    const ui::MessageBoxPlan one = ui::plan_message_box(request(1));
    expect(one.buttons[0].control == "IDC_DIALOG_BOX_BUTTON_LEFT", "one button takes the left slot");
    const ui::MessageBoxPlan two = ui::plan_message_box(request(2));
    expect(two.buttons[0].control == "IDC_DIALOG_BOX_BUTTON_LEFT" && two.buttons[1].control == "IDC_DIALOG_BOX_BUTTON_RIGHT",
           "two buttons take the left and right slots");
    const ui::MessageBoxPlan three = ui::plan_message_box(request(3));
    expect(three.buttons[0].control == "IDC_DIALOG_BOX_BUTTON_LEFT"
               && three.buttons[1].control == "IDC_DIALOG_BOX_BUTTON_CENTER"
               && three.buttons[2].control == "IDC_DIALOG_BOX_BUTTON_RIGHT",
           "three buttons take left, centre and right");
    expect(three.buttons[1].text == "TEXT_BUTTON_1" && three.buttons[2].result == 12, "text and result follow the order");

    ui::MessageBoxRequest lines = request(1);
    lines.lines[0] = "first";
    lines.lines[2] = "third";
    const ui::MessageBoxPlan lined = ui::plan_message_box(lines);
    expect(lined.lines.size() == 2 && lined.lines[0].first == "IDC_USER_TEXT_STRING_1"
               && lined.lines[1].first == "IDC_USER_TEXT_STRING_3" && lined.lines[1].second == "third",
           "an empty line keeps its gadget out of the plan, the others keep their ids");
    lines.lines[2] = "battle-\xF0\x9F\x98\x80-\xCE\xA3";
    lines.literal_lines[2] = true;
    const auto literal = ui::plan_message_box(lines);
    expect(literal.literal_line_controls == std::vector<std::string>{"IDC_USER_TEXT_STRING_3"}
        && literal.lines[1].second == lines.lines[2], "literal UTF-8 text keeps its value and gadget through planning");
    lines.lines[0] = "TEXT_BUTTON_OK";
    lines.literal_lines[0] = true;
    const auto key_name = ui::plan_message_box(lines);
    expect(key_name.literal_line_controls == std::vector<std::string>{"IDC_USER_TEXT_STRING_1", "IDC_USER_TEXT_STRING_3"}
        && key_name.lines[0].second == "TEXT_BUTTON_OK", "a user name that looks like a key stays literal");
}

void results() {
    ui::MessageBoxRequest value = request(2);
    value.buttons[1].cancel = true;
    const ui::MessageBoxPlan plan = ui::plan_message_box(value);
    expect(plan.result_for_control("IDC_DIALOG_BOX_BUTTON_LEFT") == 10, "the left button gives its result");
    expect(plan.result_for_control("IDC_DIALOG_BOX_BUTTON_RIGHT") == 11, "the right button gives its result");
    expect(!plan.result_for_control("IDC_DIALOG_BOX_BUTTON_CENTER"), "a slot the box does not have gives nothing");
    expect(plan.escape_result() == 11, "Esc gives the cancel button's result");
    expect(!ui::plan_message_box(request(1)).escape_result(), "a box with no cancel button ignores Esc");
    expect(!ui::plan_message_box(request(0)).escape_result(), "a box with no buttons ignores Esc");
}

} // namespace

int main() {
    variants();
    slots();
    results();
    if (eawr::test::ui::failures() != 0) {
        std::cerr << eawr::test::ui::failures() << " message box contract(s) failed\n";
        return 1;
    }
    std::cout << "message box contracts passed\n";
    return 0;
}
