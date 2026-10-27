#include "ui_test_support.hpp"

#include <iostream>

void layout_contracts();
void font_contracts();
void hud_contracts();
void hud_shell_contracts();
void theme_contracts();
void story_summary_contracts();

int main() {
    layout_contracts();
    font_contracts();
    hud_contracts();
    hud_shell_contracts();
    theme_contracts();
    story_summary_contracts();
    if (eawr::test::ui::failures() != 0) {
        std::cerr << eawr::test::ui::failures() << " UI model contract(s) failed\n";
        return 1;
    }
    std::cout << "UI model contracts passed\n";
    return 0;
}
