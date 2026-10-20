#include "eawr/presentation/ui/hud_shell.hpp"
#include "eawr/core/text.hpp"

#include "eawr/data/ui/text_database.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/data/tag_trace.hpp"
#include "eawr/presentation/ui/theme.hpp"
#include "eawr/presentation/animation/animation.hpp"
#include "eawr/presentation/ui/ability_buttons.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <map>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <tuple>
#include <sstream>

namespace eawr::presentation::ui {
namespace {

bool equal_name(const std::string_view a, const std::string_view b) {
    return core::iequals(a, b);
}
bool contains(const std::string_view text, const std::string_view part) {
    for (std::size_t i = 0; i + part.size() <= text.size(); ++i)
        if (equal_name(text.substr(i, part.size()), part)) return true;
    return false;
}
bool ends_with(const std::string_view text, const std::string_view suffix) {
    return text.size() >= suffix.size() && equal_name(text.substr(text.size() - suffix.size()), suffix);
}
core::Diagnostic warning(const std::string_view code, std::string message, std::string path = {}) {
    core::Diagnostic out;
    out.code = std::string(code);
    out.severity = core::Severity::warning;
    out.message = std::move(message);
    out.logical_path = std::move(path);
    return out;
}
std::string first_token(const data::ui::CommandBarComponent& component, const data::ui::Field field) {
    const auto list = component.list(field);
    return list.empty() ? std::string() : list.front();
}
HudShellButton shell_button(const data::ui::ShellAnchor& anchor, const data::ui::CommandBarComponent& component) {
    HudShellButton button;
    button.name = anchor.name;
    button.rect = anchor.rect;
    button.origin = anchor.origin.value_or(
        assets::Vec2f{anchor.rect.x + anchor.rect.width / 2.0F, anchor.rect.y + anchor.rect.height / 2.0F});
    button.scale = component.number(data::ui::Field::scale).value_or(1.0F);
    button.normal = first_token(component, data::ui::Field::icon_texture_name);
    button.mouse_over = first_token(component, data::ui::Field::mouse_over_texture_name);
    button.pressed = first_token(component, data::ui::Field::selected_texture_name);
    button.disabled = first_token(component, data::ui::Field::disabled_texture_name);
    button.blank = first_token(component, data::ui::Field::blank_texture_name);
    button.flash = first_token(component, data::ui::Field::flash_texture_name);
    button.click_shift = component.flag(data::ui::Field::click_shift);
    button.selected_alpha = component.flag(data::ui::Field::selected_alpha);
    button.tooltip = first_token(component, data::ui::Field::tooltip_text);
    const auto alternates = component.list(data::ui::Field::icon_alternate_texture_name);
    button.alternates.assign(alternates.begin(), alternates.end());
    return button;
}

HudBar shell_bar(const data::ui::ShellAnchor& anchor, const data::ui::CommandBarComponent& component,
                 std::vector<core::Diagnostic>& diagnostics) {
    HudBar bar;
    bar.name = anchor.name;
    bar.origin = anchor.origin.value_or(
        assets::Vec2f{anchor.rect.x + anchor.rect.width / 2.0F, anchor.rect.y + anchor.rect.height / 2.0F});
    bar.offset = component.vec2(data::ui::Field::offset).value_or(data::ui::Vec2{});
    bar.scale = component.number(data::ui::Field::scale).value_or(1.0F);
    const auto back = component.list(data::ui::Field::bar_texture_name);
    bar.back.assign(back.begin(), back.end());
    const auto overlay = component.list(data::ui::Field::bar_overlay_name);
    bar.overlay.assign(overlay.begin(), overlay.end());
    const auto max_level = component.integer(data::ui::Field::max_bar_level);
    if (!max_level) {
        diagnostics.push_back(warning(diagnostic_codes::hud_shell_part,
            component.name + " Max_Bar_Level is absent or malformed; the FoC value is used"));
    }
    bar.max_level = max_level.value_or(10); // xml-fallback: Max_Bar_Level; load: src/presentation/ui/hud_shell.cpp#shell_bar
    bar.smooth = component.flag(data::ui::Field::smooth_bar);
    return bar;
}

std::string numbered(const std::string_view stem, const std::size_t index) {
    std::string name(stem);
    if (index < 10) name += '0';
    return name + std::to_string(index);
}

// #425: the unit card slots and column borders, in component order until the first gap.
constexpr std::string_view card_stem = "s_select_";
constexpr std::string_view health_stem = "s_health_";
constexpr std::string_view shield_stem = "s_shield_";
constexpr std::string_view border_stem = "special_border_";
constexpr std::string_view ability_stem = "special_button_";
constexpr std::size_t card_slot_limit = 48; // COMPONENT_ID_TACTICAL_SELECT_00 to _47

// The shell names three bound parts P2-20a draws; everything else bound is a
// later ticket's (cards, bars, orders, abilities).
constexpr std::string_view radar_name = "radar";
constexpr std::string_view options_name = "b_option_t";
constexpr std::string_view planet_name_component = "Text_Planet_tactical";

// #530: the build queue slots, the credits text and the reinforcements button.
constexpr std::string_view queue_stem = "tqueue";
constexpr std::size_t queue_slot_limit = 10; // tqueue00 to tqueue09
constexpr std::string_view credits_component = "Text_Credits_tactical";
constexpr std::string_view reinforcement_component = "b_reinforcement";
// #530: the reinforcement pane shell and its parts.
constexpr std::string_view pane_shell_component = "i_main_reinforce";
constexpr std::string_view pane_shell_fallback_model = "i_main_reinforce.alo";
constexpr std::string_view pane_slot_stem = "r_";
constexpr std::size_t pane_rows = 5;
constexpr std::size_t pane_columns = 4;
constexpr std::string_view pane_close_component = "r_close";
constexpr std::string_view pane_population_component = "r_pop_text";
constexpr std::string_view pane_population_icon = "r_pop_icon";

} // namespace

std::uint32_t alt_variant(const HudFaction faction) noexcept {
    switch (faction) {
    case HudFaction::empire: return data::ui::alt_variant::empire;
    case HudFaction::rebel: return data::ui::alt_variant::rebel;
    case HudFaction::underworld: return data::ui::alt_variant::underworld;
    }
    return data::ui::alt_variant::rebel;
}

std::string tactical_tech_text(const std::string_view localized_label, const TacticalTechView& view) {
    // HCB-61: multiplayer space and Consortium campaign hide this label.
    if ((view.multiplayer && view.space) || (!view.multiplayer && view.faction == HudFaction::underworld)) return {};
    return std::string(localized_label) + " " + std::to_string(view.displayed_level);
}

std::optional<HudIconText> hud_readout_style(const data::ui::CommandBarCatalog& catalog,
    const std::string_view name, const data::ui::ShellAnchor& anchor) {
    const auto* component = catalog.find(name);
    if (!component) return std::nullopt;
    HudIconText text;
    text.text.name = std::string(name);
    text.text.rect = anchor.rect;
    text.origin = anchor.origin.value_or(assets::Vec2f{
        anchor.rect.x + anchor.rect.width / 2.0F, anchor.rect.y + anchor.rect.height / 2.0F});
    text.text.face = std::string(component->text(data::ui::Field::font_name));
    if (text.text.face.empty()) text.text.face = std::string(last_resort_face);
    text.text.point_size = component->integer(data::ui::Field::font_point_size).value_or(8);
    if (const auto colour = component->color(data::ui::Field::text_color)) text.text.colour = *colour;
    text.text.outline = component->flag(data::ui::Field::text_outline);
    text.text.emboss = component->flag(data::ui::Field::text_emboss);
    text.icon = first_token(*component, data::ui::Field::icon_texture_name);
    text.text_offset = component->vec2(data::ui::Field::text_offset).value_or(data::ui::Vec2{});
    text.right_justified = component->flag(data::ui::Field::right_justified);
    text.left_justified = component->flag(data::ui::Field::left_justified);
    text.blink_duration = component->number(data::ui::Field::blink_duration).value_or(0.0F);
    text.blink_rate = component->number(data::ui::Field::blink_rate).value_or(0.0F);
    text.icon_offset = component->vec2(data::ui::Field::icon_offset).value_or(data::ui::Vec2{});
    return text;
}

std::optional<HudTimerStyle> hud_timer_style(const data::ui::CommandBarCatalog& catalog,
                                           const std::string_view name) {
    const auto* component = catalog.find(name);
    if (!component || !component->flag(data::ui::Field::no_shell)) return std::nullopt;
    data::ui::ShellAnchor anchor;
    anchor.name = std::string(name);
    HudTimerStyle style;
    style.button = shell_button(anchor, *component);
    style.build = first_token(*component, data::ui::Field::build_texture_name);
    if (const auto colour = component->color(data::ui::Field::color)) style.colour = *colour;
    style.blink_duration = component->number(data::ui::Field::blink_duration).value_or(0.0F);
    style.blink_rate = component->number(data::ui::Field::blink_rate).value_or(0.0F);
    style.blink_fade = component->flag(data::ui::Field::blink_fade);
    return style;
}

std::optional<double> hud_timer_progress(const std::uint64_t frame,
    const std::optional<std::uint64_t> due_frame, const std::uint32_t duration_frames) noexcept {
    if (!due_frame || frame >= *due_frame || duration_frames == 0) return std::nullopt;
    const auto left = std::min<std::uint64_t>(*due_frame - frame, duration_frames);
    return static_cast<double>(duration_frames - left) / static_cast<double>(duration_frames);
}

std::string_view to_string(const HudFaction faction) noexcept {
    switch (faction) {
    case HudFaction::empire: return "empire";
    case HudFaction::rebel: return "rebel";
    case HudFaction::underworld: return "underworld";
    }
    return "rebel";
}

std::string_view special_weapon_icon(const HudShellButton& button, const HudFaction faction) noexcept {
    // WSW-28: component order differs from the HudFaction enum order.
    const auto index = faction == HudFaction::rebel ? 0U : faction == HudFaction::empire ? 1U : 2U;
    return index < button.alternates.size() ? std::string_view(button.alternates[index]) : std::string_view{};
}

std::optional<HudFaction> hud_faction_from(const std::string_view text) noexcept {
    if (equal_name(text, "empire")) return HudFaction::empire;
    if (equal_name(text, "rebel")) return HudFaction::rebel;
    if (equal_name(text, "underworld")) return HudFaction::underworld;
    return std::nullopt;
}

std::size_t allegiance_emblem_index(const std::string_view faction, const SideNames& sides) noexcept {
    const auto is = [&](const std::string_view name) { return !name.empty() && equal_name(faction, name); };
    if (is(sides.good)) return 0;
    if (is(sides.evil)) return 1;
    if (is(sides.corrupt)) return 2;
    if (is(pirates_faction_name)) return 3;
    return 4;
}

std::string tactical_shell_model(const data::ui::CommandBarCatalog& catalog) {
    const auto* shell = catalog.find(tactical_shell_component);
    if (shell != nullptr && shell->type == data::ui::ComponentType::shell) {
        const auto model = shell->text(data::ui::Field::model_name);
        if (!model.empty()) return std::string(model);
    }
    return std::string(tactical_shell_fallback_model);
}

core::Result<PauseShell> pause_shell(const vfs::Vfs& filesystem, const data::ui::CommandBarCatalog& catalog) {
    const auto* component = catalog.find("pause_shell");
    const std::string name = component ? std::string(component->text(data::ui::Field::model_name)) : std::string();
    if (name.empty()) return core::Result<PauseShell>::failure(warning(diagnostic_codes::hud_shell_part, "pause_shell names no model"));
    PauseShell out;
    out.model = "data/art/models/" + name;
    auto model = assets::load_model(filesystem, out.model);
    if (!model) return core::Result<PauseShell>::failure(model.error());
    out.animation = out.model.substr(0, out.model.find_last_of('.')) + "_idle_00.ala";
    auto clip = assets::load_animation(filesystem, out.animation);
    if (!clip) return core::Result<PauseShell>::failure(clip.error());
    auto player = animation::Player::create(model.value(), &clip.value());
    if (!player) return core::Result<PauseShell>::failure(player.error());
    auto pose = player.value().sample_position(player.value().playable_frames(), 1);
    if (!pose) return core::Result<PauseShell>::failure(pose.error());
    std::vector<data::ui::ShellTransform> transforms;
    transforms.reserve(pose.value().bones.size());
    for (const auto& bone : pose.value().bones) transforms.push_back(bone.model_asset);
    for (std::size_t index = 0; index < model.value().bones.size(); ++index) {
        if (equal_name(model.value().bones[index].name, "text_attack"))
            out.text_origin = assets::Vec2f{transforms[index][12], transforms[index][13]};
    }
    out.anchors = data::ui::shell_anchors(model.value(), transforms).shell;
    for (const auto& anchor : out.anchors.anchors()) {
        const bool additive = equal_name(anchor.shader, "MeshAdditive.fx");
        if (!anchor.visible || (!additive && !equal_name(anchor.shader, "MeshAlpha.fx"))
            || anchor.base_texture.empty()) continue;
        out.meshes.push_back({anchor.name, additive ? ShellBlend::additive : ShellBlend::alpha,
            anchor.base_texture, anchor.z_min, anchor.triangles, std::nullopt});
    }
    std::stable_sort(out.meshes.begin(), out.meshes.end(),
        [](const auto& left, const auto& right) { return left.z < right.z; });
    return core::Result<PauseShell>::success(std::move(out));
}

std::string command_bar_mega_texture(const data::ui::CommandBarCatalog& catalog) {
    std::string name = "MT_CommandBar";
    for (const auto& component : catalog.components()) {
        const auto value = component.text(data::ui::Field::mega_texture_name);
        if (!value.empty()) {
            name = std::string(value);
            break;
        }
    }
    if (!ends_with(name, ".mtd")) name += ".mtd";
    return "Data/Art/Textures/" + name;
}

core::Result<float> movie_letterbox_height(const vfs::Vfs& filesystem) {
    auto document = data::load_document(filesystem, "data/xml/gameconstants.xml");
    if (!document) return core::Result<float>::failure(document.error());
    const data::XmlNode* found = nullptr;
    for (const auto& node : document.value().root.children)
        if (equal_name(node.name, "Camera_FX_Manager_Letterbox_Height")) found = &node;
    if (found) {
        data::tag_trace::used(found);
        std::string_view text = found->raw_text;
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
        if (!text.empty() && (text.back() == 'f' || text.back() == 'F')) text.remove_suffix(1);
        float value{};
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error == std::errc{} && end == text.data() + text.size() && std::isfinite(value)
            && value >= 0.0F && value < 0.5F) return core::Result<float>::success(value);
    }
    return core::Result<float>::failure(warning(diagnostic_codes::hud_shell_part,
        "movie letterbox height must be a finite fraction in [0, 0.5)", "data/xml/gameconstants.xml"));
}

