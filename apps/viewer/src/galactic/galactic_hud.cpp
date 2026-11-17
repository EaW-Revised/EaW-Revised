#include "radar_texture_lookup.hpp"
#include "galactic_mode_internal.hpp"
#include "eawr/data/tag_trace.hpp"
#include "viewer_load_notes.hpp"
#include "galactic_lan_hud.hpp"
#include "planet_label_scale.hpp"
#include "fleet_visibility.hpp"
#include "command_bar_scale.hpp"
#include "advisor_duration.hpp"
#include "notice_text.hpp"
#include "map_mode_types.hpp"
#include "viewer_path.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/core/gc_load_trace.hpp"
#include "eawr/presentation/ui/fonts.hpp"
#include "eawr/presentation/ui/layout.hpp"
#include "eawr/data/ui/text_database.hpp"
#include "ui/tactical_hud_internal.hpp"
#include "ui/input_routing.hpp"
#include "ui/build_queue_dial.hpp"

#include <godot_cpp/classes/canvas_item_material.hpp>
#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/polygon2d.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/theme.hpp>
#include <godot_cpp/classes/text_server.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/style_box_texture.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include <algorithm>
#include <cctype>
#include <set>
#include <cmath>
#include <numbers>
#include <sstream>
#include <iomanip>
#include <charconv>
#include <unordered_map>
#include <unordered_set>
#include <pugixml.hpp>

using namespace godot;

