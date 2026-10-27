#include "galactic_mode_internal.hpp"
#include "ground_stack.hpp"
#include "eawr/sim/galactic/ground_intelligence.hpp"
#include "ui/tactical_hud_internal.hpp"
#include "ui/input_routing.hpp"
#include "eawr/presentation/ui/hud_shell.hpp"
#include "eawr/presentation/ui/layout.hpp"
#include "eawr/presentation/ui/fleet_pathfinder.hpp"
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <cmath>
#include <limits>
#include <ostream>

using namespace godot;

namespace eawr::presentation::godot_backend {
namespace {
// GHUD-S53: the shell alternate of a destroyed planet.
constexpr std::uint32_t destroyed_planet_terrain_index = 8U;

std::string first(const data::ui::CommandBarComponent& component, data::ui::Field field) {
    const auto values = component.list(field);
    return values.empty() ? std::string{} : values.front();
}
}

Ref<Texture2D> GalacticMode::State::fleet_faction_texture(const galactic_view::PlayerId owner, const bool galaxy_overview, const bool hover) {
    const auto player = std::find_if(metadata.players.begin(), metadata.players.end(),
        [owner](const auto& row) { return row.id == owner; });
    if (player == metadata.players.end()) return {};
    const auto faction = presentation::ui::hud_faction_from(player->faction);
    if (!faction) return {};
    const auto* component = command_catalog.find(galaxy_overview ? "g_planet_fleet" : "q1_fleet");
    if (!component) return {};
    const auto icons = component->list(hover ? data::ui::Field::mouse_over_texture_name : data::ui::Field::icon_alternate_texture_name);
    std::size_t index = *faction == presentation::ui::HudFaction::empire ? 1
        : *faction == presentation::ui::HudFaction::rebel ? 0 : 2;
    // FoC Config.meg DATA/XML/COMMANDBARCOMPONENTS.XML q1_fleet:
    // i_icon_rebels_big, i_icon_empire_big, i_icon_pirates_big, rebel raid.
    // g_planet_fleet: encyclopedia rebel, empire, pirates, underworld, hutt,
    // raid and death-star symbols. These table orders differ from shell ALT.
    // GHUD-C06: q1_fleet has no Underworld slot; index 2 uses its authored
    // pirates fallback pending a capture of the live FoC Underworld q1 icon.
    // q4's aggregate reuses q1's table; q4_fleet has the same authored entries
    // at indices 0..2 (Rebel/Empire/pirates), without q1's fourth raid entry.
    if (galaxy_overview && *faction == presentation::ui::HudFaction::underworld) index = 3;
    return index < icons.size() ? command_texture(icons[index]) : Ref<Texture2D>{};
}

GalacticMode::State::FleetDragSource GalacticMode::State::pathfinder_source(const std::size_t panel) const {
    if (!published_snapshot || !galaxy || panel >= 3 || !shown_panel_cards[panel] || !shown_panel_cards[panel]->local) return {};
    std::vector<std::uint8_t> slots;
    for (std::uint8_t slot = 0; slot < 3; ++slot)
        if (galactic_view::fleet_panel_for_slot(fleet_panels.mode, slot) == panel) slots.push_back(slot);
    const auto found = presentation::ui::fleet_pathfinder(*published_snapshot, galaxy->local.player, selected_planet, slots);
    if (!found) return {};
    auto type = found->member.type;
    const auto actors = published_snapshot->actor_records();
    const auto actor = std::lower_bound(actors.begin(), actors.end(), found->member.id,
        [](const auto& row, auto id) { return row.id < id; });
    if (actor != actors.end() && actor->id == found->member.id && actor->original_type) type = actor->original_type;
    return {found->fleet, {}, static_cast<std::uint8_t>(panel), type, found->member.id};
}

bool GalacticMode::State::build_fleet_hud() {
    const auto* shell = command_catalog.find("i_main_organize");
    if (!shell) return true; // Optional authored surface; no fabricated placement.
    auto loaded = data::ui::load_shell_anchors(*filesystem, shell->text(data::ui::Field::model_name));
    if (!loaded) { failure = core::format_diagnostic(loaded.error()); return false; }
    fleet_anchors = std::move(loaded).value().shell;
    auto* mask = memnew(EawrUiHitMask);
    mask->set_force_pass_scroll_events(true); // #5085: the organize shell has no wheel action.
    hud->add_child(mask); mask->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
    fleet_organize = memnew(EawrTacticalHud); mask->add_child(fleet_organize);
    EawrTacticalHud::Setup setup;
    setup.view = presentation::ui::hud_view_model(fleet_anchors, command_catalog,
        presentation::ui::alt_variant(command_faction), presentation::ui::vfs_shell_masks(*filesystem));
    // The shell's alternate index is the focused planet's terrain index, not the faction (GHUD-S rows).
    const auto look = presentation::ui::hud_shell(fleet_anchors, command_catalog, command_faction, true);
    for (const auto& mesh : look.meshes) {
        const auto path = presentation::ui::vfs_standalone_textures(*filesystem)(mesh.texture);
        if (!path) { failure = "fleet shell texture is absent: " + mesh.texture; return false; }
        auto texture = assets::load_texture(*filesystem, *path);
        if (!texture) { failure = core::format_diagnostic(texture.error()); return false; }
        std::string why;
        auto pixels = texture_image(texture.value(), why);
        if (pixels.is_null()) { failure = why; return false; }
        setup.meshes.push_back({mesh.blend, ImageTexture::create_from_image(pixels), mesh.triangles, mesh.alternate});
    }
    fleet_organize->setup(std::move(setup), nullptr);
    mask->set_hit_test([this](double x, double y) {
        return fleet_organize->is_visible_in_tree() && fleet_organize->hit(x, y);
    });
    for (const auto& anchor : fleet_anchors.anchors()) {
        const auto& name = anchor.name;
        if (name.size() < 3 || name[0] != 'q' || name[1] < '1' || name[1] > '4' || name[2] != '_') continue;
        const auto* component = command_catalog.find(name);
        if (!component || name.ends_with("_frame")) continue;
        auto* button = memnew(EawrHudButton);
        button->set_sounds(*component, [this](const std::string& cue) { if (audio) audio->ui_sound(cue); });
        button->set_name(String::utf8(name.c_str()));
        button->set_texture_normal(command_texture(first(*component, data::ui::Field::blank_texture_name)));
        button->set_texture_hover(command_texture(first(*component, data::ui::Field::mouse_over_texture_name)));
        button->set_texture_pressed(command_texture(first(*component, data::ui::Field::selected_texture_name)));
        button->set_order_icon({}, false);
        const auto* text_style = name.ends_with("_fleet")
            ? command_catalog.find("text_fleet" + std::string(1, name[1])) : component;
        // GHUD-C06: q4 has no authored text_fleet4; reuse the existing fleet
        // count font pending enemy-count label qualification, never point-size 0.
        if (!text_style && name == "q4_fleet") text_style = command_catalog.find("text_fleet1");
        if (!text_style) text_style = component;
        const auto face = hud_fonts->resolve({std::string(text_style->text(data::ui::Field::font_name)),
            text_style->integer(data::ui::Field::font_point_size).value_or(0)}, "english");
        const auto tint = text_style->color(data::ui::Field::text_color).value_or(data::ui::Rgba8{255,255,255,255});
        if (name.ends_with("_fleet")) {
            // Aggregate labels keep their separate authored text_fleet anchors.
            auto* label = memnew(Label); label->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
            button->add_child(label); fleet_counts.emplace(name, label);
            label->add_theme_font_override("font", hud_fonts->font(face));
            label->add_theme_font_size_override("font_size", std::max(1,
                presentation::ui::font_pixel_height(face.point_size, camera.height)));
            label->add_theme_color_override("font_color", Color(tint.r / 255.0F, tint.g / 255.0F, tint.b / 255.0F, tint.a / 255.0F));
            label->add_theme_constant_override("outline_size", text_style->flag(data::ui::Field::text_outline) ? 1 : 0);
        } else {
            FleetCellResources resources;
            resources.face = face;
            resources.text.font = hud_fonts->font(face);
            resources.text.top = resources.text.bottom = Color(tint.r / 255.0F, tint.g / 255.0F, tint.b / 255.0F, tint.a / 255.0F);
            resources.text.outline = text_style->flag(data::ui::Field::text_outline);
            resources.text.emboss = text_style->flag(data::ui::Field::text_emboss);
            resources.normal = button->get_texture_normal(); resources.pressed = button->get_texture_pressed();
            resources.disabled = command_texture(first(*component, data::ui::Field::disabled_texture_name));
            for (const auto& texture : component->list(data::ui::Field::mouse_over_texture_name))
                resources.hover.push_back(command_texture(texture));
            fleet_cell_resources.emplace(name, std::move(resources));
            button->set_portrait_cell_enabled(true);
        }
        button->set_action([this, name] {
            if (!map_input.hud_event() || !map_input.gates.world_allowed()) return;
            const std::size_t slot = static_cast<std::size_t>(name[1] - '1');
            if (name.ends_with("_0000")) {
                const auto picked = pathfinder_source(slot);
                if (picked.member) { selected_fleet = picked.fleet; selected_fleet_member = picked.member; update_fleet_hud(); }
                return;
            }
            // Cells act on their panel's card; the aggregate icons keep the local slot / other-owner cards.
            const sim::galactic::ObservedFleet* card = name.ends_with("_fleet") ? shown_fleet_cards[slot] : slot < 3 ? shown_panel_cards[slot] : nullptr;
            // The other owner's panel is read-only: its ships cannot be selected or given orders.
            if (card && !card->local && !name.ends_with("_fleet")) return;
            if (!card) return;
            selected_fleet = card->fleet.value_or(0); selected_fleet_member = 0;
            if (!name.ends_with("_fleet") && slot < 3) {
                const auto& buttons = fleet_group_buttons[slot];
                const auto found = std::find(buttons.begin(), buttons.end(), name);
                const auto cell = static_cast<std::size_t>(found - buttons.begin());
                const auto& owners = fleet_cell_layouts[slot].owners;
                if (cell >= owners.size() || !owners[cell]) return;
                const auto index = *owners[cell];
                if (index >= card->groups.size() || !card->groups[index].representative) return;
                selected_fleet_member = *card->groups[index].representative;
                // A merged slot card binds the group's own fleet.
                selected_fleet = card->groups[index].fleet.value_or(selected_fleet);
            }
            update_fleet_hud(); // Presentation selection only; S08 owns drag/move.
        });
        fleet_organize->add_child(button); fleet_buttons.emplace(name, button);
        if (name[1] <= '3' && name.ends_with("_fleet")) {
            attach_fleet_drag(button, name, [this, name, button] {
                if (!button->is_visible_in_tree() || button->is_disabled()) return FleetDragSource{};
                const auto* card = shown_fleet_cards[static_cast<std::size_t>(name[1] - '1')];
                // #3072: the whole card moves all its slot's fleets as one stack.
                if (!card || !card->local || !card->orbit_slot) return FleetDragSource{};
                return FleetDragSource{card->fleet.value_or(0), {*card->orbit_slot}};
            });
        }
        if (!name.ends_with("_fleet") && name[1] <= '3') {
            fleet_group_buttons[static_cast<std::size_t>(name[1] - '1')].push_back(name);
            attach_fleet_drag(button, name, [this, name, button] {
                if (!button->is_visible_in_tree() || button->is_disabled()) return FleetDragSource{};
                const auto slot = static_cast<std::size_t>(name[1] - '1');
                if (name.ends_with("_0000")) return pathfinder_source(slot);
                const auto* card = shown_panel_cards[slot];
                if (!card || !card->local || !card->fleet) return FleetDragSource{};
                const auto& names = fleet_group_buttons[slot];
                const auto cell = static_cast<std::size_t>(std::find(names.begin(), names.end(), name) - names.begin());
                const auto& owners = fleet_cell_layouts[slot].owners;
                if (cell >= owners.size() || !owners[cell]) return FleetDragSource{};
                const auto index = *owners[cell];
                // GHUD-S86: a panel icon binds one admitted unit, distinct from
                // the card header and galaxy stack's whole-fleet drag.
                if (index >= card->groups.size() || !card->groups[index].representative) return FleetDragSource{};
                auto slots = card->orbit_slot ? std::vector<std::uint8_t>{*card->orbit_slot} : std::vector<std::uint8_t>{};
                return FleetDragSource{card->groups[index].fleet.value_or(*card->fleet), std::move(slots),
                    static_cast<std::uint8_t>(slot), card->groups[index].type, *card->groups[index].representative};
            });
        }
    }
    // GHUD-R43: the leading selector has its own snapshot binding, outside ordinary packing.
    for (auto& slots : fleet_group_buttons) {
        std::sort(slots.begin(), slots.end());
        slots.erase(std::remove_if(slots.begin(), slots.end(), [](const auto& name) { return name.ends_with("_0000"); }), slots.end());
    }
    // WGUI-175: TEXT_FLEET_PANE_n captions in the authored text_fleet1..3 style, placed on each panel's frame (the
    // shell's Text_Fleet bones carry no mesh anchor); hidden with the shell.
    for (std::size_t panel = 0; panel < fleet_captions.size(); ++panel) {
        const std::string name = "text_fleet" + std::to_string(panel + 1);
        if (!fleet_anchors.find("q" + std::to_string(panel + 1) + "_frame") || !command_catalog.find(name)) continue;
        auto* label = memnew(EawrHudText);
        label->set_name(String::utf8(("fleet_caption_" + std::to_string(panel + 1)).c_str()));
        label->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        fleet_organize->add_child(label);
        fleet_captions[panel] = label;
    }
    fleet_organize->hide();
    build_fleet_drag_receiver();
    build_fleet_stack_popup();
    return true;
}

// GHUD-S87: save every currently displayed local unit's resolved cell, including
// untouched automatic groups, without changing authoritative membership.
std::map<galactic_view::ObjectId, std::int64_t> GalacticMode::State::fleet_order_for_save() const {
    auto result = fleet_gui_indices;
    if (!displayed_snapshot || !galaxy) return result;
    const auto actors = displayed_snapshot->actor_records();
    for (const auto& fleet : displayed_snapshot->fleets()) {
        if (fleet.owner != galaxy->local.player || fleet.orbit != selected_planet || fleet.orbit_slot >= shown_panel_cards.size()) continue;
        // WGUI-175: with another owner aware here, slots 0 and 2 both list in panel 3.
        const auto panel = galactic_view::fleet_panel_for_slot(fleet_panels.mode, fleet.orbit_slot);
        const auto* card = shown_panel_cards[panel];
        if (!card || !card->local) continue;
        const auto& layout = fleet_cell_layouts[panel];
        for (const auto& member : fleet.members) {
            const auto actor = std::lower_bound(actors.begin(), actors.end(), member.id,
                [](const auto& row, auto key) { return row.id < key; });
            const auto type = actor != actors.end() && actor->id == member.id && actor->original_type
                ? actor->original_type : member.type;
            const auto remembered = fleet_gui_indices.find(member.id);
            const std::optional<std::int64_t> index = remembered == fleet_gui_indices.end() ? std::nullopt : std::optional{remembered->second};
            for (std::size_t group = 0; group < card->groups.size() && group < layout.groups.size(); ++group)
                if (card->groups[group].type == type && card->groups[group].gui_index == index && layout.groups[group]) {
                    result[member.id] = static_cast<std::int64_t>(layout.groups[group]->cell); break;
                }
        }
    }
    return result;
}

// WGUI-166: hovering a stack icon opens the shared fleet encyclopedia popup
// after its authored delay, using observer-filtered contents.
void GalacticMode::State::build_fleet_stack_popup() {
    if (!hud_fonts || !command_bar) return;
    EawrUnitCards::Setup setup;
    std::vector<core::Diagnostic> diagnostics;
    fill_encyclopedia_popup(setup, command_catalog, *filesystem, *hud_fonts, "english", nullptr, diagnostics);
    setup.texture = [this](const std::string& name) { return command_texture(name); };
    setup.name = [](const std::string&) { return String(); };
    setup.encyclopedia = [](const std::string&) { return presentation::ui::UnitCardLooks{}; };
    setup.placement = [this] { return command_bar->placement(); };
    setup.space = [this] { return command_bar->space(); };
    fleet_popup = memnew(EawrUnitCards);
    fleet_popup->set_name("FleetStackEncyclopedia");
    fleet_popup->set_z_index(9);
    hud->add_child(fleet_popup);
    fleet_popup->setup(std::move(setup));
}

// The hovered icon's encyclopedia content: the owner side's fleet name, the
// stack with its best ship, the population of the listed units and one cell
// per listed type. A count-only cell has no icon and names no type.
presentation::ui::UnitCardLooks GalacticMode::State::fleet_stack_looks(const FleetStackBadge& badge) {
    presentation::ui::UnitCardLooks looks;
    const auto art_of = [this](const std::optional<sim::galactic::TypeId>& type) -> const galactic_view::ProductionArt* {
        if (!type) return nullptr;
        const auto name = production_names.find(*type);
        if (name == production_names.end()) return nullptr;
        const auto art = production_art.find(name->second);
        return art == production_art.end() ? nullptr : &art->second;
    };
    std::string title_key = badge.ground_planet ? "TEXT_PIRATE_LAND_GARRISON" : "TEXT_PIRATE_SPACE_FLEET";
    if (content) if (const auto owner = content->factions.find(badge.owner); owner != content->factions.end()) {
        // Debug build fleet name: the GameConstants good/evil/corrupt side, else pirate.
        constexpr std::array<std::string_view, 3> sides{"TEXT_GOOD_SPACE_FLEET", "TEXT_EVIL_SPACE_FLEET", "TEXT_CORRUPT_SPACE_FLEET"};
        constexpr std::array<std::string_view, 3> ground_sides{"TEXT_GOOD_LAND_GARRISON", "TEXT_EVIL_LAND_GARRISON", "TEXT_CORRUPT_LAND_GARRISON"};
        const auto side = presentation::ui::allegiance_emblem_index(owner->second.faction, content->side_names);
        if (side < sides.size()) title_key = badge.ground_planet ? ground_sides[side] : sides[side];
        if (owner->second.present) looks.icon_back_tint = data::ui::Rgba8{
            static_cast<std::uint8_t>(std::lround(std::clamp(owner->second.primary.r, 0.0F, 1.0F) * 255.0F)),
            static_cast<std::uint8_t>(std::lround(std::clamp(owner->second.primary.g, 0.0F, 1.0F) * 255.0F)),
            static_cast<std::uint8_t>(std::lround(std::clamp(owner->second.primary.b, 0.0F, 1.0F) * 255.0F)), 255};
    }
    looks.name = battle_text(title_key);
    // WGUI-92/166: an all-stealth fleet adds the first authored Tooltip_Text key of icon_fleet01.
    if (badge.stealth) if (const auto* tip = command_catalog.find("icon_fleet01")) {
        const auto keys = tip->list(data::ui::Field::tooltip_text);
        if (!keys.empty()) looks.description = battle_text(keys.front());
    }
    if (const auto* art = art_of(badge.best)) looks.icon = art->icon;
    if (const auto* style = command_catalog.find(badge.ground_planet ? "g_planet_land_forces" : "g_planet_fleet")) {
        const auto stacks = style->list(data::ui::Field::blank_texture_name);
        if (!stacks.empty()) looks.icon_back = stacks[std::min(badge.texture, stacks.size() - 1)];
        looks.icon_scale = style->vec2(data::ui::Field::size).value_or(data::ui::Vec2{}).x;
    }
    // The population of the listed types; none when a cell's type is unknown,
    // so count-only knowledge discloses nothing (shared content: negative usage).
    std::int64_t usage = 0;
    bool known = true;
    for (const auto& entry : badge.contents) {
        const auto* art = art_of(entry.type);
        looks.cells.push_back({art ? art->icon : std::string{}, static_cast<std::uint32_t>(entry.count)});
        if (!art) { known = false; continue; }
        usage += static_cast<std::int64_t>(std::max(0, -art->population)) * static_cast<std::int64_t>(entry.count);
    }
    if (known && usage > 0) looks.population = -static_cast<std::int32_t>(std::min<std::int64_t>(usage, std::numeric_limits<std::int32_t>::max()));
    return looks;
}

void GalacticMode::State::service_fleet_stack_popup() {
    if (!fleet_popup) return;
    std::uint64_t hovered{};
    const FleetStackBadge* shown{};
    fleet_popup_admitted = encyclopedia_admitted() && !dragged_fleet && !dragged_ground_planet;
    auto* pointer = hud->get_viewport()->gui_get_hovered_control();
    if (pointer != fleet_popup_pointer_control) {
        fleet_popup_pointer_control = pointer; // name only on change, no per-frame text
        fleet_popup_pointer = pointer ? galactic_detail::utf8(pointer->get_name()) : std::string{};
    }
    if (fleet_popup_admitted && pointer) {
        for (const auto& [key, badge] : fleet_badges) {
            if (!badge.shown || badge.contents.empty() || !badge.button->is_visible_in_tree()) continue;
            // The icon's drag source (attach_fleet_drag) is a child covering it.
            if (pointer != badge.button && !badge.button->is_ancestor_of(pointer)) continue;
            // Key: planet and icon, never zero (zero means no hover).
            hovered = ((key.first << 2U) | key.second) + 1U; shown = &badge; break;
        }
    }
    if (hovered == fleet_popup_key && fleet_popup_publication == fleet_publication) return;
    fleet_popup_key = hovered; fleet_popup_publication = fleet_publication;
    std::optional<presentation::ui::UnitCardLooks> looks;
    if (shown && shown->ground_planet) {
        looks = fleet_stack_looks(*shown);
    } else if (shown && fleet_publication) {
        // WGUI-166: the encyclopedia lists all admitted types. WGUI-72's
        // focused-grid capacity and individually positioned groups belong to
        // the grid, so reuse the observer filter without those display limits.
        // This work runs only when the hovered icon or publication changes.
        const auto planet = (hovered - 1U) >> 2U;
        std::vector<sim::galactic::FleetDisplayVisibility> visibility;
        for (const auto& row : fleet_visibility) if (row.planet == planet) visibility.push_back(row);
        const auto complete = sim::galactic::observed_fleets(*fleet_publication, galaxy->local.player,
            fleet_display_traits, std::numeric_limits<std::size_t>::max(), visibility);
        std::vector<const sim::galactic::ObservedFleet*> cards;
        for (const auto& card : complete.cards) if (card.planet == planet) cards.push_back(&card);
        std::optional<sim::galactic::PlayerId> other;
        for (const auto& row : complete.presence)
            if (row.planet == planet && row.owner != galaxy->local.player) { other = row.owner; break; }
        const auto icons = galactic_view::fleet_stack_icons(cards, {}, 0, other);
        for (const auto& icon : icons) {
            if (icon.icon != ((hovered - 1U) & 3U) || icon.owner != shown->owner) continue;
            FleetStackBadge popup;
            popup.owner = shown->owner; popup.best = shown->best; popup.texture = shown->texture;
            popup.contents = icon.contents; popup.stealth = icon.stealth;
            if (!popup.contents.empty()) looks = fleet_stack_looks(popup);
            break;
        }
    }
    fleet_popup->set_external_hover(hovered, std::move(looks));
}

// GHUD-S52..54: the organize shell's alternate index is the planet's Zoomed_Terrain_Index (the panel
// land_art_ALT<n> with a matching n shows; none when no panel matches, as in a mod with a larger index).
// A destroyed planet uses the fixed index of GHUD-S53. The panel dims to a quarter alpha while neither the
// ground contents nor the ground count of the planet are known to the local player.
void GalacticMode::State::update_fleet_diorama() {
    std::optional<std::uint32_t> index;
    float alpha = 1.0F;
    if (selected_planet && displayed_snapshot) {
        bool destroyed = false;
        if (displayed_snapshot->control()) {
            const auto& planets = displayed_snapshot->control()->planets;
            const auto row = std::find_if(planets.begin(), planets.end(),
                [this](const auto& value) { return value.planet == selected_planet; });
            destroyed = row != planets.end() && row->destroyed;
        }
        if (destroyed) {
            index = destroyed_planet_terrain_index;
        } else if (content) {
            const auto art = content->art.find(selected_planet);
            if (art != content->art.end()) index = art->second.terrain_index;
        }
        const auto observations = displayed_snapshot->observations();
        const auto seen = std::find_if(observations.begin(), observations.end(), [this](const auto& row) {
            return row.player == galaxy->local.player && row.planet == selected_planet; });
        alpha = seen != observations.end() && (seen->knowledge.ground_contents || seen->knowledge.ground_count)
            ? 1.0F : 0.25F;
    }
    fleet_diorama_alternate = index; fleet_diorama_alpha = alpha;
    fleet_organize->set_alternate(index, alpha);
}

void GalacticMode::State::rebuild_ground_badges() {
    const auto* style = command_catalog.find("g_planet_land_forces");
    if (!style || !displayed_snapshot || !galaxy) return;
    const auto stacks = style->list(data::ui::Field::blank_texture_name);
    const auto displays = sim::galactic::observed_ground_display(*displayed_snapshot, galaxy->local.player,
        [this](sim::galactic::TypeId type) -> std::optional<sim::galactic::GroundDisplayType> {
            const auto name = production_names.find(type);
            if (name == production_names.end()) return {};
            const auto art = production_art.find(name->second);
            if (art == production_art.end()) return {};
            const auto& look = art->second;
            return sim::galactic::GroundDisplayType{galactic_view::stack_health_rank(look.max_health),
                look.named_hero, look.generic_hero, !look.icon.empty(), look.hero_rank};
        });
    for (const auto& planet : galaxy->planets) {
        const bool surface_held = !dragged_ground_units.empty()
            || (ground_hud.pressed_object && map_input.box_end);
        const auto icon = galactic_view::ground_drop_stack(*displayed_snapshot, galaxy->local.player,
            planet.id, stacks.size(), dragged_fleet, dragged_stack, surface_held, dragged_members);
        if (!icon || stacks.empty()) continue;
        const auto key = std::pair{planet.id, std::uint8_t{3}};
        auto [entry, inserted] = fleet_badges.try_emplace(key);
        auto& badge = entry->second;
        if (inserted) {
            auto* button = memnew(EawrHudButton);
            button->set_sounds(*style, [this](const std::string& cue) { if (audio) audio->ui_sound(cue); });
            button->set_name(String::utf8(("ground_presence_" + std::to_string(planet.id)).c_str()));
            world_overlays->add_child(button); badge.button = button;
            badge.ship = memnew(TextureRect);
            badge.ship->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
            badge.ship->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
            badge.ship->set_stretch_mode(TextureRect::STRETCH_SCALE);
            button->add_child(badge.ship);
            button->set_action([this, id = planet.id] {
                if (!map_input.hud_event() || map_input.gates.selection_locked || !map_input.gates.world_allowed()) return;
                const auto planets = selection_planets();
                apply_selection(map_input.select(planets, id, seat_flight().state()));
            });
            attach_ground_stack_drag(button, planet.id);
            if (fleet_drag_receiver) hud->move_child(fleet_drag_receiver, hud->get_child_count() - 1);
        }
        badge.ground_planet = planet.id; badge.owner = icon->owner; badge.shown = true; badge.hidden = false;
        badge.texture = icon->texture; badge.contents = icon->contents;
        const auto hover = style->list(data::ui::Field::mouse_over_texture_name);
        badge.button->set_texture_hover(icon->contents_known && hover.size() > 1 ? command_texture(hover[1]) : Ref<Texture2D>{});
        // GGS-09: image permission is independent of tooltip contents. The
        // projection publishes candidates only, never a hidden payload identity.
        const auto display = std::find_if(displays.begin(), displays.end(),
            [&](const auto& row) { return row.planet == planet.id; });
        const auto best = display == displays.end() ? std::nullopt : display->image(ground_hero_preference);
        badge.best = best;
        if (badge.shown_stack.set(icon->texture)) badge.button->set_texture_normal(command_texture(stacks[icon->texture]));
        if (badge.shown_ship.set(best)) {
            Ref<Texture2D> image;
            if (best) if (const auto name = production_names.find(*best); name != production_names.end())
                if (const auto art = production_art.find(name->second); art != production_art.end()) image = command_texture(art->second.icon);
            badge.ship->set_texture(image);
        }
        if (badge.shown_owner.set(icon->owner)) {
            Color tint(1, 1, 1, 1);
            if (content) if (const auto owner = content->factions.find(icon->owner); owner != content->factions.end() && owner->second.present)
                tint = Color(owner->second.primary.r, owner->second.primary.g, owner->second.primary.b, 1);
            badge.button->set_self_modulate(tint);
        }
        visible_fleet_badges[planet.id].push_back(badge.button);
    }
}

void GalacticMode::State::rebuild_fleet_projection(const bool publication_changed) {
    fleet_publication = displayed_snapshot;
    if (!fixture_enabled(GalacticHudFixture::Feature::fleet_visibility))
        fleet_visibility = galactic_view::live_fleet_visibility(*displayed_snapshot);
    // #4316: the projection asks for a type's health once per member and per stack icon; the answer comes from the
    // authored art, which does not change, so it is looked up by name once per type.
    const viewer::galactic::StackHealth health = [this](const sim::galactic::TypeId type) -> std::optional<float> {
        if (const auto cached = fleet_health_cache.find(type); cached != fleet_health_cache.end()) return cached->second;
        std::optional<float> value;
        if (const auto name = production_names.find(type); name != production_names.end())
            if (const auto art = production_art.find(name->second); art != production_art.end()) value = art->second.max_health;
        fleet_health_cache.emplace(type, value);
        return value;
    };
    const auto capacity = fleet_group_buttons[0].size();
    const auto image_rank = [&health](const sim::galactic::TypeId type) -> std::optional<std::uint32_t> {
        return galactic_view::stack_health_rank(health(type));
    };
    if (publication_changed && !fleet_gui_indices.empty()) {
        std::set<sim::EntityId> live;
        for (const auto& fleet : displayed_snapshot->fleets()) for (const auto& member : fleet.members) live.insert(member.id);
        if (displayed_snapshot->fleet_control()) for (const auto& unit : displayed_snapshot->fleet_control()->detached_units)
            live.insert(unit.member.id);
        std::erase_if(fleet_gui_indices, [&](const auto& entry) { return !live.contains(entry.first); });
    }
    const std::vector<std::pair<sim::EntityId, std::int64_t>> indices(fleet_gui_indices.begin(), fleet_gui_indices.end());
    fleet_intelligence = sim::galactic::observed_fleets(*displayed_snapshot, galaxy->local.player,
        fleet_display_traits, capacity, fleet_visibility, image_rank, indices);
    ++fleet_projection_rebuilds;
    visible_fleet_badges.clear();
    // GHUD-S76: stack icons require a filtered fleet card, never presence
    // alone. Presence still qualifies the mixed orbit-slot layout.
    std::map<galactic_view::ObjectId, std::vector<const sim::galactic::ObservedFleet*>> by_planet;
    for (const auto& card : fleet_intelligence.cards) by_planet[card.planet].push_back(&card);
    // Any aware non-local owner flips the planet to the mixed layout, also
    // when the observer knows only its presence (no card).
    std::map<galactic_view::ObjectId, galactic_view::PlayerId> aware_other;
    for (const auto& row : fleet_intelligence.presence)
        if (row.owner != galaxy->local.player) aware_other.try_emplace(row.planet, row.owner);
    const auto* style = command_catalog.find("g_planet_fleet");
    const auto stacks = style ? style->list(data::ui::Field::blank_texture_name) : std::vector<std::string>{};
    std::vector<std::pair<galactic_view::ObjectId, viewer::galactic::FleetStackIcon>> placed;
    // fleet_stacks.hpp's mixed layout: the same any-other-owner test as fleet_stack_icons.
    mixed_fleet_planets.clear();
    for (const auto& row : aware_other) mixed_fleet_planets.insert(row.first);
    for (const auto& card : fleet_intelligence.cards) if (!card.local) mixed_fleet_planets.insert(card.planet);
    for (const auto& [planet, cards] : by_planet) {
        const auto other = aware_other.find(planet);
        const auto aware = other == aware_other.end() ? std::nullopt : std::optional{other->second};
        for (const auto& icon : viewer::galactic::fleet_stack_icons(cards, health, stacks.size(), aware))
            placed.emplace_back(planet, icon);
    }
    for (auto& [key, badge] : fleet_badges) badge.shown = false;
    rebuild_ground_badges();
    for (const auto& [planet, icon] : placed) {
        const auto key = std::pair{planet, icon.icon};
        auto [entry, inserted] = fleet_badges.try_emplace(key);
        auto& badge = entry->second;
        if (inserted) {
            auto* button = memnew(EawrHudButton);
            if (const auto* component = command_catalog.find("g_planet_fleet"))
                button->set_sounds(*component, [this](const std::string& cue) { if (audio) audio->ui_sound(cue); });
            button->set_name(String::utf8(("fleet_presence_" + std::to_string(planet) + "_" + std::to_string(icon.icon)).c_str()));
            world_overlays->add_child(button); badge.button = button;
            badge.ship = memnew(TextureRect);
            badge.ship->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
            badge.ship->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
            badge.ship->set_stretch_mode(TextureRect::STRETCH_SCALE);
            button->add_child(badge.ship);
            badge.stealth_overlay = memnew(TextureRect);
            badge.stealth_overlay->set_name("fleet_stealth_overlay");
            badge.stealth_overlay->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
            badge.stealth_overlay->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
            badge.stealth_overlay->set_stretch_mode(TextureRect::STRETCH_SCALE);
            badge.stealth_overlay->hide();
            button->add_child(badge.stealth_overlay);
            button->set_action([this, planet = planet] {
                if (!map_input.hud_event() || map_input.gates.selection_locked || !map_input.gates.world_allowed()) return;
                const auto planets = selection_planets();
                apply_selection(map_input.select(planets, planet, seat_flight().state()));
            });
            attach_fleet_drag(button, "g_planet_fleet", [this, key, button] {
                if (!button->is_visible_in_tree() || button->is_disabled()) return FleetDragSource{};
                const auto found = fleet_badges.find(key);
                if (found == fleet_badges.end() || found->second.owner != galaxy->local.player) return FleetDragSource{};
                // #3072: the stack icon moves every local fleet of the slots it shows.
                auto slots = stack_icon_slots(key.first, key.second);
                if (found->second.fleet) return FleetDragSource{*found->second.fleet, std::move(slots)};
                const auto marker = std::find_if(galaxy->fleets.begin(), galaxy->fleets.end(),
                    [&](const auto& value) { return value.planet == key.first && value.owner == found->second.owner; });
                return FleetDragSource{marker != galaxy->fleets.end() ? marker->fleet : 0, std::move(slots)};
            });
            // Control picking follows sibling order; the drag-only receiver
            // stays above badges created by later snapshot publications.
            if (fleet_drag_receiver) hud->move_child(fleet_drag_receiver, hud->get_child_count() - 1);
        }
        // #4316: a badge whose stack picture, best ship or owner colour is what it already shows takes no native call.
        badge.owner = icon.owner; badge.fleet = icon.fleet; badge.texture = icon.texture; badge.shown = true;
        badge.hidden = false;
        if (badge.contents != icon.contents) badge.contents = icon.contents;
        badge.best = icon.best;
        badge.stealth = icon.stealth;
        if (badge.shown_stealth.set(icon.stealth)) {
            // WGUI-92: the fleet overlay uses the padded asset; its art positions the badge in the corner.
            badge.stealth_overlay->set_texture(icon.stealth ? command_texture("i_icon_stealth.tga")
                                                          : Ref<Texture2D>{});
        }
        if (badge.shown_stack.set(icon.texture)) badge.button->set_texture_normal(stacks.empty() ? Ref<Texture2D>{}
            : command_texture(stacks[std::min(icon.texture, stacks.size() - 1)]));
        if (badge.shown_ship.set(icon.best)) {
            Ref<Texture2D> ship;
            if (icon.best) if (const auto name = production_names.find(*icon.best); name != production_names.end())
                if (const auto art = production_art.find(name->second); art != production_art.end())
                    ship = command_texture(art->second.icon);
            badge.ship->set_texture(ship);
        }
        if (badge.shown_owner.set(icon.owner)) {
            // Background: the owner faction's colour (debug build stack icon colouring).
            Color tint(1, 1, 1, 1);
            if (content) if (const auto owner = content->factions.find(icon.owner); owner != content->factions.end()
                && owner->second.present) tint = Color(owner->second.primary.r, owner->second.primary.g,
                    owner->second.primary.b, 1.0F);
            badge.button->set_self_modulate(tint);
        }
        visible_fleet_badges[planet].push_back(badge.button);
    }
    // A badge is hidden once when it leaves the projection, not on every publication; the revision tells the label
    // pass that the badge changed under it.
    for (auto& [key, badge] : fleet_badges) {
        if (badge.shown) continue;
        badge.contents.clear();
        if (badge.hidden) continue;
        badge.button->hide(); badge.hidden = true; ++badge.revision;
    }
    const bool selected_live = std::any_of(fleet_intelligence.cards.begin(), fleet_intelligence.cards.end(),
        [&](const auto& row) { return row.planet == selected_planet && sim::galactic::holds_fleet(row, selected_fleet); });
    if (!selected_live) { selected_fleet = 0; selected_fleet_member = 0; }
}

void GalacticMode::State::update_fleet_panel_cards() {
    fleet_view_planet = selected_planet; fleet_view_selection = selected_fleet; fleet_view_member = selected_fleet_member;
    shown_fleet_cards.fill(nullptr); shown_panel_cards.fill(nullptr); shown_enemy_count = 0;
    std::vector<const sim::galactic::ObservedFleet*> at_planet;
    for (const auto& card : fleet_intelligence.cards) if (card.planet == selected_planet) {
        at_planet.push_back(&card);
        if (card.local && card.orbit_slot && *card.orbit_slot < 3) {
            if (!shown_fleet_cards[*card.orbit_slot]) shown_fleet_cards[*card.orbit_slot] = &card;
        } else if (!card.local) {
            // WGUI-29 defines detailed awareness as size knowledge plus
            // nonstealth (allies bypass); contents is a separate predicate.
            // WGUI-42: q4 gathers all nonlocal admitted fleets, including
            // allies; "enemy" is the source panel name, not an alliance filter.
            shown_enemy_count += card.count;
            if (!shown_fleet_cards[3]) shown_fleet_cards[3] = &card;
        }
    }
    // WGUI-175: the three orbit panels show the local fleets by slot until another owner is aware here.
    fleet_panels = galactic_view::plan_fleet_panels(at_planet, mixed_fleet_planets.contains(selected_planet),
        fleet_group_buttons[0].size());
    for (std::size_t panel = 0; panel < 3; ++panel)
        if (fleet_panels.panel[panel]) shown_panel_cards[panel] = &*fleet_panels.panel[panel];
    const std::vector<std::pair<sim::EntityId, std::int64_t>> indices(fleet_gui_indices.begin(), fleet_gui_indices.end());
    for (std::size_t panel = 0; panel < 3; ++panel) if (fleet_panels.panel[panel]) {
        const auto picked = pathfinder_source(panel);
        std::vector<std::uint8_t> slots;
        for (std::uint8_t slot = 0; slot < 3; ++slot)
            if (galactic_view::fleet_panel_for_slot(fleet_panels.mode, slot) == panel) slots.push_back(slot);
        presentation::ui::separate_pathfinder(*fleet_panels.panel[panel], *displayed_snapshot, picked.member, indices, slots);
    }
}

void GalacticMode::State::rebuild_fleet_cell_layouts(const presentation::ui::ShellPlacement& placement) {
    fleet_cell_placement = placement;
    for (std::size_t slot = 0; slot < fleet_group_buttons.size(); ++slot) {
        std::vector<std::optional<presentation::ui::FleetCellCoordinate>> coordinates;
        for (const auto& name : fleet_group_buttons[slot]) coordinates.push_back(presentation::ui::fleet_cell_coordinate(name));
        std::vector<FleetCellLook> looks;
        std::vector<presentation::ui::FleetCellIcon> icons;
        if (const auto* card = shown_panel_cards[slot]) for (const auto& group : card->groups) {
            FleetCellLook look;
            look.planet = card->planet; look.owner = card->owner;
            look.type = group.type; look.gui_index = group.gui_index;
            if (group.type) if (const auto type = production_names.find(*group.type); type != production_names.end())
                if (const auto art = production_art.find(type->second); art != production_art.end()) {
                    const auto big = command_texture("big_" + art->second.icon);
                    look.icon = big.is_valid() ? big : command_texture(art->second.icon);
                    if (big.is_valid()) {
                        const auto size = big->get_size();
                        look.footprint.width = size.x; look.footprint.height = size.y;
                    }
                }
            // GHUD-S87: a split stack's remainder keeps its group identity
            // even when its representative changes. Scope the cached cell
            // to this orbit card, including its local placement index.
            if (look.type) for (std::size_t old = 0; old < fleet_cell_looks[slot].size(); ++old)
                if (fleet_cell_looks[slot][old].planet == look.planet
                    && fleet_cell_looks[slot][old].owner == look.owner
                    && fleet_cell_looks[slot][old].type == look.type
                    && fleet_cell_looks[slot][old].gui_index == look.gui_index
                    && old < fleet_cell_layouts[slot].groups.size() && fleet_cell_layouts[slot].groups[old]) {
                    look.footprint.previous = fleet_cell_layouts[slot].groups[old]->cell; break;
                }
            // GHUD-S87: local object placement, including split type stacks.
            if (group.gui_index && *group.gui_index >= 0
                && static_cast<std::uint64_t>(*group.gui_index) < coordinates.size()) {
                look.footprint.previous = static_cast<std::size_t>(*group.gui_index);
                look.footprint.nearest = true;
            }
            icons.push_back(look.footprint); looks.push_back(std::move(look));
        }
        fleet_cell_layouts[slot] = presentation::ui::fleet_cell_layout(coordinates, icons);
        fleet_cell_looks[slot] = std::move(looks);
    }
}

void GalacticMode::State::update_fleet_controls(const presentation::ui::ShellPlacement& placement, const bool changed) {
    fleet_drop_rects.fill({});
    for (const auto& [name, native] : fleet_buttons) {
        auto* button = static_cast<EawrHudButton*>(native);
        const auto* anchor = fleet_anchors.find(name);
        const auto pixels = presentation::ui::shell_to_screen(anchor->rect, placement);
        if (name.ends_with("_fleet")) {
            button->set_position({static_cast<float>(pixels.x), static_cast<float>(pixels.y)});
            button->set_size({static_cast<float>(pixels.width), static_cast<float>(pixels.height)});
            button->set_hit_rect({{}, button->get_size()});
        }
        const std::size_t slot = static_cast<std::size_t>(name[1] - '1');
        if (options.hud_provider) {
            if (const auto rect = options.hud_provider->ground_stack_rect(*this, name, slot, placement)) {
                button->set_position({static_cast<float>(rect->x), static_cast<float>(rect->y)});
                button->set_size({static_cast<float>(rect->width), static_cast<float>(rect->height)});
            }
        }
        if (name.ends_with("_fleet")) button->set_hit_rect({{}, button->get_size()});
        if (slot < 3 && !name.ends_with("_fleet") && !name.ends_with("_0000")) {
            const Rect2 rect(fleet_organize->get_global_position() + Vector2(static_cast<float>(pixels.x), static_cast<float>(pixels.y)),
                             Vector2(static_cast<float>(pixels.width), static_cast<float>(pixels.height)));
            auto& region = fleet_drop_rects[slot];
            region = region.size == Vector2{} ? rect : region.merge(rect);
            const auto& cell_names = fleet_group_buttons[slot];
            const auto cell_at = static_cast<std::size_t>(std::find(cell_names.begin(), cell_names.end(), name) - cell_names.begin());
            if (cell_at < cell_names.size()) {
                if (fleet_cell_rects[slot].size() != cell_names.size()) fleet_cell_rects[slot].assign(cell_names.size(), Rect2{});
                fleet_cell_rects[slot][cell_at] = rect;
            }
        }
        const sim::galactic::ObservedFleet* card = name.ends_with("_fleet") ? shown_fleet_cards[slot] : shown_panel_cards[slot];
        // The leading frame is drawn on every panel; it only acts on a local panel (the disabled state falls back to its
        // blank art, which is the empty rounded frame).
        const bool inert_frame = slot < 3 && name.ends_with("_0000") && !(card && card->local);
        button->set_disabled(!map_input.gates.hud_enabled || map_input.gates.tutorial_locked || inert_frame);
        if (!changed) continue; // No text, icon resolution or allocations per render frame.
        if (slot < 3 && name.ends_with("_0000")) {
            button->set_position({static_cast<float>(pixels.x), static_cast<float>(pixels.y)});
            button->set_size({static_cast<float>(pixels.width), static_cast<float>(pixels.height)});
            button->set_hit_rect({{}, button->get_size()});
            const auto picked = pathfinder_source(slot);
            button->set_meta("pathfinder_member", static_cast<std::int64_t>(picked.member));
            button->set_meta("pathfinder_fleet", static_cast<std::int64_t>(picked.fleet));
            Ref<Texture2D> icon;
            if (picked.cell_type) if (const auto type = production_names.find(*picked.cell_type); type != production_names.end())
                if (const auto art = production_art.find(type->second); art != production_art.end()) icon = command_texture(art->second.icon);
            button->set_icon_texture(icon);
            const Vector2 centre = button->get_size() * 0.5F;
            const auto size = icon.is_valid() ? icon->get_size() * static_cast<float>(placement.scale) : Vector2{};
            button->set_cell_portrait_box({centre - size * 0.5F, size});
            const auto& resources = fleet_cell_resources.at(name);
            HudButtonArt art; art.normal = resources.normal; art.pressed = resources.pressed;
            if (!resources.hover.empty()) art.hover = resources.hover.front();
            button->set_cell_state_art(std::move(art), centre, static_cast<float>(placement.scale));
            button->set_tooltip_text(inert_frame ? String{} : String::utf8(battle_text("TEXT_TOOLTIP_PATHFINDER").c_str()));
            button->set_order_selected(picked.member && selected_fleet_member == picked.member);
            button->set_visible(true);
            continue;
        }
        if (!name.ends_with("_fleet")) {
            const auto hide_cell = [&] {
                button->hide(); button->set_icon_texture({}); button->set_tooltip_text({});
                button->set_position({static_cast<float>(pixels.x), static_cast<float>(pixels.y)});
                button->set_size({static_cast<float>(pixels.width), static_cast<float>(pixels.height)});
                button->set_hit_rect({{}, button->get_size()});
            };
            if (!card || slot >= fleet_group_buttons.size()) { hide_cell(); continue; }
            const auto& names = fleet_group_buttons[slot];
            const auto cell = static_cast<std::size_t>(std::find(names.begin(), names.end(), name) - names.begin());
            const auto& layout = fleet_cell_layouts[slot];
            if (cell >= layout.owners.size() || !layout.owners[cell]) { hide_cell(); continue; }
            const auto index = *layout.owners[cell];
            if (!layout.groups[index] || layout.groups[index]->cell != cell) { hide_cell(); continue; }
            const auto& group = card->groups[index];
            const auto& look = fleet_cell_looks[slot][index];
            const auto& resources = fleet_cell_resources.at(name);
            const auto icon_size = look.icon.is_valid() ? look.icon->get_size() : Vector2{};
            const bool wide = !layout.compact && icon_size.x >= 52.0F;
            const bool high = !layout.compact && icon_size.y >= 52.0F;
            const float art_scale = layout.compact && icon_size.x >= 52.0F ? 0.5F : 1.0F;
            const Vector2 shift(wide ? 22.0F : 0.0F, high ? -22.0F : 0.0F);
            const auto origin = anchor->origin.value_or(assets::Vec2f{anchor->rect.x + anchor->rect.width / 2.0F,
                                                                     anchor->rect.y + anchor->rect.height / 2.0F});
            // Linked neighbours are one native input/drag surface; state art stays
            // natural-size and centred on the shifted bone, independently of it.
            Rect2 hit(Vector2(static_cast<float>(pixels.x), static_cast<float>(pixels.y)),
                      Vector2(static_cast<float>(pixels.width), static_cast<float>(pixels.height)));
            for (std::size_t neighbour = 0; neighbour < names.size(); ++neighbour) {
                if (layout.owners[neighbour] != index) continue;
                const auto* linked = fleet_anchors.find(names[neighbour]);
                const auto box = presentation::ui::shell_to_screen(linked->rect, placement);
                hit = hit.merge(Rect2(Vector2(static_cast<float>(box.x), static_cast<float>(box.y)),
                                     Vector2(static_cast<float>(box.width), static_cast<float>(box.height))));
            }
            // Preserve native mouse focus while a publication refreshes an
            // admitted cell: never hide it or temporarily shrink its hit area.
            button->set_position(hit.position);
            button->set_size(hit.size); button->set_hit_rect(Rect2({}, hit.size));
            const auto centre_pixels = presentation::ui::shell_to_screen({origin.x + shift.x, origin.y + shift.y, 0, 0}, placement);
            const Vector2 centre = Vector2(static_cast<float>(centre_pixels.x), static_cast<float>(centre_pixels.y)) - button->get_position();
            const auto drawn_size = icon_size * static_cast<float>(placement.scale) * art_scale;
            button->set_icon_texture(look.icon);
            button->set_cell_portrait_box(Rect2(centre - drawn_size * 0.5F, drawn_size));
            HudButtonArt art;
            art.normal = resources.normal; art.pressed = resources.pressed; art.disabled = resources.disabled;
            const std::size_t hover = (wide ? 2U : 0U) + (high ? 1U : 0U);
            if (hover < resources.hover.size()) art.hover = resources.hover[hover];
            button->set_cell_state_art(std::move(art), centre, static_cast<float>(placement.scale) * art_scale);
            const auto* text_style = command_catalog.find(name);
            const auto offset = text_style->vec2(data::ui::Field::text_offset).value_or(data::ui::Vec2{});
            // WGUI-73: compacting scales the art; text retains its authored
            // offset and resolution-based font size.
            const auto text_pixels = presentation::ui::shell_to_screen({origin.x + shift.x + offset.x - 25.0F,
                origin.y + shift.y + offset.y - 6.0F, 50.0F, 12.0F}, placement);
            auto text = resources.text;
            const auto font_pixels = presentation::ui::font_pixels({resources.face.point_size, false, 1.0},
                presentation::ui::font_screen_height(fleet_organize->space()));
            text.size = font_pixels.glyph_height;
            const auto metrics = presentation::ui::gdi_text_cell(hud_fonts->cache(), resources.face, text.size);
            text.cell_ascent = metrics.ascent; text.cell_descent = metrics.descent;
            const String value = group.count >= 2 ? String("x") + String::num_uint64(group.count) : String{};
            button->set_cell_text({text, value, Rect2(Vector2(static_cast<float>(text_pixels.x), static_cast<float>(text_pixels.y)) - button->get_position(),
                Vector2(static_cast<float>(text_pixels.width), static_cast<float>(text_pixels.height))),
                HORIZONTAL_ALIGNMENT_CENTER, static_cast<float>(placement.scale)});
            button->set_tooltip_text({});
            // GHUD-S83: the shared timed encyclopedia owns focused unit hover.
            button->set_order_selected(group.representative && selected_fleet_member == *group.representative);
            button->set_visible(true);
            continue;
        }
        auto* count = fleet_counts.at(name);
        const auto* text_anchor = fleet_anchors.find("text_fleet" + std::string(1, name[1]));
        if (text_anchor) {
            const auto text_pixels = presentation::ui::shell_to_screen(text_anchor->rect, placement);
            count->set_position(Vector2(static_cast<float>(text_pixels.x), static_cast<float>(text_pixels.y)) - button->get_position());
        }
        count->set_text({}); button->set_icon_texture({}); button->set_tooltip_text({});
        const bool visible = card != nullptr;
        if (visible) {
            count->set_text(String::num_uint64(slot < 3 ? card->count : shown_enemy_count));
            const bool one_owner = slot < 3 || std::all_of(fleet_intelligence.cards.begin(), fleet_intelligence.cards.end(),
                [&](const auto& row) { return row.local || row.planet != selected_planet || row.owner == card->owner; });
            // GHUD-C06: mixed-owner q4 icon arbitration is not sourced; retain
            // its aggregate count without choosing a first-owner faction icon.
            if (one_owner) {
                button->set_icon_texture(fleet_faction_texture(card->owner, false));
                button->set_texture_hover(fleet_faction_texture(card->owner, false, true));
            } else button->set_texture_hover({});
            button->set_order_selected(sim::galactic::holds_fleet(*card, selected_fleet));
        }
        button->set_visible(visible); button->set_disabled(!map_input.gates.hud_enabled || map_input.gates.tutorial_locked);
    }
}

void GalacticMode::State::update_fleet_captions(const presentation::ui::ShellPlacement& placement) {
    for (std::size_t panel = 0; panel < fleet_captions.size(); ++panel) {
        auto* label = fleet_captions[panel];
        if (!label) continue;
        const std::string name = "text_fleet" + std::to_string(panel + 1);
        const auto* anchor = fleet_anchors.find("q" + std::to_string(panel + 1) + "_frame");
        const auto* component = command_catalog.find(name);
        if (!anchor || !component) { label->hide(); continue; }
        auto look = *presentation::ui::hud_readout_style(command_catalog, name, *anchor);
        const auto face = hud_fonts->resolve({look.text.face, look.text.point_size}, "english");
        const int pixels = std::max(1, presentation::ui::font_pixel_height(face.point_size, camera.height));
        label->set_style(look, hud_fonts->command_font(face, pixels), [this, face](std::int32_t height) {
            return presentation::ui::gdi_text_cell(hud_fonts->cache(), face, height);
        }, pixels, static_cast<float>(placement.scale), 1.0F / static_cast<float>(placement.scale));
        // The master text keys count from zero: TEXT_FLEET_PANE_0 is "Fleet 1".
        const std::string key = "TEXT_FLEET_PANE_" + std::to_string(panel);
        label->set_text(String::utf8((battle_text ? battle_text(key) : key).c_str()));
        // Owner retail capture 2026-10-16: the caption sits on the panel's lower-left tab, its centre about 0.236 of the
        // frame width from the left edge and 0.07 of the frame height above its bottom edge (the mesh reaches past the
        // drawn panel edge), on all three panels.
        const auto box = presentation::ui::shell_to_screen(anchor->rect, placement);
        const float width = static_cast<float>(box.width) * 0.40F;
        const float height = static_cast<float>(pixels) * 1.6F;
        label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
        label->set_size(Vector2(width, height));
        label->set_position(Vector2(static_cast<float>(box.x + box.width * 0.236) - width * 0.5F,
            static_cast<float>(box.y + box.height * 0.93) - height * 0.5F));
        label->show();
    }
}

void GalacticMode::State::update_fleet_hud() {
    if (!fleet_organize || !displayed_snapshot || !galaxy) return;
    const bool publication_changed = fleet_publication != displayed_snapshot;
    // GGS-05: held targets must refresh even while paused, without rebuilding
    // the projection on every pointer motion. This also covers ground-pad drags.
    const bool ground_targets_held = dragged_fleet || !dragged_ground_units.empty()
        || (ground_hud.pressed_object && map_input.box_end);
    const bool held_changed = ground_targets_held != fleet_ground_drop_targets_held;
    fleet_ground_drop_targets_held = ground_targets_held;
    bool changed = publication_changed || fleet_layout_dirty || held_changed || fleet_view_planet != selected_planet
        || fleet_view_selection != selected_fleet || fleet_view_member != selected_fleet_member;
    if (publication_changed || fleet_layout_dirty || held_changed) {
        rebuild_fleet_projection(publication_changed);
    }
    if (changed) {
        update_fleet_panel_cards();
    }
    // GHUDL-09/11: retain the authored aggregate parking; the organization
    // shell is admitted only in the focused view, outside the advanced map.
    fleet_organize->set_visible(selected_planet && seat_flight().state() == galactic_view::CameraState::focused
        && !seat_flight().advanced_map());
    update_fleet_diorama();
    const auto placement = fleet_organize->placement();
    changed = changed || placement != fleet_cell_placement || fleet_layout_dirty;
    fleet_layout_dirty = false;
    if (changed) {
        rebuild_fleet_cell_layouts(placement);
    }
    update_fleet_controls(placement, changed);
    if (changed) update_fleet_captions(placement);
    service_fleet_stack_popup();
}

// GHUDL-16 / debug build planet fleet positioning: the left and right icons sit at the
// planet point shifted by g_planet_fleet Default_Offset X (left -X, right +X), Y up by
// Default_Offset Y; the centre icon is raised by Offset Y. Each ellipse is centred on its point.
void GalacticMode::State::update_fleet_badges(const galactic_view::ObjectId planet,
    const Vector2 anchor, const float scale, const float label_alpha) {
    // Focused labels fade out, but a held surface company still needs its orbit receivers.
    const bool surface_drag = ground_hud.pressed_object && map_input.box_end && planet == selected_planet;
    const float alpha = surface_drag ? 1.0F : label_alpha;
    const auto* style = command_catalog.find("g_planet_fleet");
    if (!style) return;
    const auto side = style->vec2(data::ui::Field::default_offset).value_or(data::ui::Vec2{});
    const auto middle = style->vec2(data::ui::Field::offset).value_or(data::ui::Vec2{});
    const float layer = middle.x;
    const auto icon_size = style->vec2(data::ui::Field::size).value_or(data::ui::Vec2{1.0F, 0.0F});
    const float icon_scale = icon_size.x > 0 ? icon_size.x : 1.0F;
    const auto place = [&](TextureButton* button, const std::uint8_t icon) {
        const auto texture = button->get_texture_normal();
        const auto size = texture.is_valid() ? texture->get_size() * scale : Vector2{};
        const float x = icon == 0 ? -side.x : icon == 2 ? side.x : 0.0F;
        const float y = icon == 1 ? middle.y : side.y;
        button->set_position(anchor + Vector2(x, -y) * scale - size * 0.5F);
        button->set_size(size); static_cast<EawrHudButton*>(button)->set_hit_rect({{}, size});
        return size;
    };
    // #3072: during a fleet drag the hovered planet also shows its empty local
    // stack icons as drop targets: the authored empty stack art (texture 0, as a
    // presence-only stack) in the local faction's colour. The receiver hit-tests them.
    if ((dragged_fleet || dragged_ground_planet || (ground_hud.pressed_object && map_input.box_end))
        && planet == fleet_drag_planet) {
        const auto stacks = style->list(data::ui::Field::blank_texture_name);
        const bool mixed = mixed_fleet_planets.contains(planet);
        for (std::size_t index = mixed ? 1 : 0; index < fleet_empty_slots.size() && !stacks.empty(); ++index) {
            const auto icon = static_cast<std::uint8_t>(index);
            const auto filled = fleet_badges.find({planet, icon});
            if (filled != fleet_badges.end() && filled->second.shown) continue;
            auto*& button = fleet_empty_slots[icon];
            if (!button) {
                button = memnew(EawrHudButton);
                button->set_name(String::utf8(("fleet_empty_slot_" + std::to_string(icon)).c_str()));
                button->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
                world_overlays->add_child(button);
                button->set_texture_normal(command_texture(stacks.front()));
                if (content) if (const auto owner = content->factions.find(galaxy->local.player);
                    owner != content->factions.end() && owner->second.present)
                    button->set_self_modulate(Color(owner->second.primary.r, owner->second.primary.g, owner->second.primary.b, 1.0F));
                if (fleet_drag_receiver) hud->move_child(fleet_drag_receiver, hud->get_child_count() - 1);
            }
            static_cast<void>(place(button, icon));
            button->set_modulate(Color(1, 1, 1, alpha));
            button->set_visible(alpha > 0 && button->get_texture_normal().is_valid());
            fleet_empty_shown[icon] = button->is_visible();
        }
    }
    for (auto found = fleet_badges.lower_bound({planet, std::uint8_t{0}});
         found != fleet_badges.end() && found->first.first == planet; ++found) {
        auto& badge = found->second;
        auto* button = badge.button;
        if (!badge.shown) continue;
        if (badge.ground_planet) {
            const auto* ground_style = command_catalog.find("g_planet_land_forces");
            if (!ground_style) { button->hide(); continue; }
            const auto offset = ground_style->vec2(data::ui::Field::offset).value_or(data::ui::Vec2{});
            const auto texture = button->get_texture_normal();
            const auto size = texture.is_valid() ? texture->get_size() * scale : Vector2{};
            button->set_position(anchor - Vector2(0, offset.y) * scale - size * 0.5F);
            button->set_size(size); static_cast<EawrHudButton*>(button)->set_hit_rect({{}, size});
            if (badge.ship->get_texture().is_valid()) {
                const auto image = badge.ship->get_texture()->get_size()
                    * ground_style->vec2(data::ui::Field::size).value_or(data::ui::Vec2{1, 0}).x * scale;
                badge.ship->set_position((size - image) * 0.5F - Vector2(0, offset.x * static_cast<float>(badge.texture) * scale));
                badge.ship->set_size(image);
            }
            button->set_modulate(Color(1, 1, 1, alpha));
            button->set_visible(alpha > 0 && texture.is_valid());
            button->set_disabled(!map_input.gates.hud_enabled || map_input.gates.tutorial_locked);
            continue;
        }
        const auto size = place(button, found->first.second);
        if (badge.ship->get_texture().is_valid()) {
            const auto image = badge.ship->get_texture()->get_size() * icon_scale * scale;
            // GHUDL-16: each stack layer lifts the ship by Offset X (sign partial).
            badge.ship->set_position((size - image) * 0.5F - Vector2(0, layer * static_cast<float>(badge.texture) * scale));
            badge.ship->set_size(image);
        }
        if (badge.stealth_overlay->get_texture().is_valid()) {
            // The overlay sits over the ship image on the stack, at the same scale and lift.
            const auto image = badge.stealth_overlay->get_texture()->get_size() * icon_scale * scale;
            badge.stealth_overlay->set_position((size - image) * 0.5F - Vector2(0, layer * static_cast<float>(badge.texture) * scale));
            badge.stealth_overlay->set_size(image);
            badge.stealth_overlay->show();
        } else badge.stealth_overlay->hide();
        button->set_modulate(Color(1, 1, 1, alpha)); button->set_visible(alpha > 0 && button->get_texture_normal().is_valid());
        button->set_disabled(!map_input.gates.hud_enabled || map_input.gates.tutorial_locked);
    }
}

void GalacticMode::State::write_fleet_report(std::ostream& output) const {
    using galactic_detail::json;
    output << "{\"selected_fleet\": " << selected_fleet << ", \"selected_member\": " << selected_fleet_member
        << ", \"diorama_panel\": " << (fleet_diorama_alternate ? std::to_string(*fleet_diorama_alternate) : "null")
        << ", \"diorama_alpha\": " << fleet_diorama_alpha
        << ", \"projection_rebuilds\": " << fleet_projection_rebuilds
        << ", \"visibility_source\": " << json(fixture_enabled(GalacticHudFixture::Feature::fleet_visibility)
            ? "explicit-synthetic-current-sight" : "live-WGUI-06-current-sight")
        << ", \"enemy_contents_layout\": \"GHUD-C06 expanded-list seam\", \"cards\": [";
    std::size_t written{};
    for (const auto& card : fleet_intelligence.cards) {
        output << (written++ ? ", " : "") << "{\"fleet\": " << (card.fleet ? std::to_string(*card.fleet) : "null")
            << ", \"planet\": " << card.planet << ", \"owner\": " << card.owner
            << ", \"slot\": " << (card.orbit_slot ? std::to_string(*card.orbit_slot) : "null")
            << ", \"local\": " << (card.local ? "true" : "false") << ", \"count\": " << card.count << ", \"groups\": [";
        for (std::size_t index = 0; index < card.groups.size(); ++index) {
            const auto& group = card.groups[index];
            output << (index ? ", " : "") << "{\"type\": " << (group.type ? std::to_string(*group.type) : "null")
                << ", \"representative\": " << (group.representative ? std::to_string(*group.representative) : "null")
                << ", \"fleet\": " << (group.fleet ? std::to_string(*group.fleet) : "null")
                << ", \"gui_index\": " << (group.gui_index ? std::to_string(*group.gui_index) : "null")
                << ", \"count\": " << group.count << ", \"superweapon\": " << (group.superweapon ? "true" : "false")
                << ", \"hero\": " << (group.hero ? "true" : "false") << "}";
        }
        output << "]}";
    }
    output << "], \"controls\": ["; written = 0;
    for (const auto& [name, button] : fleet_buttons) {
        const auto rect = button->get_global_rect();
        const auto* cell = static_cast<const EawrHudButton*>(button);
        const auto count = fleet_counts.find(name);
        const auto value = count != fleet_counts.end() ? count->second->get_text() : button->is_visible() ? cell->cell_text() : String{};
        output << (written++ ? ", " : "") << "{\"name\": " << json(name)
            << ", \"visible\": " << (button->is_visible_in_tree() ? "true" : "false")
            << ", \"count\": " << json(galactic_detail::utf8(value))
            << ", \"shared_cell\": " << (cell->shared_cell() ? "true" : "false")
            << ", \"rect\": [" << rect.position.x << ", " << rect.position.y << ", " << rect.size.x << ", " << rect.size.y << "]";
        if (!name.ends_with("_fleet") && name[1] <= '3') {
            const auto slot = static_cast<std::size_t>(name[1] - '1');
            const auto& names = fleet_group_buttons[slot];
            const auto index = static_cast<std::size_t>(std::find(names.begin(), names.end(), name) - names.begin());
            const auto& layout = fleet_cell_layouts[slot];
            const auto group = index < layout.owners.size() ? layout.owners[index] : std::nullopt;
            output << ", \"group\": " << (group ? std::to_string(*group) : "null")
                << ", \"compact\": " << (layout.compact ? "true" : "false");
            const auto write_box = [&](const char* field, const Rect2& box) {
                const auto at = button->get_global_position() + box.position;
                output << ", \"" << field << "\": [" << at.x << ", " << at.y << ", " << box.size.x << ", " << box.size.y << "]";
            };
            write_box("portrait_rect", cell->cell_portrait_box()); write_box("count_rect", cell->cell_text_box());
        }
        output << "}";
    }
    output << "], \"shell_geometry\": ["; written = 0;
    if (fleet_organize) {
        const auto placement = fleet_organize->placement();
        for (const auto* anchor : fleet_anchors.for_variant(presentation::ui::alt_variant(command_faction))) {
            if (!anchor->visible || command_catalog.find(anchor->name)) continue;
            const auto rect = presentation::ui::shell_to_screen(anchor->rect,placement);
            output << (written++ ? ", " : "") << "{\"name\": " << json(anchor->name)
                << ", \"rect\": [" << rect.x << ", " << rect.y << ", " << rect.width << ", " << rect.height << "]}";
        }
    }
    output << "], \"presence\": ["; written = 0;
    for (const auto& [key, badge] : fleet_badges) {
        if (!badge.shown) continue;
        const auto rect = badge.button->get_global_rect();
        output << (written++ ? ", " : "") << "{\"planet\": " << key.first << ", \"owner\": " << badge.owner
            << ", \"ground\": " << (badge.ground_planet ? "true" : "false")
            << ", \"icon\": " << static_cast<int>(key.second) << ", \"stack_texture\": " << badge.texture
            << ", \"fleet\": " << (badge.fleet ? std::to_string(*badge.fleet) : "null")
            << ", \"ship\": " << (badge.ship->get_texture().is_valid() ? "true" : "false")
            << ", \"display_type\": " << (badge.best ? std::to_string(*badge.best) : "null")
            << ", \"name\": " << json(galactic_detail::utf8(badge.button->get_name()))
            << ", \"visible\": " << (badge.button->is_visible_in_tree() ? "true" : "false")
            << ", \"rect\": [" << rect.position.x << ", " << rect.position.y << ", " << rect.size.x << ", " << rect.size.y << "]"
            << ", \"contents\": [";
        for (std::size_t index = 0; index < badge.contents.size(); ++index) {
            const auto& entry = badge.contents[index];
            output << (index ? ", " : "") << "{\"type\": " << (entry.type ? std::to_string(*entry.type) : "null")
                << ", \"count\": " << entry.count << "}";
        }
        output << "]}";
    }
    output << "], \"captions\": [";
    for (std::size_t panel = 0; panel < fleet_captions.size(); ++panel) {
        const auto* label = fleet_captions[panel];
        output << (panel ? ", " : "") << "{\"panel\": " << panel + 1 << ", \"exists\": " << (label ? "true" : "false");
        if (label) {
            const auto rect = label->get_global_rect();
            output << ", \"visible\": " << (label->is_visible_in_tree() ? "true" : "false")
                << ", \"text\": " << json(std::string(label->get_text().utf8().get_data()))
                << ", \"rect\": [" << rect.position.x << ", " << rect.position.y << ", " << rect.size.x << ", " << rect.size.y << "]";
        }
        output << "}";
    }
    output << "], \"stack_popup\": {\"key\": " << fleet_popup_key
        << ", \"admitted\": " << (fleet_popup_admitted ? "true" : "false")
        << ", \"pointer\": " << json(fleet_popup_pointer)
        << ", \"status_seam\": \"raid/stealth fleet line awaits published fleet flags\""
        << ", \"popup\": " << (fleet_popup ? fleet_popup->report_json() : std::string("null"))
        << "}, \"drag\": {\"active_fleet\": " << dragged_fleet
        << ", \"begins\": " << fleet_drag_begins << ", \"ends\": " << fleet_drag_ends
        << ", \"orders\": " << fleet_drag_orders << ", \"cell_reorders\": " << fleet_cell_reorders
        << ", \"last_kind\": " << static_cast<int>(last_fleet_drag_kind)
        << ", \"last_source\": " << last_fleet_drag_source << ", \"last_target\": " << last_fleet_drag_target
        << ", \"last_destination\": " << last_fleet_drag_destination
        << ", \"member_transfer\": \"unavailable: atomic provider legacy #2586\""
        << ", \"pathfinder\": \"published selector and opcode 23\""
        << ", \"binding\": \"whole-fleet from aggregate, visible group member or badge; unit-group gesture and WGUI-U04 activation seam\""
        << ", \"aggregate_placement\": " << json(fixture_enabled(GalacticHudFixture::Feature::ground_placement) ? "explicit-synthetic-visible-controls" : "authored-GHUDL-09-parking")
        << ", \"preview\": \"dragged card or stack art (#3072)\", \"stack\": [";
    for (std::size_t index = 0; index < dragged_stack.size(); ++index) output << (index ? ", " : "") << dragged_stack[index];
    output << "], \"route_preview\": {\"segments\": " << fleet_preview_quads
        << ", \"searches\": " << fleet_preview_searches << ", \"path\": [";
    for (std::size_t index = 0; index < fleet_preview_path.size(); ++index)
        output << (index ? ", " : "") << fleet_preview_path[index];
    output << "]}, \"released_path\": [";
    if (displayed_snapshot) {
        const auto fleets = displayed_snapshot->fleets();
        const auto moved = std::find_if(fleets.begin(), fleets.end(), [this](const auto& fleet) { return fleet.id == last_fleet_drag_source; });
        if (moved != fleets.end() && moved->motion.transit) {
            const auto& path = moved->motion.transit->path;
            for (std::size_t index = 0; index < path.size(); ++index) output << (index ? ", " : "") << path[index];
        }
    }
    output << "], \"hover_planet\": " << fleet_drag_planet << ", \"empty_slots\": [";
    written = 0;
    for (std::size_t icon = 0; icon < fleet_empty_slots.size(); ++icon) {
        if (!fleet_empty_shown[icon]) continue;
        const auto rect = fleet_empty_slots[icon]->get_global_rect();
        output << (written++ ? ", " : "") << "{\"icon\": " << icon << ", \"rect\": [" << rect.position.x << ", "
            << rect.position.y << ", " << rect.size.x << ", " << rect.size.y << "]}";
    }
    output << "]}, \"drop_slots\": [";
    for (std::size_t slot = 0; slot < fleet_drop_rects.size(); ++slot) {
        const auto& rect = fleet_drop_rects[slot];
        output << (slot ? ", " : "") << "{\"slot\": " << slot << ", \"rect\": [" << rect.position.x
            << ", " << rect.position.y << ", " << rect.size.x << ", " << rect.size.y << "]}";
    }
    output << "]}";
}

} // namespace eawr::presentation::godot_backend
