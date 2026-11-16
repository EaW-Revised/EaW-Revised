#include "eawr/core/text_bounds.hpp"

#include <string_view>

namespace eawr::core {

bool cut_text(std::string& text, const std::size_t limit) {
    if (text.size() <= limit) return false;
    constexpr std::string_view marker = "...[truncated]";
    auto end = limit - marker.size();
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xc0U) == 0x80U) --end;
    text.resize(end);
    text += marker;
    return true;
}

void redact_paths(std::string& text) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        const bool boundary = i == 0 || text[i - 1] == ' ' || text[i - 1] == '\'' || text[i - 1] == '"'
            || text[i - 1] == '=' || text[i - 1] == '(';
        if (!boundary) continue;
        const bool drive = i + 2 < text.size() && ((text[i] >= 'A' && text[i] <= 'Z') || (text[i] >= 'a' && text[i] <= 'z'))
            && text[i + 1] == ':' && (text[i + 2] == '/' || text[i + 2] == '\\');
        const bool rooted = i + 1 < text.size() && (text[i] == '/' || text[i] == '\\') && text[i + 1] != ' ';
        if (drive || rooted) { text.resize(i); text += "[redacted-path]"; return; }
    }
}

} // namespace eawr::core