namespace eawr::presentation::godot_backend {
using namespace galactic_detail;
namespace {
// WGUI-96: unverified renderer adapters pending a GPH-01 trace.
constexpr float unverified_default_name_justification = 0.5F;
constexpr float unverified_name_vertical_anchor = 0.5F;
constexpr int unverified_name_outline_pixels = 1;
std::string unverified_xml_name_fallback(const galactic_view::PlanetArt* art,
    const std::string& xml_name) {
    return art && !art->label.empty() ? art->label : xml_name;
}
std::string command_bar_token(const data::ui::CommandBarComponent& component, data::ui::Field field) {
    const auto list = component.list(field);
    return list.empty() ? std::string{} : list.front();
}

Rect2 command_bar_screen(const presentation::ui::ShellPlacement& placement,
    const data::ui::ReferenceRect& rect) {
    const auto pixels = presentation::ui::shell_to_screen(rect, placement);
    return Rect2(static_cast<float>(pixels.x), static_cast<float>(pixels.y),
        static_cast<float>(pixels.width), static_cast<float>(pixels.height));
}

void set_command_bar_text(EawrHudText& label, const String& value) {
    if (label.get_text() != value) label.set_text(value);
}

// The fiscal pip lit for this clock: the tenth of the fiscal window the galactic frame is in (none while the
// fiscal cycle is not running).
std::optional<std::size_t> fiscal_pip(const sim::galactic::GalacticClock& clock, const bool fiscal_available) {
    if (!fiscal_available) return std::nullopt;
    const double elapsed = clock.galactic_frame >= clock.fiscal_start
        ? static_cast<double>(clock.galactic_frame - clock.fiscal_start) : 0.0;
    return static_cast<std::size_t>(std::clamp(elapsed * 10.0
        / static_cast<double>(clock.fiscal_end - clock.fiscal_start), 0.0, 9.0));
}

}

bool GalacticMode::State::build_hud() {
    // The template is presentation data. Simulation facts never come from it.
    if (!filesystem) return true;
    // Detail timers (presentation only): where a GC Start's HUD build spends its time.
    std::optional<core::gc_load::Scope> detail(std::in_place, "gc.build_hud.command_bar_data");
    auto loaded = data::ui::load_command_bar(*filesystem);
    if (!loaded) { failure = core::format_diagnostic(loaded.error()); return false; }
    const auto* component = loaded.value().catalog.find("g_planet_name");
    if (!component) { failure = "the galactic planet name HUD template is absent"; return false; }
    planet_name_style = *component;

    detail.emplace("gc.build_hud.fonts");
    const auto arguments = OS::get_singleton()->get_cmdline_user_args();
    std::string directory = utf8(OS::get_singleton()->get_environment("EAWR_FONT_CACHE"));
    for (int64_t index = 0; index + 1 < arguments.size(); ++index) {
        if (utf8(arguments[index]) == "--eawr-font-cache") directory = utf8(arguments[index + 1]);
    }
    if (directory.empty()) {
        const auto project = ViewerPath{utf8(ProjectSettings::get_singleton()->globalize_path("res://"))}.native();
        directory = ViewerPath::utf8((project / ".." / ".." / ".." / "out" / "fonts").lexically_normal());
    }
    hud_fonts = std::make_unique<FontProvider>(load_hud_font_cache(ViewerPath{directory}.native()));
    auto face = std::string(component->text(data::ui::Field::font_name));
    const auto alternatives = component->list(data::ui::Field::alternate_font_name);
    // WGUI-38: without ground-structure knowledge the name uses frame zero.
    if (!alternatives.empty()) face = alternatives.front();
    name_font = hud_fonts->resolve({face, component->integer(data::ui::Field::font_point_size).value_or(0)}, "english");
    hud = memnew(Control);
    // Screen-space input belongs to the main viewport, outside the 3D
    // SubViewport. Parent visibility still hides the HUD during a handoff.
    galaxy_container->add_child(hud);
    hud->set_z_index(1);
    hud->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    hud->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
    // GUI picking follows sibling order, independently of canvas z_index.
    // Keep every world-attached overlay below panel masks, even when created later.
    world_overlays = memnew(Control);
    world_overlays->set_name("GalacticWorldOverlays");
    world_overlays->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    hud->add_child(world_overlays);
    world_overlays->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
    departure_hint = memnew(Label);
    departure_hint->set_name("GalacticDepartureHint");
    departure_hint->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    departure_hint->set_text("Ready to depart - resumes when unpaused");
    departure_hint->add_theme_font_override("font", hud_fonts->font(name_font));
    departure_hint->add_theme_font_size_override("font_size", 18);
    departure_hint->add_theme_constant_override("outline_size", 4);
    departure_hint->add_theme_color_override("font_outline_color", Color(0, 0, 0, 1));
    hud->add_child(departure_hint);
    departure_hint->hide();
    const auto timed = [&detail](const char* name, auto&& step) {
        detail.emplace(name);
        return step();
    };
    return timed("gc.build_hud.command_bar", [&] { return build_command_bar(); })
        && timed("gc.build_hud.fleet", [&] { return build_fleet_hud(); })
        && timed("gc.build_hud.radar", [&] { return build_radar_hud(); })
        && timed("gc.build_hud.ground", [&] { return build_ground_hud(); })
        && timed("gc.build_hud.orbit", [&] { return build_orbit_hud(); })
        && timed("gc.build_hud.corruption", [&] { return build_corruption_hud(); })
        && timed("gc.build_hud.battle_dialog", [&] { return build_battle_dialog(); })
        && timed("gc.build_hud.campaign_score", [&] { return build_campaign_score(); })
        && timed("gc.build_hud.hero_map_bar", [&] { return build_hero_map_bar(); })
        && timed("gc.build_hud.hero_action", [&] { return build_hero_action_hud(); })
        && timed("gc.build_hud.weapon_action", [&] { return build_weapon_action_hud(); })
        && timed("gc.build_hud.destruction", [&] { return build_destruction_hud(); })
        && timed("gc.build_hud.black_market", [&] { return build_black_market_hud(); });
}

void GalacticMode::State::update_hud() {
    if (!pending_texture_notes.empty()) publish_viewer_load_notes(std::exchange(pending_texture_notes, {}), {}, true);
    const FramePhaseMeasure phase(options.zoom_timing ? &frame_phase_costs[static_cast<std::size_t>(FramePhase::hud_total)] : nullptr);
    const auto started = std::chrono::steady_clock::now();
    struct Measure final {
        State& state;
        std::chrono::steady_clock::time_point started;
        ~Measure() {
            ++state.hud_updates;
            state.hud_update_nanoseconds += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
        }
    } measure{*this, started};
    // Explicit audio-adapter qualification, never fabricated campaign objectives.
    // The live story-to-HUD publication remains #2042.
    if (options.hud_provider) options.hud_provider->update_hud(*this);
    { const auto measured = measure_frame(FramePhase::hud_facts); refresh_hud_facts(); }
    if (planet_search) planet_search->service(*this);
    if (departure_hint && displayed_snapshot && galaxy) {
        const auto fleets = displayed_snapshot->fleets();
        const bool ready = std::any_of(fleets.begin(), fleets.end(), [this](const auto& fleet) {
            return fleet.owner == galaxy->local.player && presentation::galactic_departure_ready(*displayed_snapshot, fleet);
        });
        departure_hint->set_position(Vector2(24, 24));
        departure_hint->set_visible(ready && !tactical && !loading);
    }
    service_production_tooltip();
    service_hero_tooltip();
    service_reference_pages(*this);
    service_story_summary(*this);
    service_pause_banner(*this);
    service_story_text(*this);
    service_forced_clicks();
    service_story_hud();
    { const auto measured = measure_frame(FramePhase::hud_commands); update_command_bar(); }
    { const auto measured = measure_frame(FramePhase::hud_fleets); update_fleet_hud(); }
    { const auto measured = measure_frame(FramePhase::hud_ground); update_ground_hud(); }
    { const auto measured = measure_frame(FramePhase::hud_orbit); update_orbit_hud(); }
    service_encyclopedia();
    { const auto measured = measure_frame(FramePhase::hud_radar); update_radar_hud(); }
    update_corruption_hud();
    update_hero_map_bar();
    update_hero_action_hud();
    update_destruction_hud();
    { const auto measured = measure_frame(FramePhase::hud_weapon); update_weapon_action_hud(); }
    update_black_market_hud();
    service_black_market_encyclopedia();
    service_selection_cursor();
    const auto labels_timer = measure_frame(FramePhase::hud_labels);
    update_planet_labels();
}

void GalacticMode::State::reset_label_placement() {
    hud_label_key.reset();
    for (auto& [id, record] : planet_label_records) { static_cast<void>(id); record.placed = false; record.overlays = true; }
}

void GalacticMode::State::hide_planet_overlays(const galactic_view::ObjectId planet) {
    if (const auto levels = planet_levels.find(planet); levels != planet_levels.end()) {
        for (auto* pip : levels->second.ground) pip->hide();
        for (auto* pip : levels->second.space) pip->hide();
        levels->second.political->hide();
    }
    if (const auto icon = corruption_info_icons.find(planet); icon != corruption_info_icons.end()) icon->second->hide();
    for (auto found = status_effect_icons.lower_bound({planet, std::uint8_t{0}});
         found != status_effect_icons.end() && found->first.first == planet; ++found) found->second->hide();
    if (const auto icon = planet_bonus_icons.find(planet); icon != planet_bonus_icons.end()) icon->second->hide();
    if (const auto icon = planet_weather_icons.find(planet); icon != planet_weather_icons.end()) icon->second->hide();
    if (const auto label = planet_corruption_labels.find(planet); label != planet_corruption_labels.end()) label->second->hide();
}

std::uint64_t GalacticMode::State::planet_badge_digest(const galactic_view::ObjectId planet) const {
    galactic_view::HudDigest digest;
    for (auto found = fleet_badges.lower_bound({planet, std::uint8_t{0}});
         found != fleet_badges.end() && found->first.first == planet; ++found) {
        const auto& badge = found->second;
        digest.add(found->first.second); digest.add(badge.shown); digest.add(badge.revision);
        if (!badge.shown) continue;
        digest.add(badge.texture); digest.add(badge.best);
    }
    return digest.value();
}

// The rectangle the planet's visible controls cover, relative to `anchor` (where they were just placed).
galactic_view::HudBounds GalacticMode::State::planet_control_bounds(const galactic_view::ObjectId planet,
    const Vector2 anchor) const {
    galactic_view::HudBounds bounds;
    const auto include = [&](const Control* control, const Vector2 origin = {}) {
        if (!control || !control->is_visible()) return;
        const auto position = origin + control->get_position();
        const auto size = control->get_size().max(control->get_combined_minimum_size());
        bounds.include(position.x, position.y, size.x, size.y, anchor.x, anchor.y);
    };
    if (const auto found = planet_labels.find(planet); found != planet_labels.end() && found->second->is_visible()) {
        const auto* label = found->second;
        const auto measured = label->get_minimum_size();
        const auto size = label->get_size().max(label->get_combined_minimum_size());
        const float ui_scale = std::min(static_cast<float>(camera.width) / 1024.0F,
            static_cast<float>(camera.height) / 768.0F);
        const auto rect = galactic_view::planet_label_placement(*planet_name_style, anchor.x, anchor.y, ui_scale,
            measured.x, measured.y, size.x, size.y).bounds;
        bounds.include(rect.x, rect.y, rect.width, rect.height, anchor.x, anchor.y);
    }
    if (const auto found = planet_income_labels.find(planet); found != planet_income_labels.end()) include(found->second);
    if (const auto found = planet_levels.find(planet); found != planet_levels.end()) {
        for (const auto* pip : found->second.ground) include(pip);
        for (const auto* pip : found->second.space) include(pip);
        include(found->second.political);
    }
    if (const auto found = production_icons.find(planet); found != production_icons.end()) include(found->second);
    if (const auto found = corruption_info_icons.find(planet); found != corruption_info_icons.end()) include(found->second);
    for (auto found = status_effect_icons.lower_bound({planet, std::uint8_t{0}});
         found != status_effect_icons.end() && found->first.first == planet; ++found) include(found->second);
    if (const auto found = planet_bonus_icons.find(planet); found != planet_bonus_icons.end()) include(found->second);
    if (const auto found = planet_weather_icons.find(planet); found != planet_weather_icons.end()) include(found->second);
    if (const auto found = planet_corruption_labels.find(planet); found != planet_corruption_labels.end()) include(found->second);
    for (auto found = fleet_badges.lower_bound({planet, std::uint8_t{0}});
         found != fleet_badges.end() && found->first.first == planet; ++found) {
        const auto& badge = found->second;
        if (!badge.shown || !badge.button) continue;
        include(badge.button);
        // The ship picture is a child of the button and may reach past it.
        if (badge.button->is_visible()) include(badge.ship, badge.button->get_position());
    }
    return bounds;
}

void GalacticMode::State::update_planet_labels() {
    if (!hud || !planet_name_style) return;
    const auto& label_flight = seat_flight();
    const HudLabelKey label_key{displayed_snapshot, camera.eye, camera.target, camera.up,
        camera.vertical_fov_degrees, camera.near_plane, camera.far_plane, camera.width, camera.height,
        label_flight.state(), label_flight.alpha(), base_levels_visible, income_layer_visible, bonus_layer_visible,
        map_input.gates.hud_enabled, map_input.gates.tutorial_locked};
    // Hover timers, clock flashes, encyclopedia fade and radar camera tracking
    // have already been serviced. Stationary labels keep their native controls.
    if (hud_label_key == label_key) return;
    hud_label_key = label_key;
    ++hud_label_updates;
    ++planet_label_pass;
    const PlanetLabelEnvironment environment{camera.width, camera.height, base_levels_visible, income_layer_visible, bonus_layer_visible,
        map_input.gates.hud_enabled, map_input.gates.tutorial_locked};
    if (planet_label_environment != environment) { planet_label_environment = environment; ++planet_label_generation; }
    // Keep an unchanged visible label alive: hiding it during every refresh
    // cancels native hover and the tooltip timer before it can expire.
    auto& visible_labels = visible_planet_labels;
    visible_labels.clear();
    visible_labels.reserve(planet_labels.size());
    const auto hide_unused_labels = [&] {
        const auto visible = [&](const auto id) { return std::binary_search(visible_labels.begin(), visible_labels.end(), id); };
        for (auto& [id, label] : planet_labels) if (!visible(id)) label->hide();
        for (auto& [id, label] : planet_income_labels) {
            const auto facts = hud_facts.find(id);
            if (!visible(id) || facts == hud_facts.end() || !facts->second.visible_income) label->hide();
        }
        for (const auto& [id, buttons] : visible_fleet_badges)
            if (!visible(id) && !(ground_hud.pressed_object && map_input.box_end && id == selected_planet))
            for (auto* button : buttons) button->hide();
        for (const auto& [id, icon] : production_icons) {
            const auto facts = hud_facts.find(id);
            if (!visible(id) || facts == hud_facts.end() || !galactic_view::production_activity(facts->second)) icon->hide();
        }
        for (const auto& [id, icon] : planet_bonus_icons) if (!visible(id)) icon->hide();
        for (const auto& [id, icon] : planet_weather_icons) if (!visible(id)) icon->hide();
        for (const auto& [id, label] : planet_corruption_labels) if (!visible(id)) label->hide();
    };
    if (!displayed_snapshot) {
        for (const auto& [id, levels] : planet_levels) { static_cast<void>(levels); hide_planet_overlays(id); }
        for (const auto& [id, icon] : corruption_info_icons) { static_cast<void>(id); icon->hide(); }
        for (const auto& [key, icon] : status_effect_icons) { static_cast<void>(key); icon->hide(); }
        for (const auto& [id, icon] : planet_bonus_icons) { static_cast<void>(id); icon->hide(); }
        for (const auto& [id, icon] : planet_weather_icons) { static_cast<void>(id); icon->hide(); }
        for (const auto& [id, label] : planet_corruption_labels) { static_cast<void>(id); label->hide(); }
        for (auto& [id, record] : planet_label_records) { static_cast<void>(id); record.placed = false; record.overlays = false; }
        hide_unused_labels();
        return;
    }
    const bool income_changed = income_labels_snapshot != displayed_snapshot;
    const bool income_resized = income_labels_height != camera.height;
    if (income_changed) {
        std::unordered_map<galactic_view::ObjectId, std::size_t> indices;
        indices.reserve(galaxy->planets.size());
        for (std::size_t index = 0; index < galaxy->planets.size(); ++index) indices.emplace(galaxy->planets[index].id, index);
        hud_planet_indices.clear();
        for (const auto& location : displayed_snapshot->planets())
            if (const auto found = indices.find(location.id); found != indices.end()) hud_planet_indices.push_back(found->second);
    }
    if (income_resized) {
        // The text and colour of a placed planet follow its facts; only the font size follows the screen height.
        if (const auto* component = command_catalog.find("g_planet_value")) {
            const int pixels = std::max(1, presentation::ui::font_pixel_height(
                component->integer(data::ui::Field::font_point_size).value_or(0), camera.height));
            for (const auto& [id, label] : planet_income_labels) {
                static_cast<void>(id);
                label->add_theme_font_size_override("font_size", pixels);
            }
        }
    }
    const auto& style = *planet_name_style;
    const auto font = hud_fonts->font(name_font);
    const int font_height = presentation::ui::font_pixel_height(name_font.point_size, camera.height);
    const Vector3 eye(camera.eye[0], camera.eye[1], camera.eye[2]);
    const Vector3 target(camera.target[0], camera.target[1], camera.target[2]);
    const auto forward = (target - eye).normalized();
    const auto right = forward.cross(Vector3(0, 1, 0)).normalized();
    const auto up = right.cross(forward);
    const float tangent = std::tan(camera.vertical_fov_degrees * std::numbers::pi_v<float> / 360.0F);
    const float ui_scale = std::min(static_cast<float>(camera.width) / 1024.0F,
        static_cast<float>(camera.height) / 768.0F);
    const auto& flight = seat_flight();
    const double inward = flight.state() == galactic_view::CameraState::outward ? 1.0 - flight.alpha()
        : (flight.state() == galactic_view::CameraState::overview ? 0.0 : flight.alpha());
    const float alpha = static_cast<float>(galactic_view::overview_name_alpha(inward));
    // A planet whose controls all sit this far outside the screen is not placed again until it comes back.
    constexpr float cull_margin = 32.0F;
    const galactic_view::HudView view{static_cast<float>(camera.width), static_cast<float>(camera.height)};
    destruction_hud.projected = false;
    for (auto& [planet, slot] : destruction_hud.raid_slots) {
        static_cast<void>(planet); slot.projected = false;
        if (slot.button) slot.button->hide();
    }
    for (auto& [planet, slot] : destruction_hud.ability_slots) {
        static_cast<void>(planet); slot.projected = false;
        if (slot.button) slot.button->hide();
    }
    for (const auto planet_index : hud_planet_indices) {
        const auto planet = galaxy->planets.begin() + static_cast<std::ptrdiff_t>(planet_index);
        const auto observation = hud_facts.find(planet->id);
        // WGUI-38: an undiscovered planet keeps its name, in the neutral colour.
        if (observation == hud_facts.end() && options.campaign != galactic_view::synthetic_campaign) continue;
        const galactic_view::PlanetArt* art = nullptr;
        if (content) {
            const auto found = content->art.find(planet->id);
            if (found != content->art.end()) art = &found->second;
        }
        const auto point = galactic_view::render_point(planet->position);
        const auto offset = Vector3(point[0], point[1], point[2]) - eye;
        const float depth = offset.dot(forward);
        if (depth <= camera.near_plane || depth >= camera.far_plane) {
            // Hidden, but its text keeps following the facts until it returns.
            if (income_changed && observation != hud_facts.end() && observation->second.visible_income)
                if (const auto label = planet_income_labels.find(planet->id); label != planet_income_labels.end() && label->second)
                    label->second->set_text(String::utf8(observation->second.income_overlay.c_str()));
            continue;
        }
        const float scale = static_cast<float>(camera.height) / (2.0F * depth * tangent);
        Vector2 anchor(static_cast<float>(camera.width) * 0.5F + offset.dot(right) * scale,
            static_cast<float>(camera.height) * 0.5F - offset.dot(up) * scale);
        // WGUI-96: integer projected anchor, then the template's X offset
        // through the reference HUD camera. Offset Y is not used here.
        anchor = Vector2(std::trunc(anchor.x), std::trunc(anchor.y));
        const Vector2 projected = anchor;
        auto& record = planet_label_records[planet->id];
        record.pass = planet_label_pass;
        position_destruction_hud(planet->id, anchor, ui_scale, alpha);
        const PlanetLabelKey key{{anchor.x, anchor.y}, ui_scale, alpha, planet_label_generation,
            observation == hud_facts.end() ? std::uint64_t{0} : galactic_view::label_placement_digest(observation->second),
            planet_badge_digest(planet->id)};
        // A fleet drag shows drop targets on the hovered planet and shares them between planets: place everything.
        if (record.placed && !dragged_fleet) {
            const auto action = galactic_view::hud_placement(record.key, key, record.culled, record.reach, view, anchor.x,
                anchor.y, cull_margin);
            if (action != galactic_view::HudPlacement::place) {
                ++(action == galactic_view::HudPlacement::keep ? hud_label_retained : hud_label_culled);
                if (record.name_visible) visible_labels.push_back(planet->id);
                continue;
            }
        }
        const bool facts_changed = !record.placed || record.key.facts != key.facts;
        ++hud_label_placed;
        hud_planet_anchors[planet->id] = anchor;
        hide_planet_overlays(planet->id);
        update_planet_levels(planet->id, anchor, ui_scale, alpha);
        update_fleet_badges(planet->id, anchor, ui_scale, alpha);
        update_planet_status_icons(planet->id, anchor, ui_scale, alpha);
        update_planet_bonus_icon(planet->id, planet->ability.icon, anchor, ui_scale, alpha);
        update_planet_weather_icon(planet->id, planet->weather.icon, anchor, ui_scale, alpha);
        update_planet_corruption_text(planet->id, anchor, ui_scale, alpha);
        if (observation != hud_facts.end() && observation->second.visible_income) {
            if (const auto* component = command_catalog.find("g_planet_value")) {
                auto& income = planet_income_labels[planet->id];
                const bool created = income == nullptr;
                if (created) {
                    income = memnew(Label); income->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
                    income->set_name(String::utf8(("planet_income_" + std::to_string(planet->id)).c_str()));
                    world_overlays->add_child(income);
                    const auto face = hud_fonts->resolve({std::string(component->text(data::ui::Field::font_name)),
                        component->integer(data::ui::Field::font_point_size).value_or(0)}, "english");
                    income->add_theme_font_override("font", hud_fonts->font(face));
                    income->add_theme_constant_override("outline_size", component->flag(data::ui::Field::text_outline)
                        ? unverified_name_outline_pixels : 0);
                    income->add_theme_color_override("font_outline_color", Color(0, 0, 0, 1));
                }
                // GHUDL-14: the income text shares the observed name colour,
                // including Neutral when ownership is unknown.
                if (created || facts_changed) {
                    const auto tint = observation->second.name_colour;
                    income->add_theme_color_override("font_color", Color(tint.r, tint.g, tint.b, 1));
                }
                if (created || facts_changed) income->set_text(String::utf8(observation->second.income_overlay.c_str()));
                if (created) income->add_theme_font_size_override("font_size", std::max(1,
                    presentation::ui::font_pixel_height(component->integer(data::ui::Field::font_point_size).value_or(0), camera.height)));
                const auto income_offset = component->vec2(data::ui::Field::offset).value_or(data::ui::Vec2{});
                const auto income_text_offset = component->vec2(data::ui::Field::text_offset).value_or(data::ui::Vec2{});
                auto position = anchor + Vector2(income_offset.x + income_text_offset.x,
                    -income_offset.y - income_text_offset.y) * ui_scale;
                const auto size = income->get_minimum_size();
                income->set_size(size);
                if (!component->flag(data::ui::Field::left_justified)) position.x -= size.x
                    * (component->flag(data::ui::Field::right_justified) ? 1.0F : unverified_default_name_justification);
                position.y -= size.y * unverified_name_vertical_anchor;
                income->set_position(position);
                income->set_modulate(Color(1, 1, 1, alpha));
                income->set_visible(income_layer_visible && alpha > 0);
            }
        }
        record.name_visible = false;
        // Show_Name gates the name control and its native hover target (#2370).
        // Omitted tags inherit the parser's false default: an unsourced mod seam.
        if (!art || art->show_name) {
            auto [entry, inserted] = planet_labels.try_emplace(planet->id, nullptr);
            if (inserted) {
                entry->second = memnew(Label);
                world_overlays->add_child(entry->second);
                entry->second->set_mouse_filter(Control::MOUSE_FILTER_PASS);
                entry->second->set_name(String::utf8(("planet_name_" + std::to_string(planet->id)).c_str()));
            }
            auto& label = *entry->second;
            auto& content_version = hud_label_content_versions[planet->id];
            // The content follows the planet's facts and the screen height. A label that was offscreen during an
            // earlier publication or resize takes the latest content from its own version on re-entry.
            const auto current_version = std::pair{key.facts, camera.height};
            if (inserted || content_version != current_version) {
                bool content_changed = false;
                const auto name = unverified_xml_name_fallback(art, planet->name);
                const auto text = String::utf8(name.c_str()).to_upper();
                if (label.get_text() != text) { label.set_text(text); content_changed = true; }
                auto selected_font = font;
                String tooltip;
                if (observation != hud_facts.end()) {
                    const auto& facts = observation->second;
                    tooltip = String::utf8(galactic_view::unverified_intelligence_text(facts).c_str());
                    const auto alternatives = style.list(data::ui::Field::alternate_font_name);
                    std::size_t frame = 0;
                    if (name_size_thresholds && facts.ground_level_known && facts.structures) {
                        frame = *facts.structures < (*name_size_thresholds)[0] ? 0
                            : (*facts.structures < (*name_size_thresholds)[1] ? 1 : 2);
                    }
                    if (frame < alternatives.size()) selected_font = hud_fonts->font(hud_fonts->resolve(
                        {alternatives[frame], name_font.point_size}, "english"));
                }
                if (label.get_tooltip_text() != tooltip) { label.set_tooltip_text(tooltip); content_changed = true; }
                // Theme overrides invalidate native shaping/layout even when a
                // clock-only publication supplies the same visible name style.
                if (!label.has_theme_font_override("font") || label.get_theme_font("font") != selected_font) {
                    label.add_theme_font_override("font", selected_font); content_changed = true;
                }
                const int pixels = std::max(1, font_height);
                if (!label.has_theme_font_size_override("font_size") || label.get_theme_font_size("font_size") != pixels) {
                    label.add_theme_font_size_override("font_size", pixels); content_changed = true;
                }
                const int outline = style.flag(data::ui::Field::text_outline) ? unverified_name_outline_pixels : 0;
                if (!label.has_theme_constant_override("outline_size") || label.get_theme_constant("outline_size") != outline) {
                    label.add_theme_constant_override("outline_size", outline); content_changed = true;
                }
                auto colour = style.color(data::ui::Field::text_color).value_or(data::ui::Rgba8{255,255,255,255});
                Color tint(static_cast<float>(colour.r) / 255.0F, static_cast<float>(colour.g) / 255.0F,
                    static_cast<float>(colour.b) / 255.0F, static_cast<float>(colour.a) / 255.0F);
                // WGUI-38/96: ordinary overview names use primary faction colour.
                // WGP-05/06 changes a separate, existing object name (WGUI-97).
                // Until observer knowledge is published, named campaigns show the
                // effective Neutral colour. A raw owner is not an intelligence fact.
                const auto* appearance = content ? galactic_view::affiliation_for_owner(*content, std::nullopt) : nullptr;
                if (observation != hud_facts.end() && observation->second.discovered && observation->second.observed_owner) {
                    tint.r = observation->second.name_colour.r;
                    tint.g = observation->second.name_colour.g;
                    tint.b = observation->second.name_colour.b;
                } else if (options.campaign == galactic_view::synthetic_campaign && observation == hud_facts.end()) {
                    // Explicit synthetic observer fixture; IDs are not the catalog's
                    // faction iteration order. The live view resolves its metadata.
                    tint.r = planet->colour.r;
                    tint.g = planet->colour.g;
                    tint.b = planet->colour.b;
                    tint.a = 1.0F;
                } else if (appearance) {
                    tint.r = appearance->primary.r;
                    tint.g = appearance->primary.g;
                    tint.b = appearance->primary.b;
                    tint.a = 1.0F;
                }
                // WGP-06/WGUI-97: unknown ownership is not observed Neutral. Resolve
                // secondary colour only from an explicit discovered owner fact.
                const bool observed_owner = observation != hud_facts.end() && observation->second.discovered
                    && observation->second.observed_owner.has_value();
                const auto* observed = observed_owner && content
                    ? galactic_view::affiliation_for_owner(*content, observation->second.observed_owner) : nullptr;
                galactic_view::update_object_name_colour(object_names[planet->id],
                    observed ? &observed->secondary : nullptr, observed_owner);
                if (!label.has_theme_color_override("font_color") || label.get_theme_color("font_color") != tint) {
                    label.add_theme_color_override("font_color", tint); content_changed = true;
                }
                if (content_changed) ++hud_label_content_updates;
                content_version = current_version;
            }
            label.set_modulate(Color(1, 1, 1, alpha));
            const auto measured = label.get_minimum_size();
            const auto size = label.get_size().max(label.get_combined_minimum_size());
            const auto placement = galactic_view::planet_label_placement(style, anchor.x, anchor.y, ui_scale,
                measured.x, measured.y, size.x, size.y);
            label.set_scale(Vector2(placement.scale, placement.scale));
            label.set_position(Vector2(placement.x, placement.y));
            label.set_visible(alpha > 0);
            if (alpha > 0) { visible_labels.push_back(planet->id); record.name_visible = true; }
        }
        record.key = key; record.placed = true; record.overlays = true;
        record.culled = false; record.reach = {};
        if (!dragged_fleet && !view.contains(projected.x, projected.y)) {
            record.reach = planet_control_bounds(planet->id, projected);
            record.culled = record.reach.outside(view, projected.x, projected.y, cull_margin);
        }
    }
    // A planet this pass did not visit (no facts, behind the camera, not in the snapshot) loses its pips and icons,
    // once; it is placed afresh when it returns.
    const auto retire = [&](const galactic_view::ObjectId id) {
        auto& record = planet_label_records[id];
        if (record.pass == planet_label_pass) return;
        if (record.overlays) hide_planet_overlays(id);
        record.overlays = false; record.placed = false;
    };
    for (const auto& [id, levels] : planet_levels) { static_cast<void>(levels); retire(id); }
    for (const auto& [id, icon] : corruption_info_icons) { static_cast<void>(icon); retire(id); }
    for (const auto& [key, icon] : status_effect_icons) { static_cast<void>(icon); retire(key.first); }
    for (const auto& [id, icon] : planet_bonus_icons) { static_cast<void>(icon); retire(id); }
    for (const auto& [id, icon] : planet_weather_icons) { static_cast<void>(icon); retire(id); }
    for (const auto& [id, label] : planet_corruption_labels) { static_cast<void>(label); retire(id); }
    for (auto& [id, record] : planet_label_records) if (record.pass != planet_label_pass) { static_cast<void>(id); record.placed = false; }
    std::sort(visible_labels.begin(), visible_labels.end());
    hide_unused_labels();
    income_labels_snapshot = displayed_snapshot;
    income_labels_height = camera.height;
}

Ref<Texture2D> GalacticMode::State::command_texture(const std::string_view name) {
    return command_texture_lookup(name, true);
}

Ref<Texture2D> GalacticMode::State::command_texture_optional(const std::string_view name) {
    return command_texture_lookup(name, false);
}

Ref<Texture2D> GalacticMode::State::command_atlas_texture(const std::string_view name) {
    if (!galactic_view::command_bar_icon_in_atlas(command_atlas ? &command_atlas->directory : nullptr, name)) return {};
    return command_texture_optional(name);
}

Ref<Texture2D> GalacticMode::State::command_texture_lookup(const std::string_view name, const bool warn_if_missing) {
    if (!filesystem || !command_textures || name.empty()) return {};
    if (const auto cached = command_texture_cache.find(name); cached != command_texture_cache.end()) return cached->second;
    auto slot = presentation::ui::resolve_ui_texture(name,
        command_atlas ? &command_atlas->directory : nullptr,
        presentation::ui::vfs_standalone_textures(*filesystem, warn_if_missing));
    // Publish missing required textures at the next HUD update; optional probes stay silent.
    if (warn_if_missing && slot.origin == presentation::ui::TextureOrigin::missing) {
        UtilityFunctions::push_warning(String::utf8(("unresolved mounted UI texture: " + std::string(name)).c_str()));
        pending_texture_notes.push_back(galactic_view::unresolved_ui_texture_note(name));
    }
    const auto texture = command_textures->texture(slot, 1.0, 1.0);
    command_texture_cache.emplace(std::string(name), texture);
    return texture;
}

Ref<Texture2D> GalacticMode::State::radar_view_texture(const std::string_view name) {
    return viewer::galactic::radar_texture_lookup(filesystem.get(),
        command_atlas ? &*command_atlas : nullptr,
        presentation::ui::command_bar_mega_texture(command_catalog), name,
        [&] { return command_texture(name); },
        [&](assets::MegaTextureAtlas& atlas, const presentation::ui::ThemeTexture& slot)
            -> std::optional<Ref<Texture2D>> {
            UiTextures textures(atlas, *filesystem);
            const auto texture = textures.texture(slot, 1.0, 1.0);
            if (texture.is_valid()) return texture;
            return std::nullopt;
        });
}

void GalacticMode::State::refresh_hud_facts() {
    if (!displayed_snapshot || !galaxy || hud_facts_snapshot == displayed_snapshot) return;
    if (!options.hud_verify) { refresh_hud_facts_pass(); return; }
    // Verification: the facts the HUD now shows against the facts rebuilt from nothing for the same publication.
    const auto before = hud_facts;
    refresh_hud_facts_pass();
    for (const auto& [id, facts] : hud_facts)
        if (const auto old = before.find(id); old == before.end() || !(old->second == facts)) ++hud_fact_changes;
    auto shown = hud_facts;
    hud_facts_cold = true; hud_facts_snapshot.reset(); hud_offers_owner.reset();
    refresh_hud_facts_pass();
    hud_facts_cold = false;
    ++hud_verify_checks;
    if (!(hud_facts == shown)) {
        ++hud_verify_mismatches;
        if (hud_verify_detail.empty()) {
            hud_verify_detail = "facts";
            for (const auto& [id, facts] : hud_facts)
                if (const auto kept = shown.find(id); kept == shown.end() || !(kept->second == facts)) {
                    hud_verify_detail += " planet " + std::to_string(id); break;
                }
        }
    }
    hud_facts = std::move(shown);
}

void GalacticMode::State::refresh_hud_facts_pass() {
    hud_facts_snapshot = displayed_snapshot;
    ++hud_fact_rebuilds;
    const bool fixture = fixture_enabled(GalacticHudFixture::Feature::intelligence);
    // An unavailable observer supplies no campaign facts. Explicit synthetic
    // fixtures remain isolated from the installed campaign producer.
    if (!displayed_snapshot->observer() && !fixture) {
        if (options.campaign != galactic_view::synthetic_campaign || !options.hud_provider) hud_facts.clear();
        return;
    }
    // #4331: the previous publication's facts stay readable while this one is built, so a planet whose offer rows are
    // the same column keeps the choices already copied from them.
    std::map<galactic_view::ObjectId, galactic_view::PlanetHudFacts> previous_facts;
    if (hud_facts_cold) hud_facts.clear(); else previous_facts.swap(hud_facts);
    // Index each publication once; planet count must not multiply scans of
    // the observation, numeric payload or presentation metadata arrays.
    std::unordered_map<galactic_view::ObjectId, const sim::galactic::ObservedPlanet*> observations;
    std::unordered_map<galactic_view::ObjectId, const sim::galactic::ObserverPlanet*> observed_planets;
    std::unordered_map<galactic_view::ObjectId, const sim::galactic::PlanetProduction*> productions;
    std::unordered_map<galactic_view::ObjectId, const galactic_view::PlanetView*> planet_metadata;
    if (!fixture) {
        observations.reserve(displayed_snapshot->observations().size());
        for (const auto& row : displayed_snapshot->observations())
            if (row.player == galaxy->local.player) observations.try_emplace(row.planet, &row);
        observed_planets.reserve(displayed_snapshot->observer()->planets.size());
        for (const auto& row : displayed_snapshot->observer()->planets) observed_planets.try_emplace(row.planet, &row);
        if (displayed_snapshot->production()) {
            productions.reserve(displayed_snapshot->production()->planets.size());
            for (const auto& row : displayed_snapshot->production()->planets) productions.try_emplace(row.planet, &row);
        }
    }
    planet_metadata.reserve(galaxy->planets.size());
    for (const auto& row : galaxy->planets) planet_metadata.try_emplace(row.id, &row);
    bool local_benefits_from_corruption = false;
    std::unordered_set<std::uint32_t> corruption_owners;
    if (const auto& observer = displayed_snapshot->observer()) for (const auto& player : observer->players) {
        if (player.player == galaxy->local.player) local_benefits_from_corruption = player.benefits_from_corruption;
        if (player.benefits_from_corruption) corruption_owners.insert(player.player);
    }
    // #4375 / WGSI-1..7: named heroes seen per planet, and the credit siphon of each planet (the retail row reads it for
    // every discovered planet, ungated by fog).
    std::unordered_map<galactic_view::ObjectId, sim::galactic::ObservedPlanetHeroes> planet_heroes;
    std::unordered_map<galactic_view::ObjectId, const sim::galactic::CorruptionEconomyPlanet*> siphons;
    if (!fixture) {
        for (auto& row : sim::galactic::observed_planet_heroes(*displayed_snapshot, galaxy->local.player, fleet_display_traits,
                 galactic_view::live_fleet_visibility(*displayed_snapshot)))
            planet_heroes.try_emplace(row.planet, std::move(row));
        if (const auto* corruption = displayed_snapshot->economy_corruption())
            for (const auto& row : corruption->planets) if (row.siphon.active) siphons.try_emplace(row.planet, &row);
    }
    // WGUI-91: admission still comes from the publication. Index its offers
    // once, retaining source order for equal authored type-manager positions.
    // hud_offers_owner retains the immutable rows while the index points into them (#4331);
    // the name/art maps are unchanged while the choices copy their fields.
    const auto published_offers = displayed_snapshot->production_offers();
    hud_offer_max_rows = std::max(hud_offer_max_rows, static_cast<std::uint64_t>(published_offers.size()));
    const bool offers_rebuilt = !hud_offers_owner || hud_offers_data != published_offers.data()
        || hud_offers_size != published_offers.size() || hud_offers_player != galaxy->local.player;
    if (offers_rebuilt) {
        ++hud_offer_index_builds;
        hud_offers.clear();
        hud_offers.reserve(published_offers.size());
        for (const auto& row : published_offers) {
            ++hud_offer_rows_visited;
            if (row.player != galaxy->local.player) continue;
            auto [type, inserted] = hud_offer_types.try_emplace(row.type);
            if (inserted) {
                const auto name = production_names.find(row.type);
                const auto art = name == production_names.end() ? production_art.end() : production_art.find(name->second);
                if (art != production_art.end()) type->second = HudOfferType{&name->second, &art->second};
            }
            if (type->second) hud_offers.push_back({&row, type->second->name, type->second->art});
        }
        const auto before = [](const auto& a, const auto& b) {
            if (a.row->planet != b.row->planet) return a.row->planet < b.row->planet;
            return a.art->type_order < b.art->type_order;
        };
        // The published rows usually arrive in this order already; a stable sort of a sorted run changes nothing.
        if (!std::is_sorted(hud_offers.begin(), hud_offers.end(), before))
            std::stable_sort(hud_offers.begin(), hud_offers.end(), before);
        hud_offers_owner = displayed_snapshot; hud_offers_data = published_offers.data();
        hud_offers_size = published_offers.size(); hud_offers_player = galaxy->local.player;
    }
    const auto& offers = hud_offers;
    for (const auto& location : displayed_snapshot->planets()) {
        sim::galactic::ObservedPlanet observation;
        observation.player = galaxy->local.player; observation.planet = location.id;
        const sim::galactic::ObserverPlanet* observed_planet = nullptr;
        const sim::galactic::PlanetProduction* production = nullptr;
        sim::galactic::ObserverPlanet fixture_planet;
        sim::galactic::PlanetProduction fixture_production;
        std::optional<double> basic_income, actual_income;
        if (fixture) {
            options.hud_provider->observation(*this, observation, fixture_planet, fixture_production, basic_income, actual_income, location.id);
            observed_planet = &fixture_planet; production = &fixture_production;
        } else {
            if (const auto row = observations.find(location.id); row != observations.end()) observation = *row->second;
            if (const auto row = observed_planets.find(location.id); row != observed_planets.end()) observed_planet = row->second;
            if (const auto row = productions.find(location.id); row != productions.end()) production = row->second;
            bool destroyed = false;
            if (const auto* control = displayed_snapshot->control()) {
                const auto row = std::lower_bound(control->planets.begin(), control->planets.end(), location.id,
                    [](const auto& planet, const auto id) { return planet.planet < id; });
                destroyed = row != control->planets.end() && row->planet == location.id && row->destroyed;
            }
            const auto& bases = destroyed ? destroyed_planet_income : basic_planet_income;
            if (const auto row = bases.find(location.id); row != bases.end()) basic_income = row->second;
            // observer_actual_income: WGEP-05..09 needs retained base/black-market,
            // alignment, political, trade, structure and siphon contributions.
            // IncomeState's mining slice alone is not the actual-income getter.
        }
        auto facts = galactic_view::observed_hud_facts(observation, observed_planet, production,
            galaxy->local.player, basic_income, actual_income);
        facts.local_benefits_from_corruption = local_benefits_from_corruption;
        facts.hero_red_green = options.red_green_routes;
        if (const auto hero = planet_heroes.find(location.id); hero != planet_heroes.end()) {
            facts.hero_ally = hero->second.ally;
            facts.hero_foreign = hero->second.foreign;
            for (const auto& listed : hero->second.heroes) facts.hero_tips.push_back({listed.owner, listed.type, listed.allied});
            // The three faction icons are the command bar's good, evil and corrupt sides (WGSI-4).
            for (const auto owner : hero->second.owners) {
                const auto* appearance = content ? galactic_view::affiliation_for_owner(*content, owner) : nullptr;
                if (!appearance || !route_styles) continue;
                switch (galactic_view::hero_side(appearance->faction, route_styles->good_faction, route_styles->evil_faction,
                            route_styles->corrupt_faction)) {
                    case galactic_view::HeroSide::good: facts.hero_good = true; break;
                    case galactic_view::HeroSide::evil: facts.hero_evil = true; break;
                    case galactic_view::HeroSide::corrupt: facts.hero_corrupt = true; break;
                    case galactic_view::HeroSide::none: break;
                }
            }
        }
        // WGSI-5: the local player's own siphon draws the smuggler icon, anyone else's the smuggled icon.
        if (const auto siphon = siphons.find(location.id); siphon != siphons.end() && facts.discovered) {
            if (siphon->second->siphon.recipient == galaxy->local.player) facts.siphon_source = true;
            else facts.siphon_smuggled = true;
        }
        // GHUDL-15: debug build corrupts a planet when a corruption-benefiting
        // faction owns it, at start and on transfer. The sim does not publish
        // that start state yet, so a known benefiting owner stands for it.
        if (facts.observed_owner && corruption_owners.contains(static_cast<std::uint32_t>(*facts.observed_owner)))
            facts.corrupted = true;
        // The per-type icon reads the published corruption fact of the same planet (current type).
        if (const auto fact = corruption_fact_index.find(location.id); fact != corruption_fact_index.end() && fact->second->transitioning) {
            facts.corruption_percent = galactic_view::corruption_transition_percent(fact->second->fraction);
            facts.corruption_removing = !fact->second->increasing;
        }
        if (const auto fact = corruption_fact_index.find(location.id); fact != corruption_fact_index.end() && facts.corrupted)
            facts.corruption_type = fact->second->type;
        // An observer-filtered fiscal row supersedes the public base display.
        // WGUI-13 basic/actual fallback applies only to an admitted provider row.
        if (const auto* forecast = displayed_snapshot->economy_forecast(); forecast && forecast->player == galaxy->local.player) {
            const auto row = std::lower_bound(forecast->planets.begin(), forecast->planets.end(), location.id,
                [](const auto& value, auto id) { return value.planet < id; });
            if (row != forecast->planets.end() && row->planet == location.id && facts.discovered)
                facts.visible_income = static_cast<double>(row->actual.value_or(row->basic).raw()) / sim::math::Fixed::scale;
            // WGSI-11: the sim publishes the siphoned credits only to the siphon's recipient.
            if (row != forecast->planets.end() && row->planet == location.id && facts.siphon_source && row->siphoned)
                facts.siphoned_credits = galactic_view::siphon_tooltip_credits(*row->siphoned);
        }
        if (facts.visible_income) facts.income_overlay = galactic_view::income_overlay_text(*facts.visible_income);
        // #3931: the galaxy reveal also shows another player's queue, read-only.
        const auto describe_type = [this](const sim::galactic::TypeId type) {
            const auto name = production_names.find(type);
            const auto art = name == production_names.end() ? production_art.end() : production_art.find(name->second);
            return std::pair{name == production_names.end() ? std::string{} : name->second,
                art == production_art.end() ? std::string{} : art->second.icon};
        };
        if (galactic_view::publish_production_queue(facts, observation, production, options.galactic_reveal, describe_type)) {
            facts.maximum_queue = displayed_snapshot->production()->maximum_human_queue;
            // WGUI-91: admission is a sim projection, never reconstructed here.
            // The same offer column as the last publication's facts were built from, and this planet's queue was
            // published then too: its choices are exactly those, so they move over instead of being copied again.
            const auto before = offers_rebuilt || fixture ? previous_facts.end() : previous_facts.find(location.id);
            if (before != previous_facts.end() && before->second.space_queue) {
                facts.choices = std::move(before->second.choices);
            } else {
                const auto first = std::lower_bound(offers.begin(), offers.end(), location.id,
                    [](const auto& offer, const auto id) { return offer.row->planet < id; });
                auto last = first;
                while (last != offers.end() && last->row->planet == location.id) ++last;
                facts.choices.reserve(static_cast<std::size_t>(last - first));
                for (auto entry = first; entry != last; ++entry) {
                    auto choice = galactic_view::production_choice_facts(*entry->row);
                    choice.type = *entry->name;
                    choice.build_tab = entry->art->build_tab;
                    choice.base_type = entry->art->base_filter;
                    choice.gui_row = entry->art->gui_row; choice.icon = entry->art->icon;
                    facts.choices.push_back(std::move(choice));
                }
            }
        }
        const auto metadata_planet = planet_metadata.find(location.id);
        if (facts.discovered && metadata_planet != planet_metadata.end()) {
            const auto art = content ? content->art.find(location.id) : std::map<galactic_view::ObjectId, galactic_view::PlanetArt>::const_iterator{};
            facts.display_name = unverified_xml_name_fallback(content && art != content->art.end() ? &art->second : nullptr,
                metadata_planet->second->name);
        }
        const auto* owner = content ? galactic_view::affiliation_for_owner(*content, facts.observed_owner) : nullptr;
        if (owner) {
            facts.name_colour = owner->primary;
        } else facts.name_colour = {100.0F / 255.0F, 100.0F / 255.0F, 100.0F / 255.0F};
        // WGUI-100: tooltip badge follows apparent allegiance, while WGUI-96
        // overview tint continues to require current ownership knowledge.
        // A planet the seat sees with no controller is Neutral, as in retail; an empty apparent allegiance alone is unknown
        // (not visible) and shows no badge (#6985).
        if (content && (facts.apparent_allegiance_owner || facts.apparent_allegiance_none)) {
            const auto* badge = galactic_view::affiliation_for_owner(*content, facts.apparent_allegiance_owner);
            if (badge) facts.observed_faction = badge->faction;
        }
        if (options.campaign == galactic_view::synthetic_campaign) {
            // Synthetic IDs deliberately do not follow catalog faction ordering.
            for (const auto& player : metadata.players) {
                if (facts.observed_owner == std::optional(player.id))
                    facts.name_colour = {player.colour[0] / 255.0F, player.colour[1] / 255.0F, player.colour[2] / 255.0F};
                if (facts.apparent_allegiance_owner == std::optional(player.id)) facts.observed_faction = player.faction;
            }
        }
        hud_facts.emplace(location.id, std::move(facts));
    }
    if (const auto selected = hud_facts.find(selected_planet);
        selected != hud_facts.end() && !selected->second.discovered) {
        selected_planet = 0; map_input.selected = 0; ++selection_invalidations;
    }
    // WGUI-05: a discovery change swaps that planet's alternate model, so resubmit once per change.
    std::vector<galactic_view::ObjectId> ids;
    ids.reserve(galaxy->planets.size());
    for (const auto& planet : galaxy->planets) ids.push_back(planet.id);
    auto undiscovered = galactic_view::undiscovered_planets(ids, scene_objects,
        [this](const galactic_view::ObjectId id) { return planet_discovered(id); });
    if (undiscovered != undiscovered_planets) {
        undiscovered_planets = std::move(undiscovered);
        if (renderer) submit();
    }
}

void GalacticMode::State::update_planet_levels(const galactic_view::ObjectId id,
    const Vector2 anchor, const float scale, const float alpha) {
    const auto found = hud_facts.find(id);
    if (!base_levels_visible || found == hud_facts.end() || !found->second.discovered || alpha <= 0.0F) return;
    const auto& facts = found->second;
    auto [entry, inserted] = planet_levels.try_emplace(id);
    if (inserted) {
        const auto make = [&] {
            auto* image = memnew(TextureRect);
            image->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
            image->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
            world_overlays->add_child(image);
            return image;
        };
        for (auto& image : entry->second.ground) image = make();
        for (auto& image : entry->second.space) image = make();
        entry->second.political = make();
    }
    const auto pips = [&](auto& images, std::string_view component_name,
        const std::optional<std::int64_t> capacity, const std::optional<std::int64_t> level) {
        const auto* component = command_catalog.find(component_name);
        if (!component || !capacity) return;
        const auto textures = component->list(data::ui::Field::icon_alternate_texture_name);
        if (textures.empty()) return;
        const auto offset = component->vec2(data::ui::Field::default_offset).value_or(data::ui::Vec2{});
        // GHUDL-14: enabling credits shifts both pip rows by the income
        // component's authored Y offset, leaving room for its text row.
        const auto* income = income_layer_visible ? command_catalog.find("g_planet_value") : nullptr;
        const float income_y = income ? income->vec2(data::ui::Field::offset).value_or(data::ui::Vec2{}).y : 0.0F;
        const auto texture = command_texture(textures.front());
        if (texture.is_null()) return;
        const auto size = texture->get_size() * galactic_view::command_bar_component_scale(*component, scale);
        const auto positions = std::min(images.size(), static_cast<std::size_t>(std::max<std::int64_t>(0, *capacity)));
        for (std::size_t index = 0; index < images.size(); ++index) {
            auto* image = images[index];
            image->set_visible(index < positions);
            image->set_texture(texture);
            image->set_size(size);
            // GHUDL-14 / #3265: retail's drag point is the pip centre, as it
            // is for the status row. Treating it as a corner lowered the
            // level artwork into the hammer and shifted the whole row right.
            image->set_position(anchor + Vector2(offset.x * scale + size.x * static_cast<float>(index),
                -(offset.y + income_y) * scale) - size * 0.5F);
            // WGUI-39: unknown fills zero, while the fact remains absent for hover.
            const bool filled = level && static_cast<std::int64_t>(index) < *level;
            const auto tint = facts.name_colour;
            const float intensity = filled ? 1.0F : 0.3F;
            image->set_modulate(Color(tint.r * intensity, tint.g * intensity, tint.b * intensity, alpha));
        }
    };
    pips(entry->second.ground, "g_ground_level_pips", facts.ground_capacity, facts.ground_level);
    pips(entry->second.space, "g_space_level_pips", facts.space_capacity, facts.space_level);
    auto* political = entry->second.political;
    const auto* component = command_catalog.find("g_political_control");
    if (!component || !facts.political_control) return;
    const auto textures = component->list(data::ui::Field::bar_overlay_name);
    if (textures.empty()) return;
    const auto index = static_cast<std::size_t>(std::clamp(*facts.political_control, 0.0,
        static_cast<double>(textures.size() - 1)));
    const auto texture = command_texture(textures[index]);
    if (texture.is_null()) return;
    const auto offset = component->vec2(data::ui::Field::default_offset).value_or(data::ui::Vec2{});
    political->set_texture(texture); political->set_size(texture->get_size() * scale);
    political->set_position(anchor + Vector2(offset.x, -offset.y) * scale);
    political->set_modulate(Color(1, 1, 1, alpha)); political->show();
}

void GalacticMode::State::update_planet_bonus_icon(const galactic_view::ObjectId id, const std::string& icon,
    const Vector2 anchor, const float scale, const float alpha) {
    // GHUDL-18: visibility follows the third map filter; tint follows known ownership.
    const auto facts = hud_facts.find(id);
    // The atlas lookup is only needed while the layer is on (the style hides everything otherwise).
    const bool in_atlas = bonus_layer_visible
        && galactic_view::command_bar_icon_in_atlas(command_atlas ? &command_atlas->directory : nullptr, icon);
    const auto style = galactic_view::planet_bonus_icon_style(bonus_layer_visible, icon, in_atlas, alpha,
        facts == hud_facts.end() ? nullptr : &facts->second.name_colour);
    if (!style.visible) return;
    const auto* component = command_catalog.find("g_planet_ability");
    if (!component) return;
    const Ref<Texture2D> texture = command_texture(icon);
    if (texture.is_null()) return;
    auto*& slot = planet_bonus_icons[id];
    if (!slot) {
        slot = memnew(TextureRect);
        slot->set_mouse_filter(Control::MOUSE_FILTER_PASS);
        slot->set_name(String::utf8(("planet_bonus_" + std::to_string(id)).c_str()));
        slot->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE); world_overlays->add_child(slot);
    }
    const auto offset = component->vec2(data::ui::Field::offset).value_or(data::ui::Vec2{});
    const auto size = texture->get_size();
    const auto rect = galactic_view::planet_bonus_icon_rect(anchor.x, anchor.y, offset.x, offset.y,
        galactic_view::command_bar_component_scale(*component, scale), size.x, size.y);
    slot->set_texture(texture);
    slot->set_size({static_cast<float>(rect.width), static_cast<float>(rect.height)});
    slot->set_position({static_cast<float>(rect.x), static_cast<float>(rect.y)});
    slot->set_modulate(Color(style.tint.r, style.tint.g, style.tint.b, alpha));
    slot->show();
}

