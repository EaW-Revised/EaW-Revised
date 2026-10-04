#include "camera_binding_config_internal.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <initializer_list>
#include <set>
#include <tuple>

namespace eawr::viewer::camera_input {
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

} // namespace eawr::viewer::camera_input
