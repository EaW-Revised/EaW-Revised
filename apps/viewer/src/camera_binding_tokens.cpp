#include "camera_binding_config_internal.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <initializer_list>
#include <set>
#include <tuple>

namespace eawr::viewer::camera_input {
namespace camera_binding_detail {
namespace {

constexpr int max_depth = 8;

// RFC 8259 text is UTF-8. Rejecting malformed sequences (overlong forms,
// surrogates, code points above U+10FFFF, truncation) keeps every accepted
// string valid when it is echoed into the UTF-8 viewer report.
[[nodiscard]] std::size_t first_invalid_utf8(const std::string_view text) noexcept {
    std::size_t index = 0;
    while (index < text.size()) {
        const auto lead = static_cast<unsigned char>(text[index]);
        std::size_t length{};
        std::uint32_t code{};
        if (lead < 0x80U) {
            ++index;
            continue;
        }
        if (lead >= 0xC2U && lead <= 0xDFU) {
            length = 2;
            code = lead & 0x1FU;
        } else if (lead >= 0xE0U && lead <= 0xEFU) {
            length = 3;
            code = lead & 0x0FU;
        } else if (lead >= 0xF0U && lead <= 0xF4U) {
            length = 4;
            code = lead & 0x07U;
        } else {
            return index;
        }
        if (index + length > text.size()) return index;
        for (std::size_t offset = 1; offset < length; ++offset) {
            const auto next = static_cast<unsigned char>(text[index + offset]);
            if ((next & 0xC0U) != 0x80U) return index;
            code = (code << 6U) | (next & 0x3FU);
        }
        if ((length == 3 && code < 0x800U) || (length == 4 && code < 0x10000U)
            || (code >= 0xD800U && code <= 0xDFFFU) || code > 0x10FFFFU) {
            return index;
        }
        index += length;
    }
    return text.size();
}

} // namespace

const JsonValue* JsonValue::find(const std::string_view key) const noexcept {
    for (const auto& [name, value] : members) {
        if (name == key) return &value;
    }
    return nullptr;
}

[[nodiscard]] bool JsonReader::read_document(JsonValue& out) {
        if (const std::size_t invalid = first_invalid_utf8(text_); invalid != text_.size()) {
            position_ = invalid;
            return fail("document is not well-formed UTF-8");
        }
        if (text_.size() >= 3 && static_cast<unsigned char>(text_[0]) == 0xEFU
            && static_cast<unsigned char>(text_[1]) == 0xBBU
            && static_cast<unsigned char>(text_[2]) == 0xBFU) {
            position_ = 3;
        }
        skip_space();
        if (!read_value(out, 0)) return false;
        skip_space();
        if (position_ != text_.size()) return fail("trailing data after the JSON document");
        return true;
    }

[[nodiscard]] bool JsonReader::read_value(JsonValue& out, const int depth) {
        if (depth > max_depth) return fail("nesting is too deep");
        if (position_ >= text_.size()) return fail("unexpected end of document");
        const char value = text_[position_];
        switch (value) {
        case '{': return read_object(out, depth);
        case '[': return read_array(out, depth);
        case '"':
            out.kind = JsonValue::Kind::string;
            return read_string(out.text);
        case 't':
            out.kind = JsonValue::Kind::boolean;
            out.boolean = true;
            return literal("true");
        case 'f':
            out.kind = JsonValue::Kind::boolean;
            out.boolean = false;
            return literal("false");
        case 'n':
            out.kind = JsonValue::Kind::null;
            return literal("null");
        default:
            if (value == '-' || (value >= '0' && value <= '9')) return read_number(out);
            return fail("unexpected character");
        }
    }

[[nodiscard]] bool JsonReader::read_object(JsonValue& out, const int depth) {
        out.kind = JsonValue::Kind::object;
        ++position_;
        skip_space();
        if (position_ < text_.size() && text_[position_] == '}') {
            ++position_;
            return true;
        }
        while (true) {
            skip_space();
            if (position_ >= text_.size() || text_[position_] != '"') {
                return fail("object key must be a string");
            }
            std::string key;
            if (!read_string(key)) return false;
            if (out.find(key) != nullptr) return fail("duplicate object key \"" + key + "\"");
            skip_space();
            if (position_ >= text_.size() || text_[position_] != ':') return fail("expected ':'");
            ++position_;
            skip_space();
            JsonValue member;
            if (!read_value(member, depth + 1)) return false;
            out.members.push_back(JsonMember{std::move(key), std::move(member)});
            skip_space();
            if (position_ >= text_.size()) return fail("unterminated object");
            if (text_[position_] == ',') {
                ++position_;
                continue;
            }
            if (text_[position_] == '}') {
                ++position_;
                return true;
            }
            return fail("expected ',' or '}'");
        }
    }

[[nodiscard]] bool JsonReader::read_array(JsonValue& out, const int depth) {
        out.kind = JsonValue::Kind::array;
        ++position_;
        skip_space();
        if (position_ < text_.size() && text_[position_] == ']') {
            ++position_;
            return true;
        }
        while (true) {
            skip_space();
            JsonValue item;
            if (!read_value(item, depth + 1)) return false;
            out.items.push_back(std::move(item));
            skip_space();
            if (position_ >= text_.size()) return fail("unterminated array");
            if (text_[position_] == ',') {
                ++position_;
                continue;
            }
            if (text_[position_] == ']') {
                ++position_;
                return true;
            }
            return fail("expected ',' or ']'");
        }
    }

[[nodiscard]] bool JsonReader::read_string(std::string& out) {
        ++position_;
        while (position_ < text_.size()) {
            const char value = text_[position_++];
            if (value == '"') return true;
            if (static_cast<unsigned char>(value) < 0x20U) {
                return fail("control character inside a string");
            }
            if (value != '\\') {
                out.push_back(value);
                continue;
            }
            if (position_ >= text_.size()) break;
            const char escape = text_[position_++];
            switch (escape) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                // An escape decodes to the same UTF-8 bytes as its raw
                // spelling, so equal keys compare equal. A surrogate is valid
                // only as a high/low pair; a lone half has no UTF-8 encoding.
                std::uint32_t code{};
                if (!read_hex4(code)) return false;
                if (code >= 0xDC00U && code <= 0xDFFFU) {
                    return fail("unpaired low surrogate in \\u escape");
                }
                if (code >= 0xD800U && code <= 0xDBFFU) {
                    if (text_.substr(position_, 2) != "\\u") {
                        return fail("unpaired high surrogate in \\u escape");
                    }
                    position_ += 2;
                    std::uint32_t low{};
                    if (!read_hex4(low)) return false;
                    if (low < 0xDC00U || low > 0xDFFFU) {
                        return fail("unpaired high surrogate in \\u escape");
                    }
                    code = 0x10000U + ((code - 0xD800U) << 10U) + (low - 0xDC00U);
                }
                append_utf8(out, code);
                break;
            }
            default: return fail("invalid escape");
            }
        }
        return fail("unterminated string");
    }

