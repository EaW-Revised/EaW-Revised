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

} // namespace

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

} // namespace camera_binding_detail

std::string_view to_string(const Action action) noexcept {
    for (const Named& entry : action_names) {
        if (entry.value == static_cast<std::uint32_t>(action)) return entry.name;
    }
    return "invalid";
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
} // namespace eawr::viewer::camera_input
