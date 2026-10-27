#pragma once

// The story summary behind the story-arc button (WST-58, docs/behaviour/walks/gc-story.md), engine-free: the
// dialog file's chapter text and the open/refresh/flash model that consumes the session's unhashed summary
// requests. It owns no simulation state; the goal rows live with the campaign session.
//
// Dialog file grammar (WST-83..85, partly unverified): `[CHAPTER n]` starts a chapter; `TITLE key` names its
// title; each `TEXT key` is one line of body text. Timing (`WAIT`), `SFX`, `MOVIE`, `DIALOG` (speech), colours,
// `NEWLINE` and `EXTRA_TEXT` belong to the speech/movie services and are not read here. `#` comments are skipped.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::presentation::ui {

struct StoryChapterText {
    std::string title;              // text key, empty when the chapter authors none
    std::vector<std::string> lines; // text keys in file order
};

// The chapter's text keys, or nothing when the file has no such chapter.
[[nodiscard]] std::optional<StoryChapterText> story_dialog_chapter(std::string_view file_text, std::int64_t chapter);

// The summary's visible state. `open` shows the chapter (a centred popup); `refresh` re-reads the chapter only when
// the summary is already open; `flash_button` never opens it. `refresh_or_flash` chooses using this local visible state.
class StorySummary final {
public:
    enum class Request { open, refresh, flash_button, refresh_or_flash };
    void request(Request action, std::string_view tag, std::int64_t chapter);
    void close() noexcept { visible_ = false; }
    [[nodiscard]] bool visible() const noexcept { return visible_; }
    [[nodiscard]] bool button_flashing() const noexcept { return flashing_; }
    [[nodiscard]] const std::string& tag() const noexcept { return tag_; }
    [[nodiscard]] std::int64_t chapter() const noexcept { return chapter_; }
    // Bumps on every change the panel must redraw.
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

private:
    bool visible_{}, flashing_{};
    std::string tag_;
    std::int64_t chapter_{};
    std::uint64_t revision_{};
};

} // namespace eawr::presentation::ui
