#include "galactic_mode_internal.hpp"
#include "raid_input.hpp"
#include "ui/tactical_hud_internal.hpp"
#include "eawr/presentation/ui/layout.hpp"
#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/time.hpp>
#include <ostream>
using namespace godot;
namespace eawr::presentation::godot_backend {
namespace {
// The authored slot captions (the text table's ability-slot group); destruction keeps its tooltip caption.
const char* ability_slot_text(sim::galactic::AbilityKind kind) {
    using sim::galactic::AbilityKind;
    switch (kind) {
    case AbilityKind::black_market: return "TEXT_ABILITY_SLOT_BLACK_MARKET";
    case AbilityKind::corruption: return "TEXT_ABILITY_SLOT_CORRUPT_PLANET";
    case AbilityKind::superweapon_hack: return "TEXT_ABILITY_SLOT_HACK_SUPER_WEAPON";
    case AbilityKind::system_spy: return "TEXT_ABILITY_SLOT_SPY";
    case AbilityKind::technology_theft: return "TEXT_ABILITY_SLOT_SLICE";
    case AbilityKind::structure_sabotage: return "TEXT_ABILITY_SLOT_SABOTAGE";
    case AbilityKind::hero_neutralization: return "TEXT_ABILITY_SLOT_NEUTRALIZE_HERO";
    default: return "TEXT_TOOLTIP_ABILITY_DESTROY_PLANET_NAME";
    }
}
}
bool GalacticMode::State::build_destruction_hud() {
    const auto* style = command_catalog.find("g_special_ability");
    if (!style || !hud || !hud_fonts) return true;
    if (options.hud_provider && !options.hud_provider->destruction(*this)) return false;
    auto& state = destruction_hud;
    state.slot = memnew(EawrHudButton); hud->add_child(state.slot);
    state.slot->set_name("galaxy_destruction_first_slot");
    const auto texture = [&](data::ui::Field field, std::size_t index = 0) {
        const auto names = style->list(field);
        return index < names.size() ? command_texture(names[index]) : Ref<Texture2D>{};
    };
    const auto background = texture(data::ui::Field::blank_texture_name);
    state.slot->set_texture_normal(background);
    // EawrHudButton ignores texture minimum sizes: derive this standalone
    // slot's dimensions from its authored art, like the other HUD adapters.
    if (background.is_valid()) state.authored_size = background->get_size();
    state.slot->set_texture_hover(texture(data::ui::Field::mouse_over_texture_name));
    // WGCH-186: alternate textures are choices, not a default icon. The shared
    // galaxy action preview retains the template's Icon_Texture_Name, if any.
    state.slot->set_order_icon(texture(data::ui::Field::icon_texture_name), false);
    state.slot->set_tooltip_text(String::utf8(battle_text("TEXT_TOOLTIP_ABILITY_DESTROY_PLANET_NAME").c_str()));
    // WGCH-154: a click is only consumed; this surface is neither a drag source
    // nor a confirmation button. EawrFleetDrag owns the left-release route.
    state.slot->set_action([this] { static_cast<void>(map_input.hud_event()); });
    const auto offset = style->vec2(data::ui::Field::offset).value_or(data::ui::Vec2{});
    const auto text_offset = style->vec2(data::ui::Field::text_offset).value_or(data::ui::Vec2{});
    state.authored_offset = Vector2(offset.x, -offset.y);
    state.text_offset = Vector2(text_offset.x, -text_offset.y);
    state.text = memnew(Label); state.slot->add_child(state.text);
    state.text->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    state.text->set_text(String::utf8(battle_text("TEXT_TOOLTIP_ABILITY_DESTROY_PLANET_NAME").c_str()));
    const auto face = hud_fonts->resolve({std::string(style->text(data::ui::Field::font_name)),
        style->integer(data::ui::Field::font_point_size).value_or(0)}, "english");
    state.text->add_theme_font_override("font", hud_fonts->font(face));
    const auto tint = style->color(data::ui::Field::text_color).value_or(data::ui::Rgba8{255,255,255,255});
    state.text->add_theme_color_override("font_color", Color(tint.r/255.0F,tint.g/255.0F,tint.b/255.0F,tint.a/255.0F));
    state.right_justified = style->flag(data::ui::Field::right_justified);
    state.text->set_horizontal_alignment(state.right_justified ? HORIZONTAL_ALIGNMENT_RIGHT : HORIZONTAL_ALIGNMENT_LEFT);
    state.slot->hide();
    return true;
}
std::optional<galactic_view::AbilityDrop> GalacticMode::State::hero_ability_drop(galactic_view::ObjectId source, galactic_view::ObjectId target) const {
    if (!displayed_snapshot || !galaxy || !galaxy_setup.ability_host) return std::nullopt;
    galactic_view::AbilityPreviewPolicy policy;
    policy.rank = [this](sim::galactic::TypeId type) -> std::int64_t {
        const auto name = production_names.find(type);
        const auto art = name == production_names.end() ? production_art.end() : production_art.find(name->second);
        return art == production_art.end() ? 25 : art->second.hero_rank;
    };
    return galactic_view::ability_drop_candidate(*displayed_snapshot, galaxy->local.player, source,
        *galaxy_setup.ability_host, policy, target);
}
void GalacticMode::State::update_destruction_hud() {
    auto& state = destruction_hud;
    if (!state.slot || !displayed_snapshot || !galaxy) return;
    const auto publication = state.fixture ? state.fixture : displayed_snapshot;
    if (publication != state.publication) {
        state.publication = publication; state.facts.reset(); ++state.rebuilds;
        if (const auto* menus = publication->hero_action_menus(); menus && menus->player == galaxy->local.player)
            state.facts = menus->destruction;
        // A rejected order can retain the same preparation. Its pending gate then
        // stays armed until the publication retracts or replaces that capability.
        state.pending.observe(state.facts && state.facts->live_prepared ? &*state.facts->live_prepared : nullptr);
        // Preparation owns ranking, story/tutorial and fleet eligibility
        // (WGCH-152/153). Refusal is an execution fact, never a preview gate.
    }
    const auto& gates = map_input.gates;
    state.enabled = gates.galactic && gates.hud_enabled && gates.world_allowed() && !gates.tutorial_locked
        && !gates.action_menu && !gates.selection_locked && !tactical && !loading
        && !retreat_dialog_open && (!battle_dialog || !battle_dialog->visible())
        && pause_allows(presentation::GalacticPausedAction::ability);
    // The prepared destruction slot (copied or host fact) wins; otherwise the dragged hero fleet's authored action.
    const bool prepared_slot = state.facts && destruction_source(dragged_fleet);
    const auto key = static_cast<const void*>(displayed_snapshot.get());
    const bool changed = dragged_fleet != state.drop_fleet || key != state.drop_publication;
    if (changed) {
        state.drop_fleet = dragged_fleet; state.drop_publication = key; state.raids.clear(); state.abilities.clear();
        if (dragged_fleet && !state.fixture && dragged_stack.size() <= 1 && dragged_members.empty()) {
            const auto planets = input_planets();
            for (const auto& planet : planets)
                if (auto raid = galactic_view::raid_destination_intent(*displayed_snapshot, galaxy->local.player,
                    dragged_fleet, planet.id, planets)) state.raids.push_back(std::move(*raid));
        }
    }
    if (changed && dragged_fleet && galaxy_setup.ability_host && !state.fixture && !prepared_slot) {
        state.abilities = galactic_view::ability_drop_candidates(input_planets(),
            [this](auto planet) { return hero_ability_drop(dragged_fleet, planet); });
        std::erase_if(state.abilities, [&](const auto& ability) {
            return std::any_of(state.raids.begin(), state.raids.end(),
                [&](const auto& raid) { return raid.planet == ability.planet; });
        });
    }
    if (changed || state.drop_target != fleet_drag_planet) {
        state.drop_target = fleet_drag_planet; state.drop.reset();
        const auto found = std::find_if(state.abilities.begin(), state.abilities.end(),
            [&](const auto& ability) { return ability.planet == fleet_drag_planet; });
        if (found != state.abilities.end()) state.drop = *found;
    }
    const auto label = !prepared_slot && state.drop ? std::optional(state.drop->kind) : std::nullopt;
    if (label != state.drop_label) {
        state.drop_label = label;
        state.text->set_text(String::utf8(battle_text(ability_slot_text(
            label.value_or(sim::galactic::AbilityKind::planet_destruction))).c_str()));
        state.slot->set_tooltip_text(state.text->get_text());
    }
    state.slot->set_visible(state.projected && state.enabled && state.raids.empty() && (prepared_slot || (state.drop && state.abilities.empty())));
    for (auto& [planet, slot] : state.raid_slots) {
        const bool candidate = std::any_of(state.raids.begin(), state.raids.end(),
            [&](const auto& raid) { return raid.planet == planet; });
        slot.button->set_visible(candidate && slot.projected && state.enabled);
    }
    for (auto& [planet, slot] : state.ability_slots) {
        const bool candidate = std::any_of(state.abilities.begin(), state.abilities.end(),
            [&](const auto& ability) { return ability.planet == planet; });
        slot.button->set_visible(candidate && slot.projected && state.enabled);
    }
}
void GalacticMode::State::position_destruction_hud(galactic_view::ObjectId planet, Vector2 anchor, float scale, float alpha) {
    auto& state = destruction_hud;
    const auto raid = std::find_if(state.raids.begin(), state.raids.end(), [&](const auto& row) { return row.planet == planet; });
    if (raid != state.raids.end()) {
        auto& slot = state.raid_slots[planet];
        if (!slot.button) {
            const auto* style = command_catalog.find("g_special_ability");
            if (!style) return;
            slot.button = memnew(EawrHudButton); hud->add_child(slot.button);
            slot.button->set_name(String::utf8(("galaxy_raid_slot_" + std::to_string(planet)).c_str()));
            const auto texture = [&](data::ui::Field field, std::size_t index = 0) {
                const auto names = style->list(field);
                return index < names.size() ? command_texture(names[index]) : Ref<Texture2D>{};
            };
            slot.button->set_texture_normal(texture(data::ui::Field::blank_texture_name));
            slot.button->set_texture_hover(texture(data::ui::Field::mouse_over_texture_name));
            // #5506: retail draws the raid slot as the bare dark frame (owner capture 2026-10-21, held-011021:
            // an empty ellipse beside RAID, only the dragged unit's icon over it). The ALT list's raid entry
            // (rebel emblem with its own R box) is not drawn here, so no icon is set.
            slot.button->set_order_icon({}, false);
            slot.button->set_action([this] { static_cast<void>(map_input.hud_event()); });
            slot.text = memnew(Label); slot.button->add_child(slot.text);
            slot.text->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
            slot.text->set_text(String::utf8(battle_text("TEXT_ABILITY_SLOT_RAID").c_str()));
            slot.button->set_tooltip_text(slot.text->get_text());
            const auto face = hud_fonts->resolve({std::string(style->text(data::ui::Field::font_name)),
                style->integer(data::ui::Field::font_point_size).value_or(0)}, "english");
            slot.text->add_theme_font_override("font", hud_fonts->font(face));
            const auto tint = style->color(data::ui::Field::text_color).value_or(data::ui::Rgba8{255,255,255,255});
            slot.text->add_theme_color_override("font_color", Color(tint.r/255.0F,tint.g/255.0F,tint.b/255.0F,tint.a/255.0F));
            slot.text->set_horizontal_alignment(state.right_justified ? HORIZONTAL_ALIGNMENT_RIGHT : HORIZONTAL_ALIGNMENT_LEFT);
        }
        slot.projected = alpha > 0;
        slot.button->set_position(anchor + state.authored_offset * scale);
        slot.button->set_size(state.authored_size * scale);
        slot.button->set_modulate(Color(1,1,1,alpha));
        if (const auto* style = command_catalog.find("g_special_ability"))
            slot.text->add_theme_font_size_override("font_size", std::max(1,
                presentation::ui::font_pixel_height(style->integer(data::ui::Field::font_point_size).value_or(0), camera.height)));
        slot.text->set_size(slot.text->get_minimum_size());
        slot.text->set_position(state.text_offset * scale - Vector2(state.right_justified ? slot.text->get_size().x : 0, 0));
        slot.button->set_visible(slot.projected && state.enabled);
        return;
    }
    const auto ability = std::find_if(state.abilities.begin(), state.abilities.end(), [&](const auto& row) { return row.planet == planet; });
    if (ability != state.abilities.end()) {
        auto& slot = state.ability_slots[planet];
        if (!slot.button) {
            const auto* style = command_catalog.find("g_special_ability");
            if (!style) return;
            slot.button = memnew(EawrHudButton); hud->add_child(slot.button);
            slot.button->set_name(String::utf8(("galaxy_ability_slot_" + std::to_string(planet)).c_str()));
            const auto texture = [&](data::ui::Field field, std::size_t index = 0) {
                const auto names = style->list(field);
                return index < names.size() ? command_texture(names[index]) : Ref<Texture2D>{};
            };
            slot.button->set_texture_normal(texture(data::ui::Field::blank_texture_name));
            slot.button->set_texture_hover(texture(data::ui::Field::mouse_over_texture_name));
            slot.button->set_order_icon(texture(data::ui::Field::icon_texture_name), false);
            slot.button->set_action([this] { static_cast<void>(map_input.hud_event()); });
            slot.text = memnew(Label); slot.button->add_child(slot.text);
            slot.text->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
            const auto face = hud_fonts->resolve({std::string(style->text(data::ui::Field::font_name)),
                style->integer(data::ui::Field::font_point_size).value_or(0)}, "english");
            slot.text->add_theme_font_override("font", hud_fonts->font(face));
            const auto tint = style->color(data::ui::Field::text_color).value_or(data::ui::Rgba8{255,255,255,255});
            slot.text->add_theme_color_override("font_color", Color(tint.r/255.0F,tint.g/255.0F,tint.b/255.0F,tint.a/255.0F));
            slot.text->set_horizontal_alignment(state.right_justified ? HORIZONTAL_ALIGNMENT_RIGHT : HORIZONTAL_ALIGNMENT_LEFT);
        }
        if (slot.kind != ability->kind) {
            slot.kind = ability->kind;
            slot.text->set_text(String::utf8(battle_text(ability_slot_text(ability->kind)).c_str()));
            slot.button->set_tooltip_text(slot.text->get_text());
        }
        slot.projected = alpha > 0;
        slot.button->set_position(anchor + state.authored_offset * scale);
        slot.button->set_size(state.authored_size * scale);
        slot.button->set_modulate(Color(1,1,1,alpha));
        if (const auto* style = command_catalog.find("g_special_ability"))
            slot.text->add_theme_font_size_override("font_size", std::max(1,
                presentation::ui::font_pixel_height(style->integer(data::ui::Field::font_point_size).value_or(0), camera.height)));
        slot.text->set_size(slot.text->get_minimum_size());
        slot.text->set_position(state.text_offset * scale - Vector2(state.right_justified ? slot.text->get_size().x : 0, 0));
        slot.button->set_visible(slot.projected && state.enabled);
        return;
    }
    const auto slot_planet = state.facts && destruction_source(dragged_fleet)
        ? state.facts->planet : state.drop ? state.drop->planet : 0;
    if (!state.slot || !slot_planet || planet != slot_planet) return;
    state.projected = alpha > 0;
    // WGCH-DB-U01: existing authored HUD offset convention; exact special-slot
    // Offset/Text_Offset/right-justify mapping awaits retail pixel qualification.
    state.slot->set_position(anchor + state.authored_offset * scale);
    state.slot->set_size(state.authored_size * scale);
    state.slot->set_modulate(Color(1,1,1,alpha));
    if (const auto* style = command_catalog.find("g_special_ability"))
        state.text->add_theme_font_size_override("font_size", std::max(1,
            presentation::ui::font_pixel_height(style->integer(data::ui::Field::font_point_size).value_or(0), camera.height)));
    state.text->set_size(state.text->get_minimum_size());
    state.text->set_position(state.text_offset * scale - Vector2(state.right_justified ? state.text->get_size().x : 0, 0));
    update_destruction_hud();
}
bool GalacticMode::State::destruction_source(galactic_view::ObjectId source) const {
    const auto& state = destruction_hud;
    if (!source || !state.facts || !displayed_snapshot || !galaxy) return false;
    if (state.fixture) return source == state.facts->source;
    return state.facts->live_prepared
        && state.facts->live_prepared->kind == sim::galactic::AbilityKind::planet_destruction
        && state.facts->live_prepared->actor == state.facts->source
        && state.facts->live_prepared->planet == state.facts->planet
        && !state.pending.matches(owner_lifetime, *state.facts->live_prepared)
        && galactic_view::ability_source_in_fleet(*displayed_snapshot, galaxy->local.player, state.facts->source, source);
}
bool GalacticMode::State::drop_destruction(galactic_view::ObjectId source, bool submit) {
    auto& state = destruction_hud;
    if (!state.enabled || source != dragged_fleet || !destruction_source(source)) return false;
    std::optional<sim::galactic::CommandPayload> command;
    if (!state.fixture) {
        if (!game) return false;
        galactic_view::DropIntent intent;
        intent.kind = galactic_view::DropKind::ability;
        intent.source = state.facts->source; intent.planet = state.facts->planet;
        intent.ability = sim::galactic::ExecutePreparedAbility{*state.facts->live_prepared};
        command = map_input.command(intent, input_planets());
        if (!command) return false;
    }
    if (submit) {
        if (command) {
            state.pending.arm(owner_lifetime, *state.facts->live_prepared);
            game->submit(platform::LiveGameOrder{sim::SeatId{galaxy->local.seat}, std::move(*command), {}});
            ++submitted_orders;
        }
        ++state.accepts; state.accepted_source = state.facts->source; state.accepted_planet = state.facts->planet;
        // WGCH-155/156: the published original actor/container reaches the shared host.
        // Controlled observer fixtures retain their acceptance-only behavior.
    }
    return true;
}
bool GalacticMode::State::drop_raid(galactic_view::ObjectId source, galactic_view::ObjectId planet, bool submit) {
    auto& state = destruction_hud;
    if (!state.enabled || source != dragged_fleet || !game || !displayed_snapshot || !galaxy
        || state.fixture || dragged_stack.size() > 1 || !dragged_members.empty()) return false;
    const auto planets = input_planets();
    const auto intent = galactic_view::raid_destination_intent(*displayed_snapshot, galaxy->local.player, source, planet, planets);
    if (!intent) return false;
    const auto command = map_input.command(*intent, planets);
    if (!command) return false;
    if (submit) {
        game->submit(platform::LiveGameOrder{sim::SeatId{galaxy->local.seat}, *command, {}});
        ++submitted_orders; ++state.raid_accepts; ++fleet_drag_orders;
        last_fleet_drag_kind = galactic_view::DropKind::ability;
        last_fleet_drag_source = source; last_fleet_drag_target = 0; last_fleet_drag_destination = planet;
    }
    return true;
}
void GalacticMode::State::refuse_hero_ability(std::string text) {
    ability_notice_text = std::move(text);
    ability_notice_until_msec = Time::get_singleton()->get_ticks_msec() + 4000;
    if (audio) audio->negative_feedback();
}
void GalacticMode::State::service_ability_notice() {
    if (!game || !galaxy || !host) return;
    // Host-side refusals: the order was queued, and the host answered for the supplied object or the bearer it carries.
    const auto dropped = [&](std::uint64_t subject) {
        const auto source = destruction_hud.accepted_source;
        const auto actors = displayed_snapshot ? displayed_snapshot->actor_records() : std::span<const sim::galactic::CensusObject>{};
        for (std::size_t remaining = actors.size() + 1; source && subject && remaining; --remaining) {
            if (subject == source) return true;
            const auto actor = std::lower_bound(actors.begin(), actors.end(), subject,
                [](const auto& row, auto id) { return row.id < id; });
            if (actor == actors.end() || actor->id != subject) return false;
            subject = actor->container;
        }
        return false;
    };
    const auto feed = game->galactic_events_after(ability_event_cursor, sim::SeatId{galaxy->local.seat});
    ability_event_cursor = feed.through;
    for (const auto& row : feed.events) {
        const auto& event = row.event;
        if (event.kind != sim::galactic::GalacticEventKind::command_rejected
            || event.producer != sim::SeatId{galaxy->local.seat} || !dropped(event.subject)) continue;
        refuse_hero_ability(event.reason == sim::galactic::GalacticRejectReason::incompatible_location
            ? "This hero must be in orbit of that planet to use its ability."
            : "This ability cannot be used on that planet (allied or unavailable target, or nothing left to offer).");
    }
    if (!ability_notice_text.empty() && Time::get_singleton()->get_ticks_msec() >= ability_notice_until_msec) ability_notice_text.clear();
    if (ability_notice_text == ability_notice_shown) return; // No native call on an idle frame.
    ability_notice_shown = ability_notice_text;
    const bool shown = !ability_notice_text.empty();
    if (!ability_notice_label) {
        auto* layer = memnew(CanvasLayer);
        layer->set_layer(90); layer->set_name("GalacticAbilityNoticeLayer");
        ability_notice_label = memnew(Label);
        ability_notice_label->set_name("GalacticAbilityNotice");
        ability_notice_label->set_position({20, 110});
        ability_notice_label->add_theme_color_override("font_shadow_color", Color(0, 0, 0, 1));
        ability_notice_label->add_theme_constant_override("shadow_offset_x", 1);
        ability_notice_label->add_theme_constant_override("shadow_offset_y", 1);
        ability_notice_label->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        layer->add_child(ability_notice_label);
        host->add_child(layer);
    }
    ability_notice_label->set_text(String::utf8(ability_notice_text.c_str()));
    ability_notice_label->set_visible(shown);
}
bool GalacticMode::State::drop_hero_ability(galactic_view::ObjectId source, bool submit, galactic_view::ObjectId target) {
    auto& state = destruction_hud;
    if (!state.enabled || source != dragged_fleet || !game || state.fixture || !galaxy_setup.ability_host) return false;
    // Every refusal on a real release says why; a preview (submit=false) stays silent.
    const auto refuse = [&](const char* text) { if (submit) refuse_hero_ability(text); return false; };
    const auto drop = hero_ability_drop(source, target ? target : state.drop ? state.drop->planet : fleet_drag_planet);
    if (!drop) return refuse("This hero has no ability it can use on that planet.");
    galactic_view::DropIntent intent;
    intent.kind = galactic_view::DropKind::ability;
    intent.source = drop->supplied; intent.planet = drop->planet;
    intent.ability = galactic_view::ability_drop_order(*drop);
    const auto fleet = std::find_if(displayed_snapshot->fleets().begin(), displayed_snapshot->fleets().end(),
        [&](const auto& row) { return row.id == source; });
    const bool surface = fleet == displayed_snapshot->fleets().end();
    const auto from = surface ? galactic_view::actor_containing_planet(*displayed_snapshot, drop->supplied) : fleet->orbit;
    if (!from) return refuse("The hero is not at a planet.");
    if (from != drop->planet) {
        if (!submit) return displayed_snapshot->movement() != nullptr;
        if (surface) {
            const auto path = galactic_view::destination_path(*displayed_snapshot, galaxy->local.player,
                from, drop->planet, input_planets());
            if (!path) return refuse("There is no route for the hero to that planet.");
            intent = {}; intent.kind = galactic_view::DropKind::launch;
            intent.source = drop->supplied; intent.planet = from; intent.path = *path;
        } else {
            const auto movement = galactic_view::fleet_destination_intent(*displayed_snapshot,
                galaxy->local.player, source, drop->planet, input_planets(), {});
            if (!movement) return refuse("The hero cannot travel to that planet.");
            intent = *movement;
        }
        intent.destination_actor = drop->supplied;
    }
    const auto command = map_input.command(intent, input_planets());
    if (!command) return refuse("That order is not available right now.");
    if (submit) {
        // WGCH-154: no confirmation. The prepared publication opens the hero action menu (hero_action_modal);
        // a destruction release queues its execution and the host reports the result.
        game->submit(platform::LiveGameOrder{sim::SeatId{galaxy->local.seat}, *command, {}});
        ++submitted_orders; ++state.prepares;
        state.accepted_source = drop->supplied; state.accepted_planet = drop->planet;
    }
    return true;
}
void GalacticMode::State::write_destruction_report(std::ostream& output) const {
    const auto& state = destruction_hud;
    const auto* facts = state.facts ? &*state.facts : nullptr;
    const auto results = displayed_snapshot ? displayed_snapshot->ability_results() : std::span<const sim::galactic::AbilityResult>{};
    const auto result = std::find_if(results.begin(), results.end(), [&](const auto& row) {
        return row.actor == state.accepted_source && row.planet == state.accepted_planet
            && row.kind == sim::galactic::AbilityKind::planet_destruction;
    });
    const auto visible_abilities = std::count_if(state.ability_slots.begin(), state.ability_slots.end(),
        [](const auto& row) { return row.second.button && row.second.button->is_visible_in_tree(); });
    auto rect = state.slot ? state.slot->get_global_rect() : Rect2{};
    if (visible_abilities) for (const auto& [planet, slot] : state.ability_slots) {
        static_cast<void>(planet);
        if (slot.button && slot.button->is_visible_in_tree()) { rect = slot.button->get_global_rect(); break; }
    }
    output << "{\"source\": " << galactic_detail::json(state.fixture ? "controlled-observer-snapshot" : "campaign-snapshot")
        << ", \"preparation\": \"eight-kind ranked ground-ability producer WGCH-152\", \"execution_seam\": \"typed shared galactic ability host WGUI-172\""
        << ", \"unsupported\": \"siphon #1143; removal #1141; raw restrictions/story #2042; remote slots/travel #4471\""
        << ", \"geometry_qualification\": \"WGCH-DB-U01\", \"second_visible\": " << (visible_abilities > 1 ? "true" : "false")
        << ", \"visible\": " << ((state.slot && state.slot->is_visible_in_tree()) || visible_abilities ? "true" : "false")
        << ", \"actor\": " << (facts ? facts->actor : 0) << ", \"source_id\": " << (facts ? facts->source : 0)
        << ", \"planet\": " << (facts ? facts->planet : 0) << ", \"refusal\": " << (facts ? static_cast<int>(facts->refusal) : 0)
        << ", \"outcome\": " << (result == results.end() ? 0 : static_cast<int>(result->outcome))
        << ", \"raid_accepts\": " << state.raid_accepts
        << ", \"raid_slots\": " << state.raids.size()
        << ", \"raid_slot_icons_shown\": " << std::count_if(state.raid_slots.begin(), state.raid_slots.end(),
            [](const auto& row) { return row.second.button && row.second.button->icon_shown(); })
        << ", \"rebuilds\": " << state.rebuilds << ", \"accepts\": " << state.accepts << ", \"hero_drops\": " << state.prepares << ", \"enabled\": " << (state.enabled ? "true" : "false")
        << ", \"projected\": " << (state.projected ? "true" : "false") << ", \"drop_actor\": " << (state.drop ? state.drop->actor : 0)
        << ", \"drop_kind\": " << (state.drop ? static_cast<int>(state.drop->kind) : 0) << ", \"drop_fleet\": " << state.drop_fleet
        << ", \"accepted_source\": " << state.accepted_source << ", \"accepted_planet\": " << state.accepted_planet
        << ", \"rect\": [" << rect.position.x << ", " << rect.position.y << ", " << rect.size.x << ", " << rect.size.y << "]"
        << ", \"retained_raids\": [";
    bool first_raid = true;
    if (displayed_snapshot) for (const auto& fleet : displayed_snapshot->fleets()) if (fleet.raid_destination) {
        output << (first_raid ? "" : ", ") << "{\"fleet\": " << fleet.id << ", \"destination\": " << *fleet.raid_destination << "}";
        first_raid = false;
    }
    output << "]}";
}
}