std::optional<double> beacon_place_delay(const vfs::Vfs& filesystem) {
    auto document = data::load_document(filesystem, "data/xml/gameconstants.xml");
    if (!document) return std::nullopt;
    const data::XmlNode* found{};
    for (const auto& node : document.value().root.children)
        if (equal_name(node.name, "BeaconPlaceDelay")) found = &node;
    if (!found) return std::nullopt;
    data::tag_trace::used(found);
    const auto text = core::trim_view(found->raw_text, core::xml_whitespace);
    if (text.empty()) return std::nullopt;
    double value{};
    const auto [last, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || last != text.data() + text.size() || !std::isfinite(value) || value < 0)
        return std::nullopt;
    return value / 1000.0;
}

HudShell hud_shell(const data::ui::ShellAnchors& shell, const data::ui::CommandBarCatalog& catalog,
                   const HudFaction faction, const bool keep_alternates) {
    HudShell out;
    out.model = shell.model_path();
    out.faction = faction;
    const auto missing = [&](const std::string_view part, const std::string_view why) {
        out.diagnostics.push_back(warning(diagnostic_codes::hud_shell_part,
            std::string(part) + " " + std::string(why) + "; the HUD shell draws without it", out.model));
    };
    std::vector<const data::ui::ShellAnchor*> candidates;
    if (keep_alternates) {
        for (const auto& anchor : shell.anchors()) candidates.push_back(&anchor);
    } else {
        candidates = shell.for_variant(alt_variant(faction));
    }
    for (const auto* anchor : candidates) {
        const bool alpha = equal_name(anchor->shader, "MeshAlpha.fx");
        const bool additive = equal_name(anchor->shader, "MeshAdditive.fx");
        if ((!alpha && !additive) || anchor->base_texture.empty() || anchor->triangles.empty()) continue;
        // Decorative art the shell shows by itself; a bound component's mesh is
        // drawn only for the parts this ticket owns (the radar's scan lines).
        // An index-keyed alternate panel stays decorative even when the catalogue lists an empty
        // component of its name (the stock catalogue does for land_art_ALT0).
        const bool bound = catalog.find(anchor->name) != nullptr && !(keep_alternates && anchor->alt.variant);
        const bool radar = equal_name(anchor->name, radar_name);
        if (bound ? !radar : !anchor->visible) continue;
        out.meshes.push_back({anchor->name, additive ? ShellBlend::additive : ShellBlend::alpha,
                              anchor->base_texture, anchor->z_min, anchor->triangles,
                              keep_alternates ? anchor->alt.variant : std::nullopt});
    }
    std::stable_sort(out.meshes.begin(), out.meshes.end(),
                     [](const HudShellMesh& a, const HudShellMesh& b) { return a.z < b.z; });
    if (std::none_of(out.meshes.begin(), out.meshes.end(), [](const HudShellMesh& mesh) {
            return mesh.blend == ShellBlend::alpha && contains(mesh.name, "faceplate");
        })) {
        missing("the faction faceplate", "is not in the shell");
    }

    if (const auto* radar = shell.find(radar_name)) out.minimap = radar->rect;
    else missing(radar_name, "is not in the shell");

    const auto* option_anchor = shell.find(options_name);
    const auto* option_component = catalog.find(options_name);
    if (option_anchor != nullptr && option_component != nullptr) {
        out.options = shell_button(*option_anchor, *option_component);
        out.options->name = std::string(options_name);
    } else {
        missing(options_name, option_anchor == nullptr ? "is not in the shell" : "is not in the catalogue");
    }
    if (const auto* anchor = shell.find("b_camera_t")) {
        if (const auto* component = catalog.find("b_camera_t"))
            out.movie_camera = shell_button(*anchor, *component);
    }
    if (const auto* anchor = shell.find("b_special_weapon2")) {
        if (const auto* component = catalog.find("b_special_weapon2"))
            out.beacon = shell_button(*anchor, *component);
    }
    for (const std::string_view name : tactical_panel_buttons) {
        const auto* anchor = shell.find(name);
        const auto* component = catalog.find(name);
        if (anchor != nullptr && component != nullptr) out.panel_buttons.push_back(shell_button(*anchor, *component));
        else missing(name, anchor == nullptr ? "is not in the shell" : "is not in the catalogue");
    }

    const auto* text_anchor = shell.find(planet_name_component);
    if (const auto* anchor = shell.find("b_special_weapon")) {
        if (const auto* component = catalog.find("b_special_weapon")) {
            HudAbilityButton special;
            special.button = shell_button(*anchor, *component);
            special.blank = special.button.blank;
            special.build = first_token(*component, data::ui::Field::build_texture_name);
            out.special_weapon = std::move(special);
        }
    }
    // HCB-15: the component authors no Icon_Texture_Name; its icon is the first alternate.
    if (const auto* anchor = shell.find("b_retreat")) {
        if (const auto* component = catalog.find("b_retreat")) {
            HudAbilityButton retreat;
            retreat.button = shell_button(*anchor, *component);
            if (retreat.button.normal.empty() && !retreat.button.alternates.empty())
                retreat.button.normal = retreat.button.alternates.front();
            retreat.blank = retreat.button.blank;
            retreat.build = first_token(*component, data::ui::Field::build_texture_name);
            out.retreat = std::move(retreat);
        }
    }
    // BA-27: retain authored positions, including the waypoint slot.
    for (const auto name : tactical_order_buttons) {
        const auto* anchor = shell.find(name);
        const auto* component = catalog.find(name);
        if (anchor && component) out.order_buttons.push_back(shell_button(*anchor, *component));
        else missing(name, anchor == nullptr ? "is not in the shell" : "is not in the catalogue");
    }
    const auto* text_component = catalog.find(planet_name_component);
    if (text_anchor != nullptr && text_component != nullptr) {
        HudShellText text;
        text.name = std::string(planet_name_component);
        text.rect = text_anchor->rect;
        text.face = std::string(text_component->text(data::ui::Field::font_name));
        if (text.face.empty()) text.face = std::string(last_resort_face);
        text.point_size = text_component->integer(data::ui::Field::font_point_size).value_or(8);
        if (const auto colour = text_component->color(data::ui::Field::text_color)) text.colour = *colour;
        text.outline = text_component->flag(data::ui::Field::text_outline);
        text.emboss = text_component->flag(data::ui::Field::text_emboss);
        text.max_text_width = text_component->integer(data::ui::Field::max_text_width);
        out.planet_name = std::move(text);
    } else {
        missing(planet_name_component, text_anchor == nullptr ? "is not in the shell" : "is not in the catalogue");
    }

    for (std::size_t index = 0; index < card_slot_limit; ++index) {
        const std::string name = numbered(card_stem, index);
        const auto* anchor = shell.find(name);
        const auto* component = catalog.find(name);
        if (anchor == nullptr || component == nullptr) break;
        HudCardSlot slot;
        slot.card = shell_button(*anchor, *component);
        slot.count_offset = component->vec2(data::ui::Field::text_offset2).value_or(data::ui::Vec2{});
        slot.face = std::string(component->text(data::ui::Field::font_name));
        if (slot.face.empty()) slot.face = std::string(last_resort_face);
        slot.point_size = component->integer(data::ui::Field::font_point_size).value_or(8);
        if (const auto colour = component->color(data::ui::Field::color)) slot.colour = *colour;
        slot.outline = component->flag(data::ui::Field::text_outline);
        // #454 AB-08: the card's ability mark offsets and textures.
        const auto offset = [&](const data::ui::Field field) { return component->vec2(field).value_or(data::ui::Vec2{}); };
        slot.marks.icon = offset(data::ui::Field::icon_offset);
        slot.marks.dial = offset(data::ui::Field::build_dial_offset);
        slot.marks.overlay = offset(data::ui::Field::overlay_offset);
        slot.marks.second_icon = offset(data::ui::Field::upper_effect_offset);
        slot.marks.second_dial = offset(data::ui::Field::build_dial2_offset);
        slot.marks.second_overlay = offset(data::ui::Field::overlay2_offset);
        slot.marks.build = first_token(*component, data::ui::Field::build_texture_name);
        slot.marks.overlay_texture = first_token(*component, data::ui::Field::overlay_texture_name);
        slot.marks.overlay2_texture = first_token(*component, data::ui::Field::overlay2_texture_name);
        for (const auto& [stem, bar] : {std::pair{health_stem, &slot.health}, std::pair{shield_stem, &slot.shield}}) {
            const std::string bar_name = numbered(stem, index);
            const auto* bar_anchor = shell.find(bar_name);
            const auto* bar_component = catalog.find(bar_name);
            if (bar_anchor != nullptr && bar_component != nullptr) *bar = shell_bar(*bar_anchor, *bar_component, out.diagnostics);
            else missing(bar_name, bar_anchor == nullptr ? "is not in the shell" : "is not in the catalogue");
        }
        out.card_slots.push_back(std::move(slot));
    }
    // A shell without card slots draws no cards (a mod may leave them out).
    for (std::size_t index = 0; index < (out.card_slots.size() + 1) / 2; ++index) {
        const std::string name = numbered(border_stem, index);
        const auto* anchor = shell.find(name);
        const auto* component = catalog.find(name);
        if (anchor == nullptr || component == nullptr) {
            missing(name, anchor == nullptr ? "is not in the shell" : "is not in the catalogue");
            break;
        }
        out.card_borders.push_back(shell_button(*anchor, *component));
    }
    // #454: two ability buttons per column border (a group's button index is its first plus its
    // last column; a second ability takes the next one). A shell without them draws no ability
    // buttons; a partial set warns.
    for (std::size_t index = 0; index < out.card_borders.size() * 2; ++index) {
        const std::string name = numbered(ability_stem, index);
        const auto* anchor = shell.find(name);
        const auto* component = catalog.find(name);
        if (anchor == nullptr || component == nullptr) {
            if (index != 0) missing(name, anchor == nullptr ? "is not in the shell" : "is not in the catalogue");
            out.ability_buttons.clear();
            break;
        }
        HudAbilityButton button;
        button.button = shell_button(*anchor, *component);
        button.blank = first_token(*component, data::ui::Field::blank_texture_name);
        button.build = first_token(*component, data::ui::Field::build_texture_name);
        button.anim_fps = static_cast<float>(component->integer(data::ui::Field::anim_fps).value_or(5));
        out.ability_buttons.push_back(std::move(button));
    }
    // #530 PU-63: the build queue slots. A shell without them shows no queue; a partial set warns.
    for (std::size_t index = 0; index < queue_slot_limit; ++index) {
        const std::string name = std::string(queue_stem) + (index < 10 ? "0" : "") + std::to_string(index);
        const auto* anchor = shell.find(name);
        const auto* component = catalog.find(name);
        if (anchor == nullptr || component == nullptr) {
            if (index != 0) missing(name, anchor == nullptr ? "is not in the shell" : "is not in the catalogue");
            out.queue_slots.clear();
            break;
        }
        HudQueueSlot slot;
        slot.button = shell_button(*anchor, *component);
        slot.build = first_token(*component, data::ui::Field::build_texture_name);
        slot.text = component->type == data::ui::ComponentType::text_button;
        slot.text_offset = component->vec2(data::ui::Field::text_offset).value_or(data::ui::Vec2{});
        slot.face = std::string(component->text(data::ui::Field::font_name));
        if (slot.face.empty()) slot.face = std::string(last_resort_face);
        slot.point_size = component->integer(data::ui::Field::font_point_size).value_or(8);
        if (const auto colour = component->color(data::ui::Field::text_color)) slot.colour = *colour;
        slot.outline = component->flag(data::ui::Field::text_outline);
        out.queue_slots.push_back(std::move(slot));
    }
    out.reinforcement_counter = hud_timer_style(catalog, "reinforcement_counter");
    out.garrison_respawn_counter = hud_timer_style(catalog, "garrison_respawn_counter");
    // #530 PU-65: the credits text and its money icon.
    for (const auto name : {credits_component, std::string_view("Text_Planetary_Pop"), std::string_view("Text_Tactical_Tech")}) {
        const auto* credits_anchor = shell.find(name);
        const auto* credits = catalog.find(name);
        if (credits_anchor != nullptr && credits != nullptr) {
            HudIconText text = *hud_readout_style(catalog, name, *credits_anchor);
            if (name == credits_component) out.credits = std::move(text);
            else if (name == "Text_Planetary_Pop") out.population = std::move(text);
            else out.tactical_tech = std::move(text);
        } else {
            missing(name, credits_anchor == nullptr ? "is not in the shell" : "is not in the catalogue");
        }
    }
    const auto* reinforce_anchor = shell.find(reinforcement_component);
    const auto* reinforce = catalog.find(reinforcement_component);
    if (reinforce_anchor != nullptr && reinforce != nullptr) {
        out.reinforcement = shell_button(*reinforce_anchor, *reinforce);
        // PU-70: space uses the first alternate icon (the second is the same art).
        if (!out.reinforcement->alternates.empty()) out.reinforcement->normal = out.reinforcement->alternates.front();
    } else {
        missing(reinforcement_component, reinforce_anchor == nullptr ? "is not in the shell" : "is not in the catalogue");
    }
    return out;
}

