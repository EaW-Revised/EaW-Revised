#include "eawr/presentation/ui/hud_shell.hpp"

#include "eawr/data/ui/text_database.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/presentation/ui/theme.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace eawr::presentation::ui {
namespace {

char lower(const char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; }
bool equal_name(const std::string_view a, const std::string_view b) {
    return a.size() == b.size()
        && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return lower(x) == lower(y); });
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
    button.tooltip = first_token(component, data::ui::Field::tooltip_text);
    const auto alternates = component.list(data::ui::Field::icon_alternate_texture_name);
    button.alternates.assign(alternates.begin(), alternates.end());
    return button;
}

HudBar shell_bar(const data::ui::ShellAnchor& anchor, const data::ui::CommandBarComponent& component) {
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
    bar.max_level = component.integer(data::ui::Field::max_bar_level).value_or(10);
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

} // namespace

std::uint32_t alt_variant(const HudFaction faction) noexcept {
    switch (faction) {
    case HudFaction::empire: return data::ui::alt_variant::empire;
    case HudFaction::rebel: return data::ui::alt_variant::rebel;
    case HudFaction::underworld: return data::ui::alt_variant::underworld;
    }
    return data::ui::alt_variant::rebel;
}

std::string_view to_string(const HudFaction faction) noexcept {
    switch (faction) {
    case HudFaction::empire: return "empire";
    case HudFaction::rebel: return "rebel";
    case HudFaction::underworld: return "underworld";
    }
    return "rebel";
}

std::optional<HudFaction> hud_faction_from(const std::string_view text) noexcept {
    if (equal_name(text, "empire")) return HudFaction::empire;
    if (equal_name(text, "rebel")) return HudFaction::rebel;
    if (equal_name(text, "underworld")) return HudFaction::underworld;
    return std::nullopt;
}

std::string tactical_shell_model(const data::ui::CommandBarCatalog& catalog) {
    const auto* shell = catalog.find(tactical_shell_component);
    if (shell != nullptr && shell->type == data::ui::ComponentType::shell) {
        const auto model = shell->text(data::ui::Field::model_name);
        if (!model.empty()) return std::string(model);
    }
    return std::string(tactical_shell_fallback_model);
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

HudShell hud_shell(const data::ui::ShellAnchors& shell, const data::ui::CommandBarCatalog& catalog,
                   const HudFaction faction) {
    HudShell out;
    out.model = shell.model_path();
    out.faction = faction;
    const auto missing = [&](const std::string_view part, const std::string_view why) {
        out.diagnostics.push_back(warning(diagnostic_codes::hud_shell_part,
            std::string(part) + " " + std::string(why) + "; the HUD shell draws without it", out.model));
    };
    for (const auto* anchor : shell.for_variant(alt_variant(faction))) {
        const bool alpha = equal_name(anchor->shader, "MeshAlpha.fx");
        const bool additive = equal_name(anchor->shader, "MeshAdditive.fx");
        if ((!alpha && !additive) || anchor->base_texture.empty() || anchor->triangles.empty()) continue;
        // Decorative art the shell shows by itself; a bound component's mesh is
        // drawn only for the parts this ticket owns (the radar's scan lines).
        const bool bound = catalog.find(anchor->name) != nullptr;
        const bool radar = equal_name(anchor->name, radar_name);
        if (bound ? !radar : !anchor->visible) continue;
        out.meshes.push_back({anchor->name, additive ? ShellBlend::additive : ShellBlend::alpha,
                              anchor->base_texture, anchor->z_min, anchor->triangles});
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
    for (const std::string_view name : tactical_panel_buttons) {
        const auto* anchor = shell.find(name);
        const auto* component = catalog.find(name);
        if (anchor != nullptr && component != nullptr) out.panel_buttons.push_back(shell_button(*anchor, *component));
        else missing(name, anchor == nullptr ? "is not in the shell" : "is not in the catalogue");
    }

    const auto* text_anchor = shell.find(planet_name_component);
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
            if (bar_anchor != nullptr && bar_component != nullptr) *bar = shell_bar(*bar_anchor, *bar_component);
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
    const data::Definition* planet = objects != nullptr ? objects->find(out.context) : nullptr;
    if (planet == nullptr || !equal_name(planet->type_name, "Planet")) {
        return fall_back("no Planet object is named " + out.context);
    }
    auto resolved = objects->resolve(out.context);
    const data::EffectiveValue* text_id = resolved ? resolved.value().value("Text_ID") : nullptr;
    if (text_id == nullptr) return fall_back("Planet " + out.context + " has no Text_ID");
    out.text_id = text_id->value.raw_text;
    const auto trim = [](std::string& value) {
        const auto first = value.find_first_not_of(" \t\r\n");
        const auto last = value.find_last_not_of(" \t\r\n");
        value = first == std::string::npos ? std::string() : value.substr(first, last - first + 1U);
    };
    trim(out.text_id);
    const data::ui::TextEntry* entry = text != nullptr && !out.text_id.empty() ? text->find(out.text_id) : nullptr;
    if (entry == nullptr) return fall_back("the text DB has no " + out.text_id);
    out.text = data::ui::to_utf8(entry->value);
    out.source = PlanetNameSource::text;
    return out;
}

UnitCardLooks unit_card_looks(const std::string_view type, const data::Catalog* objects,
                              const data::ui::TextDatabase* text) {
    UnitCardLooks out{std::string(), std::string(type)};
    if (objects == nullptr || objects->find(type) == nullptr) return out;
    auto resolved = objects->resolve(type);
    if (!resolved) return out;
    const auto trimmed = [](const std::string& value) {
        const auto first = value.find_first_not_of(" \t\r\n");
        const auto last = value.find_last_not_of(" \t\r\n");
        return first == std::string::npos ? std::string() : value.substr(first, last - first + 1U);
    };
    if (const data::EffectiveValue* icon = resolved.value().value("Icon_Name")) out.icon = trimmed(icon->value.raw_text);
    if (const data::EffectiveValue* id = resolved.value().value("Text_ID")) {
        const std::string key = trimmed(id->value.raw_text);
        const data::ui::TextEntry* entry = text != nullptr && !key.empty() ? text->find(key) : nullptr;
        if (entry != nullptr) out.name = data::ui::to_utf8(entry->value);
    }
    return out;
}

} // namespace eawr::presentation::ui
