#pragma once

// The story summary behind the story-arc button (WST-58, docs/behaviour/walks/gc-story.md), engine-free: the
// dialog file's chapter text and the goal list / tab / selection / open-refresh-flash model that consumes the session's
// unhashed summary and goal requests. It owns no simulation state; the goal rows live with the campaign session.
//
// Dialog file grammar (WST-83..85, partly unverified): `[CHAPTER n]` starts a chapter; `TITLE key` names its
// title; each `TEXT key` is one line of body text in the colour set by the last `TEXTCOLOR r g b a` before it (the colour
// stays until the next one); `NEWLINE n` adds n blank rows. Timing (`WAIT`), `SFX`, `MOVIE`, `DIALOG` (speech) and
// `EXTRA_TEXT` belong to the speech/movie services and are not read here. `#` comments are skipped.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::presentation::ui {

struct StoryChapterLine {
    std::string key;                                   // text key; empty for a blank row (NEWLINE)
    std::optional<std::array<std::uint8_t, 4>> colour; // r g b a of the TEXTCOLOR in effect, none before the first
    bool operator==(const StoryChapterLine&) const = default;
};

struct StoryChapterText {
    std::string title;                    // text key, empty when the chapter authors none
    std::vector<StoryChapterLine> lines;  // in file order; trailing blank rows are dropped
    // The first TEXT line's key: the goal's name in the goal list (WST-58, observed in the original's list), or empty.
    [[nodiscard]] std::string first_text_key() const;
};

// The chapter's text, or nothing when the file has no such chapter.
[[nodiscard]] std::optional<StoryChapterText> story_dialog_chapter(std::string_view file_text, std::int64_t chapter);

// One holocron goal as the summary lists it (a mirror of the session's published goal row).
struct StoryGoalRow {
    std::string tag, dialog;
    std::int64_t chapter{};
    bool completed{}, hidden{}, remove_after_viewed{};
    bool operator==(const StoryGoalRow&) const = default;
};

// The summary's visible state. `open` shows the chapter (a centred popup); `refresh` re-reads the chapter only when
// the summary is already open; `flash_button` never opens it. `refresh_or_flash` chooses using this local visible state.
//
// The goal list is newest first (WST-68: every insertion prepends; an update of a known tag keeps its place). The Active
// tab lists goals not yet completed, the Completed tab the others; a hidden goal is on neither. Reading a goal's chapter
// marks it viewed; closing the summary drops goals that are both viewed and remove-after-viewed (WST-68). Viewed is local
// presentation state, not saved: a loaded game starts with every goal unviewed (a named deviation, HCB-71).
class StorySummary final {
public:
    enum class Request { open, refresh, flash_button, refresh_or_flash };
    enum class Tab { active, completed };

    void request(Request action, std::string_view tag, std::int64_t chapter);
    // Closing deletes the goals that are viewed and remove-after-viewed; returns how many went.
    std::size_t close();
    [[nodiscard]] bool visible() const noexcept { return visible_; }
    // WST-96: one nested pause belongs to an open single-player summary. The
    // caller submits the existing galaxy pause/resume command only on a change.
    [[nodiscard]] std::optional<bool> galactic_pause_change(bool multiplayer) noexcept {
        const bool requested = visible_ && !multiplayer;
        if (requested == galaxy_pause_) return std::nullopt;
        galaxy_pause_ = requested;
        return requested;
    }
    [[nodiscard]] bool button_flashing() const noexcept { return flashing_; }
    // The goal whose chapter is shown, empty when the shown tab has no goal.
    [[nodiscard]] const std::string& tag() const noexcept { return tag_; }
    [[nodiscard]] std::int64_t chapter() const noexcept { return chapter_; }
    [[nodiscard]] Tab tab() const noexcept { return tab_; }
    // Bumps on every change the panel must redraw.
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

    // The mirrored goal rows. `set_goal` inserts a new tag at the front and replaces a known tag in place.
    void set_goal(StoryGoalRow row);
    void erase_goal(std::string_view tag);
    void clear_goals();
    [[nodiscard]] bool has_goals() const noexcept; // any non-hidden goal, on either tab (WGUI-36 button gate)
    [[nodiscard]] const std::vector<StoryGoalRow>& goals() const noexcept { return goals_; }
    // The rows on the shown tab, newest first.
    [[nodiscard]] std::vector<const StoryGoalRow*> rows() const;
    [[nodiscard]] const StoryGoalRow* selected() const;
    [[nodiscard]] bool viewed(std::string_view tag) const;

    // The user's choices. Choosing a tab selects its first goal (or none); selecting a goal shows its chapter.
    void choose_tab(Tab tab);
    void select(std::string_view tag);

private:
    [[nodiscard]] const StoryGoalRow* find(std::string_view tag) const;
    void settle_selection();
    void mark_viewed();
    bool visible_{}, flashing_{}, galaxy_pause_{};
    Tab tab_{Tab::active};
    std::string tag_;
    std::int64_t chapter_{};
    std::vector<StoryGoalRow> goals_;
    std::vector<std::string> viewed_;
    std::uint64_t revision_{};
};

} // namespace eawr::presentation::ui
