#include "galactic_mode_internal.hpp"
#include "advisor_duration.hpp"
#include "notice_text.hpp"
#include "map_mode_types.hpp"
#include "viewer_path.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/presentation/ui/fonts.hpp"
#include "eawr/presentation/ui/layout.hpp"
#include "eawr/data/ui/text_database.hpp"
#include "ui/tactical_hud_internal.hpp"
#include "ui/input_routing.hpp"
#include "ui/build_queue_dial.hpp"

#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/os.hpp>
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

}

bool GalacticMode::State::build_hud() {
    // The template is presentation data. Simulation facts never come from it.
    if (!filesystem) return true;
    auto loaded = data::ui::load_command_bar(*filesystem);
    if (!loaded) { failure = core::format_diagnostic(loaded.error()); return false; }
    const auto* component = loaded.value().catalog.find("g_planet_name");
    if (!component) { failure = "the galactic planet name HUD template is absent"; return false; }
    planet_name_style = *component;

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
    return build_command_bar() && build_fleet_hud() && build_radar_hud() && build_ground_hud() && build_orbit_hud() && build_corruption_hud() && build_battle_dialog() && build_campaign_score() && build_hero_action_hud() && build_weapon_action_hud() && build_destruction_hud() && build_black_market_hud();
}

void GalacticMode::State::update_hud() {
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
    service_production_tooltip();
    service_reference_pages(*this);
    { const auto measured = measure_frame(FramePhase::hud_commands); update_command_bar(); }
    { const auto measured = measure_frame(FramePhase::hud_fleets); update_fleet_hud(); }
    { const auto measured = measure_frame(FramePhase::hud_ground); update_ground_hud(); }
    { const auto measured = measure_frame(FramePhase::hud_orbit); update_orbit_hud(); }
    service_encyclopedia();
    { const auto measured = measure_frame(FramePhase::hud_radar); update_radar_hud(); }
    update_corruption_hud();
    update_hero_action_hud();
    update_destruction_hud();
    update_weapon_action_hud();
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
    if (const auto found = planet_labels.find(planet); found != planet_labels.end()) include(found->second);
    if (const auto found = planet_income_labels.find(planet); found != planet_income_labels.end()) include(found->second);
    if (const auto found = planet_levels.find(planet); found != planet_levels.end()) {
        for (const auto* pip : found->second.ground) include(pip);
        for (const auto* pip : found->second.space) include(pip);
        include(found->second.political);
    }
    if (const auto found = production_icons.find(planet); found != production_icons.end()) include(found->second);
    if (const auto found = corruption_info_icons.find(planet); found != corruption_info_icons.end()) include(found->second);
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
        label_flight.state(), label_flight.alpha(), base_levels_visible, income_layer_visible,
        map_input.gates.hud_enabled, map_input.gates.tutorial_locked};
    // Hover timers, clock flashes, encyclopedia fade and radar camera tracking
    // have already been serviced. Stationary labels keep their native controls.
    if (hud_label_key == label_key) return;
    hud_label_key = label_key;
    ++hud_label_updates;
    ++planet_label_pass;
    const PlanetLabelEnvironment environment{camera.width, camera.height, base_levels_visible, income_layer_visible,
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
        for (const auto& [id, buttons] : visible_fleet_badges) if (!visible(id))
            for (auto* button : buttons) button->hide();
        for (const auto& [id, icon] : production_icons) {
            const auto facts = hud_facts.find(id);
            if (!visible(id) || facts == hud_facts.end() || !galactic_view::production_activity(facts->second)) icon->hide();
        }
    };
    if (!displayed_snapshot) {
        for (const auto& [id, levels] : planet_levels) { static_cast<void>(levels); hide_planet_overlays(id); }
        for (const auto& [id, icon] : corruption_info_icons) { static_cast<void>(id); icon->hide(); }
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
    const auto template_offset = style.vec2(data::ui::Field::offset).value_or(data::ui::Vec2{});
    const auto text_offset = style.vec2(data::ui::Field::text_offset).value_or(data::ui::Vec2{});
    const auto& flight = seat_flight();
    const double inward = flight.state() == galactic_view::CameraState::outward ? 1.0 - flight.alpha()
        : (flight.state() == galactic_view::CameraState::overview ? 0.0 : flight.alpha());
    const float alpha = static_cast<float>(galactic_view::overview_name_alpha(inward));
    // A planet whose controls all sit this far outside the screen is not placed again until it comes back.
    constexpr float cull_margin = 32.0F;
    const galactic_view::HudView view{static_cast<float>(camera.width), static_cast<float>(camera.height)};
    destruction_hud.projected = false;
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
        if (observation != hud_facts.end() && observation->second.visible_income) {
            if (const auto* component = command_catalog.find("g_planet_value")) {
                auto& income = planet_income_labels[planet->id];
                const bool created = income == nullptr;
                if (created) {
                    income = memnew(Label); income->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
                    income->set_name(String::utf8(("planet_income_" + std::to_string(planet->id)).c_str()));
                    hud->add_child(income);
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
            anchor += Vector2((template_offset.x + text_offset.x) * ui_scale, -text_offset.y * ui_scale);
            auto [entry, inserted] = planet_labels.try_emplace(planet->id, nullptr);
            if (inserted) {
                entry->second = memnew(Label);
                hud->add_child(entry->second);
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
            const auto minimum = label.get_minimum_size();
            if (!style.flag(data::ui::Field::left_justified)) {
                anchor.x -= minimum.x * (style.flag(data::ui::Field::right_justified) ? 1.0F : unverified_default_name_justification);
            }
            label.set_position(anchor - Vector2(0, minimum.y * unverified_name_vertical_anchor));
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
    for (auto& [id, record] : planet_label_records) if (record.pass != planet_label_pass) { static_cast<void>(id); record.placed = false; }
    std::sort(visible_labels.begin(), visible_labels.end());
    hide_unused_labels();
    income_labels_snapshot = displayed_snapshot;
    income_labels_height = camera.height;
}

Ref<Texture2D> GalacticMode::State::command_texture(const std::string_view name) {
    if (!filesystem || !command_textures || name.empty()) return {};
    if (const auto cached = command_texture_cache.find(name); cached != command_texture_cache.end()) return cached->second;
    const auto slot = presentation::ui::resolve_ui_texture(name,
        command_atlas ? &command_atlas->directory : nullptr,
        presentation::ui::vfs_standalone_textures(*filesystem));
    if (slot.origin == presentation::ui::TextureOrigin::missing)
        UtilityFunctions::push_warning(String::utf8(("unresolved mounted UI texture: " + std::string(name)).c_str()));
    const auto texture = command_textures->texture(slot, 1.0, 1.0);
    command_texture_cache.emplace(std::string(name), texture);
    return texture;
}

void GalacticMode::State::refresh_hud_facts() {
    if (!displayed_snapshot || !galaxy || hud_facts_snapshot == displayed_snapshot) return;
    hud_facts_snapshot = displayed_snapshot;
    ++hud_fact_rebuilds;
    const bool fixture = fixture_enabled(GalacticHudFixture::Feature::intelligence);
    // An unavailable observer supplies no campaign facts. Explicit synthetic
    // fixtures remain isolated from the installed campaign producer.
    if (!displayed_snapshot->observer() && !fixture) {
        if (options.campaign != galactic_view::synthetic_campaign || !options.hud_provider) hud_facts.clear();
        return;
    }
    hud_facts.clear();
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
    // WGUI-91: admission still comes from the publication. Index its offers
    // once, retaining source order for equal authored type-manager positions.
    // hud_facts_snapshot retains the immutable rows until this local index dies;
    // the name/art maps are unchanged while the choices copy their fields.
    struct HudOffer final {
        const sim::galactic::ObservedProductionOffer* row{};
        const std::string* name{};
        const galactic_view::ProductionArt* art{};
    };
    std::vector<HudOffer> offers;
    const auto published_offers = displayed_snapshot->production_offers();
    hud_offer_max_rows = std::max(hud_offer_max_rows, static_cast<std::uint64_t>(published_offers.size()));
    offers.reserve(published_offers.size());
    for (const auto& row : published_offers) {
        ++hud_offer_rows_visited;
        if (row.player != galaxy->local.player) continue;
        const auto name = production_names.find(row.type);
        if (name == production_names.end()) continue;
        const auto art = production_art.find(name->second);
        if (art == production_art.end()) continue;
        offers.push_back({&row, &name->second, &art->second});
    }
    std::stable_sort(offers.begin(), offers.end(), [](const auto& a, const auto& b) {
        if (a.row->planet != b.row->planet) return a.row->planet < b.row->planet;
        return a.art->type_order < b.art->type_order;
    });
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
        // GHUDL-15: debug build corrupts a planet when a corruption-benefiting
        // faction owns it, at start and on transfer. The sim does not publish
        // that start state yet, so a known benefiting owner stands for it.
        if (facts.observed_owner && corruption_owners.contains(static_cast<std::uint32_t>(*facts.observed_owner)))
            facts.corrupted = true;
        // An observer-filtered fiscal row supersedes the public base display.
        // WGUI-13 basic/actual fallback applies only to an admitted provider row.
        if (const auto* forecast = displayed_snapshot->economy_forecast(); forecast && forecast->player == galaxy->local.player) {
            const auto row = std::lower_bound(forecast->planets.begin(), forecast->planets.end(), location.id,
                [](const auto& value, auto id) { return value.planet < id; });
            if (row != forecast->planets.end() && row->planet == location.id && facts.discovered)
                facts.visible_income = static_cast<double>(row->actual.value_or(row->basic).raw()) / sim::math::Fixed::scale;
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
            const auto first = std::lower_bound(offers.begin(), offers.end(), location.id,
                [](const auto& offer, const auto id) { return offer.row->planet < id; });
            for (auto entry = first; entry != offers.end() && entry->row->planet == location.id; ++entry) {
                auto choice = galactic_view::production_choice_facts(*entry->row);
                choice.type = *entry->name;
                choice.build_tab = entry->art->build_tab;
                choice.base_type = entry->art->base_filter;
                choice.gui_row = entry->art->gui_row; choice.icon = entry->art->icon;
                facts.choices.push_back(std::move(choice));
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
        if (content && facts.apparent_allegiance_owner) {
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
            hud->add_child(image);
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
        const auto size = texture->get_size() * scale;
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

void GalacticMode::State::update_planet_status_icons(const galactic_view::ObjectId id,
    const Vector2 anchor, const float scale, const float alpha) {
    // GHUDL-15: debug build lays the planet's status icons out as one row from
    // the conflict component's offset, shifted below the credit and base-icon
    // rows when those layers are on. The row sits under the name, never above.
    const auto found = hud_facts.find(id);
    const auto* conflict_style = command_catalog.find("g_conflict");
    if (found == hud_facts.end() || !conflict_style || alpha <= 0.0F) return;
    const auto kinds = galactic_view::planet_status_icons(found->second);
    if (kinds.empty()) return;
    const auto authored = [&](const char* name, data::ui::Field field) -> std::optional<data::ui::Vec2> {
        const auto* component = command_catalog.find(name);
        return component ? component->vec2(field) : std::nullopt;
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
    struct Entry final { TextureRect* image{}; Ref<Texture2D> texture; };
    std::vector<Entry> entries;
    std::vector<double> widths;
    for (const auto kind : kinds) {
        // Debug build reads the information slot by a literal index after the
        // eight per-type icons; the authored list may be shorter in a mod.
        const bool build = kind == galactic_view::PlanetStatusIcon::build;
        auto* icons = build ? &production_icons : &corruption_info_icons;
        const auto name = galactic_view::status_icon_texture_name(kind,
            build_icon_tokens(), corruption_planet_icons);
        const Ref<Texture2D> texture = name.empty() ? Ref<Texture2D>{} : command_texture(name);
        // No texture, no map entry: the hide loop and report dereference every slot.
        if (texture.is_null()) continue;
        auto*& slot = (*icons)[id];
        if (!slot) {
            slot = memnew(TextureRect);
            slot->set_mouse_filter(build ? Control::MOUSE_FILTER_STOP : Control::MOUSE_FILTER_IGNORE);
            if (build) slot->set_name(String::utf8(("production_activity_" + std::to_string(id)).c_str()));
            slot->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE); hud->add_child(slot);
        }
        if (build) slot->set_meta("production_texture", String::utf8(name.c_str()));
        entries.push_back({slot, texture});
        widths.push_back(texture->get_size().x);
    }
    const auto centres = galactic_view::status_row_centres(origin.x, widths);
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto size = entries[index].texture->get_size() * scale;
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
}

void GalacticMode::State::build_command_bar_advisor(const data::ui::TextDatabase* text) {
    approach_label = memnew(Label);
    approach_label->set_name("GalacticApproachAdvisor");
    approach_label->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    approach_label->set_z_index(10);
    approach_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
    if (const auto* style = command_catalog.find("tooltip_left_text")) {
        const auto face = hud_fonts->resolve({std::string(style->text(data::ui::Field::font_name)),
            style->integer(data::ui::Field::font_point_size).value_or(0)}, "english");
        approach_label->add_theme_font_override("font", hud_fonts->font(face));
        approach_label->add_theme_font_size_override("font_size",
            std::max(1, presentation::ui::font_pixel_height(face.point_size, camera.height)));
    }
    approach_label->add_theme_color_override("font_color", Color(0, 193.0F / 255, 91.0F / 255));
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

    // WGUI-35/94: localization comes from the text DB. No invented calendar.
    if (text) {
        if (const auto* entry = text->find("TEXT_CYCLES"))
            cycle_label = data::ui::to_utf8(entry->value);
    }
}

bool GalacticMode::State::build_command_bar_shell() {
    auto* mask = memnew(EawrUiHitMask);
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
    for (const auto& mesh : look.meshes) {
        std::string why;
        const auto path = presentation::ui::vfs_standalone_textures(*filesystem)(mesh.texture);
        if (!path) { failure = "galactic shell texture is absent: " + mesh.texture; return false; }
        auto texture = assets::load_texture(*filesystem, *path);
        if (!texture) { failure = core::format_diagnostic(texture.error()); return false; }
        auto pixels = texture_image(texture.value(), why);
        if (pixels.is_null()) { failure = why; return false; }
        setup.meshes.push_back({mesh.blend, ImageTexture::create_from_image(pixels), mesh.triangles});
    }
    command_meshes = setup.meshes.size();
    command_bar->setup(std::move(setup), nullptr);
    mask->set_hit_test([this](double x, double y) { return command_bar->hit(x, y); });

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
    for (const auto name : {"b_space_tab", "b_planet_tab", "b_play_pause", "b_fast_forward", "b_filters1", "b_filters0",
             "b_scroll_left", "b_scroll_right"}) {
        const auto* component = command_catalog.find(name);
        if (!component || !command_anchors.find(name)) continue;
        auto* button = memnew(EawrHudButton);
        button->set_name(name);
        button->set_toggle_mode(std::string_view(name) == "b_space_tab" || std::string_view(name) == "b_planet_tab"
            || std::string_view(name) == "b_fast_forward" || std::string_view(name) == "b_filters1"
            || std::string_view(name) == "b_filters0");
        const auto keys = component->list(data::ui::Field::tooltip_text);
        if (text && !keys.empty()) if (const auto* entry = text->find(keys.front()))
            button->set_tooltip_text(String::utf8(data::ui::to_utf8(entry->value).c_str()));
        button->set_texture_normal(command_texture(command_bar_token(*component, data::ui::Field::icon_texture_name)));
        button->set_texture_hover(command_texture(command_bar_token(*component, data::ui::Field::mouse_over_texture_name)));
        button->set_texture_pressed(command_texture(command_bar_token(*component, data::ui::Field::selected_texture_name)));
        button->set_texture_disabled(command_texture(command_bar_token(*component, data::ui::Field::disabled_texture_name)));
        button->set_flash_texture(command_texture(command_bar_token(*component, data::ui::Field::flash_texture_name)));
        button->set_action([this, name = std::string(name)] {
            if (name == "b_space_tab") selected_hud.space_tab = true;
            else if (name == "b_filters0") income_layer_visible = !income_layer_visible;
            else if (name == "b_filters1") base_levels_visible = !base_levels_visible;
            else if (name == "b_planet_tab" && selected_hud.ground_enabled) selected_hud.space_tab = false;
            else if (name == "b_play_pause" && clock_controls_enabled) request_pause(!paused);
            else if (name == "b_fast_forward" && galactic_view::fast_forward_available(clock_controls_enabled, paused, fast_forward_factor.has_value()) && game) {
                submit_strategic_speed(galactic_view::fast_forward_target(strategic_speed_now(), *fast_forward_factor));
            }
            update_hud();
        });
        command_bar->add_child(button);
        command_buttons.emplace(name, button);
    }
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

void GalacticMode::State::build_command_bar_choices(const pugi::xml_document& document) {
    // GHUD-S92: native default is 26 total alternating slots; XML may change it.
    std::int32_t choice_slot_limit{26};
    if (const auto node = document.document_element().child("Strategic_Command_Bar_Buildable_Row_Count")) {
        std::istringstream value(node.text().as_string());
        std::int32_t authored{};
        if (value >> authored) choice_slot_limit = std::max(0, authored);
    }
    const auto authored_slots = static_cast<std::size_t>(std::count_if(command_catalog.components().begin(),
        command_catalog.components().end(), [](const auto& component) { return component.in_group("Structure Buttons"); }));
    const auto slot_count = std::min(authored_slots, static_cast<std::size_t>(choice_slot_limit));
    for (std::size_t slot = 0; slot < slot_count; ++slot) {
        // GHUD-S91: native numbered component bindings survive XML definition reordering.
        std::ostringstream name;
        name << "b_Create_" << std::setw(2) << std::setfill('0') << slot;
        const auto* authored = command_catalog.find(name.str());
        const bool present = authored && authored->in_group("Structure Buttons") && command_anchors.find(authored->name);
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
    const bool bonus_hover = planet_bonus_icon && planet_bonus_icon->is_visible_in_tree()
        && pointer == planet_bonus_icon;
    for (const auto& [name, choice] : displayed_choices) {
        if (pointer != command_buttons.at(name)) continue;
        hovered = choice.type_id; hovered_choice = &choice; break;
    }
    if (!hovered) for (const auto& [name, row] : displayed_queues) {
        if (pointer != command_buttons.at(name)) continue;
        hovered = row.type_id; hovered_queue = &row; break;
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
    if (encyclopedia_ability && encyclopedia_planet != selected_planet) {
        encyclopedia_timing.shown = 0; encyclopedia_timing.alpha = 0;
        encyclopedia_timing.remaining = 0;
    }
    encyclopedia_timing.service(bonus_hover ? planet_bonus_identity : hovered, elapsed, admitted);
    if (!admitted) { encyclopedia_panel->hide(); return; }
    if (!encyclopedia_timing.shown) { encyclopedia_panel->hide(); encyclopedia_type = 0; return; }
    if (encyclopedia_timing.shown == planet_bonus_identity) {
        if (!planet_bonus_icon || !planet_bonus_icon->is_visible_in_tree()) { encyclopedia_panel->hide(); return; }
        const auto planet = std::find_if(galaxy->planets.begin(), galaxy->planets.end(),
            [this](const auto& row) { return row.id == selected_planet; });
        if (planet == galaxy->planets.end()) { encyclopedia_panel->hide(); return; }
        if (!encyclopedia_ability || encyclopedia_planet != selected_planet || encyclopedia_snapshot != displayed_snapshot
            || encyclopedia_width != camera.width || encyclopedia_height != camera.height) {
            presentation::ui::UnitCardLooks looks;
            looks.icon = planet->ability.icon; looks.name = planet->ability.title;
            looks.icon_scale = 1.0F; // WGUI-167: bonus header uses full texture scale.
            looks.description = planet->ability.description;
            const auto placement = command_bar->placement();
            const auto point = presentation::ui::shell_to_screen({encyclopedia_anchor.x, encyclopedia_anchor.y, 0, 0}, placement);
            encyclopedia_panel->update(looks, {}, Vector2(static_cast<float>(point.x), static_cast<float>(point.y)),
                command_bar->space(), static_cast<float>(placement.scale),
                Rect2(0, 0, static_cast<float>(camera.width), static_cast<float>(camera.height)));
            encyclopedia_ability = true; encyclopedia_existing_object = false; encyclopedia_type = 0;
            encyclopedia_planet = selected_planet; encyclopedia_snapshot = displayed_snapshot;
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
        const galactic_view::QueueComponent* queue = hovered == encyclopedia_timing.shown ? hovered_queue : nullptr;
        if (!choice && !existing_object) for (const auto& [name, value] : displayed_choices) {
            static_cast<void>(name);
            if (value.type_id == encyclopedia_timing.shown) { choice = &value; break; }
        }
        if (!queue && !existing_object) for (const auto& [name, value] : displayed_queues) {
            static_cast<void>(name);
            if (value.type_id == encyclopedia_timing.shown) { queue = &value; break; }
        }
        if (!choice && !queue && !existing_object) {
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
        encyclopedia_ability = false;
        encyclopedia_existing_object = existing_object;
        encyclopedia_planet = selected_planet;
        encyclopedia_snapshot = displayed_snapshot;
        encyclopedia_width = camera.width; encyclopedia_height = camera.height;
    }
    encyclopedia_panel->set_modulate(Color(1, 1, 1, static_cast<float>(encyclopedia_timing.alpha)));
    encyclopedia_panel->show();
}

void GalacticMode::State::configure_command_bar_text(const presentation::ui::ShellPlacement& placement, EawrHudText& label,
    const data::ui::CommandBarComponent& component, const data::ui::ShellAnchor& anchor) {
    const HudTextStyleKey key{label.get_text(), &component, &anchor, camera.height, placement.scale};
    const auto previous = hud_text_styles.find(&label);
    if (previous != hud_text_styles.end() && previous->second == key) return;
    hud_text_styles.insert_or_assign(&label, key);
    ++hud_text_style_updates;
    auto look = *presentation::ui::hud_readout_style(command_catalog, component.name, anchor);
    auto face = hud_fonts->resolve({look.text.face, look.text.point_size}, "english");
    int pixels = std::max(1, presentation::ui::font_pixel_height(face.point_size, camera.height));
    auto font = hud_fonts->command_font(face, pixels);
    // GHUD-S66: width is tested in shell units; restore the authored size
    // on each refresh and reduce one point at a time, stopping at six.
    const auto max_width = component.integer(data::ui::Field::max_text_width).value_or(0);
    const auto text_scale = component.number(data::ui::Field::scale).value_or(1.0F);
    while (max_width != 0 && look.text.point_size > 6 && placement.scale > 0.0
        && font->get_string_size(label.get_text(), HORIZONTAL_ALIGNMENT_LEFT, -1, pixels).x
            * text_scale / static_cast<float>(placement.scale) > static_cast<float>(max_width)) {
        face = hud_fonts->resolve({look.text.face, --look.text.point_size}, "english");
        pixels = std::max(1, presentation::ui::font_pixel_height(face.point_size, camera.height));
        font = hud_fonts->command_font(face, pixels);
    }
    label.set_style(look, font, [this, face](std::int32_t height) {
        return presentation::ui::gdi_text_cell(hud_fonts->cache(), face, height);
    }, pixels, static_cast<float>(placement.scale),
        1.0F / static_cast<float>(placement.scale)); // GHUD-S66: one source raster pixel at every resolution.
}

void GalacticMode::State::update_command_bar_economy(const sim::galactic::GalacticClock& clock, bool fiscal_available) {
    if (economy_hud_snapshot != displayed_snapshot) {
        auto player = galactic_view::published_player_hud_facts(*displayed_snapshot, galaxy->local.player);
        if (fiscal_available) player.fiscal_cycle = clock.fiscal_cycle;
        player.hide_tech = command_faction == presentation::ui::HudFaction::underworld;
        player_hud = galactic_view::player_hud_view(player, cycle_label);
        income_tooltip = galactic_view::income_forecast_text(displayed_snapshot->economy_forecast());
        if (const auto income = command_labels.find("Text_Day_Credits"); income != command_labels.end()) {
            income->second->set_tooltip_text(String::utf8(income_tooltip.c_str()));
        }
        economy_hud_snapshot = displayed_snapshot;
        account_text.clear();
        if (displayed_snapshot->player_economy()) for (const auto& economy : displayed_snapshot->player_economy()->players) {
            if (economy.player == galaxy->local.player) account_text = galactic_view::credit_balance(economy.credits);
        }
    }
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
                const auto size = texture->get_size();
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
        set_command_bar_text(*label, String::utf8(value.c_str()).to_upper());
        configure_command_bar_text(placement, *label, *component, *anchor);
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
        const auto origin = anchor->origin.value_or(assets::Vec2f{
            anchor->rect.x + anchor->rect.width * 0.5F, anchor->rect.y + anchor->rect.height * 0.5F});
        const auto offset = component->vec2(data::ui::Field::icon_offset).value_or(data::ui::Vec2{});
        const auto size = button->get_texture_normal().is_valid() ? button->get_texture_normal()->get_size()
            : Vector2(anchor->rect.width, anchor->rect.height);
        const auto rect = command_bar_screen(placement, {origin.x + offset.x - size.x * 0.5F,
            origin.y + offset.y - size.y * 0.5F, size.x, size.y});
        button->set_position(rect.position); button->set_size(rect.size);
        auto* hud_button = static_cast<EawrHudButton*>(button);
        // Composite tab art overlaps; only the authored shell mesh is active.
        const auto hit = command_bar_screen(placement, anchor->rect);
        hud_button->set_hit_rect(Rect2(hit.position - rect.position, hit.size));
        // The tab art is one composite per button that already depicts the other tab as selected, and
        // neither tab authors a selected texture: only the unselected tab draws, the selected one is hidden.
        const bool ground_active = !selected_hud.space_tab && selected_hud.ground_enabled;
        if (name == "b_planet_tab") {
            button->set_disabled(!selected_hud.ground_enabled);
            button->set_pressed_no_signal(ground_active);
            button->set_visible(!ground_active);
        } else if (name == "b_space_tab") {
            button->set_pressed_no_signal(!ground_active);
            button->set_visible(ground_active);
        }
        else if (name == "b_filters1") button->set_pressed_no_signal(base_levels_visible);
        else if (name == "b_filters0") button->set_pressed_no_signal(income_layer_visible);
        else if (name == "b_play_pause") {
            button->set_disabled(!clock_controls_enabled);
            hud_button->set_flashing(paused && clock_controls_enabled);
        } else if (name == "b_fast_forward") {
            button->set_disabled(!galactic_view::fast_forward_available(clock_controls_enabled, paused, fast_forward_factor.has_value()));
            button->set_pressed_no_signal(fast_forward_factor && clock.strategic_speed == *fast_forward_factor);
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
    current_pip.reset();
    if (fiscal_available) {
        const double elapsed = clock.galactic_frame >= clock.fiscal_start
            ? static_cast<double>(clock.galactic_frame - clock.fiscal_start) : 0.0;
        current_pip = static_cast<std::size_t>(std::clamp(elapsed * 10.0
            / static_cast<double>(clock.fiscal_end - clock.fiscal_start), 0.0, 9.0));
    }
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

void GalacticMode::State::update_command_bar() {
    if (!command_bar || !displayed_snapshot) return;
    const bool clock_enabled = !map_input.gates.tutorial_locked && map_input.gates.world_allowed()
        && galactic_view::clock_controls_admitted(static_cast<std::size_t>(std::count_if(metadata.seats.begin(), metadata.seats.end(),
            [](const auto& seat) { return seat.active && seat.human; })), bool(options.hud_provider));
    const auto current_key = [&] {
        return HudCommandKey{displayed_snapshot, selected_planet, selected_hud.space_tab,
            base_levels_visible, income_layer_visible, clock_enabled, paused, choice_page,
            fast_forward_factor, seat_flight().state(), command_bar->placement(), camera.height};
    };
    if (hud_command_key == current_key()) return;
    ++hud_command_updates;
    const auto& clock = displayed_snapshot->clock();
    const bool fiscal_available = clock.fiscal_cycle >= 1 && clock.fiscal_end > clock.fiscal_start;
    update_command_bar_economy(clock, fiscal_available);
    const auto selected = hud_facts.find(selected_planet);
    selected_hud = galactic_view::planet_hud_view(selected == hud_facts.end() ? nullptr : &selected->second,
        selected_hud.space_tab, queue_buttons[1].size(), queue_buttons[0].size());
    // WGUI-79: missing tutorial/MP profile admission (#1979) stays an explicit seam.
    clock_controls_enabled = clock_enabled;
    // TM-03/04: a live campaign (no synthetic HUD fixture) fast-forwards at the shared 4x rule; the
    // button stays disabled with more than one human, as in retail network play.
    if (const auto live = galactic_view::live_fast_forward_factor(bool(options.hud_provider), game != nullptr)) fast_forward_factor = live;
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
}

void GalacticMode::State::enqueue_approach(sim::galactic::PlanetId planet) {
    if (advisor_character_seconds < 0 || !approach_label || !hud || !battle_text) return;
    const auto at = std::find_if(metadata.planets.begin(), metadata.planets.end(),
        [&](const auto& row) { return row.id == planet; });
    if (at == metadata.planets.end()) return;
    auto name = at->name;
    if (audio_catalog) {
        auto type = audio_catalog->resolve(name, data::Category::game_object);
        if (type) if (const auto* key = type.value().value("Text_ID")) name = battle_text(key->value.raw_text);
    }
    const std::array<std::string, 1> arguments{name};
    auto line = String::utf8(galactic_view::expand_notice_text(
        battle_text("TEXT_ENEMY_FLEET_APPROACHING_LOCATION"), arguments).c_str());
    // WGUI-66/114: urgent prepend, no duplicate suppression; interrupt removal delay.
    approach_text.push_front(std::move(line));
    approach_delay = 0;
}

void GalacticMode::State::service_approach(double delta) {
    if (advisor_character_seconds < 0 || !approach_label || !hud || !command_bar) return;
    // GFC-29: outer wall-clock update, independent of strategic pause.
    if (!hud->is_visible_in_tree()) return;
    approach_delay = std::max(0.0, approach_delay - std::max(0.0, delta));
    if (approach_delay > 0) approach_alpha = static_cast<std::uint8_t>(std::min(255, int(approach_alpha) + 8));
    else if (approach_alpha) approach_alpha = static_cast<std::uint8_t>(std::max(0, int(approach_alpha) - 8));
    else if (!approach_text.empty() && !map_input.gates.action_menu && !map_input.gates.cinematic
        && !(advanced_map_shell && advanced_map_shell->is_visible_in_tree())) {
        approach_label->set_text(approach_text.front());
        approach_delay = advisor_character_seconds * static_cast<double>(approach_text.front().length());
        approach_text.pop_front();
        if (audio) audio->advisor_urgent();
    }
    const auto placement = command_bar->placement();
    const auto point = presentation::ui::shell_to_screen({encyclopedia_anchor.x, encyclopedia_anchor.y, 0, 0}, placement);
    approach_label->set_position(Vector2(static_cast<float>(point.x), static_cast<float>(point.y)));
    approach_label->set_size(Vector2(static_cast<float>(camera.width) - static_cast<float>(point.x), 0));
    approach_label->set_modulate(Color(1, 1, 1, approach_alpha / 255.0F));
    approach_label->set_visible(approach_alpha > 0 && !encyclopedia_timing.shown);
}

} // namespace eawr::presentation::godot_backend
