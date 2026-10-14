// The credits roll's model (MNU-07, docs/behaviour/credits-roll.md CRD-01 to CRD-08): the
// GameConstants.xml values, the CreditsText lines, the scroll positions, the fade bands and the end.

#include "eawr/presentation/ui/credits.hpp"
#include "ui_test_support.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace ui = eawr::presentation::ui;
namespace data = eawr::data;

void expect(const bool condition, const std::string& message) { eawr::test::ui::expect(condition, message.c_str()); }

bool near(const float left, const float right, const float tolerance = 1.0e-4F) {
    return std::fabs(left - right) <= tolerance;
}

data::XmlNode tag(std::string name, std::string value) {
    data::XmlNode node;
    node.name = std::move(name);
    node.raw_text = std::move(value);
    return node;
}

data::XmlNode constants() {
    data::XmlNode root;
    root.name = "GameConstants";
    root.children = {
        tag("credits_spacing", " 0.04 "), tag("Credits_Font", " Arial Medium "), tag("Credits_Font_Size", "14"),
        tag("Credits_Margin", "0.05"), tag("Credits_Scroll_Rate", "-0.05"), tag("Credits_Scroll_Rate", " -0.10 "),
        tag("Credits_Top_Color", "0, 0.5, 1"), tag("Credits_Bottom_Color", "1,0.5,0"),
        tag("Credits_Header_Top_Color", "0.1, 0.2, 0.3"), tag("Credits_Header_Bottom_Color", "0.4, 0.5, 0.6"),
        tag("Credits_Logo_1_Name", "A.DDS"), tag("Credits_Logo_1_Width", "0.15"), tag("Credits_Logo_1_Height", "0.15"),
        tag("Credits_Logo_1_Y_Offset", "0.1"),
        tag("Credits_Logo_2_Name", "B.DDS"), tag("Credits_Logo_2_Width", "0.3"), tag("Credits_Logo_2_Height", "0.3"),
        tag("Credits_Logo_2_Y_Offset", "0.2"),
        tag("Credits_Logo_3_Name", "C.DDS"), tag("Credits_Logo_3_Width", "0.2"), tag("Credits_Logo_3_Height", "0.2"),
        tag("Credits_Logo_3_Y_Offset", "0.06")};
    return root;
}

void style_values() {
    const auto parsed = ui::parse_credits_style(constants());
    expect(static_cast<bool>(parsed), "the constants parse");
    if (!parsed) return;
    const ui::CreditsStyle& style = parsed.value();
    expect(style.font == "Arial Medium" && style.font_size == 14, "the font and size are the data's");
    expect(near(style.spacing, 0.04F) && near(style.margin, 0.05F), "names match without case and values are trimmed");
    expect(near(style.scroll_rate, -0.10F), "a later declaration wins");
    expect(style.top == ui::CreditsColour{0.0F, 0.5F, 1.0F} && style.bottom == ui::CreditsColour{1.0F, 0.5F, 0.0F},
           "the body colours");
    expect(style.header_top == ui::CreditsColour{0.1F, 0.2F, 0.3F}, "the header colours");
    expect(style.logos[1].texture == "B.DDS" && near(style.logos[1].y_offset, 0.2F), "a logo reads its four values");

    data::XmlNode missing = constants();
    missing.children.erase(missing.children.begin() + 1);
    const auto absent = ui::parse_credits_style(missing);
    expect(!absent && absent.error().code == ui::credits_codes::constants, "an absent value is named");
    expect(!absent && absent.error().message.find("Credits_Font") != std::string::npos, "the tag is in the message");
    data::XmlNode bad = constants();
    bad.children[6].raw_text = "0, 0.5";
    expect(!ui::parse_credits_style(bad), "a colour of two numbers is refused");
    data::XmlNode down = constants();
    down.children[5].raw_text = "0.1";
    expect(!ui::parse_credits_style(down), "a roll that scrolls down never ends and is refused");
}

