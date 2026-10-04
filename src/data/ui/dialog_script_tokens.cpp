#include "dialog_script_internal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <set>
#include <sstream>
#include <utility>

namespace eawr::data::ui {
using namespace dialog_script_detail;

namespace dialog_script_detail {
namespace {
bool digit(const char value) { return value >= '0' && value <= '9'; }
int hex_digit(const char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

} // namespace

core::Diagnostic make_diagnostic(const Source& source, const std::string_view code,
                                 const core::Severity severity, std::string message,
                                 const std::uint32_t line, const std::uint32_t column) {
    core::Diagnostic diagnostic;
    diagnostic.code = std::string(code);
    diagnostic.severity = severity;
    diagnostic.message = std::move(message);
    diagnostic.logical_path = source.logical_path;
    if (line != 0U) diagnostic.line = line;
    if (column != 0U) diagnostic.column = column;
    diagnostic.source_id = source.source_id;
    return diagnostic;
}

bool identifier_start(const char value) {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') || value == '_';
}
bool identifier_part(const char value) {
    return identifier_start(value) || (value >= '0' && value <= '9');
}
// Integer literal as the resource compiler reads it: decimal or 0x hex with
// optional L/U suffixes. Values wider than 32 bits are rejected.
std::optional<std::int64_t> integer_literal(const std::string_view text) {
    std::string_view digits = text;
    while (!digits.empty() && (digits.back() == 'L' || digits.back() == 'l' ||
                               digits.back() == 'U' || digits.back() == 'u')) {
        digits.remove_suffix(1);
    }
    if (digits.empty()) return std::nullopt;
    std::uint64_t value{};
    if (digits.size() > 2U && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
        for (const char character : digits.substr(2U)) {
            const int nibble = hex_digit(character);
            if (nibble < 0) return std::nullopt;
            value = value * 16U + static_cast<std::uint64_t>(nibble);
            if (value > 0xffffffffULL) return std::nullopt;
        }
    } else {
        for (const char character : digits) {
            if (!digit(character)) return std::nullopt;
            value = value * 10U + static_cast<std::uint64_t>(character - '0');
            if (value > 0xffffffffULL) return std::nullopt;
        }
    }
    return static_cast<std::int64_t>(value);
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}


} // namespace dialog_script_detail

namespace {
class Scanner final {
public:
    Scanner(const std::string_view text, const Source& source) : text_(text), source_(source) {}

    core::Result<RcTokens> run() {
        RcTokens result;
        bool line_start = true;
        while (position_ < text_.size()) {
            const char character = text_[position_];
            if (character == '\n') {
                advance();
                line_start = true;
                continue;
            }
            if (character == ' ' || character == '\t' || character == '\r' || character == '\f' ||
                character == '\v') {
                advance();
                continue;
            }
            if (character == '/' && peek(1U) == '/') {
                while (position_ < text_.size() && text_[position_] != '\n') advance();
                continue;
            }
            if (character == '/' && peek(1U) == '*') {
                const auto line = line_;
                const auto column = column_;
                advance();
                advance();
                while (position_ < text_.size() && !(text_[position_] == '*' && peek(1U) == '/')) {
                    advance();
                }
                if (position_ >= text_.size()) return failure("unterminated block comment", line, column);
                advance();
                advance();
                continue;
            }
            if (character == '#' && line_start) {
                result.directives.push_back(directive());
                continue;
            }
            line_start = false;
            if (character == '"' || (character == 'L' && peek(1U) == '"')) {
                auto token = string();
                if (!token) return core::Result<RcTokens>::failure(token.error());
                result.tokens.push_back(std::move(token.value()));
                continue;
            }
            if (digit(character)) {
                auto token = number();
                if (!token) return core::Result<RcTokens>::failure(token.error());
                result.tokens.push_back(std::move(token.value()));
                continue;
            }
            if (identifier_start(character)) {
                RcToken token{RcTokenKind::identifier, {}, 0, line_, column_};
                while (position_ < text_.size() && identifier_part(text_[position_])) {
                    token.text.push_back(text_[position_]);
                    advance();
                }
                result.tokens.push_back(std::move(token));
                continue;
            }
            static constexpr std::string_view punctuation = ",|(){}-+~";
            if (punctuation.find(character) != std::string_view::npos) {
                result.tokens.push_back({RcTokenKind::punctuation, std::string(1U, character), 0,
                                         line_, column_});
                advance();
                continue;
            }
            std::ostringstream message;
            message << "unexpected character 0x" << std::hex
                    << static_cast<unsigned>(static_cast<unsigned char>(character));
            return failure(message.str(), line_, column_);
        }
        return core::Result<RcTokens>::success(std::move(result));
    }

private:
    char peek(const std::size_t offset) const {
        return position_ + offset < text_.size() ? text_[position_ + offset] : '\0';
    }
    void advance() {
        if (text_[position_] == '\n') {
            ++line_;
            column_ = 1U;
        } else {
            ++column_;
        }
        ++position_;
    }
    core::Result<RcTokens> failure(std::string message, const std::uint32_t line,
                                   const std::uint32_t column) const {
        return core::Result<RcTokens>::failure(make_diagnostic(
            source_, diagnostic_codes::rc_syntax, core::Severity::error, std::move(message), line,
            column));
    }
    core::Result<RcToken> token_failure(std::string message, const std::uint32_t line,
                                        const std::uint32_t column) const {
        return core::Result<RcToken>::failure(make_diagnostic(
            source_, diagnostic_codes::rc_syntax, core::Severity::error, std::move(message), line,
            column));
    }

