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
using authoritative::BindingContext;
using authoritative::ValueList;
using numeric::LuaNumber;
using authoritative::Value;

using namespace detail;

// Registered only for a battle scheduler (a galactic goal mode binds none of them), beside register_battle_object_bindings.
void register_tactical_presentation(authoritative::ScriptScheduler& scripts, const HostPtr& host, std::vector<core::Diagnostic>& errors) {
    if (host->ai_world && host->ai_world->goal_mode() == "GALACTIC") return;
    const auto add = [&](std::string_view name, Binding binding) {
        if (auto added = scripts.register_binding(name, std::move(binding)); !added) errors.push_back(added.error());
    };
    // The global shares the member's root hide flag. Lua booleans are accepted as a documented superset of numbers.
    add("Hide_Object", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        if (host->ai_world && host->ai_world->goal_mode() == "GALACTIC") return fail("Hide_Object: Command only valid in a tactical game.");
        if (arguments.size() < 2) return fail("Hide_Object expects an object and a float.");
        if (as_handle(arguments[0], handle_game_object) == nullptr)
            return fail("Hide_Object: Expected game object parameter.");
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr) return none();
        bool hidden = true;
        if (const auto* value = std::get_if<LuaNumber>(&arguments[1].data)) hidden = *value > LuaNumber(0);
        else if (const auto* flag = std::get_if<bool>(&arguments[1].data)) hidden = *flag;
        else context.report(authoritative::codes::script_error, "Hide_Object: invalid parameter 2, expected a float.");
        context.issue_command(std::string(verb_hide_sound), {arguments[0], boolean(hidden)});
        context.issue_command(std::string(verb_hide), {number(LuaNumber(static_cast<std::int64_t>(unit->owner))), arguments[0], boolean(hidden)});
        return none();
    });
    // Sub-object hides are transient presentation commands; they are not part of a checkpoint or canonical state.
    add("Hide_Sub_Object", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        if (host->ai_world && host->ai_world->goal_mode() == "GALACTIC") return fail("Hide_Sub_Object: Command only valid in a tactical game.");
        if (arguments.size() < 3) return fail("Hide_Sub_Object expects an object, a float and a name.");
        if (as_handle(arguments[0], handle_game_object) == nullptr) return fail("Hide_Sub_Object: Expected game object parameter.");
        const auto* name = as_text(arguments[2]);
        if (!name) return fail("Hide_Sub_Object: Expected string parameter 3.");
        bool hidden = true;
        if (const auto* value = std::get_if<LuaNumber>(&arguments[1].data)) hidden = *value > LuaNumber(0);
        else if (const auto* flag = std::get_if<bool>(&arguments[1].data)) hidden = *flag;
        else context.report(authoritative::codes::script_error, "Hide_Sub_Object: invalid parameter 2, expected a float.");
        if (live_object(*host, arguments[0]) != nullptr)
            context.issue_command(std::string(verb_hide_sub_object), {arguments[0], boolean(hidden), Value::text(*name)});
        return none();
    });
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
