#pragma once
#include "eawr/data/ui/dialog_script.hpp"

namespace eawr::data::ui::dialog_script_detail {
using assets::Source;
core::Diagnostic make_diagnostic(const Source& source, std::string_view code,
    core::Severity severity, std::string message, std::uint32_t line = 0U, std::uint32_t column = 0U);
bool identifier_start(char value);
bool identifier_part(char value);
std::optional<std::int64_t> integer_literal(std::string_view text);
std::string_view trim(std::string_view text);
void parse_dialog_tokens(const RcTokens& tokens, DialogScript& script);
} // namespace eawr::data::ui::dialog_script_detail
