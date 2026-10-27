#include "eawr/presentation/ui/story_summary.hpp"

#include <algorithm>
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
            if (parsed.ec == std::errc{} && number == chapter && !found) { inside = true; found.emplace(); }
            continue;
        }
        if (!inside) continue;
        if (starts_with_word(line, "TITLE")) found->title = std::string(trimmed(line.substr(5)));
        else if (starts_with_word(line, "TEXT")) found->lines.emplace_back(trimmed(line.substr(4)));
    }
    return found;
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
}

} // namespace eawr::presentation::ui
