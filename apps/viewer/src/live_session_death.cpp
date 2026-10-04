#include "eawr/core/load_profile.hpp"
#include "live_session_view.hpp"
#include "eawr/presentation/ui/pads.hpp"

#include "shutdown_trace.hpp"
#include "frame_timer.hpp"

#include "eawr/platform/live_ai.hpp"
#include "eawr/presentation/space/live_units.hpp"
#include "eawr/presentation/space/unit_fade.hpp"
#include "eawr/presentation/ui/production.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/skirmish/roster_gate.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/units/unit_tables.hpp"

#include "viewer_path.hpp"

#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "live_session_internal.hpp"

namespace eawr::presentation::godot_backend {
namespace tactical = sim::tactical;
using namespace live_session_detail;

namespace {
[[nodiscard]] bool iequal(const std::string_view left, const std::string_view right) {
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
}

[[nodiscard]] double seconds_tag(const data::EffectiveObject& object, const std::string_view tag, const double fallback) {
    std::string text = tag_text(object, tag);
    if (!text.empty() && (text.back() == 'f' || text.back() == 'F')) text.pop_back();
    return parse_double(text).value_or(fallback);
}

// The object type a destroyed unit of this type becomes (its Death_Clone entries, UA-P1).
[[nodiscard]] std::optional<std::string> death_clone_of(const data::EffectiveObject& object) {
    std::vector<std::string> entries;
    for (const data::EffectiveValue& value : object.values) {
        if (iequal(value.value.name, "Death_Clone")) entries.push_back(value.value.raw_text);
    }
    return animation::death_clone_type(entries);
}

// The clip types #81 selects from tactical state, for the report.
constexpr std::array<std::size_t, 8> reported_clip_types{
    animation::clip_types::idle, animation::clip_types::space_idle, animation::clip_types::move,
    animation::clip_types::attack, animation::clip_types::attack_idle, animation::clip_types::die,
    animation::clip_types::deploy, animation::clip_types::undeploy};
} // namespace

void LiveSessionView::prepare_clips(const vfs::Vfs& filesystem, const data::Catalog& catalog) {
    const auto exists = [&](const std::string& path) { return static_cast<bool>(filesystem.stat(path)); };
    const auto clip_counts = [&](const std::string& model, const std::string& anim_override) {
        std::string counts = "{";
        for (std::size_t index = 0; index < reported_clip_types.size(); ++index) {
            const std::size_t type = reported_clip_types[index];
            counts += (index ? ", " : "") + json(animation::clip_type_names[type]) + ": "
                + std::to_string(animation::model_clip_paths(model, type, exists, anim_override).size());
        }
        return counts + "}";
    };
    std::map<std::string, bool> reported;
    const std::size_t units = placed_ships_.size();
    // The session's units are the first placed ships; an m2 start lists them in the same order
    // (with their fighters' craft type), a replay only by type.
    for (std::size_t index = 0; index < units; ++index) {
        const std::string unit_type = placed_ships_[index].object_id;
        const sim::EntityId entity = placed_ships_[index].live_entity;
        const std::string craft_type = start_ && index < start_->units.size() ? start_->units[index].craft_type : "";
        for (const std::string& type : {unit_type, craft_type}) {
            if (type.empty() || reported[type]) continue;
            reported[type] = true;
            auto object = catalog.resolve(type, data::Category::game_object);
            if (!object) continue;
            const std::string model = space_model_path(object.value());
            std::string row = "{\"type\": " + json(type) + ", \"model\": " + json(model) + ", \"clips\": "
                + (model.empty() ? std::string("null")
                                 : clip_counts(model, tag_text(object.value(), "Space_Model_Anim_Override_Name")));
            const auto clone_type = death_clone_of(object.value());
            row += ", \"death_clone\": " + (clone_type ? json(*clone_type) : std::string("null"));
            if (clone_type) {
                if (auto clone = catalog.resolve(*clone_type, data::Category::game_object)) {
                    const std::string clone_model = space_model_path(clone.value());
                    row += ", \"clone_model\": " + json(clone_model) + ", \"clone_clips\": "
                        + clip_counts(clone_model, tag_text(clone.value(), "Space_Model_Anim_Override_Name"));
                }
            }
            unit_clip_rows_.push_back(row + "}");
        }

        // The unit's death clone, its clip variant drawn from the unit's ID (UA-P2).
        if (auto death = prepare_death_clone(filesystem, catalog, index, entity)) {
            death_clones_.emplace(entity, std::move(*death));
        }
    }
}

std::optional<LiveSessionView::DeathClone> LiveSessionView::prepare_death_clone(const vfs::Vfs& filesystem,
    const data::Catalog& catalog, const std::size_t source, const std::uint64_t variant_key) {
    const auto exists = [&](const std::string& path) { return static_cast<bool>(filesystem.stat(path)); };
    const std::string unit_type = placed_ships_[source].object_id;
    const sim::EntityId entity = placed_ships_[source].live_entity;
    // Its type by the Death_Clone rule, its Specific_Death_Anim_Type clip (UA-08).
    auto object = catalog.resolve(unit_type, data::Category::game_object);
    if (!object) return std::nullopt;
    const auto clone_type = death_clone_of(object.value());
    if (!clone_type) return std::nullopt;
    auto clone = catalog.resolve(*clone_type, data::Category::game_object);
    std::string status = "ready";
    std::string clip_type(animation::clip_type_names[animation::clip_types::die]);
    bool removed = false;
    DeathClone death{.ship = placed_ships_.size(), .type = *clone_type};
    if (!clone) {
        status = "clone type not in the catalog";
    } else {
        const std::string declared = options_.death_anim_type.empty()
            ? tag_text(clone.value(), "Specific_Death_Anim_Type") : options_.death_anim_type;
        std::size_t type = animation::clip_types::die;
        if (!declared.empty()) {
            if (const auto found = animation::clip_type_index(declared)) {
                type = *found;
            } else {
                status = "Specific_Death_Anim_Type " + declared + " is no clip type; DIE is used (unverified)";
            }
        }
        clip_type = animation::clip_type_names[type];
        std::optional<std::uint32_t> declared_index;
        if (const std::string text = tag_text(clone.value(), "Specific_Death_Anim_Index"); !text.empty()) {
            if (const auto value = parse_u64(text); value && *value <= 0xFFFFU) {
                // 0xffff is retail's "draw one" (UC-E9).
                if (*value != 0xFFFFU) declared_index = static_cast<std::uint32_t>(*value);
            }
        }
        const std::string clone_model = space_model_path(clone.value());
        const auto clips = animation::model_clip_paths(clone_model, type, exists,
            tag_text(clone.value(), "Space_Model_Anim_Override_Name"));
        // Retail draws an undeclared variant with the synchronized random generator; the view
        // takes it from the unit's ID, so every run shows the same one (UA-P2).
        const auto variant = animation::death_clip_variant(clips.size(), declared_index, variant_key);
        if (variant) death.clip = clips[*variant];
        const std::string remove = tag_text(clone.value(), "Remove_Upon_Death");
        death.remove_upon_death = iequal(remove, "true") || iequal(remove, "yes");
        if (!variant) {
            if (animation::death_start(false, death.remove_upon_death) == animation::DeathStart::removed) {
                removed = true;
                status = "no " + clip_type + " clip: removed at once (Remove_Upon_Death)";
            } else {
                status = "no " + clip_type + " clip: the clone keeps its pose";
            }
        }
        death.playback.blend_ticks = static_cast<std::uint32_t>(
            animation::seconds_to_ticks(0.5, tactical::logical_frames_per_second).value_or(0U));
        const double persistence = options_.death_persistence.value_or(
            seconds_tag(clone.value(), "Death_Persistence_Duration", -1.0));
        if (persistence >= 0.0) {
            death.playback.persistence_ticks = animation::seconds_to_ticks(persistence, tactical::logical_frames_per_second);
        }
        death.playback.fade_ticks = animation::seconds_to_ticks(
            seconds_tag(clone.value(), "Death_Fade_Time", 0.25), tactical::logical_frames_per_second).value_or(0U);
    }
    death_clone_rows_.push_back("{\"unit\": " + std::to_string(entity) + ", \"type\": "
        + json(unit_type) + ", \"clone\": " + json(*clone_type) + ", \"clip_type\": " + json(clip_type)
        + ", \"clip\": " + json(death.clip) + ", \"remove_upon_death\": "
        + (death.remove_upon_death ? "true" : "false") + ", \"persistence_ticks\": "
        + (death.playback.persistence_ticks ? std::to_string(*death.playback.persistence_ticks) : "null")
        + ", \"status\": " + json(status) + "}");
    if (!clone || removed) return std::nullopt;
    SpacePopulation::Options::PlacedShip ship;
    ship.object_id = *clone_type;
    ship.position = placed_ships_[source].position;
    ship.yaw_degrees = placed_ships_[source].yaw_degrees;
    // The clone is its own live ship, never a session unit.
    ship.live_entity = std::numeric_limits<sim::EntityId>::max() - entity;
    ship.team_colour = placed_ships_[source].team_colour;
    ship.colour_record = placed_ships_[source].colour_record;
    ship.clip = death.clip;
    ship.death_clone = true;
    placed_ships_.push_back(std::move(ship));
    return death;
}

std::optional<animation::DeathFrame> LiveSessionView::clone_death_frame(const DeathClone& clone,
                                                                       const animation::Player* player,
                                                                       const std::uint64_t tick) {
    if (animation::death_start(player != nullptr, clone.remove_upon_death) == animation::DeathStart::removed) {
        return std::nullopt;
    }
    return animation::death_frame(clone.playback, player ? player->playable_frames() : 0U,
        player ? static_cast<std::uint32_t>(player->frames_per_second()) : tactical::logical_frames_per_second,
        tick, tactical::logical_frames_per_second);
}

std::optional<LiveSessionView::CloneFrame> LiveSessionView::clone_frame(const SpacePopulation& population,
                                                                        const std::size_t ship,
                                                                        const double tick) const {
    // A clone that left this frame still stood in the samples before its fade (#429).
    for (const std::vector<ActiveClone>* clones : {&active_clones_, &retiring_clones_}) {
        for (const ActiveClone& active : *clones) {
            const DeathClone& clone = death_clones_.at(active.unit);
            if (clone.ship != ship) continue;
            // frame()'s clock: whole ticks since the clone appeared, one tick before its death tick.
            const double elapsed = tick - (static_cast<double>(active.death_tick) - 1.0);
            if (elapsed < -1.0e-9) return std::nullopt;
            const animation::Player* player = population.live_clip(ship);
            const auto death = clone_death_frame(clone, player,
                                                 static_cast<std::uint64_t>(std::floor(elapsed + 1.0e-9)));
            if (!death || !death->shown) return std::nullopt;
            CloneFrame result{active.pose, std::nullopt};
            if (player != nullptr) result.death = *death;
            return result;
        }
    }
    return std::nullopt;
}

void LiveSessionView::retire_clones(SpacePopulation& population, GodotRenderer& renderer) {
    for (const ActiveClone& retiring : retiring_clones_) {
        population.retire_live_ship(renderer, death_clones_.at(retiring.unit).ship);
    }
    retiring_clones_.clear();
}

} // namespace eawr::presentation::godot_backend
