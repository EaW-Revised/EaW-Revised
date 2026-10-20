#include "galactic_mode_internal.hpp"
#include "ui/build_queue_dial.hpp"
#include "eawr/core/diagnostic.hpp"
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include "render_profile_viewport.hpp"
#include "ui/tactical_hud_internal.hpp"
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/classes/font_variation.hpp>

#include <cmath>
#include <algorithm>
#include <fstream>
#include <sstream>

namespace eawr::presentation::godot_backend {

using namespace galactic_detail;

namespace {

[[nodiscard]] int byte(const float value) { return static_cast<int>(std::lround(value * 255.0F)); }

[[nodiscard]] godot::Rect2 visible_texture_rect(const godot::TextureRect& control) {
    const auto texture = control.get_texture();
    if (texture.is_null()) return {};
    const auto pixels = texture->get_image();
    if (pixels.is_null() || pixels->is_empty()) return {};
    const auto used = pixels->get_used_rect();
    const auto rect = control.get_global_rect();
    const auto pixel_scale = rect.size / godot::Vector2(static_cast<float>(pixels->get_width()),
        static_cast<float>(pixels->get_height()));
    return {rect.position + godot::Vector2(static_cast<float>(used.position.x),
                static_cast<float>(used.position.y)) * pixel_scale,
        godot::Vector2(static_cast<float>(used.size.x), static_cast<float>(used.size.y)) * pixel_scale};
}

[[nodiscard]] std::string rgb(const galactic_view::Rgb& colour) {
    return "[" + std::to_string(byte(colour.r)) + ", " + std::to_string(byte(colour.g)) + ", "
        + std::to_string(byte(colour.b)) + "]";
}

[[nodiscard]] std::string point(const galactic_view::Vec3& value) {
    std::ostringstream output;
    output << "[" << value.x << ", " << value.y << ", " << value.z << "]";
    return output.str();
}

template <class Value>
[[nodiscard]] std::string triple(const std::array<Value, 3>& value) {
    std::ostringstream output;
    output << "[" << value[0] << ", " << value[1] << ", " << value[2] << "]";
    return output.str();
}

} // namespace

void GalacticMode::State::write_report_identity(std::ostringstream& output) const {
    output << "{\n  \"mode\": \"galactic\",\n  \"status\": " << json(status) << ",\n  \"failure\": " << json(failure)
           << ",\n  \"render_profile\": " << render_profile_report() << ",\n  \"campaign\": " << json(options.campaign)
           << ",\n  \"interactive\": " << (interactive ? "true" : "false") << ",\n  \"profile\": " << json(profile)
           << ",\n  \"layers\": [";
    for (std::size_t index = 0; index < layers.size(); ++index) output << (index ? ", " : "") << json(layers[index]);
    const bool campaign_snapshot = content && options.campaign != galactic_view::synthetic_campaign;
    output << "],\n  \"snapshot\": {\"source\": "
           << json(displayed_snapshot ? "LiveGameSession" : "presentation-metadata")
           << ", \"ownership\": " << json(campaign_snapshot ? "authored-starting-forces" : "synthetic-fixture");
    if (galaxy) output << ", \"tick\": " << galaxy->tick;
    output << "}";
}

void GalacticMode::State::write_report_session(std::ostringstream& output) const {
    output << ",\n  \"live_session\": {\"snapshots_presented\": " << snapshots_presented
           << ", \"native_ai_installed_at_start\": " << (options.restore_save ? "null" : native_ai_installed ? "true" : "false")
           << ", \"submitted_orders\": " << submitted_orders
           << ", \"galactic_reveal\": " << (options.galactic_reveal ? "true" : "false");
    if (displayed_snapshot) {
        const auto& clock = displayed_snapshot->clock();
        output << ", \"session_tick\": " << clock.session_tick << ", \"galactic_frame\": " << clock.galactic_frame
               << ", \"fiscal_cycle\": " << clock.fiscal_cycle << ", \"fiscal_start\": " << clock.fiscal_start
               << ", \"fiscal_end\": " << clock.fiscal_end
               << ", \"pause_depth\": " << clock.pause_depth
               << ", \"strategic_speed_raw\": " << clock.strategic_speed.raw()
               << ", \"snapshot_sha256\": " << json(published_snapshot->sha256());
    }
    // Events belong to this displayed immutable snapshot, not a render-frame counter.
    output << ", \"snapshot_events\": [";
    for (std::size_t index = 0; index < snapshot_events.size(); ++index) {
        const auto& event = snapshot_events[index];
        output << (index ? ", " : "") << "{\"tick\": " << event.tick << ", \"kind\": " << json(sim::galactic::to_string(event.kind))
               << ", \"seat\": " << event.producer.value << ", \"subject\": " << event.subject
               << ", \"sequence\": " << event.sequence << ", \"value\": " << event.value
               << ", \"reason\": " << json(sim::galactic::to_string(event.reason)) << "}";
    }
    output << "], \"clock_orders\": [";
    if (game) {
        // The accepted journal distinguishes pause's speed reset from a second purchase.
        bool first = true;
        for (const auto& command : game->record().commands) {
            const char* type = std::holds_alternative<sim::galactic::SetSpeed>(command.payload) ? "SetSpeed"
                : std::holds_alternative<sim::galactic::PauseGalaxy>(command.payload) ? "PauseGalaxy"
                : std::holds_alternative<sim::galactic::ResumeGalaxy>(command.payload) ? "ResumeGalaxy" : nullptr;
            if (!type) continue;
            output << (first ? "" : ", ") << "{\"type\": " << json(type)
                   << ", \"issuer\": " << command.key.seat.value << ", \"session_tick\": " << command.key.tick << "}";
            first = false;
        }
    }
    output << "]}";
    output << ",\n  \"perf_overlay\": " << perf_report_json();
}

void GalacticMode::State::write_report_ai(std::ostringstream& output) const {
    // Log-only: what the runtime installed, and its per-player activity counters.
    output << ",\n  \"native_ai_players\": [";
    for (std::size_t i = 0; i < native_ai_players.size(); ++i)
        output << (i ? ", " : "") << "{\"player\": " << native_ai_players[i].player << ", \"faction\": "
               << json(native_ai_players[i].faction) << ", \"player_type\": " << json(native_ai_players[i].player_type) << "}";
    output << "]";
    if (ai_activity && !options.all_ai_capture) output << ",\n  \"ai_activity\": " << capture_metrics.json();
    if (options.all_ai_capture) {
        output << ",\n  \"ai_proof\": " << capture_metrics.json() << ",\n  \"proof_capture_frames\": [";
        for (std::size_t i = 0; i < ai_capture_frames.size(); ++i) output << (i ? "," : "") << ai_capture_frames[i];
        output << "]";
        output << ",\n  \"ai_proof_seed\": " << options.capture_seed.value_or(1) << ",\n  \"ai_proof_factions\": [";
        for (std::size_t i = 0; i < metadata.players.size(); ++i) {
            const auto& player = metadata.players[i];
            output << (i ? "," : "") << "{\"player\":" << player.id << ",\"faction\":" << json(player.faction) << "}";
        }
        output << "]";
    }
}

void GalacticMode::State::write_report_controls(std::ostringstream& output) const {
    output << ",\n  \"technology\": {\"player\": " << (galaxy ? galaxy->local.player : 0);
    if (displayed_snapshot && galaxy) {
        const auto facts = galactic_view::published_player_hud_facts(*displayed_snapshot, galaxy->local.player);
        output << ", \"displayed\": " << (facts.displayed_tech ? std::to_string(*facts.displayed_tech) : "null");
        if (displayed_snapshot->player_economy()) for (const auto& account : displayed_snapshot->player_economy()->players) {
            if (account.player != galaxy->local.player) continue;
            output << ", \"current\": " << account.current_tech << ", \"maximum\": " << account.maximum_tech;
        }
    }
    output << "}";
    output << ",\n  \"battle_dialog\": " << (battle_dialog ? battle_dialog->report_json() : "null");
    output << ",\n  \"campaign_score\": " << (campaign_score ? campaign_score->report_json() : "null")
           << ",\n  \"campaign_score_consumed\": " << (score_consumed ? "true" : "false")
           << ",\n  \"campaign_score_failure\": " << json(score_failure);
    output << ",\n  \"save_controls\": " << (save_controls ? save_controls->report_json() : "null");
    output << ",\n  \"hud_work\": {\"updates\": " << hud_updates
           << ", \"update_nanoseconds\": " << hud_update_nanoseconds
           << ", \"fact_rebuilds\": " << hud_fact_rebuilds
           << ", \"label_updates\": " << hud_label_updates
           << ", \"label_content_updates\": " << hud_label_content_updates
           << ", \"label_placed\": " << hud_label_placed
           << ", \"label_retained\": " << hud_label_retained
           << ", \"label_culled\": " << hud_label_culled
           << ", \"text_style_updates\": " << hud_text_style_updates
           << ", \"offer_rows_visited\": " << hud_offer_rows_visited
           << ", \"max_offer_rows\": " << hud_offer_max_rows
           << ", \"command_updates\": " << hud_command_updates << "}";
}

void GalacticMode::State::write_report_phases(std::ostringstream& output) const {
    output << ",\n  \"presentation_phases\": [";
    constexpr std::array phase_names{"main", "snapshot", "instances", "animation", "sample", "bind", "material_clock", "corruption", "camera_hud", "submit", "routes", "hud_facts", "hud_commands", "hud_fleets", "hud_ground", "hud_orbit", "hud_radar", "hud_labels"};
    static_assert(phase_names.size() == static_cast<std::size_t>(FramePhase::count));
    for (std::size_t index = 0; index < frame_phase_costs.size(); ++index) {
        const auto& cost = frame_phase_costs[index];
        output << (index ? ", " : "") << "{\"name\": " << json(phase_names[index])
               << ", \"calls\": " << cost.calls << ", \"total_ms\": " << cost.milliseconds
               << ", \"worst_call_ms\": " << cost.worst_call_ms
               << ", \"avg_ms\": " << (cost.calls ? cost.milliseconds / static_cast<double>(cost.calls) : 0.0) << "}";
    }
    output << "]";
}

void GalacticMode::State::write_report_corruption(std::ostringstream& output) const {
    std::size_t corruption_effects{}, corruption_resources{}, corruption_reserved{}, corruption_refused{};
    std::uint64_t corruption_particle_count{};
    for (const auto& particles : corruption_particles) if (particles) {
        corruption_effects += particles->live_effects(); corruption_resources += particles->resources();
        corruption_particle_count += particles->particle_count();
        corruption_reserved += particles->reserved_particles(); corruption_refused += particles->refused_effects();
    }
    output << ",\n  \"corruption\": {\"attachments\": " << corruption_visuals.attachments.size()
           << ", \"connections\": " << corruption_visuals.connections.size()
           << ", \"route_visits\": " << corruption_visuals.work().route_visits
           << ", \"neighbor_visits\": " << corruption_visuals.work().neighbor_visits
           << ", \"adjacency_rebuilds\": " << corruption_visuals.work().adjacency_rebuilds
           << ", \"effects\": " << corruption_effects << ", \"resources\": " << corruption_resources
           << ", \"particles\": " << corruption_particle_count
           << ", \"reserved_particles\": " << corruption_reserved << ", \"refused_effects\": " << corruption_refused
           << ", \"budget_per_owner\": 262144"
           << ", \"line_effects\": " << (corruption_lines ? corruption_lines->live_effects() : 0)
           << ", \"line_resources\": " << (corruption_lines ? corruption_lines->resources() : 0)
           << ", \"line_particles\": " << (corruption_lines ? corruption_lines->particle_count() : 0)
           << ", \"line_reserved_particles\": " << (corruption_lines ? corruption_lines->reserved_particles() : 0)
           << ", \"line_refused_effects\": " << (corruption_lines ? corruption_lines->refused_effects() : 0)
           << ", \"path_quads\": " << corruption_path_quads << ", \"mapping\": \"WGP-47..49\", \"parameters\": [";
    for (std::size_t i = 0; content && i < corruption_visuals.attachments.size(); ++i) {
        const auto& fact = corruption_visuals.attachments[i].fact;
        if (i) output << ',';
        const auto colour = galactic_view::corruption_path_colour(content->corruption_style, fact);
        output << "{\"planet\":" << fact.planet << ",\"fraction\":" << fact.fraction
            << ",\"brightness\":" << fact.brightness.value_or(1) << ",\"size\":" << fact.scale.value_or(1)
            << ",\"energy\":" << fact.energy.value_or(1) << ",\"path_rgb\":["
            << colour[0] << ',' << colour[1] << ',' << colour[2] << "]}";
    }
    output << "]}";
}

void GalacticMode::State::write_report_production(std::ostringstream& output) const {
    output << ",\n  \"production_queues\": [";
    std::size_t queue_entries{};
    if (displayed_snapshot && displayed_snapshot->production()) for (const auto& planet : displayed_snapshot->production()->planets) {
        if (planet.planet != selected_planet || !selected_hud.queues_enabled) continue;
        for (std::size_t domain = 0; domain < planet.queues.size(); ++domain)
            for (const auto& entry : planet.queues[domain])
                output << (queue_entries++ ? ", " : "") << "{\"domain\": " << domain << ", \"type\": " << entry.type
                    << ", \"build_id\": " << entry.build_id << ", \"price\": " << entry.price.trunc_to_integer() << "}";
    }
    output << "]";
    // WGUI-40: the build-activity status icon per planet (authored texture size, not a model).
    output << ",\n  \"production_activity\": [";
    std::size_t activity_written = 0;
    for (const auto& [planet, icon] : production_icons) {
        const auto rect = icon->get_global_rect();
        output << (activity_written++ ? ", " : "") << "{\"planet\": " << planet
               << ", \"visible\": " << (icon->is_visible() ? "true" : "false")
               << ", \"texture\": ";
        const String texture = icon->get_meta("production_texture", String{});
        output << json(utf8(texture))
               << ", \"rect\": [" << rect.position.x << ", " << rect.position.y << ", "
               << rect.size.x << ", " << rect.size.y << "]}";
    }
    output << "]";
    output << ",\n  \"production_tooltip\": {\"planet\": " << production_hover_planet
           << ", \"visible\": " << (production_tooltip && production_tooltip->is_visible_in_tree() ? "true" : "false")
           << ", \"delay_ms\": " << production_tooltip_delay << ", \"cells\": [";
    std::size_t tooltip_written{};
    if (production_tooltip && production_tooltip->is_visible_in_tree()) for (std::size_t domain = 0; domain < 2; ++domain) {
        const auto& cell = production_tooltip_cells[domain];
        if (!cell.type) continue;
        const auto rect = cell.icon->get_global_rect();
        output << (tooltip_written++ ? ", " : "") << "{\"domain\": " << domain << ", \"type\": " << cell.type
               << ", \"build\": " << cell.build << ", \"percent\": " << cell.progress
               << ", \"rect\": [" << rect.position.x << ", " << rect.position.y << ", " << rect.size.x << ", " << rect.size.y << "]}";
    }
    output << "]}";
}

void GalacticMode::State::write_report_command_bar(std::ostringstream& output) const {
    output << ",\n  \"command_bar\": {\"faction\": " << json(presentation::ui::to_string(command_faction))
           << ", \"meshes\": " << command_meshes << ", \"cycle\": " << json(player_hud.cycle)
           << ", \"account\": " << json(account_text)
           << ", \"income\": " << json(player_hud.next_income)
           << ", \"population\": " << json(player_hud.population)
           << ", \"income_tooltip\": " << json(income_tooltip)
           << ", \"income_layer_visible\": " << (income_layer_visible ? "true" : "false")
           << ", \"ground_enabled\": " << (selected_hud.ground_enabled ? "true" : "false")
           << ", \"space_tab\": " << (selected_hud.space_tab ? "true" : "false")
           << ", \"technology_visible\": " << (command_faction == presentation::ui::HudFaction::underworld ? "false" : "true")
           << ", \"speed_source\": \"profile selection unavailable (#1979)\", \"pips\": " << fiscal_pips.size()
           << ", \"current_pip\": " << (current_pip ? std::to_string(*current_pip) : "null")
           << ", \"controls\": [";
    std::size_t controls_written = 0;
    for (const auto& [name, button] : command_buttons) {
        const auto rect = button->get_global_rect();
        const auto hit = static_cast<EawrHudButton*>(button)->hit_rect();
        const auto hit_position = rect.position + hit.position;
        output << (controls_written++ ? ", " : "") << "{\"name\": " << json(name)
               << ", \"enabled\": " << (button->is_disabled() ? "false" : "true")
               << ", \"visible\": " << (button->is_visible() ? "true" : "false")
               << ", \"rect\": [" << rect.position.x << ", " << rect.position.y << ", "
               << rect.size.x << ", " << rect.size.y << "]"
               << ", \"hit_rect\": [" << hit_position.x << ", " << hit_position.y << ", "
               << hit.size.x << ", " << hit.size.y << "]}";
    }
    output << "], \"economy_labels\": [";
    std::size_t economy_labels_written{};
    for (const auto& [name, label] : command_labels) {
        if (name != "Text_Day_Credits" && name != "Text_Pop_Cap") continue;
        const auto rect = label->get_global_rect();
        output << (economy_labels_written++ ? ", " : "") << "{\"name\": " << json(name)
            << ", \"text\": " << json(utf8(label->get_text()))
            << ", \"tooltip\": " << json(utf8(label->get_tooltip_text()))
            << ", \"rect\": [" << rect.position.x << ", " << rect.position.y << ", " << rect.size.x << ", " << rect.size.y << "]}";
    }
    // GHUD-S63/S64: the planet block's rows as drawn (text rect, tint, alignment, and each icon's rect).
    output << "], \"planet_block\": [";
    std::size_t block_written{};
    for (const auto& [name, label] : command_labels) {
        const auto rect = label->get_global_rect();
        const auto colour = label->colour();
        const auto icon = command_label_icons.find(name);
        output << (block_written++ ? ", " : "") << "{\"name\": " << json(name)
            << ", \"text\": " << json(utf8(label->get_text()))
            << ", \"visible\": " << (label->is_visible() ? "true" : "false")
            << ", \"align\": " << static_cast<int>(label->get_horizontal_alignment())
            << ", \"glyph_pixels\": " << label->glyph_pixels()
            << ", \"text_width\": " << label->get_minimum_size().x
            << ", \"rect\": [" << rect.position.x << ", " << rect.position.y << ", " << rect.size.x << ", " << rect.size.y << "]"
            << ", \"colour\": [" << byte(colour.r) << ", " << byte(colour.g) << ", " << byte(colour.b) << "]";
        if (icon != command_label_icons.end()) {
            const auto icon_rect = icon->second->get_global_rect();
            output << ", \"icon\": {\"visible\": " << (icon->second->is_visible() ? "true" : "false")
                << ", \"rect\": [" << icon_rect.position.x << ", " << icon_rect.position.y << ", "
                << icon_rect.size.x << ", " << icon_rect.size.y << "]}";
        }
        output << "}";
    }
    if (allegiance_icon) { // absent when ready() failed before the HUD (e.g. a refused restore_save)
        const auto emblem = allegiance_icon->get_global_rect();
        output << (block_written ? ", " : "") << "{\"name\": \"Text_Allegiance_A\", \"visible\": "
            << (allegiance_icon->is_visible() ? "true" : "false") << ", \"rect\": [" << emblem.position.x << ", "
            << emblem.position.y << ", " << emblem.size.x << ", " << emblem.size.y << "]}";
    }
    output << "], \"income_labels\": [";
    std::size_t income_written{};
    for (const auto& [planet, label] : planet_income_labels) {
        const auto rect = label->get_global_rect();
        const auto colour = label->get_theme_color("font_color");
        output << (income_written++ ? ", " : "")
            << "{\"planet\": " << planet << ", \"text\": " << json(utf8(label->get_text()))
            << ", \"visible\": " << (label->is_visible() ? "true" : "false")
            << ", \"rect\": [" << rect.position.x << ", " << rect.position.y << ", " << rect.size.x << ", " << rect.size.y << "]"
            << ", \"colour\": [" << byte(colour.r) << ", " << byte(colour.g) << ", " << byte(colour.b) << "]}";
    }
    output << "], \"production_choices\": [";
    std::size_t choices_written{};
    for (const auto& [name, choice] : displayed_choices) {
        output << (choices_written++ ? ", " : "") << "{\"button\": " << json(name)
            << ", \"type\": " << json(choice.type) << ", \"type_id\": " << choice.type_id
            << ", \"icon\": " << json(choice.icon) << ", \"row\": " << choice.row
            << ", \"eventual\": " << (choice.eventual ? (*choice.eventual ? "true" : "false") : "null")
            << ", \"price\": " << (choice.price ? std::to_string(choice.price->trunc_to_integer()) : "null")
            << ", \"enabled\": " << (choice.enabled ? "true" : "false");
        const auto* price = choice_price.at(name);
        const auto pad = choice_pad.find(name);
        const auto button_rect = command_buttons.at(name)->get_global_rect();
        const auto price_rect = price->get_global_rect();
        const auto price_colour = price->colour();
        const auto& price_style = price->style().text;
        const auto price_face = hud_fonts->resolve({price_style.face, price_style.point_size}, "english");
        const godot::Ref<godot::FontVariation> price_font = price->font();
        output << ", \"pad\": " << json(pad == choice_pad.end() ? std::string{} : pad->second)
            << ", \"price_text\": " << json(utf8(price->get_text()))
            << ", \"price_visible\": " << (price->is_visible_in_tree() ? "true" : "false")
            << ", \"price_font_face\": " << json(price_style.face)
            << ", \"price_font_source\": " << json(presentation::ui::to_string(price_face.source))
            << ", \"price_point_size\": " << price_style.point_size
            << ", \"price_glyph_pixels\": " << price->glyph_pixels()
            << ", \"price_outline\": " << (price_style.outline ? "true" : "false")
            << ", \"price_emboss\": " << (price_style.emboss ? "true" : "false")
            << ", \"price_font_weight\": " << (price_font.is_valid() ? price_font->get_base_font()->get_font_weight() : 0)
            << ", \"price_font_embolden\": " << (price_font.is_valid() ? price_font->get_variation_embolden() : 0.0F)
            << ", \"price_colour\": [" << byte(price_colour.r) << ", " << byte(price_colour.g) << ", "
            << byte(price_colour.b) << ", " << byte(price_colour.a) << "]"
            << ", \"button_rect\": [" << button_rect.position.x << ", " << button_rect.position.y << ", "
            << button_rect.size.x << ", " << button_rect.size.y << "]"
            << ", \"price_rect\": [" << price_rect.position.x << ", " << price_rect.position.y << ", "
            << price_rect.size.x << ", " << price_rect.size.y << "]}";
    }
    output << "], \"queue_capacity\": [" << queue_buttons[1].size() << ", " << queue_buttons[0].size()
        << "], \"queue_slots\": [";
    std::size_t slots_written{};
    for (const auto& [name, row] : displayed_queues) {
        // The queued icon and the build dial each draw at their own texture size inside the button (global pixels).
        const auto* button = static_cast<EawrHudButton*>(command_buttons.at(name));
        const auto* dial = queue_dials.at(name);
        const auto icon_rect = Rect2(button->get_global_position() + button->cell_portrait_box().position,
            button->cell_portrait_box().size);
        const auto dial_rect = dial->get_global_rect();
        output << (slots_written++ ? ", " : "") << "{\"button\": " << json(name)
            << ", \"type\": " << row.type_id << ", \"build_id\": " << row.build_id
            << ", \"space\": " << (row.space ? "true" : "false") << ", \"index\": " << row.index
            << ", \"dial\": " << row.dial << ", \"percent\": " << (row.percent ? std::to_string(*row.percent) : "null")
            << ", \"dial_veiled\": " << (static_cast<EawrBuildQueueDial*>(queue_dials.at(name))->veiled() ? "true" : "false")
            << ", \"icon_rect\": [" << icon_rect.position.x << ", " << icon_rect.position.y << ", "
            << icon_rect.size.x << ", " << icon_rect.size.y << "]"
            << ", \"dial_rect\": [" << dial_rect.position.x << ", " << dial_rect.position.y << ", "
            << dial_rect.size.x << ", " << dial_rect.size.y << "]}";
    }
    output << "]}";
}

void GalacticMode::State::write_report_panels(std::ostringstream& output) const {
    const auto encyclopedia_rect = encyclopedia_panel ? encyclopedia_panel->get_global_rect() : Rect2{};
    std::string encyclopedia_body = encyclopedia_panel ? encyclopedia_panel->get_content().description : std::string{};
    if (encyclopedia_panel && !encyclopedia_panel->get_status().disabled_reason.empty())
        encyclopedia_body += "\n" + encyclopedia_panel->get_status().disabled_reason;
    output << ",\n  \"encyclopedia\": {\"type\": " << encyclopedia_type
        << ", \"planet_ability\": " << (encyclopedia_ability ? "true" : "false")
        << ", \"visible\": " << (encyclopedia_panel && encyclopedia_panel->is_visible() ? "true" : "false")
        << ", \"alpha\": " << encyclopedia_timing.alpha
        << ", \"title\": " << json(encyclopedia_panel ? encyclopedia_panel->get_content().name : std::string{})
        << ", \"body\": " << json(encyclopedia_body)
        << ", \"price\": " << (encyclopedia_panel && encyclopedia_panel->get_status().price
            ? std::to_string(*encyclopedia_panel->get_status().price) : "null")
        << ", \"icon\": " << json(encyclopedia_panel ? encyclopedia_panel->get_content().icon : std::string{})
        << ", \"rect\": [" << encyclopedia_rect.position.x << ", " << encyclopedia_rect.position.y
        << ", " << encyclopedia_rect.size.x << ", " << encyclopedia_rect.size.y << "]"
        << ", \"encyclopedia_active\": " << (encyclopedia_timing.shown ? "true" : "false") << "}";
    output << ",\n  \"planet_ability_diagnostics\": [";
    bool first_ability_diagnostic = true;
    if (content) for (const auto& diagnostic : content->diagnostics) {
        if (!diagnostic.starts_with("planet ")) continue;
        output << (first_ability_diagnostic ? "" : ", ") << json(diagnostic);
        first_ability_diagnostic = false;
    }
    output << "]";
    output << ",\n  \"fleet_hud\": ";
    write_fleet_report(output);
    output << ",\n  \"ground_hud\": ";
    write_ground_report(output);
    output << ",\n  \"orbit_hud\": ";
    write_orbit_report(output);
    output << ",\n  \"radar_hud\": ";
    write_radar_report(output);
    output << ",\n  \"corruption_hud\": ";
    write_corruption_report(output);
    output << ",\n  \"hero_action_hud\": ";
    write_hero_action_report(output);
    output << ",\n  \"destruction_hud\": ";
    write_destruction_report(output);
    output << ",\n  \"weapon_action_hud\": ";
    write_weapon_action_report(output);
    output << ",\n  \"black_market_hud\": ";
    write_black_market_report(output);
    output << ",\n  \"galactic_audio\": ";
    if (audio) audio->write_report(output);
    else output << "null";
    output << ",\n  \"audio_event_gaps\": " << audio_event_gaps;
    output << ",\n  \"map_input\": {\"selected\": " << map_input.selected
           << ", \"deferred\": " << map_input.deferred << ", \"delay\": " << map_input.delay
           << ", \"selection_invalidations\": " << selection_invalidations
           << ", \"tutorial_notifications\": " << tutorial_notifications
           << ", \"discovery_source\": " << json(displayed_snapshot && displayed_snapshot->observer()
               ? "published-local-faction-observer" : (options.campaign == galactic_view::synthetic_campaign ? "synthetic-fixture" : "unavailable")) << "}";
    {
        // WGP-09: the selection cursor's state and how many of its surfaces the world scene draws.
        const auto cursor_model = planet_models.find(selection_cursor_id);
        output << ",\n  \"selection_cursor\": {\"model\": " << json(selection_cursor_model)
               << ", \"loaded\": " << (cursor_model != planet_models.end() && cursor_model->second ? "true" : "false")
               << ", \"shown\": " << (selection_cursor.shown ? "true" : "false")
               << ", \"planet\": " << selection_cursor.planet
               << ", \"tint\": [" << selection_cursor.tint[0] << ", " << selection_cursor.tint[1] << ", "
               << selection_cursor.tint[2] << "], \"instances\": " << std::count_if(scenes[0].instances.begin(),
                   scenes[0].instances.end(), [&](const auto& instance) {
                       return instance_planets.at(instance.entity_id) == selection_cursor_id; }) << "}";
    }
}

void GalacticMode::State::write_report_scene(std::ostringstream& output) const {
    output << ",\n  \"scene_source\": " << json(content ? "validated-campaign-art" : "synthetic-fixture")
           << ",\n  \"paused\": " << (paused ? "true" : "false")
           << ",\n  \"animation_time\": " << animation_time
           << ",\n  \"animation_updates\": " << animation_updates
           << ",\n  \"active_planet_alternate\": " << active_planet_alternate
           << ",\n  \"alternate2_planets\": [";
    for (std::size_t index = 0; index < undiscovered_planets.size(); ++index)
        output << (index ? ", " : "") << undiscovered_planets[index];
    output << "]"
           << ",\n  \"shell_models\": [";
    for (auto it = shell_planets.begin(); it != shell_planets.end(); ++it)
        output << (it == shell_planets.begin() ? "" : ", ") << *it;
    output << "]"
           << ",\n  \"shell_drawn_planets\": [";
    // Read back from each renderer's own applied visibility, not from the submit() decision.
    std::set<galactic_view::ObjectId> drawn_shell;
    const auto collect_shell = [&](const GodotRenderer& target) {
        for (const auto& evidence : target.instance_evidence()) {
            if (!evidence.visible) continue;
            const auto alternate = asset_alternates.find(evidence.asset_id);
            const auto object = instance_planets.find(evidence.entity_id);
            if (alternate == asset_alternates.end() || alternate->second != 2 || object == instance_planets.end()
                || !galaxy_planet_ids.contains(object->second)) continue;
            drawn_shell.insert(object->second);
        }
    };
    if (renderer) collect_shell(*renderer);
    for (std::size_t index = 1; index < scenes.size(); ++index) if (scenes[index].renderer) collect_shell(*scenes[index].renderer);
    for (auto it = drawn_shell.begin(); it != drawn_shell.end(); ++it)
        output << (it == drawn_shell.begin() ? "" : ", ") << *it;
    output << "]"
           << ",\n  \"animate_during_pause\": " << (content && content->animate_during_pause ? "true" : "false")
           << ",\n  \"scene_layers\": [";
    for (std::size_t index = 0; index < scenes.size(); ++index) {
        output << (index ? ", " : "") << "{\"name\": "
               << json(galactic_view::to_string(static_cast<galactic_view::SceneLayer>(index)))
               << ", \"scenario\": " << scenes[index].scenario_id
               << ", \"instances\": " << scenes[index].instances.size()
               << ", \"submitted_instances\": " << scenes[index].submitted_instances
               << ", \"resident_instances\": " << (index == 0 ? (renderer ? renderer->lifecycle_counts().instances : 0)
                   : (scenes[index].renderer ? scenes[index].renderer->lifecycle_counts().instances : 0))
               << ", \"colour_probe\": ";
        // Engine quits remove the tree before PREDELETE; read pixels only while attached.
        const auto sample = scenes[index].viewport && scenes[index].viewport->is_inside_tree()
            ? scenes[index].viewport->get_texture()->get_image() : godot::Ref<godot::Image>{};
        if (sample.is_valid() && !sample->is_empty()) {
            const auto colour = sample->get_pixel(sample->get_width() / 2, sample->get_height() / 2);
            output << "[" << colour.r << ", " << colour.g << ", " << colour.b << ", " << colour.a << "]";
        } else output << "null";
        output << "}";
    }
    output << "],\n  \"light_angles\": ";
    if (content && content->light_angles) output << point(*content->light_angles);
    else output << "null";
}

void GalacticMode::State::write_report_galaxy(std::ostringstream& output) const {
    if (galaxy) {
        const galactic_view::GalaxyView& view = *galaxy;
        // ADR-020: the seat and its faction are reported separately.
        output << ",\n  \"local\": {\"seat\": " << view.local.seat << ", \"player\": " << view.local.player
               << ", \"faction\": " << json(view.local.faction) << ", \"faction_seats\": [";
        for (std::size_t index = 0; index < view.local.faction_seats.size(); ++index) {
            output << (index ? ", " : "") << view.local.faction_seats[index];
        }
        output << "], \"camera_owner\": \"seat\", \"colour_owner\": \"faction\"},\n  \"cameras\": [";
        std::size_t written = 0;
        for (const auto& [seat, orbit] : cameras) {
            output << (written++ ? ", " : "") << "{\"seat\": " << seat << ", \"target\": " << point(orbit.target)
                   << ", \"yaw\": " << orbit.yaw_degrees << ", \"pitch\": " << orbit.pitch_degrees
                   << ", \"distance\": " << orbit.distance << "}";
        }
        output << "],\n  \"flights\": [";
        written = 0;
        for (const auto& [seat, flight] : flights) {
            const auto light = flight.lighting();
            output << (written++ ? ", " : "") << "{\"seat\": " << seat << ", \"state\": "
                   << json(galactic_view::to_string(flight.state())) << ", \"alpha\": " << flight.alpha()
                   << ", \"planet\": " << flight.planet() << ", \"advanced_map\": "
                   << (flight.advanced_map() ? "true" : "false") << ", \"alternate_model_requested\": "
                   << (flight.alternate_model() ? "true" : "false") << ", \"radar_rate\": " << flight.radar_rate()
                   << ", \"light_level\": " << light.level << ", \"light_angles\": " << point(light.angles) << "}";
        }
        output << "],\n  \"camera_constants\": {\"source\": "
               << json(constants_sha256.empty() ? "synthetic-stock-fixture" : "data/xml/gameconstants.xml")
               << ", \"sha256\": " << json(constants_sha256) << ", \"zoom_seconds\": " << camera_constants.zoom_seconds
               << ", \"scroll_plane\": " << camera_constants.scroll_plane << "},\n  \"camera\": {\"eye\": " << triple(camera.eye) << ", \"target\": " << triple(camera.target)
               << ", \"vertical_fov\": " << camera.vertical_fov_degrees << ", \"near\": " << camera.near_plane
               << ", \"far\": " << camera.far_plane << ", \"width\": " << camera.width << ", \"height\": "
               << camera.height << "},\n  \"bounds\": {\"min\": " << point(view.bounds.min) << ", \"max\": "
               << point(view.bounds.max) << "},\n  \"planets\": [";
        for (std::size_t index = 0; index < view.planets.size(); ++index) {
            const galactic_view::PlanetView& planet = view.planets[index];
            const auto chosen = planet_models.find(planet.id);
            const bool drawn = chosen != planet_models.end() && chosen->second.has_value();
            output << (index ? ",\n    " : "\n    ") << "{\"id\": " << planet.id << ", \"name\": " << json(planet.name)
                   << ", \"model\": " << json(planet.model) << ", \"drawn\": " << json(drawn ? "model" : "placeholder")
                   << ", \"owner\": " << (planet.owner ? std::to_string(*planet.owner) : std::string("null"))
                   << ", \"faction\": " << json(planet.owner_faction) << ", \"relation\": "
                   << json(galactic_view::to_string(planet.relation)) << ", \"colour\": " << rgb(planet.colour)
                   << ", \"position\": " << point(planet.position) << ", \"scale\": " << planet.scale
                   << ", \"ability\": {\"name_key\": " << json(planet.ability.name_key)
                   << ", \"description_key\": " << json(planet.ability.description_key)
                   << ", \"icon\": " << json(planet.ability.icon)
                   << ", \"title\": " << json(planet.ability.title)
                   << ", \"description\": " << json(planet.ability.description) << "}"
                   << ", \"layer\": " << json(galactic_view::to_string(static_cast<galactic_view::SceneLayer>(planet_layer(planet.id))));
            if (content && content->art.contains(planet.id)) {
                const auto& art = content->art.at(planet.id);
                const auto appearance = affiliations.find(planet.id);
                output << ", \"pick_radius\": " << art.pick_radius << ", \"label\": " << json(art.label)
                       << ", \"show_name\": " << (art.show_name ? "true" : "false")
                       << ", \"idle\": " << json(art.idle_animation)
                       << ", \"terrain_index\": "
                       << (art.terrain_index ? std::to_string(*art.terrain_index) : std::string("null"));
                if (appearance != affiliations.end()) {
                    output << ", \"secondary_colour\": " << rgb(appearance->second.secondary)
                           << ", \"affiliation_icon\": " << json(appearance->second.icon)
                           << ", \"icon_present\": " << (appearance->second.present ? "true" : "false");
                }
            }
            output << "}";
        }
        output << "],\n  \"routes\": [";
        for (std::size_t index = 0; index < view.routes.size(); ++index) {
            const galactic_view::RouteView& route = view.routes[index];
            output << (index ? ",\n    " : "\n    ") << "{\"id\": " << route.id << ", \"from\": " << route.from
                   << ", \"to\": " << route.to << ", \"typed\": " << (route.typed ? "true" : "false")
                   << ", \"colour\": " << rgb(route.colour);
            const auto from = std::find_if(view.planets.begin(), view.planets.end(),
                [&](const auto& planet) { return planet.id == route.from; });
            const auto to = std::find_if(view.planets.begin(), view.planets.end(),
                [&](const auto& planet) { return planet.id == route.to; });
            output << ", \"discovered\": " << (from != view.planets.end() && to != view.planets.end()
                && route_discovered(*from, *to) ? "true" : "false");
            const auto line = route_lines.find(route.id);
            if (line != route_lines.end()) {
                const auto colour = line->second.colour();
                output << ", \"line_type\": " << json(galactic_view::to_string(line->second.type()))
                       << ", \"line_colour\": [" << colour.r << ", " << colour.g << ", " << colour.b << ", " << colour.a << "]"
                       << ", \"zoom\": " << line->second.zoom() << ", \"width\": " << line->second.style().width
                       << ", \"texture\": " << json(line->second.style().texture)
                       << ", \"spark_texture\": " << json(line->second.style().spark_texture)
                       << ", \"sparks\": " << (line->second.sparks() ? "8" : "0");
            }
            output << "}";
        }
        output << "],\n  \"fleets\": [";
        for (std::size_t index = 0; index < view.fleets.size(); ++index) {
            const galactic_view::FleetMarker& marker = view.fleets[index];
            output << (index ? ",\n    " : "\n    ") << "{\"fleet\": " << marker.fleet << ", \"planet\": "
                   << marker.planet << ", \"owner\": " << marker.owner << ", \"faction\": " << json(marker.faction)
                   << ", \"relation\": " << json(galactic_view::to_string(marker.relation)) << ", \"transit\": "
                   << (marker.transit ? "true" : "false")
                   << ", \"world_model_instances\": " << std::count_if(instance_planets.begin(), instance_planets.end(),
                       [&](const auto& instance) { return instance.second == marker.fleet; })
                   << ", \"fleets\": " << marker.fleets << ", \"members\": "
                   << marker.members << ", \"colour\": " << rgb(marker.colour) << ", \"position\": "
                   << point(marker.position) << ", \"destination\": " << marker.destination
                   << ", \"facing\": " << point(marker.facing) << "}";
        }
        output << "]";
    }
}

void GalacticMode::State::write_report_planet_hud(std::ostringstream& output) const {
    output << ",\n  \"hud\": {\"template\": \"g_planet_name\", \"planet_source\": \"GalacticSnapshot\", "
           << "\"observer_facts\": " << hud_facts.size()
           << ", \"production_source\": \"unavailable\", \"selected\": " << selected_hud.selected
           << ", \"selected_income\": " << json(selected_hud.income)
           << ", \"name_font\": " << json(name_font.face)
           << ", \"name_font_source\": " << json(presentation::ui::to_string(name_font.source))
           << ", \"labels\": [";
    std::size_t labels_written = 0;
    for (const auto& [id, label] : planet_labels) {
        const auto position = label->get_position();
        const auto size = label->get_minimum_size();
        const auto global_rect = label->get_global_rect();
        const auto colour = label->get_theme_color("font_color");
        output << (labels_written++ ? ", " : "") << "{\"planet\": " << id
               << ", \"text\": " << json(utf8(label->get_text()))
               << ", \"visible\": " << (label->is_visible() ? "true" : "false")
               << ", \"rect\": [" << position.x << ", " << position.y << ", " << size.x << ", " << size.y << "]"
               << ", \"global_rect\": [" << global_rect.position.x << ", " << global_rect.position.y
               << ", " << global_rect.size.x << ", " << global_rect.size.y << "]"
               << ", \"colour\": [" << byte(colour.r) << ", " << byte(colour.g) << ", " << byte(colour.b) << "]"
               << ", \"alpha\": " << label->get_modulate().a;
        if (const auto anchor = hud_planet_anchors.find(id); anchor != hud_planet_anchors.end())
            output << ", \"anchor\": [" << anchor->second.x << ", " << anchor->second.y << "]";
        output << "}";
    }
    output << "], \"status_icons\": [";
    std::size_t status_written = 0;
    for (const auto& [kind, icons] : {std::pair{"build", &production_icons}, std::pair{"corruption_info", &corruption_info_icons}}) {
        for (const auto& [id, image] : *icons) {
            const auto rect = image->get_global_rect();
            output << (status_written++ ? ", " : "") << "{\"planet\": " << id << ", \"kind\": " << json(kind)
                   << ", \"visible\": " << (image->is_visible() ? "true" : "false")
                   << ", \"rect\": [" << rect.position.x << ", " << rect.position.y << ", " << rect.size.x << ", " << rect.size.y << "]";
            if (const auto anchor = hud_planet_anchors.find(id); anchor != hud_planet_anchors.end())
                output << ", \"anchor\": [" << anchor->second.x << ", " << anchor->second.y << "]";
            output << "}";
        }
    }
    output << "], \"intelligence\": [";
    const auto optional_number = [](const auto& value) { return value ? std::to_string(*value) : std::string("null"); };
    std::size_t facts_written = 0;
    for (const auto& [id, facts] : hud_facts) {
        output << (facts_written++ ? ", " : "") << "{\"planet\": " << id
               << ", \"discovered\": " << (facts.discovered ? "true" : "false")
               << ", \"name\": " << json(facts.display_name)
               << ", \"owner\": " << optional_number(facts.observed_owner)
               << ", \"income\": " << optional_number(facts.visible_income)
               << ", \"apparent_owner\": " << optional_number(facts.apparent_owner)
               << ", \"apparent_allegiance_owner\": " << optional_number(facts.apparent_allegiance_owner)
               << ", \"ground\": " << optional_number(facts.ground_level)
               << ", \"space\": " << optional_number(facts.space_level)
               << ", \"structures\": " << optional_number(facts.structures)
               << ", \"control\": " << optional_number(facts.political_control)
               << ", \"hover\": " << json(galactic_view::unverified_intelligence_text(facts)) << "}";
    }
    output << "], \"level_pips\": [";
    std::size_t pips_written{};
    for (const auto& [id, levels] : planet_levels) {
        const auto write = [&](const auto& images, const char* domain) {
            for (const auto* image : images) {
                const auto rect = image->get_global_rect();
                const auto ink = visible_texture_rect(*image);
                const auto colour = image->get_modulate();
                output << (pips_written++ ? ", " : "") << "{\"planet\": " << id << ", \"domain\": " << json(domain)
                    << ", \"visible\": " << (image->is_visible() ? "true" : "false")
                    << ", \"rect\": [" << rect.position.x << ", " << rect.position.y << ", " << rect.size.x << ", " << rect.size.y << "]"
                    << ", \"ink_rect\": [" << ink.position.x << ", " << ink.position.y << ", " << ink.size.x << ", " << ink.size.y << "]"
                    << ", \"colour\": [" << byte(colour.r) << ", " << byte(colour.g) << ", " << byte(colour.b) << "]}";
            }
        };
        write(levels.ground, "ground"); write(levels.space, "space");
    }
    output << "], \"base_levels_visible\": " << (base_levels_visible ? "true" : "false") << "}";
}

void GalacticMode::State::write_report_battle_proof(std::ostringstream& output) const {
    output << ",\n  \"tactical_admission\": " << galactic_view::battle_tactical_report_json(
        battle_dialog ? battle_dialog->view() : galactic_view::BattleDialogView{}, json);
    output << ",\n  \"space_cycle_proof\": {\"mode\": " << json(options.space_cycle_proof)
           << ",\"moved\":" << (proof_move_submitted ? "true" : "false")
           << ",\"merge_orders\":" << proof_merge_orders
           << ",\"original_members\":" << proof_original_members.size()
           << ",\"fight_enabled\":" << (proof_fight_enabled ? "true" : "false")
           << ",\"save_ready\":" << (proof_save_ready ? "true" : "false")
           << ",\"attack_tick\":" << proof_attack_tick
           << ",\"attack_target\":" << proof_attack_target << ",\"approach_target\":" << proof_approach_target
           << ",\"retreat_requested\":" << (proof_retreat_requested ? "true" : "false")
           << ",\"summary_seen\":" << (proof_summary_seen ? "true" : "false")
           << ",\"acknowledged\":" << (proof_summary_acknowledged ? "true" : "false")
           << ",\"won\":" << (proof_won ? "true" : "false")
           << ",\"invasion_offered\":" << (proof_invasion_offered ? "true" : "false")
           << ",\"summary_milliseconds\":" << proof_summary_milliseconds
           << ",\"survivors\":" << proof_survivors
           << ",\"survivors_preserved\":" << (proof_survivors_preserved ? "true" : "false")
           << ",\"losses\":[" << proof_losses[0] << ',' << proof_losses[1] << ']'
           << ",\"retreat_started\":" << (proof_retreat_started ? "true" : "false")
           << ",\"retreat_cancelled\":" << (proof_retreat_cancelled ? "true" : "false")
           << ",\"retreat_restarted\":" << (proof_retreat_restarted ? "true" : "false")
           << ",\"retreat_button\":{\"enabled\":" << (proof_retreat_button_enabled ? "true" : "false")
           << ",\"flashing\":" << (proof_retreat_button_flashing ? "true" : "false")
           << ",\"idle_after_cancel\":" << (proof_retreat_button_idle ? "true" : "false") << ",\"point\":";
    if (proof_retreat_button_point)
        output << '[' << (*proof_retreat_button_point)[0] << ',' << (*proof_retreat_button_point)[1] << ']';
    else output << "null";
    output << '}'
           << ",\"map\":" << json(proof_map) << ",\"captures\":[";
    for (std::size_t index = 0; index < proof_captures.size(); ++index)
        output << (index ? "," : "") << json(proof_captures[index]);
    output << "],\"input_refusals\":[";
    if (game) {
        const auto refused = game->rejected_orders();
        for (std::size_t index = 0; index < refused.size(); ++index)
            output << (index ? "," : "") << json(core::format_diagnostic(refused[index]));
    }
    output << "],\"unfinished_battle\":{\"tick\":";
    const auto proof_frame = game && !options.space_cycle_proof.empty() ? game->frame() : platform::LiveGameFrame{};
    output << (proof_frame.battle ? proof_frame.battle->completed_tick() : 0) << ",\"units\":[";
    if (proof_frame.battle) {
        bool first = true;
        for (const auto& unit : proof_frame.battle->instances()) {
            output << (first ? "" : ",") << "{\"id\":" << unit.entity_id << ",\"owner\":" << unit.owner
                   << ",\"type\":" << unit.type_id << ",\"hull\":";
            if (unit.durability) output << unit.durability->hull.raw(); else output << "null";
            output << '}'; first = false;
        }
    }
    output << "]}}";
}

void GalacticMode::State::write_report_handoff(std::ostringstream& output) const {
    output << ",\n  \"campaign_briefing\": {\"conflict\":" << briefing_conflict
           << ",\"done\":" << (briefing_done ? "true" : "false")
           << ",\"accepted\":" << (briefing_accepted ? "true" : "false")
           << ",\"intro_requested\":" << (intro_requested ? "true" : "false")
           << ",\"movie\":" << json(briefing_movie) << ",\"movie_playback\":false,\"intro_playback\":false}";
    output << ",\n  \"handoff\": {\"requested\": " << (options.synthetic_battle ? "true" : "false")
           << ", \"conflict\": " << conflict << ", \"returned\": " << (returned ? "true" : "false")
           << ", \"child_ready_tick\": " << child_ready_tick << ", \"loading_frames\": " << loading_frames
           << ", \"begin_wait_frames\": " << begin_wait_frames << ", \"begin_wait_last_tick\": " << begin_wait_last_tick
           << ", \"returned_frame\": " << returned_frame << ", \"load_ms\": " << battle_load_ms
           << ", \"godot_memory_bytes\": [" << galaxy_memory << ", " << combined_memory << ", " << return_memory
           << "], \"video_memory_bytes\": [" << galaxy_video_memory << ", " << combined_video_memory << ", " << return_video_memory
           << "], \"galaxy_scene_resident\": " << (galaxy_container ? "true" : "false") << "}";
    output << ",\n  \"battle_viewport\": {\"width\": " << battle_viewport_forwarded.x << ", \"height\": " << battle_viewport_forwarded.y
           << ", \"syncs\": " << battle_viewport_syncs << "}";
    output << ",\n  \"campaign_loading\": " << (loading ? loading->report_json() : "null");
    output << ",\n  \"handoff_cycles\": [";
    for (std::size_t index = 0; index < cycles.size(); ++index) {
        const auto& cycle_receipt = cycles[index];
        output << (index ? ", " : "") << "{\"cycle\": " << index + 1
               << ", \"ready_tick\": " << cycle_receipt.ready_tick << ", \"final_tick\": " << cycle_receipt.final_tick
               << ", \"load_ms\": " << cycle_receipt.load_ms
               << ", \"godot_memory_bytes\": [" << cycle_receipt.galaxy_bytes << ", " << cycle_receipt.peak_bytes << ", " << cycle_receipt.return_bytes
               << "], \"video_memory_bytes\": [" << cycle_receipt.galaxy_video << ", " << cycle_receipt.peak_video << ", " << cycle_receipt.return_video
               << "], \"child_expired\": " << (cycle_receipt.child_expired ? "true" : "false")
               << ", \"feed_expired\": " << (cycle_receipt.feed_expired ? "true" : "false")
               << ", \"scene_expired\": " << (cycle_receipt.scene_expired ? "true" : "false")
               << ", \"galaxy_same_instance\": " << (cycle_receipt.galaxy_same ? "true" : "false") << "}";
    }
    output << "],\n  \"campaign_owner_expired\": " << (owner_expired ? "true" : "false");
}

void GalacticMode::State::write_report_zoom_samples(std::ostringstream& output) const {
    output << ",\n  \"zoom_samples\": [";
    for (std::size_t index = 0; index < zoom_samples.size(); ++index) {
        const auto& sample = zoom_samples[index];
        output << (index ? ", " : "") << "{\"frame\": " << sample.frame
               << ", \"animation_time\": " << sample.animation_time << ", \"alternate\": " << sample.alternate;
        const auto palette = [&](const char* name, const auto& values) {
            output << ", \"" << name << "\": [";
            for (std::size_t component = 0; component < values.size(); ++component)
                output << (component ? ", " : "") << values[component];
            output << "]";
        };
        palette("before", sample.before);
        palette("after", sample.after);
        palette("authored_before", sample.authored_before);
        palette("authored_after", sample.authored_after);
        output << "}";
    }
    output << "]";
}

void GalacticMode::State::write_report_models(std::ostringstream& output) const {
    output << ",\n  \"models\": [";
    for (std::size_t index = 0; index < models.size(); ++index) {
        const ModelUpload& model = models[index];
        output << (index ? ",\n    " : "\n    ") << "{\"logical_path\": " << json(model.logical_path)
               << ", \"sha256\": " << json(model.sha256) << ", \"status\": " << json(model.status)
               << ", \"cause\": " << json(model.cause) << ", \"surfaces\": [";
        for (std::size_t surface = 0; surface < model.surfaces.size(); ++surface) {
            const ModelUpload::Surface& item = model.surfaces[surface];
            output << (surface ? ", " : "") << "{\"mesh\": " << json(item.mesh) << ", \"shader\": " << json(item.shader)
                   << ", \"route\": " << json(item.route) << ", \"texture\": " << json(item.texture)
                   << ", \"alternate\": " << item.alternate
                   << ", \"status\": " << json(item.status) << ", \"cause\": " << json(item.cause) << "}";
        }
        output << "]}";
    }
    output << "],\n  \"instances\": " << instances.size() << ",\n  \"overlay\": {\"line_vertices\": "
           << overlay.line_vertices() << ", \"triangle_vertices\": " << overlay.triangle_vertices()
           << "},\n  \"route_geometry\": {\"strips\": " << route_quads << ", \"sparks\": " << spark_quads
           << ", \"fleet_lines\": " << fleet_line_quads << ", \"animation_updates\": " << route_animation_updates
           << "},\n  \"frames\": " << frames << ",\n  \"capture\": ";
    if (capture_sha256.empty()) {
        output << "null";
    } else {
        output << "{\"sha256\": " << json(capture_sha256) << ", \"png\": {\"width\": " << capture_width
               << ", \"height\": " << capture_height << "}}";
    }
}

bool GalacticMode::State::write_report() const {
    if (options.report_path.empty()) return true;
    std::ostringstream output;
    write_report_identity(output);
    write_report_session(output);
    write_report_ai(output);
    write_report_controls(output);
    write_report_phases(output);
    write_report_corruption(output);
    write_report_production(output);
    write_report_command_bar(output);
    write_report_panels(output);
    write_report_scene(output);
    write_report_galaxy(output);
    write_report_planet_hud(output);
    write_report_battle_proof(output);
    write_report_handoff(output);
    write_report_zoom_samples(output);
    write_report_models(output);
    output << "\n}\n";
    std::error_code error;
    if (!options.report_path.parent_path().empty()) std::filesystem::create_directories(options.report_path.parent_path(), error);
    std::ofstream file(options.report_path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file << output.str();
    return static_cast<bool>(file);
}

} // namespace eawr::presentation::godot_backend
