// Tactical presentation verbs (the owner-set rule: a presentation verb of a battle host lives here, not in the registrar the galactic
// AI host shares). Story cinematic camera, screen fades and environment calls: docs/behaviour/tactical-story-cinematic.md.

#include "eawr/script/authoritative/story_presentation.hpp"

#include "tactical_ai_internal.hpp"

#include <array>
#include <memory>
#include <optional>
#include <utility>

namespace eawr::script::foc {
namespace tactical_ai_detail {

namespace math = sim::math;
using authoritative::Binding;
using authoritative::Value;

using namespace detail;

// Registered only for a battle scheduler (a galactic goal mode binds none of them), beside register_battle_object_bindings.
void register_tactical_presentation(authoritative::ScriptScheduler& scripts, const HostPtr& host, std::vector<core::Diagnostic>& errors) {
    if (host->ai_world && host->ai_world->goal_mode() == "GALACTIC") return;
    const auto add = [&](std::string_view name, Binding binding) {
        if (auto added = scripts.register_binding(name, std::move(binding)); !added) errors.push_back(added.error());
    };
    // TSC-01..TSC-14: presentation requests of three verbs (authoritative/story_presentation.hpp); they never reach the world, the
    // replay, a save or a hash.
    struct PositionReader final : authoritative::StoryPositionReader {
        explicit PositionReader(HostPtr world) : host(std::move(world)) {}
        std::optional<std::array<math::Fixed, 3>> read(const Value& value) const override {
            const auto position = position_of(*host, value);
            if (!position) return std::nullopt;
            return std::array<math::Fixed, 3>{position->x, position->y, position->z};
        }
        std::uint32_t object_kind() const override { return handle_game_object; }
        HostPtr host;
    };
    const std::shared_ptr<const authoritative::StoryPositionReader> story_host = std::make_shared<PositionReader>(host);
    // Written out (not looped) so the coverage scan reads each name from its registration.
    add("Set_Cinematic_Camera_Key", authoritative::story_camera_key_binding("Set_Cinematic_Camera_Key", false, false, story_host));
    add("Transition_Cinematic_Camera_Key", authoritative::story_camera_key_binding("Transition_Cinematic_Camera_Key", false, true, story_host));
    add("Set_Cinematic_Target_Key", authoritative::story_camera_key_binding("Set_Cinematic_Target_Key", true, false, story_host));
    add("Transition_Cinematic_Target_Key", authoritative::story_camera_key_binding("Transition_Cinematic_Target_Key", true, true, story_host));
    add("Start_Cinematic_Camera", authoritative::story_camera_start_binding("Start_Cinematic_Camera"));
    add("End_Cinematic_Camera", authoritative::story_camera_end_binding("End_Cinematic_Camera"));
    add("Transition_To_Tactical_Camera", authoritative::story_to_tactical_binding("Transition_To_Tactical_Camera"));
    add("Fade_Screen_In", authoritative::story_fade_binding("Fade_Screen_In", authoritative::StoryRequestKind::fade_screen_in));
    add("Fade_Screen_Out", authoritative::story_fade_binding("Fade_Screen_Out", authoritative::StoryRequestKind::fade_screen_out));
    add("Fade_On", authoritative::story_fade_binding("Fade_On", authoritative::StoryRequestKind::fade_on));
    add("Fade_Off", authoritative::story_fade_binding("Fade_Off", authoritative::StoryRequestKind::fade_off));
    add("Set_New_Environment", authoritative::story_environment_binding("Set_New_Environment", authoritative::StoryRequestKind::environment_index));
    add("Set_Cinematic_Environment", authoritative::story_environment_binding("Set_Cinematic_Environment", authoritative::StoryRequestKind::environment_cinematic));
}

} // namespace tactical_ai_detail
} // namespace eawr::script::foc