std::string reinforce_pane_model(const data::ui::CommandBarCatalog& catalog) {
    const auto* shell = catalog.find(pane_shell_component);
    if (shell != nullptr && shell->type == data::ui::ComponentType::shell) {
        const auto model = shell->text(data::ui::Field::model_name);
        if (!model.empty()) return std::string(model);
    }
    return std::string(pane_shell_fallback_model);
}

HudReinforcePane reinforce_pane(const data::ui::ShellAnchors& shell, const data::ui::CommandBarCatalog& catalog,
                                const data::ui::TextDatabase* text_database) {
    HudReinforcePane out;
    out.model = shell.model_path();
    const auto missing = [&](const std::string_view part, const std::string_view why) {
        out.diagnostics.push_back(warning(diagnostic_codes::hud_shell_part,
            std::string(part) + " " + std::string(why) + "; the reinforcement pane draws without it", out.model));
    };
    // PU-67: the pane alternates are row counts; the command bar's faction alternates do not apply.
    for (std::size_t rows = 0; rows < out.meshes.size(); ++rows) {
        for (const auto* anchor : shell.for_variant(static_cast<std::uint32_t>(rows))) {
            const bool alpha = equal_name(anchor->shader, "MeshAlpha.fx");
            const bool additive = equal_name(anchor->shader, "MeshAdditive.fx");
            if ((!alpha && !additive) || anchor->base_texture.empty() || anchor->triangles.empty()) continue;
            if (catalog.find(anchor->name) != nullptr || !anchor->visible) continue;
            out.meshes[rows].push_back({anchor->name, additive ? ShellBlend::additive : ShellBlend::alpha,
                                      anchor->base_texture, anchor->z_min, anchor->triangles, std::nullopt});
        }
        std::stable_sort(out.meshes[rows].begin(), out.meshes[rows].end(),
                         [](const HudShellMesh& a, const HudShellMesh& b) { return a.z < b.z; });
    }
    for (std::size_t row = 0; row < pane_rows; ++row) {
        for (std::size_t column = 0; column < pane_columns; ++column) {
            const std::string name = std::string(pane_slot_stem) + numbered("", row) + numbered("", column);
            const auto* anchor = shell.find(name);
            const auto* component = catalog.find(name);
            if (anchor == nullptr || component == nullptr) {
                missing(name, anchor == nullptr ? "is not in the shell" : "is not in the catalogue");
                out.slots.clear();
                row = pane_rows;
                break;
            }
            if (out.slots.empty()) {
                out.slot_face = std::string(component->text(data::ui::Field::font_name));
                if (out.slot_face.empty()) out.slot_face = std::string(last_resort_face);
                out.slot_point_size = component->integer(data::ui::Field::font_point_size).value_or(6);
                if (const auto colour = component->color(data::ui::Field::text_color)) out.slot_colour = *colour;
                out.slot_text_offset = component->vec2(data::ui::Field::text_offset).value_or(data::ui::Vec2{});
            }
            out.slots.push_back(shell_button(*anchor, *component));
        }
    }
    if (const auto* anchor = shell.find(pane_close_component); anchor != nullptr) {
        if (const auto* component = catalog.find(pane_close_component)) {
            out.close = shell_button(*anchor, *component);
            out.close->normal = first_token(*component, data::ui::Field::icon_texture_name);
            out.close_swap_texture = component->flag(data::ui::Field::swap_texture);
            HudIconText label;
            label.text.name = std::string(pane_close_component);
            label.text.rect = anchor->rect;
            label.text.face = std::string(component->text(data::ui::Field::font_name));
            label.text.point_size = component->integer(data::ui::Field::font_point_size).value_or(6);
            if (const auto colour = component->color(data::ui::Field::text_color)) label.text.colour = *colour;
            label.text.outline = component->flag(data::ui::Field::text_outline);
            label.text.emboss = component->flag(data::ui::Field::text_emboss);
            label.text_offset = component->vec2(data::ui::Field::text_offset).value_or(data::ui::Vec2{});
            out.close_text = std::move(label);
            // PU-72: battle mode resolves this key; setup mode's Begin label is outside this pane.
            out.close_text_key = "TEXT_BUTTON_CLOSE";
            if (const auto* entry = text_database ? text_database->find(out.close_text_key) : nullptr)
                out.close_label = data::ui::to_utf8(entry->value);
            else missing(pane_close_component, "has no close label in the text database");
        }
    }
    if (!out.close) missing(pane_close_component, "is missing");
    const auto* text_anchor = shell.find(pane_population_component);
    const auto* text = catalog.find(pane_population_component);
    if (text_anchor != nullptr && text != nullptr) {
        HudIconText population;
        population.text.name = std::string(pane_population_component);
        population.text.rect = text_anchor->rect;
        population.text.face = std::string(text->text(data::ui::Field::font_name));
        if (population.text.face.empty()) population.text.face = std::string(last_resort_face);
        population.text.point_size = text->integer(data::ui::Field::font_point_size).value_or(6);
        if (const auto colour = text->color(data::ui::Field::color)) population.text.colour = *colour;
        population.text_offset = text->vec2(data::ui::Field::text_offset).value_or(data::ui::Vec2{});
        if (const auto* icon_anchor = shell.find(pane_population_icon)) {
            if (const auto* icon = catalog.find(pane_population_icon)) {
                population.icon = first_token(*icon, data::ui::Field::icon_texture_name);
                population.text.rect = icon_anchor->rect;
            }
        }
        out.population = std::move(population);
    } else if (text_anchor != nullptr) {
        missing(pane_population_component, "is not in the catalogue");
    }
    // FoC's own i_main_reinforce.alo has no r_pop_text bone, so the pane carries no population
    // text there; that is the game's content, not a gap to report (space-purchasing PU-G24).
    return out;
}

