#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace eawr::viewer::galactic {
// GHUD notices: the stock master text marks each argument as "##" plus a decimal
// index ("Enemy fleet incoming at ##0."). Replace the tokens as data; an index
// without an argument stays as authored.
[[nodiscard]] inline std::string expand_notice_text(const std::string_view format,
    const std::span<const std::string> arguments) {
    std::string result;
    result.reserve(format.size());
    for (std::size_t at = 0; at < format.size();) {
        if (format.compare(at, 2, "##") == 0) {
            std::size_t end = at + 2;
            std::size_t index = 0;
            while (end < format.size() && format[end] >= '0' && format[end] <= '9' && end - at - 2 < 4)
                index = index * 10 + static_cast<std::size_t>(format[end++] - '0');
            if (end > at + 2 && index < arguments.size()) {
                result += arguments[index];
                at = end;
                continue;
            }
        }
        result += format[at++];
    }
    return result;
}
} // namespace eawr::viewer::galactic
