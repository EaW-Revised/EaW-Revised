#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace eawr::net {
// Text for the diagnostic log (#4021): dotted IPv4 (with an optional port) and bracketed IPv6 (with an optional port)
// become "<addr>", and control characters become '?', so a line carries no address and no attacker-chosen escape.
inline std::string redact_log_text(std::string_view text) {
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    std::string result;
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '[') {
            const auto close = text.find(']', i);
            if (close != std::string_view::npos && close - i <= 48) {
                i = close + 1;
                if (i < text.size() && text[i] == ':') for (++i; i < text.size() && digit(text[i]); ++i) {}
                result += "<addr>"; continue;
            }
        }
        if (digit(text[i]) && (i == 0 || !digit(text[i - 1]))) {
            std::size_t j = i, groups = 0;
            while (groups < 4) {
                const auto begin = j;
                while (j < text.size() && digit(text[j]) && j - begin < 3) ++j;
                if (j == begin) break;
                if (++groups < 4) { if (j < text.size() && text[j] == '.') ++j; else break; }
            }
            if (groups == 4) {
                if (j < text.size() && text[j] == ':') {
                    auto k = j + 1;
                    while (k < text.size() && digit(text[k])) ++k;
                    if (k > j + 1) j = k;
                }
                result += "<addr>"; i = j; continue;
            }
        }
        const auto c = static_cast<unsigned char>(text[i++]);
        result += c < 0x20 ? '?' : static_cast<char>(c);
    }
    return result;
}
} // namespace eawr::net