data::ui::ReferenceRect button_quad(const HudShellButton& button, const float texel_width,
                                    const float texel_height) noexcept {
    const float width = texel_width * button.scale;
    const float height = texel_height * button.scale;
    return {button.origin.x - width / 2.0F, button.origin.y - height / 2.0F, width, height};
}

ReferencePoint shell_point_to_screen(const double x, const double y, const ShellPlacement& shell) noexcept {
    return {shell.left + x * shell.scale, shell.bottom - y * shell.scale};
}

ShellMaskLookup vfs_shell_masks(const vfs::Vfs& filesystem) {
    struct Cache {
        std::mutex mutex;
        std::map<std::string, std::shared_ptr<const ShellAlphaMask>, std::less<>> masks;
    };
    auto cache = std::make_shared<Cache>();
    auto standalone = vfs_standalone_textures(filesystem);
    return [&filesystem, cache, standalone](const std::string_view name) -> std::shared_ptr<const ShellAlphaMask> {
        const std::lock_guard lock(cache->mutex);
        const auto found = cache->masks.find(name);
        if (found != cache->masks.end()) return found->second;
        std::shared_ptr<const ShellAlphaMask> mask;
        if (const auto path = standalone(name)) {
            if (auto texture = assets::load_texture(filesystem, *path)) {
                if (auto decoded = shell_alpha_mask(texture.value())) {
                    mask = std::make_shared<ShellAlphaMask>(std::move(decoded.value()));
                }
            }
        }
        cache->masks.emplace(std::string(name), mask);
        return mask;
    };
}

