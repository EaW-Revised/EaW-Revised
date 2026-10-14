#include "tactical_hud_internal.hpp"
#include "eawr/presentation/ui/world_encyclopedia.hpp"

namespace eawr::presentation::godot_backend {
using tactical_hud_detail::text;
using tactical_hud_detail::rect2;

namespace tactical_hud_detail {
[[nodiscard]] String text(const std::string_view value) {
    return String::utf8(value.data(), static_cast<int64_t>(value.size()));
}


[[nodiscard]] Rect2 rect2(const model::PixelRect& rect) {
    return Rect2(static_cast<float>(rect.x), static_cast<float>(rect.y), static_cast<float>(rect.width),
                 static_cast<float>(rect.height));
}

[[nodiscard]] Color colour(const data::ui::Rgba8& value) {
    return Color(value.r / 255.0F, value.g / 255.0F, value.b / 255.0F, value.a / 255.0F);
}

} // namespace tactical_hud_detail

void TacticalHud::set_world_hover(const std::optional<std::pair<std::uint64_t, std::string>>& hovered) {
    State& state = *state_;
    if (!state.cards) return;
    if (hovered == state.world_hover && state.ability_hover_type.empty()) return;
    state.ability_hover_type.clear();
    state.world_hover = hovered;
    const bool admitted = hovered && model::world_encyclopedia_admitted(hovered->second, state.objects);
    state.cards->set_external_hover(admitted ? hovered->first : 0,
        admitted ? std::optional(state.encyclopedia_type(hovered->second)) : std::nullopt);
}

void TacticalHud::set_ability_hover(const std::string& type, const std::uint32_t ability,
                                  const std::size_t component) {
    State& state = *state_;
    if (!state.cards) return;
    if (type == state.ability_hover_type && ability == state.ability_hover_kind
        && component == state.ability_hover_component) return;
    state.world_hover.reset();
    state.ability_hover_type = type;
    state.ability_hover_kind = ability;
    state.ability_hover_component = component;
    auto content = model::ability_card_looks(type, ability, state.objects, state.text_database ? &*state.text_database : nullptr);
    state.cards->set_external_hover((std::uint64_t{1} << 62U) + component,
        content.name.empty() ? std::nullopt : std::optional(std::move(content)));
}

const model::UnitCardLooks& TacticalHud::State::encyclopedia_type(const std::string& type) {
    if (const auto found = encyclopedia_types.find(type); found != encyclopedia_types.end()) return found->second;
    return encyclopedia_types.emplace(type, model::unit_card_looks(type, objects,
        text_database ? &*text_database : nullptr)).first->second;
}

const std::pair<std::string, String>& TacticalHud::State::card_type(const std::string& type) {
    const auto found = card_types.find(type);
    if (found != card_types.end()) return found->second;
    const model::UnitCardLooks found_looks =
        model::unit_card_looks(type, objects, text_database ? &*text_database : nullptr);
    std::pair<std::string, String> looks{found_looks.icon, text(found_looks.name)};
    if (looks.first.empty()) warn("EAWR-UI-0322", type + " has no Icon_Name; its unit card shows the engine's temporary portrait", type);
    return card_types.emplace(type, std::move(looks)).first->second;
}


Ref<Texture2D> TacticalHud::State::command_texture(const std::string& name, const std::string& use, const bool quiet) {
    const auto found = card_textures.find(name);
    if (found != card_textures.end()) return found->second;
    const model::ThemeTexture slot = model::resolve_ui_texture(name, atlas ? &atlas->directory : nullptr, standalone);
    Ref<Texture2D> result = textures ? textures->texture(slot, 1.0, 1.0) : Ref<Texture2D>();
    if (result.is_null() && !quiet) warn("EAWR-UI-0322", use + " texture " + name + " cannot be drawn", name);
    card_textures.emplace(name, result);
    return result;
}


const model::MinimapTypeLooks& TacticalHud::minimap_looks(const std::string_view type) {
    State& state = *state_;
    const auto found = state.minimap_types.find(type);
    if (found != state.minimap_types.end()) return found->second;
    return state.minimap_types.emplace(std::string(type), model::minimap_type_looks(type, state.objects)).first->second;
}

std::optional<data::ui::Rgba8> TacticalHud::faction_colour(const std::string_view faction, const bool no_colorization) const {
    return model::faction_colour(state_->minimap_settings, faction, no_colorization);
}


} // namespace eawr::presentation::godot_backend