void GalacticMode::State::update_planet_weather_icon(const galactic_view::ObjectId id, const std::string& icon,
    const Vector2 anchor, const float scale, const float alpha) {
    // WGFI-1: the weather icon follows the same filter bit as the bonus icon, sits at the planet's point plus the `g_weather`
    // offset and is never tinted (the original colours only the bonus icon); a name outside the command-bar atlas draws nothing.
    const bool in_atlas = bonus_layer_visible
        && galactic_view::command_bar_icon_in_atlas(command_atlas ? &command_atlas->directory : nullptr, icon);
    constexpr galactic_view::Rgb untinted{1.0F, 1.0F, 1.0F};
    const auto style = galactic_view::planet_bonus_icon_style(bonus_layer_visible, icon, in_atlas, alpha, &untinted);
    if (!style.visible) return;
    const auto* component = command_catalog.find("g_weather");
    if (!component) return;
    const Ref<Texture2D> texture = command_texture(icon);
    if (texture.is_null()) return;
    auto*& slot = planet_weather_icons[id];
    if (!slot) {
        slot = memnew(TextureRect);
        slot->set_mouse_filter(Control::MOUSE_FILTER_PASS);
        slot->set_name(String::utf8(("planet_weather_" + std::to_string(id)).c_str()));
        slot->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE); world_overlays->add_child(slot);
    }
    const auto offset = component->vec2(data::ui::Field::offset).value_or(data::ui::Vec2{});
    const auto size = texture->get_size();
    const auto rect = galactic_view::planet_bonus_icon_rect(anchor.x, anchor.y, offset.x, offset.y,
        galactic_view::command_bar_component_scale(*component, scale), size.x, size.y);
    slot->set_texture(texture);
    slot->set_size({static_cast<float>(rect.width), static_cast<float>(rect.height)});
    slot->set_position({static_cast<float>(rect.x), static_cast<float>(rect.y)});
    slot->set_modulate(Color(1.0F, 1.0F, 1.0F, alpha));
    slot->show();
}

void GalacticMode::State::update_planet_corruption_text(const galactic_view::ObjectId id, const Vector2 anchor,
    const float scale, const float alpha) {
    // WGFI-3: while a corruption transition runs the planet shows "Corrupting: n%" (or "Removing Corruption: n%") at the
    // authored `g_corruption_text` offset, in its authored font and colour, whatever the filters. Hidden otherwise.
    const auto facts = hud_facts.find(id);
    const auto* component = command_catalog.find("g_corruption_text");
    if (facts == hud_facts.end() || !facts->second.corruption_percent || !component || !battle_text || !hud_fonts || alpha <= 0.0F) return;
    const auto format = battle_text(facts->second.corruption_removing ? "TEXT_CORRUPTION_REMOVE_PERCENT" : "TEXT_CORRUPTION_PERCENT");
    if (format.empty()) return;
    const std::vector<std::string> arguments{std::to_string(*facts->second.corruption_percent)};
    const auto text = galactic_view::expand_printf_notice(format, arguments);
    auto*& label = planet_corruption_labels[id];
    if (!label) {
        label = memnew(Label); label->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        label->set_name(String::utf8(("planet_corruption_" + std::to_string(id)).c_str()));
        world_overlays->add_child(label);
        const auto face = hud_fonts->resolve({std::string(component->text(data::ui::Field::font_name)),
            component->integer(data::ui::Field::font_point_size).value_or(0)}, "english");
        label->add_theme_font_override("font", hud_fonts->font(face));
        label->add_theme_constant_override("outline_size", component->flag(data::ui::Field::text_outline)
            ? unverified_name_outline_pixels : 0);
        label->add_theme_color_override("font_outline_color", Color(0, 0, 0, 1));
        const auto tint = component->color(data::ui::Field::text_color).value_or(data::ui::Rgba8{255, 255, 255, 255});
        label->add_theme_color_override("font_color", Color(tint.r / 255.0F, tint.g / 255.0F, tint.b / 255.0F, 1));
    }
    label->add_theme_font_size_override("font_size", std::max(1,
        presentation::ui::font_pixel_height(component->integer(data::ui::Field::font_point_size).value_or(0), camera.height)));
    label->set_text(String::utf8(text.c_str()));
    const auto offset = component->vec2(data::ui::Field::offset).value_or(data::ui::Vec2{});
    const auto text_offset = component->vec2(data::ui::Field::text_offset).value_or(data::ui::Vec2{});
    auto position = anchor + Vector2(offset.x + text_offset.x, -offset.y - text_offset.y) * scale;
    const auto size = label->get_minimum_size();
    label->set_size(size);
    if (!component->flag(data::ui::Field::left_justified)) position.x -= size.x
        * (component->flag(data::ui::Field::right_justified) ? 1.0F : unverified_default_name_justification);
    position.y -= size.y * unverified_name_vertical_anchor;
    label->set_position(position);
    label->set_modulate(Color(1, 1, 1, alpha));
    label->show();
}

void GalacticMode::State::update_planet_status_icons(const galactic_view::ObjectId id,
    const Vector2 anchor, const float scale, const float alpha) {
    // GHUDL-15: debug build lays the planet's status icons out as one row from
    // the conflict component's offset, shifted below the credit and base-icon
    // rows when those layers are on. The row sits under the name, never above.
    const auto found = hud_facts.find(id);
    const auto* conflict_style = command_catalog.find("g_conflict");
    if (found == hud_facts.end() || !conflict_style || alpha <= 0.0F) return;
    const auto kinds = galactic_view::planet_status_icons(found->second, bonus_layer_visible);
    if (kinds.empty()) return;
    const auto authored = [&](const char* name, data::ui::Field field) -> std::optional<data::ui::Vec2> {
        const auto* component = command_catalog.find(name);
        return component ? component->vec2(field) : std::nullopt;
    };
    const auto component_list = [&](const char* name, data::ui::Field field) {
        const auto* component = command_catalog.find(name);
        if (!component) return std::vector<std::string>{};
        const auto list = component->list(field);
        return std::vector<std::string>(list.begin(), list.end());
    };
    const auto build_icon_tokens = [&] {
        const auto* component = command_catalog.find("g_build");
        if (!component) return std::vector<std::string>{};
        const auto list = component->list(data::ui::Field::icon_texture_name);
        return std::vector<std::string>(list.begin(), list.end());
    };
    std::optional<double> credit_y, base_y;
    if (income_layer_visible) if (const auto offset = authored("g_planet_value", data::ui::Field::offset)) credit_y = offset->y;
    if (base_levels_visible) if (const auto offset = authored("g_ground_icon", data::ui::Field::default_offset)) base_y = offset->y;
    const auto conflict_offset = conflict_style->vec2(data::ui::Field::offset).value_or(data::ui::Vec2{});
    const auto origin = galactic_view::status_row_origin(conflict_offset.x, conflict_offset.y, credit_y, base_y);
    struct Entry final { TextureRect* image{}; Ref<Texture2D> texture; float authored_scale{}; };
    std::vector<Entry> entries;
    std::vector<double> widths;
    for (const auto kind : kinds) {
        // Debug build reads the information slot by a literal index after the
        // eight per-type icons; the authored list may be shorter in a mod.
        const bool build = kind == galactic_view::PlanetStatusIcon::build;
        const auto effect_component = galactic_view::status_effect_component_name(kind);
        const bool effect = !effect_component.empty();
        auto* icons = build ? &production_icons : &corruption_info_icons;
        // Hero icons draw an alternate texture of the authored `g_hero` list (WGSI-3); the smuggler, bounty and smuggled icons
        // draw their own component's first texture.
        auto name = effect ? (galactic_view::is_hero_status_icon(kind)
                ? galactic_view::hero_status_icon_name(kind, component_list("g_hero", data::ui::Field::icon_alternate_texture_name))
                : galactic_view::status_effect_icon_name(kind, component_list(effect_component.c_str(), data::ui::Field::icon_texture_name)))
            : galactic_view::status_icon_texture_name(kind,
                build_icon_tokens(), corruption_planet_icons, found->second.corruption_type);
        const Ref<Texture2D> texture = name.empty() ? Ref<Texture2D>{} : command_texture(name);
        // No texture, no map entry: the hide loop and report dereference every slot.
        if (texture.is_null()) continue;
        auto*& slot = effect ? status_effect_icons[{id, static_cast<std::uint8_t>(kind)}] : (*icons)[id];
        if (!slot) {
            slot = memnew(TextureRect);
            slot->set_mouse_filter(build ? Control::MOUSE_FILTER_STOP : Control::MOUSE_FILTER_IGNORE);
            if (build) slot->set_name(String::utf8(("production_activity_" + std::to_string(id)).c_str()));
            if (!build && !effect) slot->set_name(String::utf8(("planet_corruption_icon_" + std::to_string(id)).c_str()));
            if (effect) slot->set_name(String::utf8(("planet_status_" + std::to_string(id) + "_"
                + std::to_string(static_cast<unsigned>(kind))).c_str()));
            slot->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE); world_overlays->add_child(slot);
        }
        if (!build && !effect) {
            // WGCI-1/2: both corruption icons answer the pointer; the information icon carries the authored plain tooltip
            // line, the per-type icon none (its popup is service_encyclopedia's).
            slot->set_mouse_filter(Control::MOUSE_FILTER_STOP);
            String text;
            if (kind == galactic_view::PlanetStatusIcon::corruption_info && battle_text)
                text = String::utf8(battle_text(galactic_view::corruption_information_tooltip_key).c_str());
            if (slot->get_tooltip_text() != text) slot->set_tooltip_text(text);
        }
        if (build) slot->set_meta("production_texture", String::utf8(name.c_str()));
        if (effect) {
            // #7177 / WGSI-8..11: the hero, smuggled, bounty-hunter and smuggler icons answer the pointer (the hero tooltip
            // is service_hero_tooltip's; the others show their authored Tooltip_Text as one line, the smuggler's with the
            // siphoned credits in its %d).
            const bool hero_icon = galactic_view::is_hero_status_icon(kind);
            const bool text_icon = kind == galactic_view::PlanetStatusIcon::smuggled || kind == galactic_view::PlanetStatusIcon::bounty_hunter;
            const bool smuggler_icon = kind == galactic_view::PlanetStatusIcon::smuggler;
            slot->set_mouse_filter(hero_icon || text_icon || smuggler_icon ? Control::MOUSE_FILTER_STOP : Control::MOUSE_FILTER_IGNORE);
            if (text_icon && slot->get_tooltip_text().is_empty()) {
                const auto* style = command_catalog.find(effect_component);
                const auto keys = style ? style->list(data::ui::Field::tooltip_text) : std::span<const std::string>{};
                if (!keys.empty() && battle_text) slot->set_tooltip_text(String::utf8(battle_text(keys.front()).c_str()));
            }
            if (smuggler_icon) {
                const auto* style = command_catalog.find(effect_component);
                const auto keys = style ? style->list(data::ui::Field::tooltip_text) : std::span<const std::string>{};
                const auto& credits = found->second.siphoned_credits;
                String text;
                if (!keys.empty() && battle_text && credits) {
                    const std::vector<std::string> arguments{std::to_string(*credits)};
                    text = String::utf8(galactic_view::expand_printf_notice(battle_text(keys.front()), arguments).c_str());
                }
                if (slot->get_tooltip_text() != text) slot->set_tooltip_text(text);
            }
        }
        const auto* component = command_catalog.find(effect_component.empty()
            ? (build ? "g_build" : "g_corruption_icon") : effect_component);
        const float icon_scale = component ? galactic_view::command_bar_component_scale(*component, 1.0F) : 1.0F;
        entries.push_back({slot, texture, icon_scale});
        widths.push_back(texture->get_size().x * icon_scale);
    }
    const auto centres = galactic_view::status_row_centres(origin.x, widths);
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto size = entries[index].texture->get_size() * scale * entries[index].authored_scale;
        const Vector2 centre = anchor + Vector2(static_cast<float>(centres[index]), static_cast<float>(-origin.y)) * scale;
        entries[index].image->set_texture(entries[index].texture);
        entries[index].image->set_size(size);
        entries[index].image->set_position(centre - size * 0.5F);
        entries[index].image->set_modulate(Color(1, 1, 1, alpha));
        entries[index].image->show();
    }
}