std::string_view to_string(const PlanetNameSource source) noexcept {
    switch (source) {
    case PlanetNameSource::text: return "text";
    case PlanetNameSource::context_name: return "context_name";
    case PlanetNameSource::none: return "none";
    }
    return "none";
}

PlanetName planet_name(const std::optional<std::string>& context_name, const data::Catalog* objects,
                       const data::ui::TextDatabase* text) {
    PlanetName out;
    if (!context_name || context_name->empty()) {
        out.diagnostics.push_back(warning(diagnostic_codes::hud_planet_name,
            "the map names no planet (root field 0x09); the HUD shows no planet name"));
        return out;
    }
    out.context = *context_name;
    const auto fall_back = [&](std::string why) {
        out.text = out.context;
        out.source = PlanetNameSource::context_name;
        out.diagnostics.push_back(warning(diagnostic_codes::hud_planet_name,
            std::move(why) + "; the HUD shows the map's context name " + out.context));
        return out;
    };
    const data::Definition* planet = objects != nullptr ? objects->find(out.context, data::Category::game_object) : nullptr;
    if (planet == nullptr || !equal_name(planet->type_name, "Planet")) {
        return fall_back("no Planet object is named " + out.context);
    }
    auto resolved = objects->resolve(out.context, data::Category::game_object);
    const data::EffectiveValue* text_id = resolved ? resolved.value().value("Text_ID") : nullptr;
    if (text_id == nullptr) return fall_back("Planet " + out.context + " has no Text_ID");
    out.text_id = text_id->value.raw_text;
    const auto trim = [](std::string& value) {
        value = core::trim(value, core::xml_whitespace);
    };
    trim(out.text_id);
    const data::ui::TextEntry* entry = text != nullptr && !out.text_id.empty() ? text->find(out.text_id) : nullptr;
    if (entry == nullptr) return fall_back("the text DB has no " + out.text_id);
    out.text = data::ui::to_utf8(entry->value);
    out.source = PlanetNameSource::text;
    return out;
}

