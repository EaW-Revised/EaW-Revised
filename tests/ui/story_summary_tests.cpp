// The story summary (WST-58): the dialog file's chapter text and the open/refresh/flash model.

#include "eawr/presentation/ui/story_summary.hpp"
#include "ui_test_support.hpp"

#include <string>
#include <vector>

namespace {
namespace ui = eawr::presentation::ui;

void expect(const bool condition, const std::string& message) { eawr::test::ui::expect(condition, message.c_str()); }

const std::string dialog =
    "[CHAPTER 0]\r\n\r\nTITLE TEXT_FIRST_TITLE\r\n# MOVIE Skipped_Loop\r\nWAIT 500\r\nSFX Button_Press\r\n"
    "TEXTCOLOR 255 255 255 176\r\nTEXT TEXT_ZERO_A\r\nTEXT TEXT_ZERO_B\r\n# DIALOG SPEECH_ZERO\r\nEXTRA_TEXT\r\n\r\n"
    "[chapter 1]\r\n\r\nTITLE TEXT_SECOND_TITLE\r\nTEXT TEXT_ONE_A\r\nNEWLINE 2\r\nTEXT   TEXT_ONE_B  \r\n";

void chapters() {
    const auto zero = ui::story_dialog_chapter(dialog, 0);
    expect(zero && zero->title == "TEXT_FIRST_TITLE" && zero->lines == (std::vector<std::string>{"TEXT_ZERO_A", "TEXT_ZERO_B"}),
        "WST-58 chapter 0 gives its title and text keys, without commented, timing or colour lines");
    const auto one = ui::story_dialog_chapter(dialog, 1);
    expect(one && one->title == "TEXT_SECOND_TITLE" && one->lines == (std::vector<std::string>{"TEXT_ONE_A", "TEXT_ONE_B"}),
        "WST-58 the chapter heading is case-insensitive and the last chapter runs to the end of the file");
    expect(!ui::story_dialog_chapter(dialog, 2), "a missing chapter is nothing");
    expect(!ui::story_dialog_chapter({}, 0), "an empty file has no chapter");
}

void summary() {
    using Request = ui::StorySummary::Request;
    ui::StorySummary model;
    model.request(Request::refresh, "Tag", 3);
    expect(!model.visible() && model.revision() == 0, "refresh does nothing while the summary is closed");
    model.request(Request::flash_button, "Tag", 3);
    expect(!model.visible() && model.button_flashing(), "the quiet path flashes the button and opens nothing");
    model.request(Request::open, "Intro_Tag", 9);
    expect(model.visible() && !model.button_flashing() && model.tag() == "Intro_Tag" && model.chapter() == 9,
        "the popup path opens the summary at the chapter");
    const auto revision = model.revision();
    model.request(Request::refresh, "Other", 2);
    expect(model.visible() && model.chapter() == 2 && model.revision() > revision, "an open summary refreshes to the new chapter");
    const auto before_quiet = model.revision();
    model.request(Request::refresh_or_flash, "Quiet_Tag", 7);
    expect(model.visible() && !model.button_flashing() && model.tag() == "Quiet_Tag" && model.chapter() == 7
        && model.revision() > before_quiet, "WST-70 a quiet request refreshes the real open summary");
    model.close();
    expect(!model.visible(), "closing hides the summary");
    model.request(Request::refresh_or_flash, "Closed_Tag", 8);
    expect(!model.visible() && model.button_flashing() && model.tag() == "Quiet_Tag",
        "WST-70 the same quiet request flashes only while the summary is closed");
}
} // namespace

void story_summary_contracts() {
    chapters();
    summary();
}
