#pragma once

#include <string>
#include <string_view>

namespace eawr::core {

// Quote a byte string. Controls use lowercase \u00xx, except newline, carriage
// return and tab, which retain their short escapes. Non-ASCII bytes pass through.
[[nodiscard]] std::string json_quote(std::string_view text);

// Existing report formats differ in how they encode control and non-ASCII bytes.
// Keep those formats explicit rather than silently changing their output.
[[nodiscard]] std::string json_quote_short_controls(std::string_view text);
[[nodiscard]] std::string json_quote_ascii(std::string_view text);
[[nodiscard]] std::string json_quote_ascii_short_controls(std::string_view text);
[[nodiscard]] std::string json_quote_space_controls(std::string_view text);
[[nodiscard]] std::string json_quote_drop_controls(std::string_view text);

// Legacy UI reports quote punctuation but retain control bytes verbatim.
[[nodiscard]] std::string json_quote_verbatim_controls(std::string_view text);

} // namespace eawr::core
