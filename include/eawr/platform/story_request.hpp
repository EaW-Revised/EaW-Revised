#pragma once

#include "eawr/core/from_chars.hpp"

// The session's side of the story presentation commands (docs/behaviour/tactical-story-cinematic.md): the request a staged command
// reads back as, with real numbers. Outside the script library, which has no floating point.

#include "eawr/script/authoritative/story_presentation.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <system_error>
#include <variant>

namespace eawr::platform {

using script::authoritative::ScriptCommand;
using script::authoritative::StoryRequestKind;
using script::authoritative::Value;
using script::authoritative::verb_story_camera;
using script::authoritative::verb_story_environment;
using script::authoritative::verb_story_fade;

// One call. `seconds` is the transition time (0 for a Set call, a Fade_Screen call's time, Transition_To_Tactical_Camera's time).
struct StoryRequest {
    StoryRequestKind kind{};
    bool transition{};                      // a Transition_ call (camera_key, target_key)
    std::array<double, 3> position{};       // the key position in source coordinates
    double distance{}, pitch{}, yaw{};      // the key offset (cartesian x, y, z when !euler)
    bool euler{};                           // the offset is distance, pitch and yaw (degrees)
    std::optional<std::uint64_t> object;    // the object the key attaches to
    bool use_object_rotation{};
    bool bounds_center{};                   // the eighth value: aim at the object's drawn centre
    double seconds{};
    bool flag{};                            // camera_start: story music; environment_cinematic: on
    std::int64_t index{};                   // environment_index
    friend bool operator==(const StoryRequest&, const StoryRequest&) = default;
};

namespace story_detail {

inline std::optional<double> parse_number(const Value& value) {
    const auto* text = std::get_if<std::string>(&value.data);
    if (text == nullptr) return std::nullopt;
    double out{};
    const auto parsed = eawr::core::from_chars(text->data(), text->data() + text->size(), out);
    if (parsed.ec != std::errc{} || parsed.ptr != text->data() + text->size() || !std::isfinite(out)) return std::nullopt;
    return out;
}

inline const std::string* text_of(const Value& value) { return std::get_if<std::string>(&value.data); }

} // namespace story_detail

[[nodiscard]] inline std::optional<StoryRequest> parse_story_command(const ScriptCommand& command) {
    const auto& a = command.arguments;
    StoryRequest request;
    if (command.verb == verb_story_fade) {
        if (a.size() != 2 || story_detail::text_of(a[0]) == nullptr) return std::nullopt;
        const auto seconds = story_detail::parse_number(a[1]);
        if (!seconds) return std::nullopt;
        const auto& op = *story_detail::text_of(a[0]);
        if (op == "in") request.kind = StoryRequestKind::fade_screen_in;
        else if (op == "out") request.kind = StoryRequestKind::fade_screen_out;
        else if (op == "on") request.kind = StoryRequestKind::fade_on;
        else if (op == "off") request.kind = StoryRequestKind::fade_off;
        else return std::nullopt;
        request.seconds = std::max(0.0, *seconds);
        return request;
    }
    if (command.verb == verb_story_environment) {
        if (a.size() != 2 || story_detail::text_of(a[0]) == nullptr || story_detail::text_of(a[1]) == nullptr) return std::nullopt;
        const auto& op = *story_detail::text_of(a[0]);
        const auto number = story_detail::parse_number(a[1]);
        if (!number) return std::nullopt;
        if (op == "index") { request.kind = StoryRequestKind::environment_index; request.index = static_cast<std::int64_t>(*number); }
        else if (op == "cinematic") { request.kind = StoryRequestKind::environment_cinematic; request.flag = *number != 0.0; }
        else return std::nullopt;
        return request;
    }
    if (command.verb != verb_story_camera || a.empty() || story_detail::text_of(a[0]) == nullptr) return std::nullopt;
    const auto& op = *story_detail::text_of(a[0]);
    if (op == "start") {
        if (a.size() != 2) return std::nullopt;
        const auto music = story_detail::parse_number(a[1]);
        if (!music) return std::nullopt;
        request.kind = StoryRequestKind::camera_start;
        request.flag = *music != 0.0;
        return request;
    }
    if (op == "end") {
        if (a.size() != 1) return std::nullopt;
        request.kind = StoryRequestKind::camera_end;
        return request;
    }
    if (op == "tactical") {
        if (a.size() != 2) return std::nullopt;
        const auto seconds = story_detail::parse_number(a[1]);
        if (!seconds) return std::nullopt;
        request.kind = StoryRequestKind::to_tactical;
        request.seconds = std::max(0.0, *seconds);
        return request;
    }
    if ((op != "camera" && op != "target") || a.size() != 13 || story_detail::text_of(a[1]) == nullptr) return std::nullopt;
    const auto& mode = *story_detail::text_of(a[1]);
    if (mode != "set" && mode != "transition") return std::nullopt;
    std::array<std::optional<double>, 11> n{};
    for (std::size_t i = 2; i < a.size(); ++i) if (i != 10) n[i - 2] = story_detail::parse_number(a[i]);
    for (std::size_t i = 0; i < n.size(); ++i) if (i != 8 && !n[i]) return std::nullopt;
    request.kind = op == "camera" ? StoryRequestKind::camera_key : StoryRequestKind::target_key;
    request.transition = mode == "transition";
    request.position = {*n[0], *n[1], *n[2]};
    request.seconds = std::max(0.0, *n[3]);
    request.distance = *n[4];
    request.pitch = *n[5];
    request.yaw = *n[6];
    request.euler = *n[7] != 0.0;
    if (const auto* object = story_detail::text_of(a[10]); object != nullptr && *object != "-") {
        std::uint64_t id{};
        const auto parsed = eawr::core::from_chars(object->data(), object->data() + object->size(), id);
        if (parsed.ec != std::errc{} || parsed.ptr != object->data() + object->size()) return std::nullopt;
        request.object = id;
    }
    request.use_object_rotation = *n[9] != 0.0;
    request.bounds_center = *n[10] != 0.0;
    return request;
}

} // namespace eawr::platform