std::optional<data::ui::Rgba8> production_queue_tint(const vfs::Vfs& filesystem) {
    const auto document = data::load_document(filesystem, "data/xml/gameconstants.xml");
    if (!document) return std::nullopt;
    for (const auto& node : document.value().root.children) {
        if (!equal_name(node.name, "Right_Queue_Tint")) continue;
        data::tag_trace::used(node);
        std::string raw = node.raw_text;
        std::replace(raw.begin(), raw.end(), ',', ' ');
        std::istringstream stream(raw);
        std::array<int, 4> channels{0, 0, 0, 255};
        std::size_t count{};
        std::string token;
        while (stream >> token) {
            int value{};
            const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
            if (error != std::errc{} || end != token.data() + token.size()
                || count == channels.size() || value < 0 || value > 255) return std::nullopt;
            channels[count++] = value;
        }
        if (!stream.eof() || (count != 3 && count != 4)) return std::nullopt;
        return data::ui::Rgba8{static_cast<std::uint8_t>(channels[0]), static_cast<std::uint8_t>(channels[1]),
            static_cast<std::uint8_t>(channels[2]), static_cast<std::uint8_t>(channels[3])};
    }
    return std::nullopt;
}

EncyclopediaConstants encyclopedia_constants(const vfs::Vfs& filesystem) {
    EncyclopediaConstants result;
    const auto document = data::load_document(filesystem, "data/xml/gameconstants.xml");
    if (!document) {
        result.diagnostics.push_back(warning("EAWR-UI-0321",
            "encyclopedia globals are unavailable; defaults retained", "data/xml/gameconstants.xml"));
        return result;
    }
    const auto scalar = [&](const std::string_view tag, const bool nonnegative) -> std::optional<double> {
        const auto invalid = [&]() -> std::optional<double> {
            result.diagnostics.push_back(warning("EAWR-UI-0321",
                std::string(tag) + " is missing or malformed; default retained", "data/xml/gameconstants.xml"));
            return std::nullopt;
        };
        const data::XmlNode* found{};
        for (const auto& child : document.value().root.children) if (equal_name(child.name, tag)) found = &child;
        if (!found) return invalid();
        const auto raw = core::trim_view(found->raw_text, core::xml_whitespace);
        if (raw.empty()) return invalid();
        double value{};
        const auto [last, error] = std::from_chars(raw.data(), raw.data() + raw.size(), value);
        if (error != std::errc{} || last != raw.data() + raw.size() || !std::isfinite(value)
            || (nonnegative && value < 0)) return invalid();
        return value;
    };
    EncyclopediaTiming timing;
    bool complete = true;
    for (const auto& [tag, destination, divisor] : std::array{
        std::tuple{"Encyclopedia_Delay", &timing.delay, 1000.0},
        std::tuple{"Encyclopedia_Min_Display_Time", &timing.minimum, 1.0},
        std::tuple{"Encyclopedia_Fade_Rate", &timing.fade_rate, 1.0}}) {
        if (const auto value = scalar(tag, true)) *destination = *value / divisor;
        else complete = false;
    }
    if (complete) result.timing = timing;
    for (const auto& [tag, destination] : std::array{
        std::pair{"Encyclopedia_Population_Offset", &result.population_offset},
        std::pair{"Encyclopedia_Name_Offset", &result.name_offset},
        std::pair{"Encyclopedia_Cost_Offset", &result.cost_offset},
        std::pair{"Encyclopedia_Icon_X_Offset", &result.icon_x_offset},
        std::pair{"Encyclopedia_Icon_Y_Offset", &result.icon_y_offset},
        std::pair{"Encyclopedia_Class_Y_Offset", &result.class_y_offset}}) {
        if (const auto value = scalar(tag, false)) {
            // Offsets may be signed, but must remain representable in shell coordinates.
            if (std::abs(*value) <= std::numeric_limits<float>::max()) *destination = static_cast<float>(*value);
            else result.diagnostics.push_back(warning("EAWR-UI-0321",
                std::string(tag) + " is out of range; default retained", "data/xml/gameconstants.xml"));
        }
    }
    return result;
}