    RcDirective directive() {
        RcDirective result{{}, line_};
        advance();
        while (position_ < text_.size() && text_[position_] != '\n') {
            if (text_[position_] == '\\' && (peek(1U) == '\n' || (peek(1U) == '\r' && peek(2U) == '\n'))) {
                while (text_[position_] != '\n') advance();
                advance();
                continue;
            }
            if (text_[position_] != '\r') result.text.push_back(text_[position_]);
            advance();
        }
        return result;
    }

    core::Result<RcToken> string() {
        RcToken token{RcTokenKind::string, {}, 0, line_, column_};
        if (text_[position_] == 'L') advance();
        advance();
        while (true) {
            if (position_ >= text_.size() || text_[position_] == '\n') {
                return token_failure("unterminated string", token.line, token.column);
            }
            const char character = text_[position_];
            if (character == '"') {
                if (peek(1U) == '"') {
                    token.text.push_back('"');
                    advance();
                    advance();
                    continue;
                }
                advance();
                return core::Result<RcToken>::success(std::move(token));
            }
            if (character == '\\' && position_ + 1U < text_.size() && text_[position_ + 1U] != '\n') {
                advance();
                const char escaped = text_[position_];
                advance();
                switch (escaped) {
                case 'n': token.text.push_back('\n'); break;
                case 'r': token.text.push_back('\r'); break;
                case 't': token.text.push_back('\t'); break;
                case 'a': token.text.push_back('\a'); break;
                case '0': token.text.push_back('\0'); break;
                case '\\': token.text.push_back('\\'); break;
                case '"': token.text.push_back('"'); break;
                case 'x': {
                    int value = 0;
                    int count = 0;
                    while (count < 2 && position_ < text_.size() && hex_digit(text_[position_]) >= 0) {
                        value = value * 16 + hex_digit(text_[position_]);
                        advance();
                        ++count;
                    }
                    if (count == 0) {
                        token.text.append("\\x");
                    } else {
                        token.text.push_back(static_cast<char>(value));
                    }
                    break;
                }
                default:
                    // Unknown escapes stay literal, backslash included.
                    token.text.push_back('\\');
                    token.text.push_back(escaped);
                    break;
                }
                continue;
            }
            token.text.push_back(character);
            advance();
        }
    }

    core::Result<RcToken> number() {
        RcToken token{RcTokenKind::number, {}, 0, line_, column_};
        while (position_ < text_.size() && identifier_part(text_[position_])) {
            token.text.push_back(text_[position_]);
            advance();
        }
        const auto value = integer_literal(token.text);
        if (!value) {
            return token_failure("invalid integer literal '" + token.text + "'", token.line,
                                 token.column);
        }
        token.value = *value;
        return core::Result<RcToken>::success(std::move(token));
    }

    std::string_view text_;
    const Source& source_;
    std::size_t position_{};
    std::uint32_t line_{1U};
    std::uint32_t column_{1U};
};
} // namespace

core::Result<RcTokens> tokenize_rc(const std::string_view text, const Source& source) {
    return Scanner(text, source).run();
}


} // namespace eawr::data::ui