bool GalacticMode::State::configure_command_bar_constants(const pugi::xml_document& document) {
    std::array<std::size_t, 2> thresholds{};
    std::size_t threshold_index = 0;
    for (const auto tag : {"Num_Structures_For_Medium_Planet_Name", "Num_Structures_For_Large_Planet_Name"}) {
        const std::string value = document.document_element().child(tag).text().as_string();
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), thresholds[threshold_index++]);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
            failure = "planet name thresholds require nonnegative integer values"; return false;
        }
    }
    if (thresholds[0] > thresholds[1]) { failure = "planet name thresholds are out of order"; return false; }
    name_size_thresholds = thresholds;
    advisor_character_seconds = galactic_view::advisor_hint_duration(
        document.document_element().child("Advisor_Hint_Duration").text().as_string()).value_or(-1);
    // Negative is this adapter's unavailable marker, never a replacement duration.
    // GFC-28: the hint interval in galactic frames; WGUI-87: the Droid Log date and separator colours. Missing values leave
    // the producer silent or the archive line in its default colour; nothing is substituted.
    hint_interval = galactic_view::hint_interval_frames(
        document.document_element().child("Advisor_Hint_Interval").text().as_string(), sim::galactic::galactic_frames_per_second);
    droid_date_colour = galactic_view::authored_rgba(document.document_element().child("Droid_Date_Color").text().as_string());
    droid_separator_colour = galactic_view::authored_rgba(
        document.document_element().child("Droid_Seperator_Color").text().as_string());
    ability_legend_icons = galactic_view::hint_keys(document.document_element().child("Planet_Ability_Icon_Names").text().as_string());
    ability_legend_text_ids = galactic_view::hint_keys(document.document_element().child("Planet_Ability_Text_IDs").text().as_string());
    ability_legend_rgb.clear();
    for (const auto& token : galactic_view::hint_keys(document.document_element().child("Planet_Ability_RGBs").text().as_string())) {
        int value{};
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()) { ability_legend_rgb.clear(); break; }
        ability_legend_rgb.push_back(value);
    }
    {
        // GHUD-S72: pointer tooltip delay is milliseconds, independent of campaign pause.
        // WGUI-40: the retail gate waits only for a positive signed delay.
        std::istringstream tooltip_delay_stream(document.document_element().child("Tooltip_Delay").text().as_string());
        // GHUD-S89: the debug build resets the constant to 500 ms before XML loading.
        std::int32_t tooltip_delay_value{500};
        const bool valid_delay = static_cast<bool>(tooltip_delay_stream >> tooltip_delay_value);
        tooltip_delay_stream >> std::ws;
        if (!valid_delay || !tooltip_delay_stream.eof()) {
            tooltip_delay_value = 500;
        }
        production_tooltip_delay = static_cast<std::uint64_t>(std::max(0, tooltip_delay_value));
    }
    {
        // GHUDL-15: per-type corruption icons, then the information icon.
        std::string icons = document.document_element().child("Corruption_Planet_Icon").text().as_string();
        std::replace(icons.begin(), icons.end(), ',', ' ');
        std::istringstream names(icons);
        corruption_planet_icons.clear();
        for (std::string icon; names >> icon;) corruption_planet_icons.push_back(icon);
        // WGCI-1: the per-type icon's popup reads three more authored lists, each by the same type index.
        const auto read_list = [&](const char* tag, std::vector<std::string>& out) {
            std::string text = document.document_element().child(tag).text().as_string();
            std::replace(text.begin(), text.end(), ',', ' ');
            std::istringstream stream(text);
            out.clear();
            for (std::string token; stream >> token;) out.push_back(token);
        };
        read_list("Corruption_Mission_Requirement_Icon_Name", corruption_requirement_icons);
        read_list("Corruption_Planet_Icon_Encyclopedia_Name", corruption_icon_names);
        read_list("Corruption_Planet_Icon_Encyclopedia_Desc", corruption_icon_descriptions);
    }
    return true;
}

bool GalacticMode::State::configure_command_bar_encyclopedia() {
    for (const auto name : {"encyclopedia_header_text", "encyclopedia_text"}) {
        const auto* component = command_catalog.find(name);
        if (!component || component->text(data::ui::Field::font_name).empty()
            || !component->integer(data::ui::Field::font_point_size)
            || !component->color(data::ui::Field::text_color)) {
            failure = "encyclopedia labels require authored font and colour components"; return false;
        }
    }
    encyclopedia_globals = presentation::ui::encyclopedia_constants(*filesystem);
    for (const auto& diagnostic : encyclopedia_globals.diagnostics)
        UtilityFunctions::push_warning(String::utf8(core::format_diagnostic(diagnostic).c_str()));
    if (!encyclopedia_globals.timing) {
        failure = "encyclopedia timing requires authored nonnegative values"; return false;
    }
    encyclopedia_timing = *encyclopedia_globals.timing;

    return true;
}

void GalacticMode::State::build_command_bar_tooltip() {
    // The native tooltip is a named text/layout adapter (GPH-01), but its
    // background and font still come from the authored command-bar components.
    Ref<Theme> tooltip_theme;
    tooltip_theme.instantiate();
    if (const auto* background = command_catalog.find("tooltip_back")) {
        const auto textures = background->list(data::ui::Field::blank_texture_name);
        if (!textures.empty()) {
            Ref<StyleBoxTexture> panel;
            panel.instantiate(); panel->set_texture(command_texture(textures.front()));
            tooltip_theme->set_stylebox("panel", "TooltipPanel", panel);
        }
    }
    if (const auto* style = command_catalog.find("tooltip_left_text")) {
        const auto face = hud_fonts->resolve({std::string(style->text(data::ui::Field::font_name)),
            style->integer(data::ui::Field::font_point_size).value_or(0)}, "english");
        tooltip_theme->set_font("font", "TooltipLabel", hud_fonts->font(face));
        tooltip_theme->set_font_size("font_size", "TooltipLabel",
            std::max(1, presentation::ui::font_pixel_height(face.point_size, camera.height)));
    }
    hud->set_theme(tooltip_theme);
    production_tooltip = memnew(Control);
    production_tooltip->set_name("GalacticConstructionTooltip");
    production_tooltip->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    production_tooltip->set_z_index(10);
    hud->add_child(production_tooltip);
    const auto tooltip_image = [&] {
        auto* result = memnew(TextureRect);
        result->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        result->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
        production_tooltip->add_child(result);
        return result;
    };
    production_tooltip_frame = tooltip_image();
    production_tooltip_back = tooltip_image();
    if (const auto* background = command_catalog.find("tooltip_back")) {
        const auto backs = background->list(data::ui::Field::blank_texture_name);
        const auto tooltip_frames = background->list(data::ui::Field::icon_alternate_texture_name);
        if (!backs.empty()) production_tooltip_back->set_texture(command_texture(backs.front()));
        // GHUD-S72: the pointer tooltip uses the first alternate frame for every side.
        if (!tooltip_frames.empty()) production_tooltip_frame->set_texture(command_texture(tooltip_frames.front()));
    }
    for (auto& cell : production_tooltip_cells) {
        cell.frame = tooltip_image();
        cell.icon = tooltip_image();
        cell.dial = memnew(EawrBuildQueueDial);
        production_tooltip->add_child(cell.dial);
    }
    production_tooltip->hide();
    // #7177 / WGSI-8: the hero icons' tooltip shares the construction tooltip's backdrop and frame art.
    hero_tooltip = memnew(Control);
    hero_tooltip->set_name("GalacticHeroTooltip");
    hero_tooltip->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    hero_tooltip->set_z_index(10);
    hud->add_child(hero_tooltip);
    const auto hero_tooltip_image = [&] {
        auto* result = memnew(TextureRect);
        result->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        result->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
        hero_tooltip->add_child(result);
        return result;
    };
    hero_tooltip_frame = hero_tooltip_image();
    hero_tooltip_back = hero_tooltip_image();
    if (const auto* background = command_catalog.find("tooltip_back")) {
        const auto backs = background->list(data::ui::Field::blank_texture_name);
        const auto tooltip_frames = background->list(data::ui::Field::icon_alternate_texture_name);
        if (!backs.empty()) hero_tooltip_back->set_texture(command_texture(backs.front()));
        if (!tooltip_frames.empty()) hero_tooltip_frame->set_texture(command_texture(tooltip_frames.front()));
    }
    for (auto& cell : hero_tooltip_cells) {
        cell.frame = hero_tooltip_image();
        cell.icon = hero_tooltip_image();
    }
    hero_tooltip->hide();
}

void GalacticMode::State::build_command_bar_advisor(const data::ui::TextDatabase* text) {
    // #7196/#7444: the popup's back sits behind its text, in the authored help back's textures; no back when the data has none.
    // It is the encyclopedia back's construction (CARD-19, WGUI-69): glow, edge and backdrop, then the connector, in this order.
    if (const auto* back = command_catalog.find("help_back")) {
        approach_glow = memnew(TextureRect);
        approach_glow->set_name("GalacticAdvisorGlow");
        approach_frame = memnew(TextureRect);
        approach_frame->set_name("GalacticAdvisorFrame");
        approach_back = memnew(TextureRect);
        approach_back->set_name("GalacticAdvisorBack");
        for (auto* image : {approach_glow, approach_frame, approach_back}) {
            image->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
            image->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
            image->set_z_index(10);
            hud->add_child(image);
            image->hide();
        }
        Ref<CanvasItemMaterial> additive;
        additive.instantiate();
        additive->set_blend_mode(CanvasItemMaterial::BLEND_MODE_ADD);
        approach_glow->set_material(additive);
        const auto backs = back->list(data::ui::Field::blank_texture_name);
        const auto frames = back->list(data::ui::Field::icon_alternate_texture_name);
        if (!backs.empty()) approach_back->set_texture(command_texture(backs.front()));
        // CARD-19: the two alternate textures are the edge and the enlarged glow, for either faction.
        if (!frames.empty()) approach_frame->set_texture(command_texture(frames[0]));
        if (frames.size() > 1) approach_glow->set_texture(command_texture(frames[1]));
        approach_back_width_units = static_cast<float>(back->vec2(data::ui::Field::size).value_or(data::ui::Vec2{}).x);
        const auto offset = back->vec2(data::ui::Field::offset).value_or(data::ui::Vec2{});
        approach_text_offset_units = Vector2(static_cast<float>(offset.x), static_cast<float>(offset.y));
        approach_connector = memnew(Polygon2D);
        approach_connector->set_name("GalacticAdvisorConnector");
        approach_connector->set_z_index(10);
        hud->add_child(approach_connector);
        approach_connector->hide();
    }
    approach_label = memnew(Label);
    approach_label->set_name("GalacticApproachAdvisor");
    approach_label->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    approach_label->set_z_index(10);
    approach_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
    // #7196: the advisor text is the authored help text style (the shell's galactic help text); the tooltip text style it used
    // before stays the fallback when the data has none. #7444: every line takes that style's ink and outline, whatever its
    // producer's code colour (the original shows control changes, battle results and hints alike in it).
    const auto* advisor_style = command_catalog.find("text_galactic_help");
    if (advisor_style == nullptr) advisor_style = command_catalog.find("tooltip_left_text");
    Color ink(192.0F / 255, 192.0F / 255, 192.0F / 255);
    if (const auto* style = advisor_style) {
        const auto face = hud_fonts->resolve({std::string(style->text(data::ui::Field::font_name)),
            style->integer(data::ui::Field::font_point_size).value_or(0)}, "english");
        // CARD-19: command-bar text requests bold whatever its Font_Name (the original's advisor lines are bold, #7444).
        approach_label->add_theme_font_override("font", hud_fonts->font(face, true));
        approach_label->add_theme_font_size_override("font_size",
            std::max(1, presentation::ui::font_pixel_height(face.point_size, camera.height)));
        // As CARD-19's body text: the authored RGB, with the fade alpha replacing the ink alpha.
        if (const auto tint = style->color(data::ui::Field::text_color)) ink = Color(tint->r / 255.0F, tint->g / 255.0F, tint->b / 255.0F);
        approach_label->add_theme_constant_override("outline_size",
            style->flag(data::ui::Field::text_outline) ? unverified_name_outline_pixels : 0);
        approach_label->add_theme_color_override("font_outline_color", Color(0, 0, 0, 1));
    }
    approach_label->add_theme_color_override("font_color", ink);
    hud->add_child(approach_label);
    approach_label->hide();
    EncyclopediaPopupStyle popup_style;
    std::vector<core::Diagnostic> popup_diagnostics;
    fill_encyclopedia_popup(popup_style, command_catalog, *filesystem, *hud_fonts, "english",
        text, popup_diagnostics);
    for (const auto& diagnostic : popup_diagnostics)
        UtilityFunctions::push_warning(String::utf8(core::format_diagnostic(diagnostic).c_str()));
    popup_style.texture = [this](const std::string& name) { return command_texture(name); };
    encyclopedia_panel = memnew(EawrEncyclopediaPopup);
    encyclopedia_panel->set_name("GalacticEncyclopedia");
    encyclopedia_panel->set_z_index(9);
    hud->add_child(encyclopedia_panel);
    encyclopedia_panel->setup(std::move(popup_style));
    encyclopedia_panel->hide();

    EawrUnitCards::Setup queue_setup;
    fill_encyclopedia_popup(queue_setup, command_catalog, *filesystem, *hud_fonts, "english",
        text, popup_diagnostics);
    queue_setup.texture = [this](const std::string& name) { return command_texture(name); };
    queue_setup.name = [](const std::string&) { return String(); };
    queue_setup.encyclopedia = [](const std::string&) { return presentation::ui::UnitCardLooks{}; };
    queue_setup.placement = [this] { return command_bar->placement(); };
    queue_setup.space = [this] { return command_bar->space(); };
    queue_setup.external_popup_anchor = [this] {
        const auto point = presentation::ui::shell_to_screen(
            {encyclopedia_anchor.x, encyclopedia_anchor.y, 0, 0}, command_bar->placement());
        return Vector2(static_cast<float>(point.x), static_cast<float>(point.y));
    };
    queue_encyclopedia = memnew(EawrUnitCards);
    queue_encyclopedia->set_name("GalacticQueueEncyclopedia");
    queue_encyclopedia->set_z_index(9);
    hud->add_child(queue_encyclopedia);
    queue_encyclopedia->setup(std::move(queue_setup));

    // WGUI-35/94: localization comes from the text DB. No invented calendar.
    if (text) {
        if (const auto* entry = text->find("TEXT_CYCLES"))
            cycle_label = data::ui::to_utf8(entry->value);
        if (const auto* entry = text->find("TEXT_TECH"))
            technology_label = data::ui::to_utf8(entry->value);
    }
}

bool GalacticMode::State::build_command_bar_shell() {
    auto* mask = memnew(EawrUiHitMask);
    mask->set_force_pass_scroll_events(true); // #5085: only wheel consumers stop map zoom.
    // Planet overlays stay below the shell and all of its controls, including
    // overlays created after the command bar (owner playtest, GHUDL-13).
    mask->set_z_index(1);
    hud->add_child(mask);
    mask->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
    command_bar = memnew(EawrTacticalHud);
    mask->add_child(command_bar);
    EawrTacticalHud::Setup setup;
    setup.view = presentation::ui::hud_view_model(command_anchors, command_catalog,
        presentation::ui::alt_variant(command_faction), presentation::ui::vfs_shell_masks(*filesystem));
    const auto look = presentation::ui::hud_shell(command_anchors, command_catalog, command_faction);
    // Report construction-time omissions once; frame refresh never repeats these warnings.
    std::set<std::string> reported;
    std::vector<std::string> optional_bindings;
    for (const auto& diagnostic : look.diagnostics) {
        const auto message = core::format_diagnostic(diagnostic);
        if (!reported.insert(message).second) continue;
        if (diagnostic.message.find("has no HUD blend") != std::string::npos)
            UtilityFunctions::push_warning(String::utf8(message.c_str()));
        else optional_bindings.push_back(message);
    }
    if (!optional_bindings.empty()) {
        std::string summary = "GC shell (" + look.model + "): " + std::to_string(optional_bindings.size())
            + " optional binding diagnostics; inspect definitions/anchors only for controls intended for GC"
              " (tactical-only controls may remain absent). First examples: ";
        for (std::size_t index = 0; index < std::min<std::size_t>(3, optional_bindings.size()); ++index) {
            if (index) summary += " | ";
            summary += optional_bindings[index];
        }
        UtilityFunctions::push_warning(String::utf8(summary.c_str()));
    }
    for (const auto& mesh : look.meshes) {
        std::string why;
        const auto path = presentation::ui::vfs_standalone_textures(*filesystem)(mesh.texture);
        if (!path) { failure = "galactic shell texture is absent: " + mesh.texture; return false; }
        auto texture = assets::load_texture(*filesystem, *path);
        if (!texture) { failure = core::format_diagnostic(texture.error()); return false; }
        auto pixels = texture_image(texture.value(), why);
        if (pixels.is_null()) { failure = why; return false; }
        setup.meshes.push_back({mesh.blend, ImageTexture::create_from_image(pixels), mesh.triangles, {}});
    }
    command_meshes = setup.meshes.size();
    command_bar->setup(std::move(setup), nullptr);
    mask->set_hit_test([this](double x, double y) { return command_bar->is_visible_in_tree() && command_bar->hit(x, y); });

    return true;
}

void GalacticMode::State::build_command_bar_labels(const data::ui::TextDatabase* text) {
    // WGUI-34: focused structures use icon slots; Text_Planetary_Pop belongs to tactical shells.
    for (const auto name : {"Text_Planet", "Text_Planetary_Income", "Text_Credits",
             "Text_Day_Counter", "Text_Tech_Level", "Text_Day_Credits", "Text_Pop_Cap"}) {
        if (!command_catalog.find(name)) continue;
        if (!command_anchors.find(name)) continue;
        auto* label = memnew(EawrHudText);
        label->set_name(name);
        label->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        command_bar->add_child(label);
        const auto* style = command_catalog.find(name);
        const auto keys = style->list(data::ui::Field::tooltip_text);
        if (text && !keys.empty()) if (const auto* entry = text->find(keys.front())) {
            label->set_mouse_filter(Control::MOUSE_FILTER_PASS);
            label->set_tooltip_text(String::utf8(data::ui::to_utf8(entry->value).c_str()));
        }
        command_labels.emplace(name, label);
        if (!style->list(data::ui::Field::icon_texture_name).empty()) {
            // A text component's own icon (the population readouts), drawn at the bone origin + Icon_Offset.
            auto* icon = memnew(TextureRect);
            icon->set_name(String(name) + "_icon");
            icon->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
            icon->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
            command_bar->add_child(icon);
            command_label_icons.emplace(name, icon);
        }
    }
}

void GalacticMode::State::build_command_bar_controls(const data::ui::TextDatabase* text) {
    std::vector<std::string> names{"b_space_tab", "b_planet_tab", "b_play_pause", "b_fast_forward", "b_filters1", "b_filters0", "b_filters2", "b_filters3",
             "b_scroll_left", "b_scroll_right"};
    for (const auto& name : galactic_view::command_filter_names(command_catalog))
        if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
    for (const auto& name : galactic_view::command_menu_names(command_catalog))
        if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
    authored_controls = galactic_view::ControlStates{};
    build_command_controls(names, {}, text, true);
    add_story_controls(text);
}

// #5857: any other authored button the loaded plots listen to (a STORY_CLICK_GUI event names it) and whose shell anchor
// exists is built as a plain control, so its click can reach the story. The production slots and queues keep their own
// builders. The plots are known only once the game session exists, so this runs again after it starts; a control already
// built is skipped.
void GalacticMode::State::add_story_controls(const data::ui::TextDatabase* text) {
    if (!game || !command_bar) return;
    std::vector<std::string> names;
    for (const auto& component : command_catalog.components()) {
        if (component.type != data::ui::ComponentType::button || component.in_group("Structure Buttons")
            || component.in_group("Build_Queue") || component.in_group("Build_Queue_Land")
            || command_buttons.contains(component.name) || !command_anchors.find(component.name)
            || !game->story_listens_for_click(component.name)) continue;
        names.push_back(component.name);
    }
    if (names.empty()) return;
    std::optional<data::ui::TextDatabase> loaded;
    if (!text) if (auto database = data::ui::load_language_text_database(*filesystem, "english")) { loaded.emplace(std::move(database).value()); text = &*loaded; }
    build_command_controls(names, std::set<std::string>(names.begin(), names.end()), text, false);
    // finish_command_bar_controls already ran for the first build, so the late controls get their sounds here (WGP-182),
    // and the placement key is dropped so the next refresh places them (a new untoggled button leaves it unchanged).
    for (const auto& name : names) {
        auto* button = Object::cast_to<EawrHudButton>(command_buttons.contains(name) ? command_buttons.at(name) : nullptr);
        const auto* component = command_catalog.find(name);
        if (button && component) button->set_sounds(*component, [this](const std::string& sfx) { if (audio) audio->ui_sound(sfx); });
    }
    hud_command_key.reset();
}

