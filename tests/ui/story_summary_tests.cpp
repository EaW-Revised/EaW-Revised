// The story summary (WST-58/68): the dialog file's chapter text, the open/refresh/flash model and the goal list.

#include "eawr/presentation/ui/story_summary.hpp"
#include "ui_test_support.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace {
namespace ui = eawr::presentation::ui;

void expect(const bool condition, const std::string& message) { eawr::test::ui::expect(condition, message.c_str()); }

const std::string dialog =
    "[CHAPTER 0]\r\n\r\nTITLE TEXT_FIRST_TITLE\r\n# MOVIE Skipped_Loop\r\nWAIT 500\r\nSFX Button_Press\r\n"
    "TEXTCOLOR 255 255 255 176\r\nTEXT TEXT_ZERO_A\r\nTEXT TEXT_ZERO_B\r\n# DIALOG SPEECH_ZERO\r\nEXTRA_TEXT\r\n\r\n"
    "[chapter 1]\r\n\r\nTITLE TEXT_SECOND_TITLE\r\nTEXT TEXT_ONE_A\r\nNEWLINE 2\r\nTEXT   TEXT_ONE_B  \r\n";

std::vector<std::string> keys(const ui::StoryChapterText& chapter) {
    std::vector<std::string> result;
    for (const auto& line : chapter.lines) result.push_back(line.key);
    return result;
}

