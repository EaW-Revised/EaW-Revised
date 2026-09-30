#include "camera_input_internal.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <initializer_list>
#include <set>
#include <tuple>

namespace eawr::viewer::camera_input {
namespace {

[[nodiscard]] char fold(const char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

[[nodiscard]] bool ieq(const std::string_view left, const std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (fold(left[index]) != fold(right[index])) return false;
    }
    return true;
}

// --- Strict JSON subset reader ------------------------------------------------
//
// The viewer has no JSON dependency, and the binding file is small, so a strict
// RFC 8259 reader lives here. Duplicate object keys are rejected because a
// binding table whose meaning depends on which duplicate wins is ambiguous.

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

const JsonValue* JsonValue::find(const std::string_view key) const noexcept {
    for (const auto& [name, value] : members) {
        if (name == key) return &value;
    }
    return nullptr;
}

class JsonReader final {
public:
    explicit JsonReader(const std::string_view text) : text_(text) {}

    [[nodiscard]] bool read_document(JsonValue& out) {
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

    [[nodiscard]] bool read_value(JsonValue& out, const int depth) {
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

    [[nodiscard]] bool read_object(JsonValue& out, const int depth) {
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

    [[nodiscard]] bool read_array(JsonValue& out, const int depth) {
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

    [[nodiscard]] bool read_string(std::string& out) {
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

    [[nodiscard]] bool read_number(JsonValue& out) {
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

    std::string_view text_;
    std::size_t position_{};
    std::string error_;
};

// --- Vocabulary ---------------------------------------------------------------

struct Named final {
    std::string_view name;
    std::uint32_t value;
};

constexpr std::array<Named, 24> action_names{{
    {"pan_left", static_cast<std::uint32_t>(Action::pan_left)},
    {"pan_right", static_cast<std::uint32_t>(Action::pan_right)},
    {"pan_forward", static_cast<std::uint32_t>(Action::pan_forward)},
    {"pan_back", static_cast<std::uint32_t>(Action::pan_back)},
    {"push_scroll", static_cast<std::uint32_t>(Action::push_scroll)},
    {"zoom", static_cast<std::uint32_t>(Action::zoom)},
    {"rotate_grab", static_cast<std::uint32_t>(Action::rotate_grab)},
    {"rotate", static_cast<std::uint32_t>(Action::rotate)},
    {"reset_view", static_cast<std::uint32_t>(Action::reset_view)},
    {"free_toggle", static_cast<std::uint32_t>(Action::free_toggle)},
    {"free_move_left", static_cast<std::uint32_t>(Action::free_move_left)},
    {"free_move_right", static_cast<std::uint32_t>(Action::free_move_right)},
    {"free_move_forward", static_cast<std::uint32_t>(Action::free_move_forward)},
    {"free_move_back", static_cast<std::uint32_t>(Action::free_move_back)},
    {"free_rise", static_cast<std::uint32_t>(Action::free_rise)},
    {"free_descend", static_cast<std::uint32_t>(Action::free_descend)},
    {"free_look_grab", static_cast<std::uint32_t>(Action::free_look_grab)},
    {"free_look_yaw", static_cast<std::uint32_t>(Action::free_look_yaw)},
    {"free_look_pitch", static_cast<std::uint32_t>(Action::free_look_pitch)},
    {"pan_motion_x", static_cast<std::uint32_t>(Action::pan_motion_x)},
    {"pan_motion_y", static_cast<std::uint32_t>(Action::pan_motion_y)},
    {"orbit_pitch", static_cast<std::uint32_t>(Action::orbit_pitch)},
    {"translate_x", static_cast<std::uint32_t>(Action::translate_x)},
    {"translate_y", static_cast<std::uint32_t>(Action::translate_y)},
}};
// Existing v1 IDs stay stable; the tactical pointer IDs above the v2 range
// (Alt pan, orbit pitch and the grab translate) are also accepted by v1.
constexpr std::uint32_t first_v2_action = static_cast<std::uint32_t>(Action::free_toggle);

[[nodiscard]] bool tactical_pointer_action(const Action action) noexcept {
    return action == Action::pan_motion_x || action == Action::pan_motion_y
        || action == Action::orbit_pitch || action == Action::translate_x
        || action == Action::translate_y;
}

// Named keys spelled as Godot's OS::get_keycode_string reports them.
constexpr std::array<std::string_view, 12> named_keys{{
    "Left", "Right", "Up", "Down", "Space", "Home", "End", "PageUp", "PageDown",
    "Insert", "Delete", "Tab",
}};
// Codes: 1..26 letters, 27..36 digits, 37.. named keys.
constexpr std::uint32_t first_digit_code = 27;
constexpr std::uint32_t first_named_code = 37;

struct Compatibility final {
    Action action;
    Trigger trigger;
    Device device;
};

// The complete action/trigger/device matrix. Anything absent is rejected. The
// Existing v1 rows stay compatible; the pointer-pan and orbit rows are
// tactical in either version, and the remaining rows describe v2 free actions.
constexpr std::array<Compatibility, 41> compatibility{{
    {Action::pan_left, Trigger::held, Device::keyboard},
    {Action::pan_left, Trigger::held, Device::mouse_button},
    {Action::pan_right, Trigger::held, Device::keyboard},
    {Action::pan_right, Trigger::held, Device::mouse_button},
    {Action::pan_forward, Trigger::held, Device::keyboard},
    {Action::pan_forward, Trigger::held, Device::mouse_button},
    {Action::pan_back, Trigger::held, Device::keyboard},
    {Action::pan_back, Trigger::held, Device::mouse_button},
    {Action::push_scroll, Trigger::held, Device::keyboard},
    {Action::push_scroll, Trigger::held, Device::mouse_button},
    {Action::zoom, Trigger::delta, Device::mouse_wheel},
    {Action::zoom, Trigger::pressed, Device::keyboard},
    {Action::rotate_grab, Trigger::held, Device::keyboard},
    {Action::rotate_grab, Trigger::held, Device::mouse_button},
    {Action::rotate, Trigger::delta, Device::mouse_motion},
    {Action::pan_motion_x, Trigger::delta, Device::mouse_motion},
    {Action::pan_motion_y, Trigger::delta, Device::mouse_motion},
    {Action::orbit_pitch, Trigger::delta, Device::mouse_motion},
    {Action::translate_x, Trigger::delta, Device::mouse_motion},
    {Action::translate_y, Trigger::delta, Device::mouse_motion},
    {Action::reset_view, Trigger::pressed, Device::keyboard},
    {Action::reset_view, Trigger::pressed, Device::mouse_button},
    {Action::zoom, Trigger::pressed, Device::mouse_button},
    {Action::free_toggle, Trigger::pressed, Device::keyboard},
    {Action::free_toggle, Trigger::pressed, Device::mouse_button},
    {Action::free_move_left, Trigger::held, Device::keyboard},
    {Action::free_move_left, Trigger::held, Device::mouse_button},
    {Action::free_move_right, Trigger::held, Device::keyboard},
    {Action::free_move_right, Trigger::held, Device::mouse_button},
    {Action::free_move_forward, Trigger::held, Device::keyboard},
    {Action::free_move_forward, Trigger::held, Device::mouse_button},
    {Action::free_move_back, Trigger::held, Device::keyboard},
    {Action::free_move_back, Trigger::held, Device::mouse_button},
    {Action::free_rise, Trigger::held, Device::keyboard},
    {Action::free_rise, Trigger::held, Device::mouse_button},
    {Action::free_descend, Trigger::held, Device::keyboard},
    {Action::free_descend, Trigger::held, Device::mouse_button},
    {Action::free_look_grab, Trigger::held, Device::keyboard},
    {Action::free_look_grab, Trigger::held, Device::mouse_button},
    {Action::free_look_yaw, Trigger::delta, Device::mouse_motion},
    {Action::free_look_pitch, Trigger::delta, Device::mouse_motion},
}};

// The toggle is the only action valid in every context; tactical actions
// belong to land/space and free-flight actions to `free`.
[[nodiscard]] bool context_allows(const Action action, const Context context) noexcept {
    if (action == Action::free_toggle) return true;
    if (tactical_pointer_action(action)) return context != Context::free;
    const bool free_action = static_cast<std::uint32_t>(action) >= first_v2_action;
    return free_action == (context == Context::free);
}

[[nodiscard]] bool compatible(const Action action, const Trigger trigger, const Device device) {
    return std::any_of(compatibility.begin(), compatibility.end(), [&](const Compatibility& row) {
        return row.action == action && row.trigger == trigger && row.device == device;
    });
}

[[nodiscard]] std::optional<Action> parse_action(
    const std::string_view name, const std::int64_t version) {
    for (const Named& entry : action_names) {
        if (entry.name != name) continue;
        if (version == bindings_version && entry.value >= first_v2_action
            && !tactical_pointer_action(static_cast<Action>(entry.value))) return std::nullopt;
        return static_cast<Action>(entry.value);
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<Context> parse_context(
    const std::string_view name, const std::int64_t version) {
    if (name == "land") return Context::land;
    if (name == "space") return Context::space;
    if (name == "free" && version == bindings_version_v2) return Context::free;
    return std::nullopt;
}

[[nodiscard]] std::optional<Device> parse_device(const std::string_view name) {
    if (name == "keyboard") return Device::keyboard;
    if (name == "mouse_button") return Device::mouse_button;
    if (name == "mouse_wheel") return Device::mouse_wheel;
    if (name == "mouse_motion") return Device::mouse_motion;
    return std::nullopt;
}

[[nodiscard]] std::optional<Trigger> parse_trigger(const std::string_view name) {
    if (name == "held") return Trigger::held;
    if (name == "pressed") return Trigger::pressed;
    if (name == "delta") return Trigger::delta;
    return std::nullopt;
}

[[nodiscard]] std::optional<std::uint8_t> parse_modifier(const std::string_view name) {
    if (name == "shift") return modifier::shift;
    if (name == "ctrl") return modifier::ctrl;
    if (name == "alt") return modifier::alt;
    if (name == "meta") return modifier::meta;
    return std::nullopt;
}

[[nodiscard]] std::optional<std::uint32_t> parse_control(
    const Device device, const std::string_view name) {
    switch (device) {
    case Device::keyboard:
        return key_code(name);
    case Device::mouse_button:
        if (name == "left") return mouse_code::left;
        if (name == "right") return mouse_code::right;
        if (name == "middle") return mouse_code::middle;
        return std::nullopt;
    case Device::mouse_wheel:
        if (name == "up") return mouse_code::wheel_up;
        if (name == "down") return mouse_code::wheel_down;
        return std::nullopt;
    case Device::mouse_motion:
        if (name == "x") return mouse_code::motion_x;
        if (name == "y") return mouse_code::motion_y;
        return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] bool valid_id(const std::string_view id) noexcept {
    if (id.empty() || id.size() > 64) return false;
    for (std::size_t index = 0; index < id.size(); ++index) {
        const char value = id[index];
        const bool alnum = (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9');
        if (alnum) continue;
        if (index != 0 && (value == '.' || value == '_' || value == '-')) continue;
        return false;
    }
    return true;
}

[[nodiscard]] core::Result<BindingTable> reject(
    const std::string_view code, std::string message) {
    return core::Result<BindingTable>::failure(failure(code, std::move(message)));
}

[[nodiscard]] bool only_keys(const JsonValue& object,
                             const std::initializer_list<std::string_view> allowed,
                             std::string& unknown) {
    for (const auto& [name, value] : object.members) {
        static_cast<void>(value);
        if (std::find(allowed.begin(), allowed.end(), name) == allowed.end()) {
            unknown = name;
            return false;
        }
    }
    return true;
}

// The v2 free_camera block: exactly the five project-authored settings, each a
// JSON number, then the controller's own validation. No field is defaulted.
[[nodiscard]] core::Result<camera::FreeCameraSettings> parse_free_camera(
    const JsonValue* block) {
    using Parsed = core::Result<camera::FreeCameraSettings>;
    if (!block || block->kind != JsonValue::Kind::object) {
        return Parsed::failure(failure(diagnostic_codes::invalid_bindings,
            "schema v2 requires a free_camera settings object"));
    }
    std::string unknown;
    if (!only_keys(*block, {"move_speed", "vertical_speed", "look_degrees_per_unit",
                            "pitch_min_degrees", "pitch_max_degrees"}, unknown)) {
        return Parsed::failure(failure(diagnostic_codes::invalid_bindings,
            "free_camera has unknown key \"" + unknown + "\""));
    }
    camera::FreeCameraSettings settings;
    const std::array<std::pair<std::string_view, float*>, 5> fields{{
        {"move_speed", &settings.move_speed},
        {"vertical_speed", &settings.vertical_speed},
        {"look_degrees_per_unit", &settings.look_degrees_per_unit},
        {"pitch_min_degrees", &settings.pitch_min_degrees},
        {"pitch_max_degrees", &settings.pitch_max_degrees},
    }};
    for (const auto& [name, target] : fields) {
        const JsonValue* value = block->find(name);
        if (!value || value->kind != JsonValue::Kind::number) {
            return Parsed::failure(failure(diagnostic_codes::invalid_bindings,
                "free_camera." + std::string(name) + " must be a number"));
        }
        *target = static_cast<float>(value->number);
    }
    if (auto valid = camera::validate(settings); !valid) {
        return Parsed::failure(failure(diagnostic_codes::invalid_bindings,
            "free_camera settings rejected: " + valid.error().code + ": "
                + valid.error().message));
    }
    return Parsed::success(settings);
}

} // namespace

std::string_view to_string(const Action action) noexcept {
    for (const Named& entry : action_names) {
        if (entry.value == static_cast<std::uint32_t>(action)) return entry.name;
    }
    return "invalid";
}

std::span<const std::string_view> rate_source_tags(const Action action) noexcept {
    // Pan speed interpolates the two scroll speeds over normalised distance;
    // push scrolling multiplies it; a wheel detent is Distance_Per_Mouse_Unit
    // world units, converted to a zoom step over the Distance_Max -
    // Distance_Min span; a rotate unit is Yaw_Per_Mouse_Unit degrees and an
    // orbit pitch unit Pitch_Per_Mouse_Unit degrees (a unit is 1/100 of the
    // screen, the FoC mouse unit). A pointer drag of one viewport height moves
    // the frustum height at the live distance, which follows the distance
    // range and Fov_Default. A translate unit moves 1/100 of the live
    // distance, which follows the distance range.
    static constexpr std::array<std::string_view, 2> pan{
        "Tactical_Min_Scroll_Speed", "Tactical_Max_Scroll_Speed"};
    static constexpr std::array<std::string_view, 3> drag{
        "Distance_Min", "Distance_Max", "Fov_Default"};
    static constexpr std::array<std::string_view, 1> push{"Push_Scroll_Speed_Modifier"};
    static constexpr std::array<std::string_view, 3> zoom{
        "Distance_Per_Mouse_Unit", "Distance_Min", "Distance_Max"};
    static constexpr std::array<std::string_view, 1> rotate{"Yaw_Per_Mouse_Unit"};
    static constexpr std::array<std::string_view, 1> tilt{"Pitch_Per_Mouse_Unit"};
    static constexpr std::array<std::string_view, 2> translate{"Distance_Min", "Distance_Max"};
    switch (action) {
    case Action::pan_left:
    case Action::pan_right:
    case Action::pan_forward:
    case Action::pan_back: return pan;
    case Action::pan_motion_x:
    case Action::pan_motion_y: return drag;
    case Action::push_scroll: return push;
    case Action::zoom: return zoom;
    case Action::rotate: return rotate;
    case Action::orbit_pitch: return tilt;
    case Action::translate_x:
    case Action::translate_y: return translate;
    default: return {};
    }
}

std::span<const std::string_view> edge_scroll_rate_tags() noexcept {
    static constexpr std::array<std::string_view, 4> tags{
        "Tactical_Edge_Scroll_Region", "Tactical_Offscreen_Scroll_Region",
        "Tactical_Min_Scroll_Speed", "Tactical_Max_Scroll_Speed"};
    return tags;
}

std::string_view to_string(const Context context) noexcept {
    switch (context) {
    case Context::land: return "land";
    case Context::space: return "space";
    case Context::free: return "free";
    }
    return "invalid";
}

std::string_view to_string(const Device device) noexcept {
    switch (device) {
    case Device::keyboard: return "keyboard";
    case Device::mouse_button: return "mouse_button";
    case Device::mouse_wheel: return "mouse_wheel";
    case Device::mouse_motion: return "mouse_motion";
    }
    return "invalid";
}

std::string_view to_string(const Trigger trigger) noexcept {
    switch (trigger) {
    case Trigger::held: return "held";
    case Trigger::pressed: return "pressed";
    case Trigger::delta: return "delta";
    }
    return "invalid";
}

std::optional<std::uint32_t> key_code(const std::string_view name) noexcept {
    if (name.size() == 1) {
        const char value = fold(name[0]);
        if (value >= 'a' && value <= 'z') return static_cast<std::uint32_t>(value - 'a' + 1);
        if (value >= '0' && value <= '9') {
            return first_digit_code + static_cast<std::uint32_t>(value - '0');
        }
        return std::nullopt;
    }
    for (std::size_t index = 0; index < named_keys.size(); ++index) {
        if (ieq(named_keys[index], name)) {
            return first_named_code + static_cast<std::uint32_t>(index);
        }
    }
    return std::nullopt;
}

std::string_view key_name(const std::uint32_t code) noexcept {
    static constexpr std::string_view letters = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static constexpr std::string_view digits = "0123456789";
    if (code >= 1 && code < first_digit_code) return letters.substr(code - 1, 1);
    if (code >= first_digit_code && code < first_named_code) {
        return digits.substr(code - first_digit_code, 1);
    }
    if (code >= first_named_code && code - first_named_code < named_keys.size()) {
        return named_keys[code - first_named_code];
    }
    return {};
}

std::string json_string_literal(const std::string_view value) {
    constexpr std::string_view hex = "0123456789abcdef";
    std::string output;
    output.reserve(value.size() + 2U);
    output.push_back('"');
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        switch (character) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (byte < 0x20U) {
                output += "\\u00";
                output.push_back(hex[byte >> 4U]);
                output.push_back(hex[byte & 0x0FU]);
            } else {
                output.push_back(character);
            }
            break;
        }
    }
    output.push_back('"');
    return output;
}

std::optional<Context> context_for(const camera::Mode mode) noexcept {
    switch (mode) {
    case camera::Mode::land: return Context::land;
    case camera::Mode::space: return Context::space;
    case camera::Mode::unlocked: return std::nullopt;
    }
    return std::nullopt;
}

core::Result<BindingTable> parse_binding_table(const std::string_view json) {
    if (json.size() > max_binding_document_bytes) {
        return reject(diagnostic_codes::invalid_bindings, "binding document exceeds 1 MiB");
    }
    JsonValue document;
    JsonReader reader(json);
    if (!reader.read_document(document)) {
        return reject(diagnostic_codes::invalid_bindings, reader.error());
    }
    if (document.kind != JsonValue::Kind::object) {
        return reject(diagnostic_codes::invalid_bindings, "binding document must be an object");
    }

    // Schema identity and version first, so a future file gets a version
    // diagnostic instead of a misleading field complaint.
    const JsonValue* schema = document.find("schema");
    if (!schema || schema->kind != JsonValue::Kind::string || schema->text != bindings_schema) {
        return reject(diagnostic_codes::unsupported_bindings_version,
            "binding document schema must be \"" + std::string(bindings_schema) + "\"");
    }
    const JsonValue* version = document.find("version");
    const bool is_v1 = version && version->kind == JsonValue::Kind::number && version->integral
        && version->number == static_cast<double>(bindings_version);
    const bool is_v2 = version && version->kind == JsonValue::Kind::number && version->integral
        && version->number == static_cast<double>(bindings_version_v2);
    if (!is_v1 && !is_v2) {
        return reject(diagnostic_codes::unsupported_bindings_version,
            "binding document version must be the integer " + std::to_string(bindings_version)
                + " or " + std::to_string(bindings_version_v2));
    }
    const std::int64_t document_version = is_v1 ? bindings_version : bindings_version_v2;
    std::string unknown;
    const bool known_keys = is_v1
        ? only_keys(document, {"schema", "version", "provenance", "notice", "edge_scroll",
                               "pan_speed_scale", "click_reset", "screen_mouse_units", "bindings"},
                    unknown)
        : only_keys(document, {"schema", "version", "provenance", "notice", "edge_scroll",
                               "pan_speed_scale", "click_reset", "screen_mouse_units", "free_camera",
                               "bindings"},
                    unknown);
    if (!known_keys) {
        return reject(diagnostic_codes::invalid_bindings,
            "unknown top-level key \"" + unknown + "\"");
    }

    BindingTable table;
    table.version = document_version;
    const JsonValue* provenance = document.find("provenance");
    if (!provenance || provenance->kind != JsonValue::Kind::string
        || provenance->text != project_authored_provenance) {
        return reject(diagnostic_codes::invalid_bindings,
            "provenance must be \"project-authored\"; schema v"
                + std::to_string(document_version) + " cannot carry retail evidence");
    }
    table.provenance = provenance->text;
    const JsonValue* notice = document.find("notice");
    if (!notice || notice->kind != JsonValue::Kind::string || notice->text.empty()) {
        return reject(diagnostic_codes::invalid_bindings, "notice must be a nonempty string");
    }
    table.notice = notice->text;
    const JsonValue* edge = document.find("edge_scroll");
    if (!edge || edge->kind != JsonValue::Kind::boolean) {
        return reject(diagnostic_codes::invalid_bindings, "edge_scroll must be a boolean");
    }
    table.edge_scroll = edge->boolean;
    if (const JsonValue* pan_scale = document.find("pan_speed_scale")) {
        // A project gain on the XML pan speed; zero or negative would stop or
        // reverse panning, so only a finite positive number is accepted.
        const float value = pan_scale->kind == JsonValue::Kind::number
            ? static_cast<float>(pan_scale->number) : 0.0F;
        if (!finite(value) || !(value > 0.0F)) {
            return reject(diagnostic_codes::invalid_bindings,
                "pan_speed_scale must be a finite positive number");
        }
        table.pan_speed_scale = value;
    }
    if (const JsonValue* click = document.find("click_reset")) {
        if (click->kind != JsonValue::Kind::boolean) {
            return reject(diagnostic_codes::invalid_bindings, "click_reset must be a boolean");
        }
        table.click_reset = click->boolean;
    }
    if (const JsonValue* units = document.find("screen_mouse_units")) {
        if (units->kind != JsonValue::Kind::boolean) {
            return reject(diagnostic_codes::invalid_bindings, "screen_mouse_units must be a boolean");
        }
        table.screen_mouse_units = units->boolean;
    }
    if (is_v2) {
        auto settings = parse_free_camera(document.find("free_camera"));
        if (!settings) return core::Result<BindingTable>::failure(settings.error());
        table.free_camera = settings.value();
    }
    const JsonValue* bindings = document.find("bindings");
    if (!bindings || bindings->kind != JsonValue::Kind::array || bindings->items.empty()) {
        return reject(diagnostic_codes::invalid_bindings, "bindings must be a nonempty array");
    }

    std::set<std::string> ids;
    using Chord = std::tuple<Context, Device, std::uint32_t, std::uint8_t>;
    std::map<Chord, std::string> chords;
    for (std::size_t index = 0; index < bindings->items.size(); ++index) {
        const JsonValue& entry = bindings->items[index];
        std::string where = "bindings[" + std::to_string(index) + "]";
        if (entry.kind != JsonValue::Kind::object) {
            return reject(diagnostic_codes::invalid_bindings, where + " must be an object");
        }
        if (!only_keys(entry, {"id", "action", "context", "device", "control", "modifiers",
                               "trigger", "scale"}, unknown)) {
            return reject(diagnostic_codes::invalid_bindings,
                where + " has unknown key \"" + unknown + "\"");
        }
        const auto text_field = [&](const std::string_view key) -> const std::string* {
            const JsonValue* value = entry.find(key);
            if (!value || value->kind != JsonValue::Kind::string) return nullptr;
            return &value->text;
        };
        const std::string* id = text_field("id");
        if (!id || !valid_id(*id)) {
            return reject(diagnostic_codes::invalid_bindings,
                where + ".id must match [a-z0-9][a-z0-9._-]{0,63}");
        }
        where += " (" + *id + ")";
        if (!ids.insert(*id).second) {
            return reject(diagnostic_codes::invalid_bindings, where + " duplicates an id");
        }

        Binding binding;
        binding.id = *id;
        const std::string* action = text_field("action");
        const auto parsed_action =
            action ? parse_action(*action, document_version) : std::nullopt;
        if (!parsed_action) {
            return reject(diagnostic_codes::invalid_bindings,
                where + ".action is unknown in schema v" + std::to_string(document_version));
        }
        binding.action = *parsed_action;
        const std::string* context = text_field("context");
        const auto parsed_context =
            context ? parse_context(*context, document_version) : std::nullopt;
        if (!parsed_context) {
            return reject(diagnostic_codes::invalid_bindings, is_v1
                ? where + ".context must be land or space in schema v1"
                : where + ".context must be land, space or free in schema v2");
        }
        binding.context = *parsed_context;
        if (!context_allows(binding.action, binding.context)) {
            return reject(diagnostic_codes::incompatible_binding,
                where + ": action " + std::string(to_string(binding.action))
                    + " cannot be bound in context " + std::string(to_string(binding.context)));
        }
        const std::string* device = text_field("device");
        const auto parsed_device = device ? parse_device(*device) : std::nullopt;
        if (!parsed_device) {
            return reject(diagnostic_codes::invalid_bindings, where + ".device is unknown");
        }
        binding.device = *parsed_device;
        const std::string* control = text_field("control");
        const auto parsed_control =
            control ? parse_control(binding.device, *control) : std::nullopt;
        if (!parsed_control) {
            return reject(diagnostic_codes::invalid_bindings,
                where + ".control is not a supported " + std::string(to_string(binding.device))
                    + " control");
        }
        binding.code = *parsed_control;
        const std::string* trigger = text_field("trigger");
        const auto parsed_trigger = trigger ? parse_trigger(*trigger) : std::nullopt;
        if (!parsed_trigger) {
            return reject(diagnostic_codes::invalid_bindings, where + ".trigger is unknown");
        }
        binding.trigger = *parsed_trigger;
        if (!compatible(binding.action, binding.trigger, binding.device)) {
            return reject(diagnostic_codes::incompatible_binding,
                where + ": action " + std::string(to_string(binding.action))
                    + " cannot use trigger " + std::string(to_string(binding.trigger))
                    + " on device " + std::string(to_string(binding.device)));
        }

        if (const JsonValue* modifiers = entry.find("modifiers")) {
            if (modifiers->kind != JsonValue::Kind::array) {
                return reject(diagnostic_codes::invalid_bindings,
                    where + ".modifiers must be an array");
            }
            for (const JsonValue& name : modifiers->items) {
                const auto bit = name.kind == JsonValue::Kind::string
                    ? parse_modifier(name.text) : std::nullopt;
                if (!bit) {
                    return reject(diagnostic_codes::invalid_bindings,
                        where + ".modifiers has an unknown modifier");
                }
                if ((binding.modifiers & *bit) != 0U) {
                    return reject(diagnostic_codes::invalid_bindings,
                        where + ".modifiers repeats a modifier");
                }
                binding.modifiers = static_cast<std::uint8_t>(binding.modifiers | *bit);
            }
        }

        const JsonValue* scale = entry.find("scale");
        if (!scale || scale->kind != JsonValue::Kind::number) {
            return reject(diagnostic_codes::invalid_bindings, where + ".scale must be a number");
        }
        binding.scale = static_cast<float>(scale->number);
        if (!finite(binding.scale) || binding.scale == 0.0F) {
            return reject(diagnostic_codes::invalid_bindings,
                where + ".scale must be finite and nonzero");
        }
        // Held, reset and toggle bindings carry no gain: a held pan's
        // magnitude comes from the XML pan speed (free flight: the authored
        // free_camera speed), and inventing a per-key gain would be an
        // unsupported movement law.
        const bool unit_only = binding.trigger == Trigger::held
            || binding.action == Action::reset_view || binding.action == Action::free_toggle;
        if (unit_only && binding.scale != 1.0F) {
            return reject(diagnostic_codes::invalid_bindings, is_v1
                ? where + ".scale must be exactly 1 for held and reset_view bindings"
                : where + ".scale must be exactly 1 for held, reset_view and free_toggle bindings");
        }

        const Chord chord{binding.context, binding.device, binding.code, binding.modifiers};
        if (const auto existing = chords.find(chord); existing != chords.end()) {
            return reject(diagnostic_codes::ambiguous_binding_chord,
                where + " repeats the chord already bound by " + existing->second);
        }
        chords.emplace(chord, binding.id);
        table.bindings.push_back(std::move(binding));
    }
    if (is_v2) {
        // A way into free flight, or anything bound inside it, needs a way out.
        bool enters_or_flies = false;
        bool exits = false;
        for (const Binding& binding : table.bindings) {
            const bool free_context = binding.context == Context::free;
            if (binding.action == Action::free_toggle && free_context) exits = true;
            if (binding.action == Action::free_toggle || free_context) enters_or_flies = true;
        }
        if (enters_or_flies && !exits) {
            return reject(diagnostic_codes::invalid_bindings,
                "schema v2 table binds free flight but no free_toggle in the free context");
        }
    }
    return core::Result<BindingTable>::success(std::move(table));
}

} // namespace eawr::viewer::camera_input