void GalacticMode::State::build_command_controls(const std::vector<std::string>& names, const std::set<std::string>& story_controls,
        const data::ui::TextDatabase* text, const bool first_build) {
    std::vector<core::LoadNote> notes;
    const auto note = [&](const data::ui::CommandBarComponent& component, std::string tag, std::string value,
                          std::string message) {
        core::LoadNote entry; entry.code = galactic_view::command_menu_action(component.name) == galactic_view::CommandMenuAction::none
            ? "EAWR-LOAD-UI-FILTER" : "EAWR-LOAD-UI-COMMAND"; entry.file = component.logical_path;
        entry.line = component.line; entry.object = component.name; entry.tag = std::move(tag);
        entry.authored_value = std::move(value); entry.action = core::LoadAction::runtime_degraded;
        entry.message = std::move(message); notes.push_back(std::move(entry));
    };
    for (const auto& name : names) {
        const auto* component = command_catalog.find(name);
        if (!component) continue;
        const bool filter = galactic_view::is_command_filter(*component);
        const auto menu = galactic_view::command_menu_action(name);
        const bool authored = filter || menu != galactic_view::CommandMenuAction::none
            || story_controls.contains(name); // state and flags come from the component
        if (!command_anchors.find(name)) {
            if (filter) note(*component, "Group", name, "Filter button has no shell anchor");
            else if (menu != galactic_view::CommandMenuAction::none)
                note(*component, "Type", name, "Command button has no shell anchor");
            continue;
        }
        auto* button = memnew(EawrHudButton);
        button->set_name(String::utf8(name.c_str()));
        if (authored) {
            button->set_disabled(component->flag(data::ui::Field::disabled));
            if (component->flag(data::ui::Field::hidden)) button->hide();
            authored_controls.add(*component);
        }
        button->set_toggle_mode(std::string_view(name) == "b_space_tab" || std::string_view(name) == "b_planet_tab"
            || std::string_view(name) == "b_play_pause" || std::string_view(name) == "b_fast_forward" || std::string_view(name) == "b_filters1"
            || std::string_view(name) == "b_filters0" || std::string_view(name) == "b_filters2" || std::string_view(name) == "b_filters3"
            || (authored && authored_controls.holds_selection(name)));
        const auto keys = component->list(data::ui::Field::tooltip_text);
        if (text && !keys.empty()) if (const auto* entry = text->find(keys.front()))
            button->set_tooltip_text(String::utf8(data::ui::to_utf8(entry->value).c_str()));
        if (options.lan_match && std::string_view(name) == "b_play_pause") {
            // #4574: the shared clock's debounce belongs to the control that spends it.
            String tooltip = button->get_tooltip_text();
            if (!tooltip.is_empty()) tooltip += String("\n");
            tooltip += String("Shared clock: any active human may pause or resume; a repeated press within 1 s is ignored");
            button->set_tooltip_text(tooltip);
        }
        if (options.lan_match && std::string_view(name) == "b_fast_forward") {
            // #5355: the shared 2x and its limits belong to the control that requests it.
            String tooltip = button->get_tooltip_text();
            if (!tooltip.is_empty()) tooltip += String("\n");
            tooltip += String::utf8(galactic_view::lan_speed_tooltip().c_str());
            button->set_tooltip_text(tooltip);
        }
        const auto texture = [&](data::ui::Field field) {
            const auto token = command_bar_token(*component, field);
            const auto resolved = command_texture(token);
            if (filter && !token.empty() && resolved.is_null())
                note(*component, "Texture", token, "Filter button texture is unresolved");
            return resolved;
        };
        if (filter && command_bar_token(*component, data::ui::Field::icon_texture_name).empty())
            note(*component, "Icon_Texture_Name", {}, "Filter button has no authored icon texture");
        if (component->flag(data::ui::Field::swap_texture)) {
            button->setup_art({.normal = texture(data::ui::Field::icon_texture_name),
                .hover = texture(data::ui::Field::mouse_over_texture_name),
                .pressed = texture(data::ui::Field::selected_texture_name),
                .disabled = texture(data::ui::Field::disabled_texture_name), .portrait = {}, .overlays = {}});
        } else {
            // GCF-01/HCB-12g/TM-14: non-swapping state art overlays the retained icon.
            // The filter rollover is translucent; replacing the icon exposes the dark shell below it.
            const auto blank = texture(data::ui::Field::blank_texture_name);
            button->setup_art({.normal = blank, .hover = {}, .pressed = {}, .disabled = blank,
                .portrait = HudButtonArt::Portrait{texture(data::ui::Field::icon_texture_name),
                    component->flag(data::ui::Field::click_shift), false},
                .overlays = HudButtonArt::Overlays{texture(data::ui::Field::mouse_over_texture_name),
                    texture(data::ui::Field::selected_texture_name), texture(data::ui::Field::disabled_texture_name),
                    !component->flag(data::ui::Field::selected_alpha), true}});
        }
        button->set_flash_texture(texture(data::ui::Field::flash_texture_name));
        if (menu != galactic_view::CommandMenuAction::none)
            button->set_disabled(galactic_view::command_menu_disabled(menu,
                component->flag(data::ui::Field::disabled), story_summary && story_summary->has_goals()));
        button->set_action([this, name = std::string(name)] { press_command_control(name); });
        command_bar->add_child(button);
        command_buttons.emplace(name, button);
    }
    if (!notes.empty()) publish_viewer_load_notes(std::move(notes), {}, true);
    if (!first_build) return;
    for (auto& pip : fiscal_pips) {
        auto* image = memnew(EawrHudButton);
        image->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        if (const auto* component = command_catalog.find("balance_pip")) {
            const auto textures = component->list(data::ui::Field::selected_texture_name);
            if (!textures.empty()) image->set_flash_texture(command_texture(textures.front()));
        }
        pip = image;
        command_bar->add_child(pip);
    }
}

// One press of an authored or stock command-bar control, from a human or from a FORCE_CLICK_GUI reward. The action and
// the press state are shared. A press made while the forced requests are drained is a forced one; it differs in not
// raising the story click (ControlStateRules::forced_press_raises_story_click, off: the debug build's reward calls only
// the control's click slot, not the story click the mouse handler raises; the slot's own body was not read) and in not refreshing the whole HUD
// from inside the frame's own refresh.
void GalacticMode::State::press_command_control(const std::string& name) {
    if (story_hud.controls_locked() && !command_presses.forced_press()) return; // LOCK_CONTROLS (WST-75): a human press is refused; a forced one runs
    class Effects final : public galactic_view::CommandPresses::Effects {
    public:
        explicit Effects(State& state) : state_(state) {}
        [[nodiscard]] bool story_listens(const std::string& control) const override {
            return state_.game && state_.game->story_listens_for_click(control);
        }
        void raise_click(const std::string& control) override { state_.submit_gui_click(control); }
        void action(const std::string& control) override { state_.command_control_action(control); }
        void refresh() override { state_.update_hud(); }
    private:
        State& state_;
    } effects(*this);
    command_presses.press(authored_controls, name, effects);
}

// What a press of a named control does besides its selection state and the story click.
void GalacticMode::State::command_control_action(const std::string& name) {
    const auto menu = galactic_view::command_menu_action(name);
    if (menu == galactic_view::CommandMenuAction::options) {
        if (save_controls) save_controls->open_menu();
        return;
    }
    if (menu == galactic_view::CommandMenuAction::story_summary) {
        service_story_summary(*this);
        if (story_summary) story_summary->open(*this);
        return;
    }
    if (menu == galactic_view::CommandMenuAction::cinematic_camera) { // #6291 (GCT-02): the camera button starts the galactic tour
        static_cast<void>(start_tour());
        return;
    }
    if (name == "b_space_tab") { space_tab_choice = true; selected_hud.space_tab = true; authored_controls.domain_changed(); }
    else if (name == "b_filters0") income_layer_visible = !income_layer_visible;
    else if (name == "b_filters1") base_levels_visible = !base_levels_visible;
    else if (name == "b_filters2") bonus_layer_visible = !bonus_layer_visible;
    else if (name == "b_filters3") {
        ground_hero_preference = !ground_hero_preference;
        fleet_popup_publication.reset();
        fleet_layout_dirty = true;
    }
    else if (name == "b_planet_tab" && selected_hud.ground_enabled) { space_tab_choice = false; selected_hud.space_tab = false; authored_controls.domain_changed(); }
    else if (name == "b_play_pause" && clock_controls_enabled) request_pause(!paused);
    else if (name == "b_fast_forward" && galactic_view::fast_forward_available(fast_forward_controls_enabled, paused, fast_forward_factor.has_value()) && game) {
        // A press acts on what the button shows: the speed request in flight, else the current speed (owner 2026-11-17).
        const auto shown = sim::math::Fixed::from_raw(speed_guard.shown(strategic_speed_now().raw()));
        submit_strategic_speed(galactic_view::fast_forward_target(shown, *fast_forward_factor));
    }
}

// The button a story names; a name matches a control case-insensitively.
static EawrHudButton* find_command_button(const std::map<std::string, godot::TextureButton*>& controls, std::string_view component) {
    const auto found = std::find_if(controls.begin(), controls.end(), [&](const auto& entry) {
        return entry.first.size() == component.size() && std::equal(entry.first.begin(), entry.first.end(), component.begin(),
            [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); }); });
    return found == controls.end() ? nullptr : static_cast<EawrHudButton*>(found->second);
}

// WST-75 / #5857: drains this seat's FORCE_CLICK_GUI requests. Each runs the named control's press path (its action and
// press state) but raises no story click. An unknown, hidden or disabled control has no effect (a project choice: the
// reward itself has no such gate in the debug build's reward function, and the slot's own check was not read); presses
// beyond one drain's budget are dropped. Every drop is reported once per (control, reason), never an abort.
void GalacticMode::State::service_forced_clicks() {
    if (!game || !galaxy || command_presses.forced_press()) return;
    const auto published = game->story_presentation_after(forced_click_cursor, sim::SeatId{galaxy->local.seat});
    std::vector<std::string> components;
    for (const auto& row : published.events) {
        forced_click_cursor = row.cursor;
        const auto* request = std::get_if<sim::galactic::story::ConsumerRequest>(&row.event.payload);
        if (!request || request->consumer != sim::galactic::story::Consumer::gui || request->reward_name != "FORCE_CLICK_GUI"
            || request->parameters.empty()) continue;
        components.push_back(request->parameters.front());
    }
    forced_click_cursor = std::max(forced_click_cursor, published.through);
    // The widgets behind the names; a name matches a control case-insensitively.
    class Buttons final : public galactic_view::CommandPresses::Surface {
    public:
        explicit Buttons(std::map<std::string, godot::TextureButton*>& controls) : controls_(controls) {}
        [[nodiscard]] galactic_view::CommandPresses::Target find(std::string_view component) const override {
            const auto* button = lookup(component);
            using Target = galactic_view::CommandPresses::Target;
            return button ? Target{true, button->is_disabled(), button->is_visible_in_tree()} : Target{};
        }
        void activate(std::string_view component) override {
            if (auto* button = lookup(component)) button->activate(); // the human press path minus the pointer and minus the story click
        }
    private:
        [[nodiscard]] EawrHudButton* lookup(std::string_view component) const { return find_command_button(controls_, component); }
        std::map<std::string, godot::TextureButton*>& controls_;
    } buttons(command_buttons);
    command_presses.drain(components, buttons, sim::galactic::max_gui_clicks_per_tick, [](const galactic_view::CommandPresses::Drop& dropped) {
        core::LoadNote entry; entry.code = "EAWR-FORCE-CLICK"; entry.object = dropped.component;
        entry.tag = "FORCE_CLICK_GUI"; entry.authored_value = dropped.component; entry.action = core::LoadAction::runtime_degraded;
        entry.phase = core::LoadPhase::runtime; entry.message = dropped.reason;
        UtilityFunctions::push_warning(String::utf8(core::format_diagnostic(core::load_note_diagnostic(entry)).c_str()));
    });
}

// #236 (G9/G10; WST-75): the local HUD state a story plot drives for this seat: FLASH_GUI pulses the named control,
// LOCK_CONTROLS locks the human's command-bar presses (a forced press still runs) and SCROLL_LOCK locks the camera pan
// (see the map's frame step). Presentation only: nothing here is saved, hashed or replayed. A flash naming a control the
// loaded data does not have is reported once per name and changes nothing; never an abort. The scope of the lock beyond
// the command bar and the pan, and the meaning of the flash numbers, are unread in the debug build (WST-U09).
void GalacticMode::State::service_story_hud() {
    if (!game || !galaxy) return;
    const auto published = game->story_presentation_after(story_hud_cursor, sim::SeatId{galaxy->local.seat});
    for (const auto& row : published.events) {
        story_hud_cursor = row.cursor;
        const auto* request = std::get_if<sim::galactic::story::ConsumerRequest>(&row.event.payload);
        if (!request || !story_hud.apply(*request)) continue;
        if (request->reward_name != "FLASH_GUI" || find_command_button(command_buttons, request->parameters.front())) continue;
        if (!story_hud_reported.insert(request->parameters.front()).second) continue;
        core::LoadNote entry; entry.code = "EAWR-STORY-FLASH"; entry.object = request->parameters.front();
        entry.tag = "FLASH_GUI"; entry.authored_value = request->parameters.front(); entry.action = core::LoadAction::runtime_degraded;
        entry.phase = core::LoadPhase::runtime; entry.message = "the HUD has no control of this name; no flash";
        UtilityFunctions::push_warning(String::utf8(core::format_diagnostic(core::load_note_diagnostic(entry)).c_str()));
    }
    story_hud_cursor = std::max(story_hud_cursor, published.through);
    const auto now = Time::get_singleton()->get_ticks_msec();
    const double seconds = story_hud_ticks ? static_cast<double>(now - story_hud_ticks) / 1000.0 : 0.0;
    story_hud_ticks = now;
    story_hud.advance(seconds);
    for (const auto& flash : story_hud.flashes()) {
        if (auto* button = find_command_button(command_buttons, flash.component)) {
            const auto level = 1.0F + 0.8F * static_cast<float>(story_hud.intensity(flash));
            button->set_modulate(Color(level, level, level, 1.0F));
        }
    }
    for (const auto& name : story_hud.take_finished()) {
        if (auto* button = find_command_button(command_buttons, name)) button->set_modulate(Color(1, 1, 1, 1));
    }
    service_story_letterbox();
}

void GalacticMode::State::build_command_bar_choices(const pugi::xml_document& document) {
    // GHUD-S92: native default is 26 total alternating slots; XML may change it.
    std::int32_t choice_slot_limit{26};
    if (const auto node = document.document_element().child("Strategic_Command_Bar_Buildable_Row_Count")) {
        std::istringstream value(node.text().as_string());
        std::int32_t authored{};
        if (value >> authored) choice_slot_limit = std::max(0, authored);
    }
    std::size_t slot_count{};
    for (const auto& component : command_catalog.components()) {
        constexpr std::string_view prefix = "b_Create_";
        if (galactic_view::is_command_filter(component) || !component.in_group("Structure Buttons") || !component.name.starts_with(prefix)) continue;
        const std::string_view suffix(component.name.data() + prefix.size(), component.name.size() - prefix.size());
        std::size_t index{};
        const auto parsed = std::from_chars(suffix.data(), suffix.data() + suffix.size(), index);
        if (parsed.ec != std::errc{} || parsed.ptr != suffix.data() + suffix.size()
            || index >= static_cast<std::size_t>(choice_slot_limit)) continue;
        std::ostringstream canonical;
        canonical << prefix << std::setw(2) << std::setfill('0') << index;
        if (canonical.str() == component.name) slot_count = std::max(slot_count, index + 1);
    }
    for (std::size_t slot = 0; slot < slot_count; ++slot) {
        // GHUD-S91: native numbered component bindings survive XML definition reordering.
        std::ostringstream name;
        name << "b_Create_" << std::setw(2) << std::setfill('0') << slot;
        const auto* authored = command_catalog.find(name.str());
        const bool present = authored && !galactic_view::is_command_filter(*authored)
            && authored->in_group("Structure Buttons") && command_anchors.find(authored->name);
        choice_buttons.push_back(present ? authored->name : std::string{});
        if (!present) continue;
        const auto& component = *authored;
        auto* button = memnew(EawrHudButton);
        button->set_name(String::utf8(component.name.c_str())); button->hide();
        button->set_order_icon({}, component.flag(data::ui::Field::click_shift));
        const auto pads = component.list(data::ui::Field::blank_texture_name);
        const auto dark = component.list(data::ui::Field::disabled_texture_name);
        // #2909 (GHUD-S57): the pad is chosen per displayed production tab in update_command_bar; the build only
        // seeds the ground-tab entry so the card has a size before the first update.
        if (!pads.empty()) button->set_texture_normal(command_texture(pads.front()));
        if (!dark.empty()) button->set_texture_disabled(command_texture(dark.front()));
        button->set_texture_hover(command_texture(command_bar_token(component, data::ui::Field::mouse_over_texture_name)));
        button->set_texture_pressed(command_texture(command_bar_token(component, data::ui::Field::selected_texture_name)));
        button->set_action([this, name = component.name] {
            raise_story_click(name); // #5922: the plots hear a production-card press (BFC names its cards), whatever the card does
            const auto choice = displayed_choices.find(name);
            if (choice == displayed_choices.end() || !choice->second.enabled || !game) return;
            // WGEP-97: underway feedback belongs to the admitted UI submission, before the request.
            if (audio) audio->build(audio::GalacticBuildCue::underway, choice->second.type, true);
            game->submit(platform::LiveGameOrder{sim::SeatId{galaxy->local.seat},
                sim::galactic::ProductionCommand{selected_planet, choice->second.type_id,
                    sim::galactic::ProductionAction::buy, choice->second.space
                        ? sim::galactic::ProductionDomain::space : sim::galactic::ProductionDomain::ground, 0}, {}});
            ++submitted_orders;
        });
        // #2890 (GHUD-S57): the card's price is a text child on the pad's lower edge.
        auto* price = memnew(EawrHudText); price->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        price->set_name("build_card_price");
        button->add_child(price); choice_price.emplace(component.name, price);
        command_bar->add_child(button); command_buttons.emplace(component.name, button);
    }
}

void GalacticMode::State::build_command_bar_queues() {
    for (const auto& component : command_catalog.components()) {
        const bool ground = component.in_group("Build_Queue_Land");
        if ((!ground && !component.in_group("Build_Queue")) || !command_anchors.find(component.name)) continue;
        // U-07 named grouping adapter: queue06 has dual membership; its ground
        // tooltip/art gives it one native Control in the ground strip.
        queue_buttons[ground ? 0U : 1U].push_back(component.name);
        auto* button = memnew(EawrHudButton); button->hide();
        button->set_name(String::utf8(component.name.c_str()));
        button->set_button_mask(MOUSE_BUTTON_MASK_RIGHT);
        button->set_order_icon({}, false);
        button->set_texture_normal(command_texture(command_bar_token(component, data::ui::Field::blank_texture_name)));
        button->set_texture_hover(command_texture(command_bar_token(component, data::ui::Field::mouse_over_texture_name)));
        auto* dial = memnew(EawrBuildQueueDial);
        dial->set_name("build_queue_dial");
        dial->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        dial->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
        dial->set_texture(command_texture(command_bar_token(component, data::ui::Field::build_texture_name)));
        button->add_child(dial); queue_dials.emplace(component.name, dial);
        auto* percent = memnew(EawrHudText); percent->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        button->add_child(percent); queue_percent.emplace(component.name, percent);
        // #5922: a queue slot cancels on a right release, but the story hears the left release (generic rule, WGP-182; no walked
        // rule says a right release raises it: unverified, WST-37).
        button->set_left_release_action([this, name = component.name] { raise_story_click(name); });
        button->set_action([this, name = component.name] {
            const auto row = displayed_queues.find(name);
            if (row == displayed_queues.end() || !row->second.visible || !game
                || !published_snapshot->production()) return;
            const auto planet = std::find_if(published_snapshot->production()->planets.begin(),
                published_snapshot->production()->planets.end(), [this](const auto& value) { return value.planet == selected_planet; });
            if (planet == published_snapshot->production()->planets.end()) return;
            const auto domain = row->second.space ? sim::galactic::ProductionDomain::space : sim::galactic::ProductionDomain::ground;
            const auto& queue = planet->queues[static_cast<std::size_t>(domain)];
            const auto index = row->second.index;
            if (index >= queue.size() || queue[index].build_id != row->second.build_id || queue[index].type != row->second.type_id) return;
            // #3931: a queue row built by another player is never cancellable, with the reveal on or off.
            if (queue[index].builder != galaxy->local.player) return;
            game->submit(platform::LiveGameOrder{sim::SeatId{galaxy->local.seat},
                sim::galactic::ProductionCommand{selected_planet, row->second.type_id,
                    sim::galactic::ProductionAction::cancel_index, domain, index, row->second.build_id}, {}});
            ++submitted_orders;
        });
        command_bar->add_child(button); command_buttons.emplace(component.name, button);
    }
}

void GalacticMode::State::finish_command_bar_controls() {
    // #3075: every GC command-bar button plays its own authored Click_SFX / Mouse_Over_SFX.
    for (const auto& [name, control] : command_buttons) {
        auto* button = Object::cast_to<EawrHudButton>(control);
        const auto* component = command_catalog.find(name);
        if (!button || !component) continue;
        button->set_sounds(*component,
            [this](const std::string& sfx) { if (audio) audio->ui_sound(sfx); });
    }
    allegiance_icon = memnew(TextureRect);
    allegiance_icon->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    allegiance_icon->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
    command_bar->add_child(allegiance_icon);
}

bool GalacticMode::State::build_command_bar() {
    hud_text_styles.clear();
    auto loaded = data::ui::load_command_bar(*filesystem);
    if (!loaded) { failure = core::format_diagnostic(loaded.error()); return false; }
    command_catalog = std::move(loaded).value().catalog;
    const auto* shell = command_catalog.find("i_main_commandbar");
    if (!shell) { failure = "the galactic command-bar shell is absent"; return false; }
    auto shell_model = assets::load_model(*filesystem,
        "data/art/models/" + std::string(shell->text(data::ui::Field::model_name)));
    if (!shell_model) { failure = core::format_diagnostic(shell_model.error()); return false; }
    command_anchors = data::ui::shell_anchors(shell_model.value()).shell;
    // GHUD-C04: ordinary GC encyclopedia uses the advisor popup's bone point,
    // including a point with no drawable mesh. Its position comes from the mod's shell.
    // The source position getter returns the shell origin for a missing bone.
    encyclopedia_anchor = data::ui::shell_point(shell_model.value(), "text_galactic_help")
        .value_or(assets::Vec2f{});
    if (const auto* background = command_catalog.find("encyclopedia_back")) {
        const auto offset = background->vec2(data::ui::Field::default_offset).value_or(data::ui::Vec2{});
        encyclopedia_anchor.x += offset.x;
        encyclopedia_anchor.y += offset.y;
    }
    // WGUI-38: thresholds are authored data, never universal UI literals.
    auto constants = filesystem->open("data/xml/gameconstants.xml");
    if (!constants) { failure = core::format_diagnostic(constants.error()); return false; }
    pugi::xml_document document;
    if (!document.load_buffer(constants.value().data(), constants.value().size(), pugi::parse_default | pugi::parse_doctype)) {
        failure = "planet name thresholds require valid GameConstants XML"; return false;
    }
    for (const auto node : document.children()) if (node.type() == pugi::node_doctype) {
        failure = "planet name thresholds forbid a document type"; return false;
    }
    if (!configure_command_bar_constants(document)) return false;
    if (!configure_command_bar_encyclopedia()) return false;
    // WGUI-80: the local seat's faction selects the shell, never selected ownership.
    command_faction = presentation::ui::hud_faction_from(galaxy->local.faction)
        .value_or(presentation::ui::HudFaction::underworld);
    auto atlas = assets::load_mega_texture_atlas(*filesystem,
        presentation::ui::command_bar_mega_texture(command_catalog));
    if (!atlas) { failure = core::format_diagnostic(atlas.error()); return false; }
    command_atlas.emplace(std::move(atlas).value());
    command_textures = std::make_unique<UiTextures>(*command_atlas, *filesystem);
    if (!command_textures->page_failure().empty()) { failure = command_textures->page_failure(); return false; }
    build_command_bar_tooltip();
    auto text = data::ui::load_language_text_database(*filesystem, "english");
    build_command_bar_advisor(text ? &text.value() : nullptr);
    if (!build_command_bar_shell()) return false;
    build_command_bar_labels(text ? &text.value() : nullptr);
    build_command_bar_controls(text ? &text.value() : nullptr);
    build_command_bar_choices(document);
    build_command_bar_queues();
    finish_command_bar_controls();
    if (options.hud_provider && !options.hud_provider->configure_hud(*this)) return false;
    return true;
}

