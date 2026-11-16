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
// The planet name an advisor notice shows: the text of the planet type's Text_ID when the text database has it, else the
// planet's own name. Never gated on discovery (the notices that use it name planets the observer has not seen).
[[nodiscard]] inline std::string notice_planet_name(const std::string& own_name, const std::string& type_text) {
    return type_text.empty() ? own_name : type_text;
}

// A few stock lines are printf formats ("%ls: Control changed from %ls to %ls"). Replace each %ls / %s / %S / %d with the next
// argument in order ("%%" is one percent sign); a conversion without an argument, and any other "%", stay as authored.
[[nodiscard]] inline std::string expand_printf_notice(const std::string_view format,
    const std::span<const std::string> arguments) {
    std::string result;
    result.reserve(format.size());
    std::size_t next = 0;
    for (std::size_t at = 0; at < format.size();) {
        if (format[at] == '%' && at + 1 < format.size()) {
            if (format[at + 1] == '%') { result += '%'; at += 2; continue; }
            std::size_t length = 0;
            if (format.compare(at + 1, 2, "ls") == 0) length = 3;
            else if (format[at + 1] == 's' || format[at + 1] == 'S' || format[at + 1] == 'd') length = 2;
            if (length != 0 && next < arguments.size()) { result += arguments[next++]; at += length; continue; }
        }
        result += format[at++];
    }
    return result;
}
} // namespace eawr::viewer::galactic
