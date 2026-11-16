#include "galactic/notice_text.hpp"
#include "eawr/data/ui/text_database.hpp"
#include "ui_test_support.hpp"

#include <array>
#include <span>
#include <string>
#include <string_view>

namespace {
using eawr::test::ui::expect;
namespace view = eawr::viewer::galactic;

bool starts_with_hash_digits(const std::string& line) {
    return line.size() > 1 && line[0] == '#' && line[1] >= '0' && line[1] <= '9';
}

void formats_tokens() {
    const std::array<std::string, 2> arguments{"Alpha", "Beta"};
    expect(view::expand_notice_text("at ##0.", arguments) == "at Alpha.", "##0 takes the first argument");
    expect(view::expand_notice_text("##1 then ##0", arguments) == "Beta then Alpha", "indices pick their argument");
    expect(view::expand_notice_text("at ##2 #", arguments) == "at ##2 #", "an index without an argument stays as authored");
    expect(view::expand_notice_text("no tokens", arguments) == "no tokens", "plain text is unchanged");
}

void stock_approach_notice() {
    auto filesystem = eawr::test::ui::foc_corpus("galactic notice text");
    if (!filesystem) return;
    auto text = eawr::data::ui::load_language_text_database(*filesystem, "english");
    expect(static_cast<bool>(text), "FoC text database loads");
    if (!text) return;
    const auto* format = text.value().find("TEXT_ENEMY_FLEET_APPROACHING_LOCATION");
    const auto* planet = text.value().find("TEXT_OBJECT_STAR_SYSTEM_CORUSCANT");
    expect(format && planet, "the approach notice and a stock planet name are in the master text");
    if (!format || !planet) return;
    const std::array<std::string, 1> arguments{eawr::data::ui::to_utf8(planet->value)};
    const auto line = view::expand_notice_text(eawr::data::ui::to_utf8(format->value), arguments);
    expect(line.find(arguments[0]) != std::string::npos, "the approach notice names the planet");
    expect(line.find("##") == std::string::npos && !starts_with_hash_digits(line),
        "no token or hash-like value is left in the approach notice");
}
}

// #6964 / WGUI-179: the stock control-change lines are printf formats filled in argument order.
void printf_formats() {
    const std::array<std::string, 4> arguments{"Kuat", "Empire", "Rebel Alliance", "250"};
    expect(view::expand_printf_notice("%ls: Control changed from %ls to %ls", arguments)
            == "Kuat: Control changed from Empire to Rebel Alliance", "each %ls takes the next argument in order");
    expect(view::expand_printf_notice("%ls: Control changed from %ls to %ls. Credit bonus: %d", arguments)
            == "Kuat: Control changed from Empire to Rebel Alliance. Credit bonus: 250", "%d takes the bonus argument");
    expect(view::expand_printf_notice("100%% of %s", arguments) == "100% of Kuat", "%% is one percent sign");
    expect(view::expand_printf_notice("%ls and %ls", std::span<const std::string>(arguments).first(1)) == "Kuat and %ls",
        "a conversion without an argument stays as authored");
    expect(view::expand_printf_notice("50% off %x", arguments) == "50% off %x", "other percent signs stay as authored");
}

// #6964 review: the planet name of an advisor notice comes from the planet type's text and never depends on discovery.
void planet_notice_name() {
    expect(view::notice_planet_name("Kuat_Planet", "Kuat") == "Kuat", "an undiscovered planet is still named from its type text");
    expect(view::notice_planet_name("Kuat_Planet", "") == "Kuat_Planet", "no type text: the planet's own name");
}

void stock_control_change_notice() {
    auto filesystem = eawr::test::ui::foc_corpus("galactic control change notice");
    if (!filesystem) return;
    auto text = eawr::data::ui::load_language_text_database(*filesystem, "english");
    expect(static_cast<bool>(text), "FoC text database loads");
    if (!text) return;
    const auto* plain = text.value().find("TEXT_PLANET_CHANGED_HANDS");
    const auto* bonus = text.value().find("TEXT_PLANET_CHANGED_HANDS_BONUS");
    expect(plain && bonus, "the control-change lines and their credit-bonus form are in the master text");
    if (!plain || !bonus) return;
    const std::array<std::string, 4> arguments{"Planet", "OldOwner", "NewOwner", "321"};
    const auto line = view::expand_printf_notice(eawr::data::ui::to_utf8(plain->value), arguments);
    expect(line.find("Planet") != std::string::npos && line.find("OldOwner") < line.find("NewOwner")
        && line.find("321") == std::string::npos && line.find('%') == std::string::npos,
        "the plain stock line names the planet then the old and new owner, with no bonus and no conversion left");
    const auto rich = view::expand_printf_notice(eawr::data::ui::to_utf8(bonus->value), arguments);
    expect(rich.find("321") != std::string::npos && rich.find('%') == std::string::npos,
        "the stock bonus line carries the amount and no conversion is left");
}

int main() {
    formats_tokens();
    printf_formats();
    planet_notice_name();
    stock_control_change_notice();
    stock_approach_notice();
    return eawr::test::ui::failures() ? 1 : 0;
}
