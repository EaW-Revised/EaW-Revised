// #5345: where a radio button or check box draws its glyph and caption (docs/behaviour/walks/dialog-toggles.md, DTG-1..4).
// The glyph is the OFF art's size at the control's top-left; every state draws into it; the authored rectangle's width
// does not size it. Synthetic sizes only.

#include "eawr/presentation/ui/toggle_layout.hpp"
#include "ui_test_support.hpp"

#include <iostream>
#include <string>

namespace {
using namespace eawr;
namespace ui = presentation::ui;

void expect(const bool condition, const std::string& message) { test::ui::expect(condition, message.c_str()); }

// DTG-1: the glyph box comes from the OFF art alone, at the origin, whatever the other states' arts or the rectangle are.
void test_glyph_is_the_off_art() {
    const ui::ToggleBox glyph = ui::toggle_glyph_box({23, 23});
    expect(glyph.x == 0.0F && glyph.y == 0.0F && glyph.width == 23.0F && glyph.height == 23.0F,
           "the glyph box is the OFF art's size at the control's top-left");
    expect(ui::toggle_glyph_box({}).width == 0.0F, "no art, no glyph");
}

// The Difficulty block: three push-style radios whose authored rectangles differ in width (186, 216 and 206 units) get one glyph
// box: same x, same size, so the dots no longer follow the row widths.
void test_rows_of_different_width_share_one_glyph() {
    const ui::ToggleBox easy = ui::toggle_glyph_box({23, 23});
    const ui::ToggleBox medium = ui::toggle_glyph_box({23, 23});
    const ui::ToggleBox hard = ui::toggle_glyph_box({23, 23});
    expect(easy.x == medium.x && medium.x == hard.x, "one x line for every row");
    expect(easy.width == medium.width && medium.width == hard.width && easy.height == hard.height,
           "one glyph size for every row, whatever the authored rectangle");
    // The caption of a push-style radio is centred on the glyph, not on the rectangle.
    for (const float width : {186.0F, 216.0F, 206.0F}) {
        const ui::ToggleBox caption = ui::toggle_caption_box(easy, true, width, ui::toggle_caption_gap(easy));
        expect(caption.x == easy.x && caption.width == easy.width && caption.height == easy.height,
               "a push-style caption box is the glyph box, whatever the rectangle's width");
    }
}

// DTG-2: an ordinary radio or check starts its caption one gap after the glyph, and the caption box is as high as the glyph so
// the text is centred on the glyph's vertical centre.
void test_caption_follows_the_glyph() {
    const ui::ToggleBox glyph = ui::toggle_glyph_box({22, 22});
    const float gap = ui::toggle_caption_gap(glyph);
    expect(gap == 7.0F, "the gap is 30% of the glyph's width, rounded");
    const ui::ToggleBox caption = ui::toggle_caption_box(glyph, false, 200.0F, gap);
    expect(caption.x == 22.0F + 7.0F && caption.y == glyph.y && caption.height == glyph.height,
           "the caption starts after the glyph and shares its vertical extent");
    expect(caption.x + caption.width == 200.0F, "the caption ends at the control's width");
    expect(ui::toggle_caption_box(glyph, false, 10.0F, gap).width == 0.0F, "a control narrower than its glyph leaves no caption width");
}

void test_zero_resource_dimensions_keep_a_radio_clickable() {
    const auto horizontal = ui::toggle_minimum_size({70, 0}, {23, 21});
    expect(horizontal.width == 0 && horizontal.height == 21,
           "a horizontal zero-height radio gets its OFF-art height without changing the authored width");
    const auto stock = ui::toggle_minimum_size({186, 25}, {23, 21});
    expect(stock.width == 0 && stock.height == 0, "positive stock resource dimensions impose no new minimum");
    const auto narrow = ui::toggle_minimum_size({10, 8}, {23, 21});
    expect(narrow.width == 0 && narrow.height == 0, "even a small positive resource rectangle stays authored");
    const auto empty = ui::toggle_minimum_size({}, {23, 21});
    expect(empty.width == 23 && empty.height == 21, "both empty dimensions use the glyph");
    const auto missing = ui::toggle_minimum_size({}, {});
    expect(missing.width == 0 && missing.height == 0, "absent art invents no hit dimensions");
}
} // namespace

int main() {
    test_glyph_is_the_off_art();
    test_rows_of_different_width_share_one_glyph();
    test_caption_follows_the_glyph();
    test_zero_resource_dimensions_keep_a_radio_clickable();
    if (test::ui::failures() != 0) {
        std::cerr << test::ui::failures() << " toggle layout check(s) failed\n";
        return 1;
    }
    std::cout << "ui toggle layout passed\n";
    return 0;
}
