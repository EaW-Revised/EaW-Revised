#pragma once

// Story cinematic camera, screen fades and environment calls of a tactical script (docs/behaviour/tactical-story-cinematic.md,
// TSC-xx). The calls only stage presentation commands of three verbs of the host (arguments are text, written locale-free);
// the host never turns them into world input, a replay record, a save field or a hash. The session hands them to the viewer,
// which draws them (the camera through the same movie-camera path the battle cameras use).

#include "eawr/script/authoritative/scheduler.hpp"
#include "eawr/sim/math/fixed.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::script::authoritative {

inline constexpr std::string_view verb_story_camera = "foc.story_camera";
inline constexpr std::string_view verb_story_fade = "foc.story_fade";
inline constexpr std::string_view verb_story_environment = "foc.story_environment";

enum class StoryRequestKind : std::uint8_t {
    camera_start,      // Start_Cinematic_Camera(music)
    camera_end,        // End_Cinematic_Camera()
    camera_key,        // Set_ / Transition_Cinematic_Camera_Key
    target_key,        // Set_ / Transition_Cinematic_Target_Key
    to_tactical,       // Transition_To_Tactical_Camera(seconds)
    fade_screen_out,   // Fade_Screen_Out(seconds): clear to black over the time
    fade_screen_in,    // Fade_Screen_In(seconds): black to clear over the time
    fade_on,           // Fade_On(): black at once
    fade_off,          // Fade_Off(): clear at once
    environment_index, // Set_New_Environment(index)
    environment_cinematic, // Set_Cinematic_Environment(on)
};

// What the shared bindings need from the host that registers them: the position of a position or object argument (nullopt:
// neither), and the kind word of its game-object handles.
class StoryPositionReader {
public:
    virtual ~StoryPositionReader() = default;
    [[nodiscard]] virtual std::optional<std::array<sim::math::Fixed, 3>> read(const Value& value) const = 0;
    [[nodiscard]] virtual std::uint32_t object_kind() const = 0;
};

// The bindings: each validates its arguments like the original (the named script errors, the one warning for a key whose first
// value is no position) and stages one command of the verbs above. None reaches the world.
[[nodiscard]] Binding story_camera_key_binding(std::string name, bool target, bool transition, std::shared_ptr<const StoryPositionReader> host);
[[nodiscard]] Binding story_camera_start_binding(std::string name);
[[nodiscard]] Binding story_camera_end_binding(std::string name);
[[nodiscard]] Binding story_to_tactical_binding(std::string name);
[[nodiscard]] Binding story_fade_binding(std::string name, StoryRequestKind kind); // the four Fade_ calls
[[nodiscard]] Binding story_environment_binding(std::string name, StoryRequestKind kind);

[[nodiscard]] bool is_story_verb(std::string_view verb) noexcept;

} // namespace eawr::script::authoritative
