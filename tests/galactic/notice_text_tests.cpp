#include "galactic/notice_text.hpp"
#include "eawr/data/ui/text_database.hpp"
#include "ui_test_support.hpp"

#include <array>
#include <string>

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

int main() {
    formats_tokens();
    stock_approach_notice();
    return eawr::test::ui::failures() ? 1 : 0;
}