[[nodiscard]] bool JsonReader::read_number(JsonValue& out) {
        const std::size_t start = position_;
        bool integral = true;
        if (text_[position_] == '-') ++position_;
        const auto digits = [this]() {
            const std::size_t first = position_;
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
                ++position_;
            }
            return position_ - first;
        };
        if (position_ >= text_.size()) return fail("truncated number");
        if (text_[position_] == '0') {
            ++position_;
        } else if (digits() == 0) {
            return fail("invalid number");
        }
        if (position_ < text_.size() && text_[position_] == '.') {
            integral = false;
            ++position_;
            if (digits() == 0) return fail("invalid number fraction");
        }
        if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
            integral = false;
            ++position_;
            if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) {
                ++position_;
            }
            if (digits() == 0) return fail("invalid number exponent");
        }
        double parsed{};
        const auto* begin = text_.data() + start;
        const auto* end = text_.data() + position_;
        const auto result = std::from_chars(begin, end, parsed);
        if (result.ec != std::errc{} || result.ptr != end || !std::isfinite(parsed)) {
            return fail("number is out of range");
        }
        out.kind = JsonValue::Kind::number;
        out.number = parsed;
        out.integral = integral;
        return true;
    }

} // namespace camera_binding_detail

} // namespace eawr::viewer::camera_input
