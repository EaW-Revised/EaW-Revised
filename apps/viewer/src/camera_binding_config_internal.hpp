#pragma once

// Private scanner and typed-value seam for the viewer binding loader.
#include "camera_input_internal.hpp"

#include <string>
#include <vector>

namespace eawr::viewer::camera_input {
namespace camera_binding_detail {

// --- Strict JSON subset reader ------------------------------------------------
//
// The viewer has no JSON dependency, and the binding file is small, so a strict
// RFC 8259 reader lives here. Duplicate object keys are rejected because a
// binding table whose meaning depends on which duplicate wins is ambiguous.

struct JsonMember;

struct JsonValue final {
    enum class Kind : std::uint8_t { null, boolean, number, string, array, object };
    Kind kind{Kind::null};
    bool boolean{};
    double number{};
    bool integral{};
    std::string text;
    std::vector<JsonValue> items;
    std::vector<JsonMember> members;

    [[nodiscard]] const JsonValue* find(std::string_view key) const noexcept;
};

struct JsonMember final {
    std::string name;
    JsonValue value;
};

class JsonReader final {
public:
    explicit JsonReader(const std::string_view text) : text_(text) {}

    [[nodiscard]] bool read_document(JsonValue& out);

    [[nodiscard]] const std::string& error() const noexcept { return error_; }

private:
    [[nodiscard]] bool fail(const std::string_view message) {
        if (error_.empty()) {
            error_ = "JSON byte " + std::to_string(position_) + ": " + std::string(message);
        }
        return false;
    }

    void skip_space() noexcept {
        while (position_ < text_.size()) {
            const char value = text_[position_];
            if (value != ' ' && value != '\t' && value != '\n' && value != '\r') break;
            ++position_;
        }
    }

    [[nodiscard]] bool literal(const std::string_view word) {
        if (text_.substr(position_, word.size()) != word) return fail("invalid literal");
        position_ += word.size();
        return true;
    }

    [[nodiscard]] bool read_value(JsonValue& out, const int depth);

    [[nodiscard]] bool read_object(JsonValue& out, const int depth);

    [[nodiscard]] bool read_array(JsonValue& out, const int depth);

    [[nodiscard]] bool read_string(std::string& out);

    // Exactly four hex digits; std::from_chars would also accept fewer.
    [[nodiscard]] bool read_hex4(std::uint32_t& code) {
        if (position_ + 4 > text_.size()) return fail("truncated \\u escape");
        code = 0;
        for (int digit = 0; digit < 4; ++digit) {
            const char value = text_[position_];
            std::uint32_t nibble{};
            if (value >= '0' && value <= '9') {
                nibble = static_cast<std::uint32_t>(value - '0');
            } else if (value >= 'a' && value <= 'f') {
                nibble = static_cast<std::uint32_t>(value - 'a' + 10);
            } else if (value >= 'A' && value <= 'F') {
                nibble = static_cast<std::uint32_t>(value - 'A' + 10);
            } else {
                return fail("invalid \\u escape");
            }
            code = (code << 4U) | nibble;
            ++position_;
        }
        return true;
    }

    // `code` is a Unicode scalar value: never a surrogate, at most U+10FFFF.
    static void append_utf8(std::string& out, const std::uint32_t code) {
        if (code < 0x80U) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800U) {
            out.push_back(static_cast<char>(0xC0U | (code >> 6U)));
            out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
        } else if (code < 0x10000U) {
            out.push_back(static_cast<char>(0xE0U | (code >> 12U)));
            out.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
        } else {
            out.push_back(static_cast<char>(0xF0U | (code >> 18U)));
            out.push_back(static_cast<char>(0x80U | ((code >> 12U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
        }
    }

    [[nodiscard]] bool read_number(JsonValue& out);

    std::string_view text_;
    std::size_t position_{};
    std::string error_;
};


[[nodiscard]] bool context_allows(const Action action, const Context context) noexcept;
[[nodiscard]] bool compatible(const Action action, const Trigger trigger, const Device device);
[[nodiscard]] std::optional<Action> parse_action(
    const std::string_view name, const std::int64_t version);
[[nodiscard]] std::optional<Context> parse_context(
    const std::string_view name, const std::int64_t version);
[[nodiscard]] std::optional<Device> parse_device(const std::string_view name);
[[nodiscard]] std::optional<Trigger> parse_trigger(const std::string_view name);
[[nodiscard]] std::optional<std::uint8_t> parse_modifier(const std::string_view name);
[[nodiscard]] std::optional<std::uint32_t> parse_control(
    const Device device, const std::string_view name);
[[nodiscard]] bool valid_id(const std::string_view id) noexcept;

} // namespace camera_binding_detail

using namespace camera_binding_detail;

} // namespace eawr::viewer::camera_input