void lines() {
    data::ui::TextDatabase database;
    const auto add = [&database](std::string key, std::u16string value) {
        data::ui::TextEntry entry;
        entry.key = std::move(key);
        entry.value = std::move(value);
        database.entries.push_back(std::move(entry));
    };
    add("CENTER", u"[TBL]");
    add("HEADER", u"Producer");
    add("CENTER", u"NAME \u00c9");
    add("HEADER_2", u"");
    const std::vector<ui::CreditsLine> list = ui::credits_lines(database);
    expect(list.size() == 4, "one line per record, in file order");
    if (list.size() != 4) return;
    expect(list[0].text.empty() && !list[0].header, "[TBL] is a blank body line");
    expect(list[1].text == "Producer" && list[1].header, "a HEADER key is a header");
    expect(list[2].text == "NAME \xC3\x89" && !list[2].header, "values are UTF-8");
    expect(list[3].header, "the marker is a part of the key");
    const auto path = ui::credits_text_path("English");
    expect(path && path.value() == "Data/Text/CreditsText_English.dat", "the path follows the master text file's");
    expect(!ui::credits_text_path("../x"), "the language is one word");
}

void roll() {
    const auto parsed = ui::parse_credits_style(constants());
    if (!parsed) return;
    // 800 pixels high: the 0.04 pitch is 32 whole pixels.
    ui::CreditsRoll roll(parsed.value(), 10, {0.1F, 0.0F, 0.2F}, 800.0F);
    expect(near(roll.line_height(), 0.04F), "the pitch is whole pixels");
    expect(near(roll.line_y(0), 1.0F + 0.5F / 800.0F), "the first line starts below the screen");
    expect(near(roll.line_y(3) - roll.line_y(2), 0.04F), "lines are one pitch apart");
    expect(!roll.logo_present(1) && roll.logo_present(0), "a logo with no height is absent");
    expect(near(roll.logo_y(0) - roll.line_y(0), 10.0F * 0.04F + 0.1F), "a logo sits its offset under the last line");
    expect(near(roll.alpha_at(0.02F), 0.0F), "above the margin is clear");
    expect(near(roll.alpha_at(0.5F), 1.0F), "the middle is opaque");
    expect(roll.alpha_at(0.075F) > 0.0F && roll.alpha_at(0.075F) < 1.0F, "inside the top band it fades in");
    expect(roll.alpha_at(0.925F) > 0.0F && roll.alpha_at(0.925F) < 1.0F, "inside the bottom band it fades out");
    expect(near(roll.alpha_at(0.96F), 0.0F), "below the bottom margin is clear");
    expect(!roll.done(), "a roll that has not moved is not done");

    roll.advance(1.0);
    // 0.10 of 800 pixels a second: 80 pixels up.
    expect(near(roll.scroll_distance(), 0.1F), "a second moves a tenth of the screen up");
    expect(near(roll.line_y(0), 1.0F + 0.5F / 800.0F - 0.1F), "lines move with the roll");
    roll.restart();
    expect(near(roll.line_y(0), 1.0F + 0.5F / 800.0F), "restart returns to the start");

    // The end: the last logo's bottom edge passes the top. Logo 3 sits 1 + 10 * 0.04 + 0.06 down and is
    // 0.2 high: 1.66 screens at 0.1 a second.
    double seconds = 0.0;
    while (!roll.done() && seconds < 100.0) {
        seconds += 0.5;
        roll.advance(0.5);
    }
    expect(roll.done() && seconds < 100.0, "the roll ends");
    expect(near(static_cast<float>(seconds), 16.6F, 0.6F), "it ends once the last logo has gone");
    expect(roll.logo_y(2) + roll.logo_height(2) <= 0.0F && roll.line_y(9) + roll.line_height() <= 0.0F,
           "everything is above the top");
}

} // namespace

int main() {
    style_values();
    lines();
    roll();
    if (eawr::test::ui::failures() != 0) {
        std::cerr << eawr::test::ui::failures() << " credits check(s) failed\n";
        return 1;
    }
    std::cout << "credits contracts passed\n";
    return 0;
}
