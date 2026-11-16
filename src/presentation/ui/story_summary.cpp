#include "eawr/presentation/ui/story_summary.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>

namespace eawr::presentation::ui {
namespace {
std::string_view trimmed(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return text;
}
bool iequals(std::string_view left, std::string_view right) {
    return std::equal(left.begin(), left.end(), right.begin(), right.end(), [](char a, char b) {
        return std::toupper(static_cast<unsigned char>(a)) == std::toupper(static_cast<unsigned char>(b)); });
}
bool starts_with_word(std::string_view line, std::string_view word) {
    if (line.size() <= word.size() || !iequals(line.substr(0, word.size()), word)) return false;
    return std::isspace(static_cast<unsigned char>(line[word.size()])) != 0;
}
} // namespace

std::optional<StoryChapterText> story_dialog_chapter(const std::string_view file_text, const std::int64_t chapter) {
    std::optional<StoryChapterText> found;
    std::optional<std::array<std::uint8_t, 4>> colour;
    bool inside = false;
    std::string_view rest = file_text;
    while (!rest.empty()) {
        const auto end = rest.find('\n');
        const auto line = trimmed(rest.substr(0, end));
        rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
        if (line.empty() || line.front() == '#') continue;
        if (line.front() == '[') {
            inside = false;
            const auto close = line.find(']');
            auto body = trimmed(line.substr(1, close == std::string_view::npos ? std::string_view::npos : close - 1));
            constexpr std::string_view word = "CHAPTER";
            if (body.size() <= word.size() || !iequals(body.substr(0, word.size()), word)) continue;
            body = trimmed(body.substr(word.size()));
            std::int64_t number{};
            const auto parsed = std::from_chars(body.data(), body.data() + body.size(), number);
            if (parsed.ec == std::errc{} && number == chapter && !found) { inside = true; found.emplace(); colour.reset(); }
            continue;
        }
        if (!inside) continue;
        if (starts_with_word(line, "TITLE")) found->title = std::string(trimmed(line.substr(5)));
        else if (starts_with_word(line, "TEXT")) found->lines.push_back({std::string(trimmed(line.substr(4))), colour});
        else if (starts_with_word(line, "TEXTCOLOR")) {
            // Four integers r g b a; a malformed line keeps the colour in effect (diagnosis belongs to the loader).
            std::array<std::uint8_t, 4> parts{};
            auto body = trimmed(line.substr(9));
            bool good = true;
            for (auto& part : parts) {
                body = trimmed(body);
                int value{};
                const auto parsed = std::from_chars(body.data(), body.data() + body.size(), value);
                if (parsed.ec != std::errc{}) { good = false; break; }
                part = static_cast<std::uint8_t>(std::clamp(value, 0, 255));
                body.remove_prefix(static_cast<std::size_t>(parsed.ptr - body.data()));
            }
            if (good) colour = parts;
        } else if (starts_with_word(line, "NEWLINE")) {
            // n blank rows; a malformed or missing count is one row. Bounded so a bad file cannot flood the list.
            int count = 1;
            const auto body = trimmed(line.substr(7));
            const auto parsed = std::from_chars(body.data(), body.data() + body.size(), count);
            if (parsed.ec != std::errc{}) count = 1;
            for (int row = 0; row < std::clamp(count, 0, 32); ++row) found->lines.push_back({{}, colour});
        }
    }
    if (found) while (!found->lines.empty() && found->lines.back().key.empty()) found->lines.pop_back();
    return found;
}

std::string StoryChapterText::first_text_key() const {
    for (const auto& line : lines) if (!line.key.empty()) return line.key;
    return {};
}

void StorySummary::request(const Request action, const std::string_view tag, const std::int64_t chapter) {
    switch (action) {
    case Request::open:
        visible_ = true; flashing_ = false;
        break;
    case Request::refresh:
        if (!visible_) return;
        break;
    case Request::flash_button:
        flashing_ = true; ++revision_;
        return;
    case Request::refresh_or_flash:
        if (!visible_) { flashing_ = true; ++revision_; return; }
        flashing_ = false; // WST-70: an open summary refreshes without the quiet branch
        break;
    }
    tag_ = std::string(tag); chapter_ = chapter; ++revision_;
    if (const auto* goal = find(tag_)) tab_ = goal->completed ? Tab::completed : Tab::active;
    mark_viewed();
}

std::size_t StorySummary::close() {
    const bool was_visible = visible_;
    visible_ = false;
    std::size_t removed = 0;
    for (auto at = goals_.begin(); at != goals_.end();) {
        if (at->remove_after_viewed && viewed(at->tag)) { at = goals_.erase(at); ++removed; } else ++at;
    }
    std::erase_if(viewed_, [&](const std::string& tag) { return !find(tag); });
    if (removed || was_visible) { settle_selection(); ++revision_; }
    return removed;
}

const StoryGoalRow* StorySummary::find(const std::string_view tag) const {
    for (const auto& goal : goals_) if (goal.tag == tag) return &goal;
    return nullptr;
}

bool StorySummary::viewed(const std::string_view tag) const {
    return std::find(viewed_.begin(), viewed_.end(), tag) != viewed_.end();
}

void StorySummary::mark_viewed() {
    if (visible_ && !tag_.empty() && find(tag_) && !viewed(tag_)) viewed_.push_back(tag_);
}

bool StorySummary::has_goals() const noexcept {
    return std::any_of(goals_.begin(), goals_.end(), [](const StoryGoalRow& goal) { return !goal.hidden; });
}

std::vector<const StoryGoalRow*> StorySummary::rows() const {
    std::vector<const StoryGoalRow*> result;
    for (const auto& goal : goals_)
        if (!goal.hidden && goal.completed == (tab_ == Tab::completed)) result.push_back(&goal);
    return result;
}

const StoryGoalRow* StorySummary::selected() const {
    const auto* goal = find(tag_);
    return goal && !goal->hidden && goal->completed == (tab_ == Tab::completed) ? goal : nullptr;
}

// Keeps the shown goal on the shown tab: a goal that left it (completed, hidden, removed) gives way to the tab's first.
void StorySummary::settle_selection() {
    if (const auto* goal = selected()) { chapter_ = goal->chapter; return; }
    const auto shown = rows();
    if (shown.empty()) { if (find(tag_)) { tag_.clear(); chapter_ = 0; } return; } // an unlisted request tag stays as asked
    tag_ = shown.front()->tag; chapter_ = shown.front()->chapter;
    mark_viewed();
}

void StorySummary::set_goal(StoryGoalRow row) {
    auto at = std::find_if(goals_.begin(), goals_.end(), [&](const StoryGoalRow& goal) { return goal.tag == row.tag; });
    if (at == goals_.end()) goals_.insert(goals_.begin(), std::move(row)); else *at = std::move(row);
    ++revision_;
    if (!tag_.empty() || visible_) settle_selection();
}

void StorySummary::erase_goal(const std::string_view tag) {
    const auto before = goals_.size();
    std::erase_if(goals_, [&](const StoryGoalRow& goal) { return goal.tag == tag; });
    if (goals_.size() == before) return;
    std::erase_if(viewed_, [&](const std::string& seen) { return seen == tag; });
    ++revision_;
    settle_selection();
}

void StorySummary::clear_goals() {
    if (goals_.empty() && viewed_.empty()) return;
    goals_.clear(); viewed_.clear(); tag_.clear(); chapter_ = 0; ++revision_;
}

void StorySummary::choose_tab(const Tab tab) {
    if (tab == tab_) return;
    tab_ = tab; tag_.clear(); chapter_ = 0;
    settle_selection();
    ++revision_;
}

void StorySummary::select(const std::string_view tag) {
    const auto* goal = find(tag);
    if (!goal || goal->hidden || goal->completed != (tab_ == Tab::completed) || tag_ == tag) return;
    tag_ = goal->tag; chapter_ = goal->chapter;
    mark_viewed();
    ++revision_;
}

} // namespace eawr::presentation::ui