void GalacticMode::State::service_production_tooltip() {
    if (!production_tooltip || !hud || !command_bar) return;
    const auto now = Time::get_singleton()->get_ticks_msec();
    const auto pointer = hud->get_viewport()->get_mouse_position();
    const auto* control = hud->get_viewport()->gui_get_hovered_control();
    galactic_view::ObjectId hovered{};
    if (encyclopedia_admitted() && !dragged_fleet) for (const auto& [id, icon] : production_icons) {
        const auto facts = hud_facts.find(id);
        if (control == icon && icon->is_visible_in_tree() && facts != hud_facts.end()
            && galactic_view::production_activity(facts->second)) { hovered = id; break; }
    }
    if (hovered != production_hover_planet || pointer != production_hover_position) {
        production_hover_planet = hovered;
        production_hover_position = pointer;
        production_hover_since = now;
        production_tooltip->hide();
    }
    if (!hovered || now - production_hover_since < production_tooltip_delay) {
        production_tooltip->hide(); return;
    }
    const auto& facts = hud_facts.at(hovered);
    const float scale = static_cast<float>(command_bar->placement().scale);
    // GHUD-S72: fixed 52-unit construction cells; two-unit inset, four-unit
    // backdrop expansion and two more for its frame. Slot assignment runs in
    // reverse, so the space head appears left of the ground head.
    constexpr float cell_units = 52.0F;
    const float cell_size = cell_units * scale;
    std::size_t count{};
    for (std::size_t domain = 0; domain < 2; ++domain) {
        auto& cell = production_tooltip_cells[domain];
        cell.frame->hide(); cell.icon->hide(); cell.dial->hide();
        cell.type = 0; cell.build = 0;
        const auto& queue = domain == 0 ? facts.ground_queue : facts.space_queue;
        const auto* component = command_catalog.find(domain == 0 ? "tooltip_icon_land" : "tooltip_icon");
        if (!queue || queue->empty() || !component) continue;
        const auto& head = queue->front();
        const auto texture = command_texture(head.icon);
        if (texture.is_null()) continue;
        cell.type = head.type_id; cell.build = head.build_id;
        cell.progress = head.progress_percent;
        cell.icon->set_texture(texture);
        const float authored_scale = static_cast<float>(component->number(data::ui::Field::scale).value_or(1.0));
        const Vector2 size = texture->get_size() * scale * authored_scale;
        cell.icon->set_size(size);
        const auto blanks = component->list(data::ui::Field::blank_texture_name);
        const auto blank = blanks.empty() ? Ref<Texture2D>{} : command_texture(blanks.front());
        cell.frame->set_texture(blank);
        cell.frame->set_size(blank.is_valid() ? blank->get_size() * scale * authored_scale : Vector2{});
        auto* dial = static_cast<EawrBuildQueueDial*>(cell.dial);
        const auto builds = component->list(data::ui::Field::build_texture_name);
        const auto build = builds.empty() ? Ref<Texture2D>{} : command_texture(builds.front());
        dial->set_texture(build); dial->set_percent(head.progress_percent);
        dial->set_size(build.is_valid() ? build->get_size() * scale * authored_scale : Vector2{});
        // GHUD-S72: construction hover sets the dial, but no percentage text.
        cell.frame->show(); cell.icon->show(); cell.dial->show();
        ++count;
    }
    if (!count) { production_tooltip->hide(); return; }
    const Vector2 size(static_cast<float>(count) * cell_size + 6.0F * scale, cell_size + 6.0F * scale);
    production_tooltip->set_size(size);
    production_tooltip_frame->set_size(size);
    production_tooltip_back->set_position(Vector2(scale, scale));
    production_tooltip_back->set_size(size - Vector2(2.0F * scale, 2.0F * scale));
    std::size_t slot{};
    for (std::size_t index = production_tooltip_cells.size(); index-- > 0;) {
        auto& cell = production_tooltip_cells[index];
        if (!cell.type) continue;
        const Vector2 centre(3.0F * scale + (static_cast<float>(slot++) + 0.5F) * cell_size,
            3.0F * scale + cell_size * 0.5F);
        cell.icon->set_position(centre - cell.icon->get_size() * 0.5F);
        cell.frame->set_position(centre - cell.frame->get_size() * 0.5F);
        cell.dial->set_position(centre - cell.dial->get_size() * 0.5F);
    }
    // GHUD-S72: the pointer gap uses the tooltip's fixed 8192/6144
    // conversion dimensions (one horizontal / three vertical ratios);
    // flip beside the pointer at the right or bottom edge, then clip to screen.
    const Vector2 gap(8192.0F / static_cast<float>(camera.width) * scale,
        6144.0F / static_cast<float>(camera.height) * 3.0F * scale);
    Vector2 position = production_hover_position + gap;
    if (position.x + size.x > static_cast<float>(camera.width)) position.x = production_hover_position.x - size.x - gap.x;
    if (position.y + size.y > static_cast<float>(camera.height)) position.y = production_hover_position.y - size.y;
    position.x = std::clamp(position.x, 0.0F, std::max(0.0F, static_cast<float>(camera.width) - size.x));
    position.y = std::clamp(position.y, 0.0F, std::max(0.0F, static_cast<float>(camera.height) - size.y));
    production_tooltip->set_position(position);
    production_tooltip->show();
}

// Future story/modal producers must supply their gate (legacy #1158/#2009).
bool GalacticMode::State::encyclopedia_admitted() const {
    return map_input.gates.hud_enabled && map_input.gates.active_mode
        && !map_input.gates.action_menu && !map_input.gates.tutorial_locked && map_input.gates.world_allowed()
        && !(weapon_menu.blocker && weapon_menu.blocker->is_visible_in_tree())
        && !(hero_menu.blocker && hero_menu.blocker->is_visible_in_tree())
        && !(corruption_menu.blocker && corruption_menu.blocker->is_visible_in_tree())
        && !(market_menu.blocker && market_menu.blocker->is_visible_in_tree());
}

void GalacticMode::State::service_encyclopedia() {
    if (!encyclopedia_panel || !displayed_snapshot) return;
    const auto now = Time::get_singleton()->get_ticks_msec();
    const double elapsed = encyclopedia_wall_time ? static_cast<double>(now - encyclopedia_wall_time) / 1000.0 : 0.0;
    encyclopedia_wall_time = now;
    sim::galactic::TypeId hovered{};
    const galactic_view::BuildChoiceComponent* hovered_choice = nullptr;
    const galactic_view::QueueComponent* hovered_queue = nullptr;
    const auto* pointer = hud->get_viewport()->gui_get_hovered_control();
    // GHUD-S95/S96: a separate identity keeps the selected planet's bonus
    // encyclopedia out of the production type/price namespace.
    constexpr std::uint64_t planet_bonus_identity = std::uint64_t{1} << 63;
    // GHUDL-18: the filtered map icon of any planet, or the selected planet's advanced-screen icon.
    galactic_view::ObjectId bonus_planet{};
    if (planet_bonus_icon && planet_bonus_icon->is_visible_in_tree() && pointer == planet_bonus_icon) bonus_planet = selected_planet;
    if (!bonus_planet && pointer) for (const auto& [id, icon] : planet_bonus_icons)
        if (pointer == icon && icon->is_visible_in_tree()) { bonus_planet = id; break; }
    const bool bonus_hover = bonus_planet != 0;
    // #2987 / WGFI-2: the weather icon opens the weather entry the same way.
    constexpr std::uint64_t planet_weather_identity = std::uint64_t{1} << 62;
    galactic_view::ObjectId weather_planet{};
    if (!bonus_hover && pointer) for (const auto& [id, icon] : planet_weather_icons)
        if (pointer == icon && icon->is_visible_in_tree()) { weather_planet = id; break; }
    const bool weather_hover = weather_planet != 0;
    // WGCI-1: the per-type corruption icon of a planet (the information icon has no popup here); its popup keeps its own identity
    // bit so a planet change restarts the timer.
    constexpr std::uint64_t planet_corruption_flag = std::uint64_t{1} << 61;
    galactic_view::ObjectId corruption_planet{};
    if (!bonus_hover && !weather_hover && pointer) for (const auto& [id, icon] : corruption_info_icons) {
        if (pointer != icon || !icon->is_visible_in_tree()) continue;
        const auto facts = hud_facts.find(id);
        if (facts != hud_facts.end() && facts->second.corrupted && facts->second.corruption_type != 0) { corruption_planet = id; break; }
    }
    const bool corruption_hover = corruption_planet != 0;
    const auto special_planet = bonus_hover ? bonus_planet : weather_hover ? weather_planet : corruption_planet;
    for (const auto& [name, choice] : displayed_choices) {
        if (pointer != command_buttons.at(name)) continue;
        hovered = choice.type_id; hovered_choice = &choice; break;
    }
    if (!hovered) for (const auto& [name, row] : displayed_queues) {
        const auto* button = command_buttons.at(name);
        if (pointer != button || !button->is_visible_in_tree()) continue;
        hovered_queue = &row; break;
    }
    // PU-73 / GHUD-C04: queue details use the shared external unit-card hook,
    // full header and admitted queue price (WGEP-18..34), without remaining time.
    // Borrow retained rows each frame; own content only when admission changes.
    const auto publications = queue_hover.publications();
    const bool queue_admitted = hovered_queue && encyclopedia_admitted();
    if (queue_admitted) {
        if (queue_hover_planet != selected_planet) queue_hover.clear();
        queue_hover.publish(hovered_queue->build_id, hovered_queue->type, true);
    } else queue_hover.clear();
    if (queue_hover.publications() != publications) {
        queue_encyclopedia->set_external_hover(0, std::nullopt);
        if (const auto* active = queue_hover.get()) {
            const auto art = production_art.find(active->type);
            if (art != production_art.end())
                queue_encyclopedia->set_external_hover(active->identity, art->second.encyclopedia_looks,
                    {hovered_queue->price, {}, false});
        }
        queue_hover_planet = selected_planet;
    }
    if (queue_admitted) {
        encyclopedia_timing = *encyclopedia_globals.timing;
        encyclopedia_type = 0;
        encyclopedia_panel->hide();
        return;
    }
    const auto focused_type = focused_planet_hover_type();
    if (!hovered) hovered = focused_type;
    if (encyclopedia_existing_object && (encyclopedia_planet != selected_planet
        || !fleet_organize || !fleet_organize->is_visible_in_tree())) {
        encyclopedia_timing.shown = 0; encyclopedia_timing.alpha = 0;
        encyclopedia_timing.remaining = 0;
    }
    // WGUI-18, GHUD-S82: focused identities come from observer-filtered cells.
    const bool admitted = encyclopedia_admitted();
    if (encyclopedia_ability && encyclopedia_planet != (special_planet ? special_planet : selected_planet)) {
        encyclopedia_timing.shown = 0; encyclopedia_timing.alpha = 0;
        encyclopedia_timing.remaining = 0;
    }
    encyclopedia_timing.service(bonus_hover ? planet_bonus_identity : weather_hover ? planet_weather_identity
        : corruption_hover ? (planet_corruption_flag | corruption_planet) : hovered, elapsed, admitted);
    if (!admitted) { encyclopedia_panel->hide(); return; }
    if (!encyclopedia_timing.shown) { encyclopedia_panel->hide(); encyclopedia_type = 0; return; }
    if (encyclopedia_timing.shown == planet_bonus_identity) {
        // While the popup fades out after the pointer left, it keeps the planet it was built for.
        const auto shown_planet = bonus_hover ? bonus_planet : (encyclopedia_ability ? encyclopedia_planet : selected_planet);
        if (!bonus_hover && !(planet_bonus_icon && planet_bonus_icon->is_visible_in_tree())) { encyclopedia_panel->hide(); return; }
        const auto planet = std::find_if(galaxy->planets.begin(), galaxy->planets.end(),
            [shown_planet](const auto& row) { return row.id == shown_planet; });
        if (planet == galaxy->planets.end()) { encyclopedia_panel->hide(); return; }
        if (!encyclopedia_ability || encyclopedia_weather || encyclopedia_corruption || encyclopedia_planet != shown_planet
            || encyclopedia_snapshot != displayed_snapshot || encyclopedia_width != camera.width || encyclopedia_height != camera.height) {
            presentation::ui::UnitCardLooks looks;
            looks.icon = planet->ability.icon; looks.name = planet->ability.title;
            looks.icon_scale = 1.0F; // WGUI-167: bonus header uses full texture scale.
            looks.description = planet->ability.description;
            const auto placement = command_bar->placement();
            const auto point = presentation::ui::shell_to_screen({encyclopedia_anchor.x, encyclopedia_anchor.y, 0, 0}, placement);
            encyclopedia_panel->update(looks, {}, Vector2(static_cast<float>(point.x), static_cast<float>(point.y)),
                command_bar->space(), static_cast<float>(placement.scale),
                Rect2(0, 0, static_cast<float>(camera.width), static_cast<float>(camera.height)));
            encyclopedia_ability = true; encyclopedia_weather = false; encyclopedia_corruption = false;
            encyclopedia_existing_object = false; encyclopedia_type = 0;
            encyclopedia_planet = shown_planet; encyclopedia_snapshot = displayed_snapshot;
            encyclopedia_width = camera.width; encyclopedia_height = camera.height;
        }
        encyclopedia_panel->set_modulate(Color(1, 1, 1, static_cast<float>(encyclopedia_timing.alpha)));
        encyclopedia_panel->show(); return;
    }
    if (encyclopedia_timing.shown == planet_weather_identity) {
        // WGFI-2: header icon and name, then the planet's weather text; the popup keeps its planet while it fades out.
        const auto shown_planet = weather_hover ? weather_planet : (encyclopedia_ability ? encyclopedia_planet : selected_planet);
        const auto planet = std::find_if(galaxy->planets.begin(), galaxy->planets.end(),
            [shown_planet](const auto& row) { return row.id == shown_planet; });
        if (planet == galaxy->planets.end()) { encyclopedia_panel->hide(); return; }
        if (!encyclopedia_ability || !encyclopedia_weather || encyclopedia_corruption || encyclopedia_planet != shown_planet
            || encyclopedia_snapshot != displayed_snapshot || encyclopedia_width != camera.width || encyclopedia_height != camera.height) {
            presentation::ui::UnitCardLooks looks;
            looks.icon = planet->weather.icon; looks.name = planet->weather.name;
            looks.icon_scale = 1.0F;
            looks.description = planet->weather.info;
            const auto placement = command_bar->placement();
            const auto point = presentation::ui::shell_to_screen({encyclopedia_anchor.x, encyclopedia_anchor.y, 0, 0}, placement);
            encyclopedia_panel->update(looks, {}, Vector2(static_cast<float>(point.x), static_cast<float>(point.y)),
                command_bar->space(), static_cast<float>(placement.scale),
                Rect2(0, 0, static_cast<float>(camera.width), static_cast<float>(camera.height)));
            encyclopedia_ability = true; encyclopedia_weather = true; encyclopedia_corruption = false;
            encyclopedia_existing_object = false; encyclopedia_type = 0;
            encyclopedia_planet = shown_planet; encyclopedia_snapshot = displayed_snapshot;
            encyclopedia_width = camera.width; encyclopedia_height = camera.height;
        }
        encyclopedia_panel->set_modulate(Color(1, 1, 1, static_cast<float>(encyclopedia_timing.alpha)));
        encyclopedia_panel->show(); return;
    }
    if (encyclopedia_timing.shown & planet_corruption_flag) {
        // WGCI-1: header icon, type name and mission blurb of the hovered per-type corruption icon; the popup keeps its planet
        // while it fades out.
        const auto shown_planet = static_cast<galactic_view::ObjectId>(encyclopedia_timing.shown & ~planet_corruption_flag);
        const auto facts = hud_facts.find(shown_planet);
        if (facts == hud_facts.end() || !battle_text) { encyclopedia_panel->hide(); return; }
        const auto content = galactic_view::corruption_icon_encyclopedia(facts->second.corruption_type,
            corruption_requirement_icons, corruption_icon_names, corruption_icon_descriptions);
        if (!content) { encyclopedia_panel->hide(); return; }
        if (!encyclopedia_ability || !encyclopedia_corruption || encyclopedia_planet != shown_planet
            || encyclopedia_snapshot != displayed_snapshot || encyclopedia_width != camera.width || encyclopedia_height != camera.height
            || encyclopedia_corruption_type != facts->second.corruption_type) {
            presentation::ui::UnitCardLooks looks;
            looks.icon = content->icon; looks.name = battle_text(content->name_key);
            looks.icon_scale = 1.0F;
            if (!content->description_key.empty()) looks.description = battle_text(content->description_key);
            const auto placement = command_bar->placement();
            const auto point = presentation::ui::shell_to_screen({encyclopedia_anchor.x, encyclopedia_anchor.y, 0, 0}, placement);
            encyclopedia_panel->update(looks, {}, Vector2(static_cast<float>(point.x), static_cast<float>(point.y)),
                command_bar->space(), static_cast<float>(placement.scale),
                Rect2(0, 0, static_cast<float>(camera.width), static_cast<float>(camera.height)));
            encyclopedia_ability = true; encyclopedia_weather = false; encyclopedia_corruption = true;
            encyclopedia_existing_object = false; encyclopedia_type = 0;
            encyclopedia_planet = shown_planet; encyclopedia_snapshot = displayed_snapshot;
            encyclopedia_corruption_type = facts->second.corruption_type;
            encyclopedia_width = camera.width; encyclopedia_height = camera.height;
        }
        encyclopedia_panel->set_modulate(Color(1, 1, 1, static_cast<float>(encyclopedia_timing.alpha)));
        encyclopedia_panel->show(); return;
    }
    const bool existing_object = focused_type == encyclopedia_timing.shown
        || (hovered != encyclopedia_timing.shown && encyclopedia_existing_object
            && encyclopedia_type == encyclopedia_timing.shown);
    // Prepared prices are location-specific; GUI selection can change while
    // the same immutable snapshot remains published.
    if (encyclopedia_ability || encyclopedia_type != encyclopedia_timing.shown || encyclopedia_snapshot != displayed_snapshot
        || encyclopedia_planet != selected_planet || encyclopedia_width != camera.width || encyclopedia_height != camera.height
        || encyclopedia_existing_object != existing_object) {
        const galactic_view::BuildChoiceComponent* choice = hovered == encyclopedia_timing.shown ? hovered_choice : nullptr;
        if (!choice && !existing_object) for (const auto& [name, value] : displayed_choices) {
            static_cast<void>(name);
            if (value.type_id == encyclopedia_timing.shown) { choice = &value; break; }
        }
        if (!choice && !existing_object) {
            encyclopedia_timing.shown = 0; encyclopedia_timing.alpha = 0; encyclopedia_panel->hide(); return;
        }
        const auto type_name = production_names.find(static_cast<sim::galactic::TypeId>(encyclopedia_timing.shown));
        if (type_name == production_names.end()) { encyclopedia_panel->hide(); return; }
        const auto name = choice ? choice->type : type_name->second;
        const auto art = production_art.find(name);
        if (art == production_art.end()) { encyclopedia_panel->hide(); return; }
        EncyclopediaPopupStatus popup_status;
        // GC retains the ordinary class and static-ability header (GHUD-C04).
        // Only tactical production selects the stripped CARD-14 header.
        // Only observer-prepared prices and admission status reach the widget.
        if (choice && choice->price) popup_status.price = choice->price->trunc_to_integer();
        if (choice && !choice->enabled) popup_status.disabled_reason = galactic_view::production_denial_text(choice->admission);
        const auto placement = command_bar->placement();
        // GHUD-C04: bottom-left at the advisor point. The dialog-only droid
        // offset is zero for ordinary build/queue hover (no droid log or tech tree).
        const auto point = presentation::ui::shell_to_screen({encyclopedia_anchor.x,
            encyclopedia_anchor.y, 0, 0}, placement);
        encyclopedia_panel->update(art->second.encyclopedia_looks, popup_status,
            Vector2(static_cast<float>(point.x), static_cast<float>(point.y)), command_bar->space(),
            static_cast<float>(placement.scale), Rect2(0, 0, static_cast<float>(camera.width), static_cast<float>(camera.height)));
        encyclopedia_type = encyclopedia_timing.shown;
        encyclopedia_ability = false; encyclopedia_weather = false; encyclopedia_corruption = false;
        encyclopedia_existing_object = existing_object;
        encyclopedia_planet = selected_planet;
        encyclopedia_snapshot = displayed_snapshot;
        encyclopedia_width = camera.width; encyclopedia_height = camera.height;
    }
    encyclopedia_panel->set_modulate(Color(1, 1, 1, static_cast<float>(encyclopedia_timing.alpha)));
    encyclopedia_panel->show();
}