void chapters() {
    const auto zero = ui::story_dialog_chapter(dialog, 0);
    expect(zero && zero->title == "TEXT_FIRST_TITLE" && keys(*zero) == (std::vector<std::string>{"TEXT_ZERO_A", "TEXT_ZERO_B"}),
        "WST-58 chapter 0 gives its title and text keys, without commented, timing or sound lines");
    expect(zero && zero->lines.size() == 2 && zero->lines[0].colour == (std::array<std::uint8_t, 4>{255, 255, 255, 176})
        && zero->lines[1].colour == zero->lines[0].colour, "TEXTCOLOR sets the colour of the text lines after it, and it stays");
    const auto one = ui::story_dialog_chapter(dialog, 1);
    expect(one && one->title == "TEXT_SECOND_TITLE" && keys(*one) == (std::vector<std::string>{"TEXT_ONE_A", "", "", "TEXT_ONE_B"}),
        "WST-58 the chapter heading is case-insensitive, NEWLINE n gives n blank rows and the last chapter runs to the end");
    expect(one && !one->lines.front().colour, "text before any TEXTCOLOR has no authored colour");
    expect(!ui::story_dialog_chapter(dialog, 2), "a missing chapter is nothing");
    expect(!ui::story_dialog_chapter({}, 0), "an empty file has no chapter");
    const auto stock = ui::story_dialog_chapter(
        "[CHAPTER 0]\nTITLE T\nTEXTCOLOR 255 255 255 176\nTEXT A\nTEXT B\nNEWLINE 1\nTEXTCOLOR 255 024 039 255\nTEXT C\nTEXT D\n"
        "WAIT 1000\nNEWLINE 2\nTEXTCOLOR 255 255 255 176\nEXTRA_TEXT\n", 0);
    expect(stock && keys(*stock) == (std::vector<std::string>{"A", "B", "", "C", "D"}), "trailing blank rows are dropped");
    expect(stock && stock->lines[3].colour == (std::array<std::uint8_t, 4>{255, 24, 39, 255})
        && stock->lines[2].colour == stock->lines[1].colour, "a later TEXTCOLOR (leading zeros read as decimal) applies from there");
    expect(stock && stock->first_text_key() == "A", "the goal's name is the first text line");
    const auto bad = ui::story_dialog_chapter("[CHAPTER 0]\nTEXTCOLOR 1 2\nTEXT A\nNEWLINE x\nTEXT B\nNEWLINE 999\nTEXT C\n", 0);
    expect(bad && !bad->lines[0].colour && bad->lines.size() == 1 + 1 + 1 + 32 + 1,
        "a malformed TEXTCOLOR keeps no colour; a malformed NEWLINE is one row and a large count is bounded");
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
ui::StoryGoalRow goal(const char* tag, std::int64_t chapter, bool completed = false, bool remove_after_viewed = false) {
    ui::StoryGoalRow row;
    row.tag = tag; row.dialog = "dialog_x"; row.chapter = chapter; row.completed = completed;
    row.remove_after_viewed = remove_after_viewed;
    return row;
}
std::vector<std::string> tags(const ui::StorySummary& model) {
    std::vector<std::string> result;
    for (const auto* row : model.rows()) result.push_back(row->tag);
    return result;
}

void goals() {
    using Request = ui::StorySummary::Request;
    using Tab = ui::StorySummary::Tab;
    ui::StorySummary model;
    expect(!model.has_goals(), "WGUI-36 no goal: the button has nothing to open");
    model.set_goal(goal("Older", 1));
    model.set_goal(goal("Newer", 2));
    expect(model.has_goals() && tags(model) == (std::vector<std::string>{"Newer", "Older"}),
        "WST-68 a new goal goes to the head of the list");
    model.set_goal(goal("Older", 5));
    expect(tags(model) == (std::vector<std::string>{"Newer", "Older"}) && model.goals().back().chapter == 5,
        "a known tag is updated in place");
    expect(!model.visible() && !model.viewed("Newer"), "a goal nobody has read is unviewed");

    model.request(Request::open, "Newer", 2);
    expect(model.visible() && model.selected() && model.selected()->tag == "Newer" && model.chapter() == 2 && model.viewed("Newer")
        && !model.viewed("Older"), "WST-68 opening at a goal shows its chapter and marks that goal viewed");
    model.select("Older");
    expect(model.tag() == "Older" && model.chapter() == 5 && model.viewed("Older"),
        "selecting a row shows that goal's chapter (its own chapter) and marks it viewed");
    model.select("Missing");
    expect(model.tag() == "Older", "selecting a tag that is not listed changes nothing");

    model.set_goal(goal("Older", 5, true));
    expect(model.tag() == "Newer" && tags(model) == (std::vector<std::string>{"Newer"}),
        "a goal that completes leaves the Active tab and the selection moves to the tab's first goal");
    model.choose_tab(Tab::completed);
    expect(model.tab() == Tab::completed && tags(model) == (std::vector<std::string>{"Older"}) && model.tag() == "Older"
        && model.chapter() == 5, "the Completed tab lists completed goals and selects its first");
    model.choose_tab(Tab::active);
    expect(model.tag() == "Newer", "choosing a tab selects its first goal");

    ui::StorySummary hidden;
    hidden.set_goal(goal("Shown", 0));
    auto secret = goal("Secret", 0); secret.hidden = true;
    hidden.set_goal(secret);
    expect(tags(hidden) == (std::vector<std::string>{"Shown"}), "a hidden goal is on neither tab");
    hidden.erase_goal("Shown");
    expect(!hidden.has_goals(), "only hidden goals left: the button has nothing to open");

    ui::StorySummary removal;
    removal.set_goal(goal("Keep", 0));
    removal.set_goal(goal("Once", 1, false, true));
    removal.set_goal(goal("Unread", 2, false, true));
    removal.request(Request::open, "Once", 1);
    removal.select("Keep");
    expect(removal.close() == 1 && !removal.visible(), "WST-68 closing drops a goal that is viewed and remove-after-viewed");
    expect(tags(removal) == (std::vector<std::string>{"Unread", "Keep"}) && !removal.viewed("Once"),
        "a tagged (kept) goal and a goal never read both stay");
    removal.request(Request::open, "Unread", 2);
    expect(removal.visible() && removal.viewed("Unread") && removal.tag() == "Unread", "reading it marks it viewed");
    expect(removal.close() == 1 && tags(removal) == (std::vector<std::string>{"Keep"}) && removal.tag() == "Keep",
        "and closing drops it too; the selection settles on what is left");

    ui::StorySummary reopened;
    reopened.set_goal(goal("Done", 3, true));
    reopened.request(Request::open, "Done", 3);
    expect(reopened.tab() == Tab::completed && reopened.selected() && reopened.selected()->tag == "Done",
        "an open at a completed goal shows the Completed tab");
    reopened.clear_goals();
    expect(!reopened.has_goals() && reopened.tag().empty(), "clearing the goals leaves nothing selected");
}
} // namespace

void story_summary_contracts() {
    chapters();
    summary();
    goals();
}
