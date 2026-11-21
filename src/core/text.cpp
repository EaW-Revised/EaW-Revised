#include "eawr/core/text.hpp"

#include <cmath>
#include <locale>
#include <sstream>

namespace eawr::core {

std::optional<double> parse_decimal_double(const std::string_view token) {
    if (token.empty()) return std::nullopt;
    for (const char character : token) {
        const bool allowed = (character >= '0' && character <= '9') || character == '.' || character == 'e' ||
            character == 'E' || character == '+' || character == '-';
        if (!allowed) return std::nullopt;
    }
    if (token.front() == '+') return std::nullopt;
    std::istringstream stream{std::string(token)};
    stream.imbue(std::locale::classic());
    double parsed{};
    stream >> parsed;
    if (stream.fail() || stream.peek() != std::istringstream::traits_type::eof() || !std::isfinite(parsed)) {
        return std::nullopt;
    }
    return parsed;
}

} // namespace eawr::core
