#include "ui_test_support.hpp"

#include <iostream>

void dialog_catalog_contracts();
void text_database_contracts();
void command_bar_contracts();
void shell_anchor_contracts();
void movie_contracts();

int main() {
    text_database_contracts();
    dialog_catalog_contracts();
    command_bar_contracts();
    shell_anchor_contracts();
    movie_contracts();
    if (eawr::test::ui::failures() != 0) {
        std::cerr << eawr::test::ui::failures() << " UI data contract(s) failed\n";
        return 1;
    }
    std::cout << "UI data contracts passed\n";
    return 0;
}