std::optional<EncyclopediaTiming> encyclopedia_timing(const vfs::Vfs& filesystem) {
    return encyclopedia_constants(filesystem).timing;
}

static UnitCardLooks ability_card_looks_bounded(const std::string_view type, const std::uint32_t ability,
                               const data::Catalog* objects, const data::ui::TextDatabase* text, const std::size_t depth) {
    // A malformed cyclic/nested roster must not exhaust the presentation stack.
    constexpr std::size_t maximum_roster_depth = 32;
    if (depth >= maximum_roster_depth) return {};
    UnitCardLooks out;
    const std::string name(ability_name(ability));
    if (name.empty()) return out;
    std::string title = "TEXT_TOOLTIP_ABILITY_" + name + "_NAME";
    std::string description = "TEXT_TOOLTIP_ABILITY_" + name + "_DESCRIPTION";
    out.icon = ability_icon(ability);
    const auto trim = [](std::string_view raw) {
        return core::trim(raw, core::xml_whitespace);
    };
    if (objects && objects->find(type, data::Category::game_object)) {
        if (auto resolved = objects->resolve(type, data::Category::game_object)) {
            if (const auto* roster = resolved.value().value("Squadron_Units")) {
                const auto raw = trim(roster->value.raw_text);
                const auto leader = raw.substr(0, raw.find_first_of(" ,\t\r\n"));
                if (!equal_name(leader, type) && objects->find(leader, data::Category::game_object))
                    return ability_card_looks_bounded(leader, ability, objects, text, depth + 1);
            }
            if (const auto* abilities = resolved.value().value("Unit_Abilities_Data")) {
                for (const auto& entry : abilities->value.children) {
                    bool matches{};
                    for (const auto& value : entry.children)
                        if (equal_name(value.name, "Type")) matches = equal_name(trim(value.raw_text), name);
                    if (!matches) continue;
                    // CARD-15: independently authored overrides fall back only
                    // when their keys are empty, before localization.
                    for (const auto& value : entry.children) {
                        const auto raw = trim(value.raw_text);
                        if (raw.empty()) continue;
                        if (equal_name(value.name, "Alternate_Name_Text")) title = raw;
                        if (equal_name(value.name, "Alternate_Description_Text")) description = raw;
                        if (equal_name(value.name, "Alternate_Icon_Name")) out.icon = raw;
                    }
                    break;
                }
            }
        }
    }
    if (text) {
        if (const auto* entry = text->find(title)) out.name = data::ui::to_utf8(entry->value);
        if (const auto* entry = text->find(description)) out.description = data::ui::to_utf8(entry->value);
    }
    return out;
}