void GalacticMode::State::configure_command_bar_text(const presentation::ui::ShellPlacement& placement, EawrHudText& label,
    const data::ui::CommandBarComponent& component, const data::ui::ShellAnchor& anchor, const bool second_text,
    const bool scale_text) {
    const HudTextStyleKey key{label.get_text(), &component, &anchor, camera.height, placement.scale};
    const auto previous = hud_text_styles.find(&label);
    if (previous != hud_text_styles.end() && previous->second == key) return;
    hud_text_styles.insert_or_assign(&label, key);
    ++hud_text_style_updates;
    auto look = *presentation::ui::hud_readout_style(command_catalog, component.name, anchor);
    // A text button's second text sits at its own authored offset (debug build: Text_Offset2).
    if (second_text) look.text_offset = component.vec2(data::ui::Field::text_offset2).value_or(data::ui::Vec2{});
    auto face = hud_fonts->resolve({look.text.face, look.text.point_size}, "english");
    const auto text_scale = component.number(data::ui::Field::scale).value_or(1.0F);
    // #6756: a readout's authored Scale sizes its text as well as its icon (the retail population readout of the
    // Remake chain authors 0.7: small digits beside a small icon); the glyph raster height follows it.
    const auto glyph_pixels = [&](const int point_size) {
        const int natural = std::max(1, presentation::ui::font_pixel_height(point_size, camera.height));
        return scale_text ? galactic_view::scaled_glyph_pixels(natural, text_scale) : natural;
    };
    int pixels = glyph_pixels(face.point_size);
    auto font = hud_fonts->command_font(face, pixels);
    // GHUD-S66: width is tested in shell units; restore the authored size
    // on each refresh and reduce one point at a time, stopping at six.
    const auto max_width = component.integer(data::ui::Field::max_text_width).value_or(0);
    const float width_scale = scale_text ? 1.0F : text_scale; // a scaled glyph raster is already measured scaled
    while (max_width != 0 && look.text.point_size > 6 && placement.scale > 0.0
        && font->get_string_size(label.get_text(), HORIZONTAL_ALIGNMENT_LEFT, -1, pixels).x
            * width_scale / static_cast<float>(placement.scale) > static_cast<float>(max_width)) {
        face = hud_fonts->resolve({look.text.face, --look.text.point_size}, "english");
        pixels = glyph_pixels(face.point_size);
        font = hud_fonts->command_font(face, pixels);
    }
    label.set_style(look, font, [this, face](std::int32_t height) {
        return presentation::ui::gdi_text_cell(hud_fonts->cache(), face, height);
    }, pixels, static_cast<float>(placement.scale),
        1.0F / static_cast<float>(placement.scale)); // GHUD-S66: one source raster pixel at every resolution.
}

// True when the economy readouts (the player facts, the income tooltip, the credit balance) now differ from what the bar
// last drew. They are derived once per publication; an unchanged result writes nothing.
bool GalacticMode::State::update_command_bar_economy(const sim::galactic::GalacticClock& clock, bool fiscal_available) {
    if (economy_hud_snapshot == displayed_snapshot) return false;
    auto player = galactic_view::published_player_hud_facts(*displayed_snapshot, galaxy->local.player);
    if (fiscal_available) player.fiscal_cycle = clock.fiscal_cycle;
    player.hide_tech = command_faction == presentation::ui::HudFaction::underworld;
    auto view = galactic_view::player_hud_view(player, cycle_label, technology_label);
    auto tooltip = galactic_view::income_forecast_text(displayed_snapshot->economy_forecast());
    std::string account;
    if (displayed_snapshot->player_economy()) for (const auto& economy : displayed_snapshot->player_economy()->players) {
        if (economy.player == galaxy->local.player) account = galactic_view::credit_balance(economy.credits);
    }
    const bool first = !economy_hud_snapshot;
    economy_hud_snapshot = displayed_snapshot;
    if (!first && view == player_hud && tooltip == income_tooltip && account == account_text) return false;
    player_hud = std::move(view);
    income_tooltip = std::move(tooltip);
    if (const auto income = command_labels.find("Text_Day_Credits"); income != command_labels.end()) {
        income->second->set_tooltip_text(String::utf8(income_tooltip.c_str()));
    }
    account_text = std::move(account);
    return true;
}

void GalacticMode::State::update_command_bar_labels(const presentation::ui::ShellPlacement& placement,
    const std::map<galactic_view::ObjectId, galactic_view::PlanetHudFacts>::iterator selected) {
    for (const auto& [name, label] : command_labels) {
        const auto* anchor = command_anchors.find(name);
        const auto* component = command_catalog.find(name);
        if (!anchor) {
            label->hide();
            if (const auto icon = command_label_icons.find(name); icon != command_label_icons.end()) icon->second->hide();
            continue;
        }
        const auto rect = command_bar_screen(placement, anchor->rect);
        label->set_position(rect.position);
        label->set_size(rect.size);
        // The original anchors a text component at its bone origin plus Text_Offset: left-justified text starts
        // there, right-justified text ends there and unjustified text is centred on it (debug build).
        const bool left_justified = component->flag(data::ui::Field::left_justified);
        const bool right_justified = !left_justified && component->flag(data::ui::Field::right_justified);
        const auto origin = anchor->origin.value_or(assets::Vec2f{
            anchor->rect.x + anchor->rect.width * 0.5F, anchor->rect.y + anchor->rect.height * 0.5F});
        const auto text_offset = component->vec2(data::ui::Field::text_offset).value_or(data::ui::Vec2{});
        // The bone origin plus Text_Offset (shell units, y up); its y shifts the label off the centred-band baseline.
        const auto anchor_point = presentation::ui::text_anchor({origin.x, origin.y, 0.0F, 0.0F},
            {text_offset.x, text_offset.y}, placement);
        const float point_x = static_cast<float>(anchor_point.x);
        const float offset_y = static_cast<float>(anchor_point.y_shift);
        const float label_x = left_justified ? point_x : right_justified ? point_x - rect.size.x
            : point_x - rect.size.x * 0.5F;
        label->set_position(Vector2(label_x, rect.position.y + offset_y));
        if (const auto icon_slot = command_label_icons.find(name); icon_slot != command_label_icons.end()) {
            const auto names = component->list(data::ui::Field::icon_texture_name);
            const auto texture = command_texture(names.front());
            const auto icon_offset = component->vec2(data::ui::Field::icon_offset).value_or(data::ui::Vec2{});
            icon_slot->second->set_texture(texture);
            if (texture.is_valid()) {
                // #6756: the component's authored Scale sizes its icon (1 when it authors none).
                const auto size = texture->get_size() * component->number(data::ui::Field::scale).value_or(1.0F);
                const auto centre = command_bar_screen(placement, {origin.x + icon_offset.x, origin.y + icon_offset.y, 0.0F, 0.0F});
                icon_slot->second->set_size(size);
                icon_slot->second->set_position(centre.position - size * 0.5F);
            }
        }
        const auto tint = component->color(data::ui::Field::text_color).value_or(data::ui::Rgba8{255,255,255,255});
        label->set_colour(Color(tint.r / 255.0F, tint.g / 255.0F, tint.b / 255.0F, tint.a / 255.0F));
        label->set_horizontal_alignment(left_justified ? HORIZONTAL_ALIGNMENT_LEFT
            : right_justified ? HORIZONTAL_ALIGNMENT_RIGHT : HORIZONTAL_ALIGNMENT_CENTER);
        std::string value;
        if (name == "Text_Planet") value = selected_hud.name;
        else if (name == "Text_Planetary_Income") value = selected_hud.income;
        else if (name == "Text_Credits" && !account_text.empty()) {
            // The reserve is "$ " plus the engine's localized, digit-grouped whole number (GHUD-S63).
            value = "$ " + galactic_view::grouped_number(account_text, "english");
        }
        else if (name == "Text_Day_Credits") value = player_hud.next_income;
        else if (name == "Text_Pop_Cap") value = player_hud.population;
        else if (name == "Text_Day_Counter") value = player_hud.cycle;
        else if (name == "Text_Tech_Level") value = player_hud.technology;
        // gc_economy_hover_routing: native mouse-stop is the current input
        // adapter for WGUI-51; retail economic label hit shape is unqualified.
        if (name == "Text_Day_Credits") {
            label->set_mouse_filter(Control::MOUSE_FILTER_STOP);
        } else if (name == "Text_Pop_Cap") label->set_mouse_filter(Control::MOUSE_FILTER_STOP);
        // GHUDL-19: readout labels retain their localized case; only planet names are uppercase.
        auto displayed_text = String::utf8(value.c_str());
        if (name == "Text_Planet") displayed_text = displayed_text.to_upper();
        set_command_bar_text(*label, displayed_text);
        configure_command_bar_text(placement, *label, *component, *anchor, false, true);
        label->set_colour(Color(tint.r / 255.0F, tint.g / 255.0F, tint.b / 255.0F, tint.a / 255.0F));
        // Text wider than its authored box overflows about the anchor, not about the box edge the alignment uses.
        if (const float needed = label->get_minimum_size().x; needed > rect.size.x) {
            label->set_size(Vector2(needed, label->get_size().y));
            label->set_position(Vector2(left_justified ? point_x : right_justified ? point_x - needed
                : point_x - needed * 0.5F, label->get_position().y));
        }
        label->set_visible(name != "Text_Tech_Level" || command_faction != presentation::ui::HudFaction::underworld);
        // The selected planet's owner colour tints both the name and the planet value (debug build).
        if ((name == "Text_Planet" || name == "Text_Planetary_Income") && selected != hud_facts.end()) {
            const auto colour = selected->second.name_colour;
            label->set_colour(Color(colour.r, colour.g, colour.b));
        }
        if (const auto icon_slot = command_label_icons.find(name); icon_slot != command_label_icons.end()) {
            icon_slot->second->set_visible(label->is_visible() && !value.empty());
        }
    }
}

void GalacticMode::State::update_command_bar_allegiance(const presentation::ui::ShellPlacement& placement,
    const std::map<galactic_view::ObjectId, galactic_view::PlanetHudFacts>::iterator selected) {
    allegiance_icon->hide();
    // The emblem is the Icon_Alternate_Texture_Name entry the engine picks for the observed owner's faction:
    // the GameConstants good / evil / corrupt side (0, 1, 2), the Pirates literal (3), else 4 (debug build). Drawn
    // at its authored size centred on the bone origin; no emblem while ownership is not visible.
    if (selected != hud_facts.end() && selected->second.observed_owner && content) {
        const auto* owner = galactic_view::affiliation_for_owner(*content, selected->second.observed_owner);
        const auto* anchor = command_anchors.find("Text_Allegiance_A");
        const auto* style = command_catalog.find("Text_Allegiance_A");
        if (owner && anchor && style) {
            const auto textures = style->list(data::ui::Field::icon_alternate_texture_name);
            const auto index = presentation::ui::allegiance_emblem_index(owner->faction, content->side_names);
            // A list shorter than the engine's five entries draws no emblem for an index it lacks.
            if (index < textures.size()) {
                const auto texture = command_texture(textures[index]);
                const auto scale = style->number(data::ui::Field::scale).value_or(1.0F);
                if (texture.is_valid()) {
                    const auto origin = anchor->origin.value_or(assets::Vec2f{
                        anchor->rect.x + anchor->rect.width * 0.5F, anchor->rect.y + anchor->rect.height * 0.5F});
                    const auto size = texture->get_size() * scale;
                    const auto centre = command_bar_screen(placement, {origin.x, origin.y, 0.0F, 0.0F});
                    allegiance_icon->set_texture(texture);
                    allegiance_icon->set_size(size);
                    allegiance_icon->set_position(centre.position - size * 0.5F);
                    allegiance_icon->show();
                }
            }
        }
    }
}

void GalacticMode::State::update_command_bar_pads() {
    // #2909 (GHUD-S57): the pad is the authored entry for the displayed production tab (no entry, no pad). Chosen
    // here because the tab changes after the build.
    for (const auto& name : choice_buttons) {
        if (name.empty()) continue;
        const auto* component = command_catalog.find(name);
        auto* button = static_cast<EawrHudButton*>(command_buttons.at(name));
        const auto pads = component->list(data::ui::Field::blank_texture_name);
        const auto dark = component->list(data::ui::Field::disabled_texture_name);
        const auto pad = galactic_view::build_card_pad(pads, selected_hud.space_tab);
        const auto disabled_pad = galactic_view::build_card_pad(dark, selected_hud.space_tab);
        button->set_texture_normal(pad.empty() ? Ref<Texture2D>() : command_texture(pad));
        button->set_texture_disabled(disabled_pad.empty() ? Ref<Texture2D>() : command_texture(disabled_pad));
        if (pad.empty()) choice_pad.erase(name);
        else choice_pad.insert_or_assign(name, std::string(pad));
    }
}

void GalacticMode::State::update_command_bar_buttons(const presentation::ui::ShellPlacement& placement, const sim::galactic::GalacticClock& clock) {
    for (const auto& [name, button] : command_buttons) {
        const auto* anchor = command_anchors.find(name);
        const auto* component = command_catalog.find(name);
        const auto offset = component->vec2(data::ui::Field::icon_offset).value_or(data::ui::Vec2{});
        const auto size = button->get_texture_normal().is_valid() ? button->get_texture_normal()->get_size()
            : Vector2(anchor->rect.width, anchor->rect.height);
        const auto geometry = galactic_view::command_button_geometry(*anchor, offset, size.x, size.y,
            static_cast<float>(component->number(data::ui::Field::scale).value_or(1.0F)),
            name == "b_space_tab" || name == "b_planet_tab");
        const auto rect = command_bar_screen(placement, geometry.art);
        button->set_position(rect.position); button->set_size(rect.size);
        auto* hud_button = static_cast<EawrHudButton*>(button);
        if (!component->flag(data::ui::Field::swap_texture)
            && (galactic_view::is_command_filter(*component)
                || name == "b_filters0" || name == "b_filters1" || name == "b_filters2" || name == "b_filters3"
                || galactic_view::command_menu_action(name) != galactic_view::CommandMenuAction::none)) {
            // GHUD-S73/HCB-12g: retain each layer at its own scaled texture size.
            const auto icon = command_texture_optional(command_bar_token(*component, data::ui::Field::icon_texture_name));
            const float layer_scale = galactic_view::command_bar_component_scale(*component,
                static_cast<float>(placement.scale));
            const auto backing = button->get_texture_normal();
            if (icon.is_valid() && (backing.is_null() || icon->get_size() != backing->get_size())) {
                const auto box = galactic_view::command_layer_rect(rect.size.x, rect.size.y,
                    static_cast<float>(icon->get_width()), static_cast<float>(icon->get_height()), layer_scale);
                hud_button->set_cell_portrait_box(Rect2(box.x, box.y, box.width, box.height));
                hud_button->set_special_overlay_natural_scale(layer_scale);
            }
        }
        // Composite tab art overlaps; only the authored shell mesh is active.
        const auto hit = command_bar_screen(placement, geometry.hit);
        hud_button->set_hit_rect(Rect2(hit.position - rect.position, hit.size));
        // Stock tab art is one composite per button that already depicts the other tab as selected, and
        // neither stock tab authors a selected texture: only the unselected tab draws. A tab that authors
        // Selected_Texture_Name (RaW) stays drawn and swaps to that art when selected (#5490).
        const bool ground_active = !selected_hud.space_tab && selected_hud.ground_enabled;
        // The active tab without a selected art is not drawn but keeps its hit area, so it can still be pressed: a plot's
        // click on the active tab (the capture's "press the active space tab") and a forced press both need it (#5857).
        if (name == "b_planet_tab") {
            button->set_disabled(!selected_hud.ground_enabled);
            button->set_pressed_no_signal(ground_active);
            button->set_self_modulate(ground_active && galactic_view::selected_tab_hidden(*component) ? Color(1, 1, 1, 0) : Color(1, 1, 1, 1));
        } else if (name == "b_space_tab") {
            button->set_pressed_no_signal(!ground_active);
            button->set_self_modulate(!ground_active && galactic_view::selected_tab_hidden(*component) ? Color(1, 1, 1, 0) : Color(1, 1, 1, 1));
        }
        else if (name == "b_filters1") button->set_pressed_no_signal(base_levels_visible);
        else if (name == "b_filters0") button->set_pressed_no_signal(income_layer_visible);
        else if (name == "b_filters2") button->set_pressed_no_signal(bonus_layer_visible);
        else if (name == "b_filters3") button->set_pressed_no_signal(ground_hero_preference);
        else if (authored_controls.holds_selection(name)) button->set_pressed_no_signal(authored_controls.selected(name));
        else if (name == "b_play_pause") {
            button->set_disabled(!clock_controls_enabled);
            // Retail draws the pause button lit (its selected art) for as long as the game is paused, and dim otherwise.
            button->set_pressed_no_signal(paused);
        } else if (name == "b_fast_forward") {
            button->set_disabled(!galactic_view::fast_forward_available(fast_forward_controls_enabled, paused, fast_forward_factor.has_value()));
            button->set_pressed_no_signal(fast_forward_shown_pressed(clock));
        }
        if (!component->flag(data::ui::Field::swap_texture) && button->is_visible_in_tree()) {
            // Programmatic toggle/disabled changes emit no button signals; update the retained layers too.
            hud_button->set_order_selected(button->is_pressed());
            hud_button->set_special_disabled(button->is_disabled());
        }
    }
}

void GalacticMode::State::update_command_bar_choices(const presentation::ui::ShellPlacement& placement) {
    const auto choice_slots = galactic_view::production_choice_slots(selected_hud.choices, choice_buttons.size());
    displayed_choices.clear();
    for (std::size_t index = 0; index < choice_buttons.size(); ++index) {
        const auto& name = choice_buttons[index];
        if (name.empty()) continue;
        auto* button = static_cast<EawrHudButton*>(command_buttons.at(name));
        const auto choice_index = choice_slots[index];
        auto* price = choice_price.at(name);
        if (!choice_index) {
            button->hide(); button->set_disabled(true); button->set_icon_texture({});
            button->set_tooltip_text({}); set_command_bar_text(*price, {}); continue;
        }
        const auto& choice = selected_hud.choices[*choice_index];
        displayed_choices.emplace(name, choice);
        button->set_icon_texture(command_texture(choice.icon));
        button->set_disabled(!choice.enabled); button->show();
        button->set_tooltip_text(String{}); // Shared timed encyclopedia owns object hover.
        // #2890: whole credits, centred in the strip from Text_Offset2 (negative is up from the pad's bottom edge) to that edge. The face
        // and outline are the card's own; the colour is the price state's (#2985).
        const auto* component = command_catalog.find(name);
        set_command_bar_text(*price, choice.price ? String::num_int64(choice.price->trunc_to_integer()) : String{});
        const auto offset = component->vec2(data::ui::Field::text_offset2).value_or(data::ui::Vec2{});
        configure_command_bar_text(placement, *price, *component, *command_anchors.find(name));
        // GHUD-S57: the debug build overwrites the price text's authored colour (Text_Color2) on every refresh with the
        // price-vs-base state colour; the component's Text_Color belongs to its other text.
        const auto& tint = choice.price_colour;
        price->set_colour(Color(tint[0] / 255.0F, tint[1] / 255.0F, tint[2] / 255.0F, tint[3] / 255.0F));
        price->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
        // Retail's digits centre in the strip between the Text_Offset2 line and the pad's bottom edge (pixel-compared
        // at 1280x720: 2 px above retail), not hang from the line.
        price->set_anchors_and_offsets_preset(Control::PRESET_BOTTOM_WIDE);
        price->set_offset(SIDE_TOP, offset.y * static_cast<float>(command_bar->placement().scale));
        price->set_offset(SIDE_BOTTOM, 0.0F);
        price->set_offset(SIDE_LEFT, offset.x * static_cast<float>(command_bar->placement().scale));
        price->set_offset(SIDE_RIGHT, offset.x * static_cast<float>(command_bar->placement().scale));
    }
    for (const auto name : {"b_scroll_left", "b_scroll_right"}) if (command_buttons.contains(name)) {
        auto* button = command_buttons.at(name);
        // GHUD-S92: GC refresh always hides both scroll controls, including overflow.
        button->hide();
        button->set_disabled(true);
    }
}

void GalacticMode::State::update_command_bar_queues(const presentation::ui::ShellPlacement& placement) {
    displayed_queues.clear();
    std::size_t queue_slot{};
    for (const auto domain : {1U, 0U}) for (const auto& name : queue_buttons[domain]) {
        const auto& row = selected_hud.queues[queue_slot++];
        auto* button = static_cast<EawrHudButton*>(command_buttons.at(name));
        button->set_visible(row.visible); button->set_disabled(!row.visible);
        const auto icon = command_texture(row.icon);
        button->set_icon_texture(icon);
        if (row.visible) displayed_queues.emplace(name, row);
        auto* dial = static_cast<EawrBuildQueueDial*>(queue_dials.at(name));
        dial->set_percent(row.percent.value_or(100));
        auto* percent = queue_percent.at(name);
        set_command_bar_text(*percent, row.percent ? String::num_int64(*row.percent) + "%" : String{});
        const auto* component = command_catalog.find(name);
        // GHUD-R04 / PU-63: the queued icon and the build dial each draw at their own texture size times the
        // component Scale, centred on the button (the bone, plus Icon_Offset), not stretched over the blank
        // texture's 79-unit quad: the retail capture shows the queue icon at the build card's icon size.
        {
            const float scale = component->number(data::ui::Field::scale).value_or(1.0F);
            const auto centred = [&](const Ref<Texture2D>& texture) {
                if (texture.is_null()) return Rect2(Vector2{}, button->get_size());
                const auto size = command_bar_screen(placement, {0.0F, 0.0F, texture->get_width() * scale, texture->get_height() * scale}).size;
                return Rect2((button->get_size() - size) * 0.5F, size);
            };
            const auto icon_box = centred(icon);
            button->set_cell_portrait_box(icon_box);
            const auto build = component->list(data::ui::Field::build_texture_name);
            const auto dial_box = centred(build.empty() ? Ref<Texture2D>() : command_texture(build.front()));
            dial->set_anchors_preset(Control::PRESET_TOP_LEFT);
            dial->set_position(dial_box.position); dial->set_size(dial_box.size);
        }
        const auto offset = component->vec2(data::ui::Field::text_offset).value_or(data::ui::Vec2{});
        // PU-64 (#2902): centre the percentage on the shell point bone + Text_Offset
        // (shell y is up), in the same text box the tactical queue uses.
        const auto* anchor = command_anchors.find(name);
        const auto origin = anchor->origin.value_or(assets::Vec2f{
            anchor->rect.x + anchor->rect.width * 0.5F, anchor->rect.y + anchor->rect.height * 0.5F});
        const auto box = command_bar_screen(placement, {origin.x + offset.x - 25.0F, origin.y + offset.y - 6.0F, 50.0F, 12.0F});
        configure_command_bar_text(placement, *percent, *component, *anchor);
        // A line taller than the box keeps the same centre rather than growing down.
        const auto size = box.size.max(percent->get_minimum_size());
        percent->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
        percent->set_size(size);
        percent->set_position(box.get_center() - size * 0.5F - button->get_position());
        button->set_tooltip_text(String{});
    }
}