UnitCardLooks ability_card_looks(const std::string_view type, const std::uint32_t ability,
                               const data::Catalog* objects, const data::ui::TextDatabase* text) {
    return ability_card_looks_bounded(type, ability, objects, text, 0);
}

UnitCardLooks unit_card_looks(const std::string_view type, const data::Catalog* objects,
                              const data::ui::TextDatabase* text, const bool multiplayer) {
    UnitCardLooks out;
    out.name = type;
    if (objects == nullptr || objects->find(type, data::Category::game_object) == nullptr) return out;
    auto resolved = objects->resolve(type, data::Category::game_object);
    if (!resolved) return out;
    const auto trimmed = [](const std::string& value) {
        return core::trim(value, core::xml_whitespace);
    };
    if (const data::EffectiveValue* icon = resolved.value().value("Icon_Name")) out.icon = trimmed(icon->value.raw_text);
    const auto integer = [&](const std::string_view tag) {
        const auto* field = resolved.value().value(tag);
        if (!field) return std::int32_t{};
        const auto raw = trimmed(field->value.raw_text);
        std::int32_t value{};
        const auto [last, error] = std::from_chars(raw.data(), raw.data() + raw.size(), value);
        return error == std::errc{} && last == raw.data() + raw.size() ? value : 0;
    };
    const auto usage = integer("Population_Value");
    out.population = usage > 0 ? -usage : 0;
    if (!out.population) out.population = std::max(0, integer("Additional_Population_Capacity"));
    if (const data::EffectiveValue* cost = resolved.value().value("Tactical_Build_Cost_Multiplayer")) {
        const std::string digits = trimmed(cost->value.raw_text);
        std::int64_t value{};
        const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
        if (error == std::errc{} && end == digits.data() + digits.size()) out.build_cost = value;
    }
    if (const data::EffectiveValue* id = resolved.value().value("Text_ID")) {
        const std::string key = trimmed(id->value.raw_text);
        const data::ui::TextEntry* entry = text != nullptr && !key.empty() ? text->find(key) : nullptr;
        if (entry != nullptr) out.name = data::ui::to_utf8(entry->value);
    }
    const auto tokens = [&](const std::string_view tag) {
        std::vector<std::string> result;
        const auto* value = resolved.value().value(tag);
        if (!value) return result;
        const auto& raw = value->value.raw_text;
        auto begin = raw.find_first_not_of(" ,\t\r\n");
        while (begin != std::string::npos) {
            const auto end = raw.find_first_of(" ,\t\r\n", begin);
            result.push_back(raw.substr(begin, end == std::string::npos ? end : end - begin));
            begin = end == std::string::npos ? end : raw.find_first_not_of(" ,\t\r\n", end);
        }
        return result;
    };
    const auto localized = [&](const std::string_view key) {
        const auto* entry = text ? text->find(key) : nullptr;
        return entry ? data::ui::to_utf8(entry->value) : std::string();
    };
    // P-1/P-2: choose the authored list before localization, including unknown MP keys.
    auto descriptions = multiplayer ? tokens("MP_Encyclopedia_Text") : std::vector<std::string>{};
    if (descriptions.empty()) descriptions = tokens("Encyclopedia_Text");
    for (std::size_t index = 0; index < descriptions.size(); ++index) {
        if (index) out.description += '\n';
        out.description += localized(descriptions[index]);
    }
    // P-3: the class is a text key, not the object's simulation class.
    if (const auto* value = resolved.value().value("Encyclopedia_Unit_Class"))
        out.unit_class = localized(trimmed(value->value.raw_text));
    const auto icons = [&](const std::string_view tag) {
        std::vector<std::string> result;
        for (const auto& name : tokens(tag)) {
            if (result.size() == 3) break;
            std::string icon;
            if (objects->find(name, data::Category::game_object)) {
                auto target = objects->resolve(name, data::Category::game_object);
                if (target) {
                    if (const auto* value = target.value().value("Icon_Name")) icon = trimmed(value->value.raw_text);
                }
            }
            result.push_back(std::move(icon));
        }
        return result;
    };
    // P-4: blank icons retain their position; a fourth entry is dropped.
    out.good_against_icons = icons("Encyclopedia_Good_Against");
    out.vulnerable_to_icons = icons("Encyclopedia_Vulnerable_To");
    // CARD-16: a company uses its first authored craft's static abilities.
    auto ability_source = resolved;
    if (const auto roster = tokens("Squadron_Units"); !roster.empty()
        && objects->find(roster.front(), data::Category::game_object))
        ability_source = objects->resolve(roster.front(), data::Category::game_object);
    if (ability_source) if (const auto* abilities = ability_source.value().value("Unit_Abilities_Data")) {
        for (const auto& entry : abilities->value.children) for (const auto& value : entry.children) {
            if (!equal_name(value.name, "Type")) continue;
            const auto look = ability_card_looks(ability_source.value().object_id, ability_index(trimmed(value.raw_text)), objects, text);
            if (!look.name.empty() && !look.icon.empty()) out.ability_icons.push_back(look.icon);
        }
    }
    return out;
}

} // namespace eawr::presentation::ui