void GalacticMode::State::update_command_bar_pips(const presentation::ui::ShellPlacement& placement,
    const sim::galactic::GalacticClock& clock, bool fiscal_available) {
    current_pip = fiscal_pip(clock, fiscal_available);
    const auto* pip_component = command_catalog.find("balance_pip");
    const auto* pip_anchor = command_anchors.find("Text_Day_Counter");
    if (pip_component && pip_anchor) {
        const auto textures = pip_component->list(data::ui::Field::icon_alternate_texture_name);
        for (std::size_t index = 0; index < fiscal_pips.size(); ++index) {
            auto* pip = fiscal_pips[index];
            pip->set_visible(current_pip.has_value());
            // WGUI-94: only the current pip flashes; the shared HUD leaf owns
            // flash cadence and the authored white flash texture.
            static_cast<EawrHudButton*>(pip)->set_flashing(current_pip && index == *current_pip && !paused);
            if (!current_pip || textures.size() < 2) continue;
            const auto& texture_name = textures[index <= *current_pip ? 1 : 0];
            const auto texture = command_texture(texture_name);
            pip->set_texture_normal(texture);
            if (texture.is_null()) {
                if (missing_pip_textures.insert(texture_name).second)
                    UtilityFunctions::push_error(String::utf8(("EAWR galactic fiscal pip texture is unavailable: "
                        + texture_name).c_str()));
                pip->hide();
                continue;
            }
            const auto offset = pip_component->vec2(data::ui::Field::offset).value_or(data::ui::Vec2{});
            auto rect = pip_anchor->rect;
            // The repeated pips have no shell meshes. Their group follows the
            // current-cycle text anchor; WGUI-94 owns count/progress, not time.
            rect.x = pip_anchor->rect.right() + offset.x * static_cast<float>(index);
            const auto icon = pip_component->vec2(data::ui::Field::icon_offset).value_or(data::ui::Vec2{});
            rect.x += icon.x; rect.y += icon.y;
            rect.width = static_cast<float>(texture->get_width());
            rect.height = static_cast<float>(texture->get_height());
            const auto pixels = command_bar_screen(placement, rect);
            pip->set_position(pixels.position); pip->set_size(pixels.size);
        }
    }
}

galactic_view::ClockGates GalacticMode::State::clock_gates() const {
    galactic_view::ClockGates gates;
    gates.tutorial_locked = map_input.gates.tutorial_locked;
    gates.world_allowed = map_input.gates.world_allowed();
    // During a LAN battle the tactical clock owns time controls; guard the galaxy pointer until it is available.
    gates.lan_battle = options.lan_match && game && galaxy && game->frame(sim::SeatId{galaxy->local.seat}).battle;
    gates.active_humans = static_cast<std::size_t>(std::count_if(metadata.seats.begin(), metadata.seats.end(),
        [](const auto& seat) { return seat.active && seat.human; }));
    gates.fixture = bool(options.hud_provider);
    gates.lan_match = bool(options.lan_match);
    gates.lan_gameplay = options.lan_match && galactic_lan_gameplay_enabled(options.lan_match->view());
    return gates;
}

void GalacticMode::State::update_command_bar() {
    if (!command_bar || !displayed_snapshot) return;
    // WGUI-36: presentation goals can change without moving the retained economy/selection key.
    // Refresh their admission before that cache can return; authored Disabled always takes precedence.
    for (const auto& [name, button] : command_buttons) {
        const auto menu = galactic_view::command_menu_action(name);
        if (menu != galactic_view::CommandMenuAction::story_summary) continue;
        const auto* component = command_catalog.find(name);
        const bool disabled = galactic_view::command_menu_disabled(menu,
            component->flag(data::ui::Field::disabled), story_summary && story_summary->has_goals());
        if (button->is_disabled() != disabled) button->set_disabled(disabled);
    }
    const auto gates = clock_gates();
    const bool clock_enabled = galactic_view::clock_controls_open(gates);
    // #5355: fast forward follows the clock controls; in multiplayer it is the shared 2x through the agreed command.
    const bool fast_forward_enabled = clock_enabled;
    const auto& clock = displayed_snapshot->clock();
    const bool fiscal_available = clock.fiscal_cycle >= 1 && clock.fiscal_end > clock.fiscal_start;
    // TM-03/04, MP-06: a live solo campaign (no synthetic HUD fixture) fast-forwards at 4x; a multiplayer campaign at 2x.
    if (const auto live = galactic_view::live_fast_forward_factor(bool(options.hud_provider), game != nullptr,
            gates.lan_match || gates.active_humans > 1)) fast_forward_factor = live;
    // #4331: derive what the bar reads of this publication; only a result that differs from the last one moves a revision.
    if (update_command_bar_economy(clock, fiscal_available)) ++hud_economy_revision;
    const auto selected = hud_facts.find(selected_planet);
    // A render frame with the same publication and input must not copy the selected planet's strings/vectors.
    // Queue-slot capacities are fixed by build_command_bar_queues; a rebuilt bar has no retained command key.
    if (!hud_command_key || hud_view_fact_revision != hud_fact_rebuilds
        || hud_command_key->selection != selected_planet || hud_command_key->space_tab != selected_hud.space_tab
        || hud_command_key->economy_revision != hud_economy_revision) {
        ++hud_view_rebuilds;
        if (auto view = galactic_view::planet_hud_view(selected == hud_facts.end() ? nullptr : &selected->second,
                space_tab_choice, queue_buttons[1].size(), queue_buttons[0].size()); !(view == selected_hud)) {
            selected_hud = std::move(view); ++hud_view_revision;
        }
        hud_view_fact_revision = hud_fact_rebuilds;
    }
    if (options.hud_verify) {
        ++hud_verify_checks;
        if (!(galactic_view::planet_hud_view(selected == hud_facts.end() ? nullptr : &selected->second,
                space_tab_choice, queue_buttons[1].size(), queue_buttons[0].size()) == selected_hud)) {
            ++hud_verify_mismatches;
            if (hud_verify_detail.empty()) hud_verify_detail = "command view";
        }
    }
    const auto native_toggles = [&] {
        // This allocation-free read also sees native presses whose callback is refused (for example during a drag).
        std::uint32_t bits{}, bit{1};
        for (const auto& entry : command_buttons) {
            if (entry.second->is_toggle_mode() && entry.second->is_pressed()) bits |= bit;
            bit <<= 1;
        }
        return bits;
    };
    const auto current_key = [&] {
        return HudCommandKey{selected_planet, selected_hud.space_tab,
            base_levels_visible, income_layer_visible, clock_enabled, fast_forward_enabled, paused, choice_page,
            fast_forward_factor, seat_flight().state(), command_bar->placement(), camera.height,
            hud_economy_revision, hud_view_revision, fiscal_pip(clock, fiscal_available),
            fast_forward_shown_pressed(clock),
            selected == hud_facts.end() ? std::nullopt : std::optional(selected->second.name_colour),
            selected == hud_facts.end() ? std::nullopt : selected->second.observed_owner, native_toggles()};
    };
    const bool unchanged = hud_command_key == current_key();
    if (unchanged && !options.hud_verify) return;
    // Verification: an unchanged key still runs the whole update, and it must leave every control as it was.
    const auto drawn = unchanged ? command_bar_fingerprint() : std::map<std::string, std::uint64_t>{};
    if (!unchanged) ++hud_command_updates;
    // WGUI-79: missing tutorial/MP profile admission (#1979) stays an explicit seam.
    clock_controls_enabled = clock_enabled;
    fast_forward_controls_enabled = fast_forward_enabled;
    const auto placement = command_bar->placement();
    update_command_bar_labels(placement, selected);
    update_command_bar_allegiance(placement, selected);
    update_command_bar_pads();
    update_command_bar_buttons(placement, clock);
    update_command_bar_choices(placement);
    update_command_bar_queues(placement);
    update_command_bar_pips(placement, clock, fiscal_available);
    // Admission may force the space tab or clamp the page during this refresh.
    hud_command_key = current_key();
    if (unchanged) {
        ++hud_verify_checks;
        if (const auto now = command_bar_fingerprint(); now != drawn) {
            ++hud_verify_mismatches;
            if (hud_verify_detail.empty()) {
                hud_verify_detail = "command bar";
                for (const auto& [name, digest] : now)
                    if (const auto was = drawn.find(name); was == drawn.end() || was->second != digest) {
                        hud_verify_detail += " " + name; break;
                    }
            }
        }
    }
}

namespace {
void digest_control(galactic_view::HudDigest& digest, const Control* control) {
    digest.add(control != nullptr);
    if (!control) return;
    digest.add(control->is_visible());
    const auto position = control->get_position(), size = control->get_size();
    digest.add(position.x); digest.add(position.y); digest.add(size.x); digest.add(size.y);
}
void digest_texture(galactic_view::HudDigest& digest, const Ref<Texture2D>& texture) {
    digest.add(texture.is_valid() ? static_cast<std::uint64_t>(texture->get_instance_id()) : std::uint64_t{0});
}
void digest_button(galactic_view::HudDigest& digest, const TextureButton* button) {
    digest_control(digest, button);
    if (!button) return;
    digest.add(button->is_disabled()); digest.add(button->is_pressed());
    digest.add(std::string_view(utf8(button->get_tooltip_text())));
    digest_texture(digest, button->get_texture_normal()); digest_texture(digest, button->get_texture_disabled());
}
void digest_text(galactic_view::HudDigest& digest, const EawrHudText* label) {
    digest_control(digest, label);
    if (!label) return;
    digest.add(std::string_view(utf8(label->get_text())));
    const auto colour = label->colour();
    digest.add(colour.r); digest.add(colour.g); digest.add(colour.b); digest.add(colour.a);
    digest.add(static_cast<std::int64_t>(label->get_horizontal_alignment()));
}
}

// What the command bar's controls hold, for the verification run: the text, colour, place, visibility, enabled state,
// pressed state, tooltip and textures of every control the update writes.
std::map<std::string, std::uint64_t> GalacticMode::State::command_bar_fingerprint() const {
    std::map<std::string, std::uint64_t> parts;
    for (const auto& [name, label] : command_labels) {
        galactic_view::HudDigest digest; digest_text(digest, label); parts.emplace("label " + name, digest.value());
    }
    for (const auto& [name, icon] : command_label_icons) {
        galactic_view::HudDigest digest; digest_control(digest, icon); if (icon) digest_texture(digest, icon->get_texture());
        parts.emplace("label icon " + name, digest.value());
    }
    for (const auto& [name, button] : command_buttons) {
        galactic_view::HudDigest digest; digest_button(digest, button); parts.emplace("button " + name, digest.value());
    }
    for (const auto& [name, price] : choice_price) {
        galactic_view::HudDigest digest; digest_text(digest, price); parts.emplace("price " + name, digest.value());
    }
    for (const auto& [name, percent] : queue_percent) {
        galactic_view::HudDigest digest; digest_text(digest, percent); parts.emplace("percent " + name, digest.value());
    }
    for (const auto& [name, dial] : queue_dials) {
        galactic_view::HudDigest digest; digest_control(digest, dial); parts.emplace("dial " + name, digest.value());
    }
    for (std::size_t index = 0; index < fiscal_pips.size(); ++index) {
        galactic_view::HudDigest digest; digest_button(digest, fiscal_pips[index]);
        parts.emplace("pip " + std::to_string(index), digest.value());
    }
    galactic_view::HudDigest emblem; digest_control(emblem, allegiance_icon);
    if (allegiance_icon) digest_texture(emblem, allegiance_icon->get_texture());
    parts.emplace("allegiance", emblem.value());
    galactic_view::HudDigest shown; shown.add(displayed_choices.size()); shown.add(displayed_queues.size());
    parts.emplace("displayed", shown.value());
    return parts;
}

void GalacticMode::State::enqueue_approach(sim::galactic::PlanetId planet) {
    if (advisor_character_seconds < 0 || !approach_label || !hud || !battle_text) return;
    const auto name = planet_notice_name(planet);
    if (name.empty()) return;
    const std::array<std::string, 1> arguments{name};
    auto line = String::utf8(galactic_view::expand_notice_text(
        battle_text("TEXT_ENEMY_FLEET_APPROACHING_LOCATION"), arguments).c_str());
    // WGUI-66/114: urgent prepend, no duplicate suppression; interrupt removal delay.
    AdvisorLine entry; entry.text = std::move(line); entry.urgent = true;
    approach_text.push_front(std::move(entry));
    approach_delay = 0;
}

// The planet's text for the advisor notices: its type's Text_ID text, from the planet metadata alone. The notices carry no
// discovery or visibility gate (WGUI-179), so this deliberately does not use the HUD facts' discovery-gated display name.
std::string GalacticMode::State::planet_notice_name(sim::galactic::PlanetId planet) const {
    const auto at = std::find_if(metadata.planets.begin(), metadata.planets.end(),
        [&](const auto& row) { return row.id == planet; });
    if (at == metadata.planets.end()) return {};
    std::string type_text;
    if (audio_catalog) {
        const auto type = audio_catalog->resolve(at->name, data::Category::game_object);
        if (type) if (const auto* field = type.value().value("Text_ID"))
            type_text = galactic_view::authored_display_text(field->value.raw_text, {}, battle_text);
    }
    return galactic_view::notice_planet_name(at->name, type_text);
}
// A player's faction display name (its Text_ID text), else the faction name; an unassigned owner is the Neutral faction.
std::string GalacticMode::State::faction_display_name(std::optional<sim::galactic::PlayerId> owner) const {
    const auto* affiliation = content ? galactic_view::affiliation_for_owner(*content, owner) : nullptr;
    std::string faction = affiliation ? affiliation->faction : std::string("Neutral");
    if (!audio_catalog) return faction;
    const auto resolved = audio_catalog->resolve(faction, data::Category::faction);
    if (!resolved) return faction;
    const auto* field = resolved.value().value("Text_ID");
    if (!field) return faction;
    return galactic_view::authored_display_text(field->value.raw_text, faction, battle_text);
}

void GalacticMode::State::enqueue_owner_change(sim::galactic::PlanetId planet, std::optional<sim::galactic::PlayerId> old_owner,
    std::optional<sim::galactic::PlayerId> new_owner, std::optional<std::int64_t> bonus) {
    if (advisor_character_seconds < 0 || !approach_label || !hud || !battle_text || !content) return;
    const auto planet_name = planet_notice_name(planet);
    if (planet_name.empty()) return;
    // The stock line names the two owners by their player (faction) display names; an unassigned owner is the Neutral faction.
    std::vector<std::string> arguments{planet_name, faction_display_name(old_owner), faction_display_name(new_owner)};
    const char* key = "TEXT_PLANET_CHANGED_HANDS";
    if (bonus) { key = "TEXT_PLANET_CHANGED_HANDS_BONUS"; arguments.push_back(std::to_string(*bonus)); }
    const auto format = battle_text(key);
    if (format.empty() || format == key) return; // no text database: no invented wording
    AdvisorLine entry;
    entry.text = String::utf8(galactic_view::expand_printf_notice(format, arguments).c_str());
    entry.colour = {255, 24, 39, 255}; // the fixed warning red of the stock line
    approach_text.push_back(std::move(entry)); // ordinary: appended, no sound of its own beyond the queue's message cue
}

void GalacticMode::State::service_advisor_hints() {
    // WGUI-71/GFC-28: only a live single-player campaign's local human gets hints, from the galactic mode frame while the
    // galactic mode is the active one (a battle uses its own mode entry, not serviced here).
    if (!game || options.hud_provider || options.lan_match || tactical || !displayed_snapshot || !galaxy || !battle_text) return;
    if (advisor_character_seconds < 0 || !hud) return; // no advisor popup to drain the queue
    if (clock_gates().active_humans > 1) return;
    if (!hint_data_resolved && audio_catalog) {
        hint_data_resolved = true;
        if (const auto faction = audio_catalog->resolve(galaxy->local.faction, data::Category::faction)) {
            if (const auto* field = faction.value().value("Galactic_Advisor_Hints")) {
                data::tag_trace::used(field->value);
                galactic_hint_keys = galactic_view::hint_keys(field->value.raw_text);
            }
            if (const auto* field = faction.value().value("SFXEvent_HUD_Advisor_Hint")) {
                data::tag_trace::used(field->value);
                const auto names = galactic_view::hint_keys(field->value.raw_text);
                if (!names.empty()) hint_sound = names.front();
            }
        }
    }
    if (galactic_hint_keys.empty() || !hint_interval) return;
    const auto frame = displayed_snapshot->clock().galactic_frame;
    auto& entry = advisor_hints[static_cast<std::size_t>(galactic_view::HintMode::galactic)];
    if (!hint_clock_seen) {
        hint_clock_seen = true;
        galactic_view::first_hint_frame(entry, frame);
    }
    const auto index = galactic_view::due_hint(entry, frame, galactic_hint_keys.size(), *hint_interval);
    if (!index) return;
    const auto& key = galactic_hint_keys[*index];
    const auto text = battle_text(key);
    if (text.empty() || text == key) return; // no text database entry: no invented wording (the attempt still counted)
    AdvisorLine line;
    line.text = String::utf8(text.c_str());
    line.colour = {0, 193, 91, 255}; // WGUI-71: the hint producer's code colour
    line.sound = hint_sound;
    approach_text.push_back(std::move(line)); // ordinary: appended
}

void GalacticMode::State::service_approach(double delta) {
    if (advisor_character_seconds < 0 || !approach_label || !hud || !command_bar) return;
    // GFC-29: outer wall-clock update, independent of strategic pause.
    if (!hud->is_visible_in_tree()) return;
    if (test_advisor && !test_advisor_posted && !metadata.planets.empty()) {
        // Test only: the two lines the owner compared, posted through their ordinary producers.
        test_advisor_posted = true;
        const auto planet = metadata.planets.front().id;
        enqueue_approach(planet);
        const auto queued = approach_text.size();
        enqueue_owner_change(planet, std::nullopt, std::optional<sim::galactic::PlayerId>(galaxy->local.player), std::nullopt);
        if (approach_text.size() == queued && battle_text) {
            // The fixture has no scene content to name the owners from: the same stock line with fixed names.
            const auto format = battle_text("TEXT_PLANET_CHANGED_HANDS");
            const std::vector<std::string> names{planet_notice_name(planet), "Neutral", "Test"};
            AdvisorLine entry;
            entry.text = String::utf8((format.empty() || format == "TEXT_PLANET_CHANGED_HANDS"
                ? std::string("Control changed from Neutral to Test") : galactic_view::expand_printf_notice(format, names)).c_str());
            entry.colour = {255, 24, 39, 255};
            approach_text.push_back(std::move(entry));
        }
    }
    approach_delay = std::max(0.0, approach_delay - std::max(0.0, delta));
    if (approach_delay > 0) approach_alpha = static_cast<std::uint8_t>(std::min(255, int(approach_alpha) + 8));
    else if (approach_alpha) approach_alpha = static_cast<std::uint8_t>(std::max(0, int(approach_alpha) - 8));
    else if (!approach_text.empty() && !map_input.gates.action_menu && !map_input.gates.cinematic
        && !(advanced_map_shell && advanced_map_shell->is_visible_in_tree())) {
        const auto line = std::move(approach_text.front());
        approach_text.pop_front();
        approach_label->set_text(line.text); // in the authored advisor ink; the line's own colour is the history's (WGUI-68)
        approach_delay = advisor_character_seconds * static_cast<double>(line.text.length());
        // WGUI-66/WGP-127: an explicit SFX event wins; otherwise the faction's urgent or message cue.
        if (audio) {
            if (!line.sound.empty()) audio->ui_sound(line.sound);
            else if (line.urgent) audio->advisor_urgent();
            else audio->advisor_message();
        }
        // WGUI-68/87/106, WGP-122: the accepted popup also goes to the archive while the galactic mode is active: the
        // cycle line at submission, the message in its own colour, then the separator line.
        if (!tactical && battle_text && displayed_snapshot) {
            const std::array<std::string, 1> cycle{std::to_string(displayed_snapshot->clock().fiscal_cycle)};
            const auto date_format = battle_text("TEXT_LOG_CYCLE_01");
            const auto separator = battle_text("TEXT_HELP_SEPERATOR");
            galactic_view::archive_submission(advisor_archive,
                date_format == "TEXT_LOG_CYCLE_01" ? std::string() : galactic_view::expand_notice_text(date_format, cycle),
                droid_date_colour.value_or(galactic_view::Rgba{255, 255, 255, 255}),
                std::string(line.text.utf8().get_data()), line.colour,
                separator == "TEXT_HELP_SEPERATOR" ? std::string() : separator,
                droid_separator_colour.value_or(galactic_view::Rgba{255, 255, 255, 255}));
            ++advisor_archive_revision;
        }
    }
    const auto placement = command_bar->placement();
    const auto point = presentation::ui::shell_to_screen({encyclopedia_anchor.x, encyclopedia_anchor.y, 0, 0}, placement);
    const bool shown = approach_alpha > 0 && !encyclopedia_timing.shown;
    const float alpha = approach_alpha / 255.0F;
    // #7444: the popup's bottom-left is the advisor point and it grows upward, as the encyclopedia at the same hint bone
    // (GHUD-C04, CARD-19: (60,430) at 1280x720, the place the original draws its advisor lines). With the authored back the
    // text starts at the back's Offset inside it and wraps to its width less that offset on both sides; the back hugs
    // the text height plus the offset above and below.
    const float scale = static_cast<float>(placement.scale);
    const bool boxed = approach_back != nullptr && approach_back_width_units > 0.0F;
    const Vector2 inset = boxed ? approach_text_offset_units * scale : Vector2();
    const float back_width = boxed ? approach_back_width_units * scale : static_cast<float>(camera.width) - static_cast<float>(point.x);
    const float text_width = std::max(1.0F, back_width - 2.0F * inset.x);
    approach_label->set_size(Vector2(text_width, 0));
    const float text_height = approach_label->get_combined_minimum_size().y;
    const Rect2 panel(static_cast<float>(point.x), static_cast<float>(point.y) - text_height - 2.0F * inset.y,
        back_width, text_height + 2.0F * inset.y);
    approach_label->set_position(panel.position + inset);
    approach_label->set_modulate(Color(1, 1, 1, alpha));
    approach_label->set_visible(shown);
    if (approach_back != nullptr) {
        const std::array<std::pair<TextureRect*, Rect2>, 3> layers{{{approach_glow, panel.grow(popup_glow_units * scale)},
            {approach_frame, panel.grow(popup_edge_units * scale)}, {approach_back, panel}}};
        for (const auto& [image, box] : layers) {
            image->set_position(box.position);
            image->set_size(box.size);
            image->set_visible(shown && boxed && image->get_texture().is_valid());
            image->set_modulate(Color(1, 1, 1, alpha));
        }
        approach_glow->set_modulate(popup_glow_tint(alpha));
        PackedVector2Array points;
        PackedColorArray colours;
        popup_connector(panel, scale, alpha, points, colours);
        approach_connector->set_polygon(points);
        approach_connector->set_vertex_colors(colours);
        approach_connector->set_visible(shown && boxed);
    }
}

} // namespace eawr::presentation::godot_backend
