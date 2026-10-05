#include "world_ui_view.hpp"

#include "ui/theme_builder.hpp"

#include "eawr/assets/assets.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/data/ui/command_bar.hpp"
#include "eawr/presentation/ui/layout.hpp"
#include "eawr/presentation/ui/pads.hpp"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/theme_db.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <utility>

#include "world_ui_internal.hpp"

namespace eawr::presentation::godot_backend {
using namespace godot;
namespace tactical = sim::tactical;
using namespace world_ui_detail;

namespace {

[[nodiscard]] std::string trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return std::string(text);
}

[[nodiscard]] std::string lower(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const char character : text) result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    return result;
}

[[nodiscard]] std::string tag(const data::EffectiveObject& object, const std::string_view name) {
    const data::EffectiveValue* value = object.value(name);
    return value == nullptr ? std::string{} : trim(value->value.raw_text);
}

// A number as FoC's XML writes it ("1.0f" included); empty or malformed text is none.
[[nodiscard]] std::optional<float> number(const std::string& text) {
    if (text.empty()) return std::nullopt;
    char* end = nullptr;
    const float value = std::strtof(text.c_str(), &end);
    if (end == text.c_str() || !std::isfinite(value)) return std::nullopt;
    return value;
}

[[nodiscard]] bool yes(const std::string& text) {
    const std::string value = lower(text);
    return value == "yes" || value == "true" || value == "1";
}

// "0,255,0,255" (Selection_Blob_RGBA).
[[nodiscard]] std::optional<ui::Rgb> rgba(const std::string& text) {
    ui::Rgb result{};
    std::size_t start = 0;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const std::size_t comma = text.find(',', start);
        const auto value = number(text.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
        if (!value || *value < 0.0F || *value > 255.0F) return std::nullopt;
        result[channel] = static_cast<std::uint8_t>(*value);
        if (comma == std::string::npos) return channel == 2 ? std::optional<ui::Rgb>(result) : std::nullopt;
        start = comma + 1;
    }
    return result;
}
} // namespace

Ref<ImageTexture> WorldUiView::standalone(const vfs::Vfs& filesystem, const std::string& stem) {
    for (const std::string& candidate : {"data/art/textures/" + stem + ".dds", "data/art/textures/" + stem + ".tga"}) {
        if (!filesystem.stat(candidate)) continue;
        auto loaded = assets::load_texture(filesystem, candidate);
        if (!loaded) {
            unresolved_.push_back(candidate + ": " + core::format_diagnostic(loaded.error()));
            continue;
        }
        std::string failure;
        const Ref<Image> image = texture_image(loaded.value(), failure);
        if (image.is_null()) {
            unresolved_.push_back(candidate + ": " + failure);
            continue;
        }
        return ImageTexture::create_from_image(image);
    }
    unresolved_.push_back("texture " + stem + " not found");
    return {};
}

std::optional<Rect2> WorldUiView::atlas_region(const std::string& name) const {
    const auto found = atlas_regions_.find(lower(name));
    if (found == atlas_regions_.end()) return std::nullopt;
    return found->second;
}

void WorldUiView::prepare(const vfs::Vfs& filesystem, const data::Catalog& catalog, const LiveSessionView& live) {
    // WU-46: compare identical world frames with/without ability art, including
    // the moving background seen through translucent squadron identity pixels.
    const char* ability_art_control = std::getenv("EAWR_WORLD_ABILITY_ART_CONTROL");
    ability_art_enabled_ = ability_art_control == nullptr || std::string_view(ability_art_control) != "off";
    // WU-43..WU-45: validated component data, with the UI's existing missing-font fallback.
    const Ref<Font> fallback_font = ThemeDB::get_singleton()->get_fallback_font();
    icon_group_.font = fallback_font;
    bracket_group_.font = fallback_font;
    if (auto components = data::ui::load_command_bar(filesystem)) {
        using Field = data::ui::Field;
        const auto read_text = [&](const char* name, GroupText& text) {
            const auto* component = components.value().catalog.find(name);
            if (component == nullptr) return;
            text.points = component->integer(Field::font_point_size).value_or(9);
            text.outline = component->flag(Field::text_outline);
            if (const auto offset = component->vec2(Field::text_offset)) text.offset = {offset->x, offset->y};
            if (const auto tint = component->color(Field::text_color)) {
                text.colour = Color(tint->r / 255.0F, tint->g / 255.0F, tint->b / 255.0F, tint->a / 255.0F);
            }
        };
        read_text("st_grab_bar", icon_group_);
        read_text("st_control_group", bracket_group_);
        if (const auto* component = components.value().catalog.find("st_grab_bar")) {
            // WU-43: active squadron art belongs to the separate lower effect quad.
            if (const auto offset = component->vec2(Field::lower_effect_offset)) squadron_ability_offset_ = {offset->x, offset->y};
        }
        if (const auto* component = components.value().catalog.find("st_ability_icon")) {
            bracket_icon_scale_ = component->number(Field::scale).value_or(1.0F);
            // WU-44: the additional active bracket slot uses the lower effect too.
            if (const auto offset = component->vec2(Field::lower_effect_offset)) second_ability_offset_ = {offset->x, offset->y};
        }
    }
    // WU-01 (debug build; Factions.xml, ShadowBlobMaterials.xml): the local player's
    // faction names its space selection material; the ring is that material's second texture,
    // tinted by the faction's Selection_Blob_RGBA.
    const std::string faction = live.local_faction();
    std::string material = "Selection_Rebel_Space";
    if (auto object = catalog.resolve(faction, data::Category::faction)) {
        if (const std::string named = tag(object.value(), "Space_Mode_Selection_Blob_Material_Name"); !named.empty()) {
            material = named;
        }
        if (const auto tint = rgba(tag(object.value(), "Selection_Blob_RGBA"))) ring_tint_ = *tint;
    } else if (lower(faction) == "empire") {
        material = "Selection_Empire_Space";
    }
    const std::string ring = lower(material) == "selection_empire_space" ? "i_selection_empire" : "i_selection_rebel";
    ring_source_ = faction + ":" + material + ":" + ring;
    ring_ = standalone(filesystem, ring);
    if (ring_.is_valid()) {
        ring_material_.instantiate();
        ring_material_->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
        ring_material_->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
        ring_material_->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
        ring_material_->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, ring_);
        ring_material_->set_albedo(colour(ring_tint_));
        ring_material_->set_flag(BaseMaterial3D::FLAG_DISABLE_FOG, true);
        ring_mesh_.instantiate();
        ring_mesh_->set_size(Vector2(1.0F, 1.0F));
        ring_mesh_->set_material(ring_material_);
    }

    // The command bar atlas holds the squadron icon frames and the unit icons.
    if (auto atlas = assets::load_mega_texture_atlas(filesystem, "Data/Art/Textures/MT_CommandBar.mtd")) {
        std::string failure;
        const Ref<Image> page = texture_image(atlas.value().page, failure);
        if (page.is_valid()) {
            atlas_ = ImageTexture::create_from_image(page);
            for (const assets::MegaTextureEntry& entry : atlas.value().directory.entries) {
                const auto& rect = entry.rectangle;
                atlas_regions_[lower(entry.name)] = Rect2(static_cast<float>(rect.x), static_cast<float>(rect.y),
                                                          static_cast<float>(rect.width), static_cast<float>(rect.height));
            }
        } else {
            unresolved_.push_back("MT_CommandBar page: " + failure);
        }
    } else {
        unresolved_.push_back("MT_CommandBar: " + core::format_diagnostic(atlas.error()));
    }

    const units::UnitTables* tables = live.tables();
    const sim::tactical::CombatTable* combat = live.combat();
    const sim::tactical::DurabilityTable* durability = live.durability();
    if (tables == nullptr) return;
    for (const units::UnitType& type : tables->units) {
        const sim::tactical::TypeId id = skirmish::type_id(type.id);
        TypeUi ui_type;
        ui_type.selectable = type.selectable;
        ui_type.bar_admitted = type.bar_admitted;
        ui_type.name = type.id;
        ui_type.shielded = type.shielded;
        ui_type.layer_z = to_float(type.movement.layer_z_adjust.value_or(sim::math::Fixed{}));
        ui_type.max_thrust = to_float(type.movement.max_thrust.value_or(sim::math::Fixed{}));
        for (std::size_t slot = 0; slot < std::min(type.abilities.size(), ui_type.abilities.size()); ++slot) {
            ui_type.abilities[slot] = ui::ability_index(type.abilities[slot].type);
            ui_type.ability_regions[slot] = atlas_region(std::string(ui::ability_icon(ui_type.abilities[slot])));
        }
        std::string ship_class;
        std::optional<int> bracket;
        if (auto object = catalog.resolve(type.id, data::Category::game_object)) {
            const auto scale = type.scale_factor ? to_float(*type.scale_factor) : number(tag(object.value(), "Scale_Factor")).value_or(1.0F);
            if (const auto box = number(tag(object.value(), "Select_Box_Scale"))) {
                ui_type.circle_side = ui::selection_circle_side(*box, scale);
            }
            ship_class = lower(tag(object.value(), "Ship_Class"));
            if (const auto size = number(tag(object.value(), "GUI_Bracket_Size"))) bracket = static_cast<int>(*size);
            ui_type.bounds_scale = number(tag(object.value(), "GUI_Bounds_Scale")).value_or(1.0F);
            ui_type.hide_health_bar = yes(tag(object.value(), "GUI_Hide_Health_Bar"));
            ui_type.icon = lower(tag(object.value(), "Icon_Name"));
            ui_type.hero_head = ui::hero_world_identity(type.named_hero, yes(tag(object.value(), "Show_Hero_Head")));
        } else {
            unresolved_.push_back("type " + type.id + ": " + core::format_diagnostic(object.error()));
        }
        ui_type.bar = ui::bar_size(ship_class, bracket);
        const sim::tactical::CombatProfile* profile = nullptr;
        if (combat != nullptr) {
            for (const auto& candidate : combat->profiles) {
                if (candidate.type_id == id) profile = &candidate;
            }
        }
        const sim::tactical::DurabilityProfile* health = durability != nullptr ? durability->find(id) : nullptr;
        for (std::size_t index = 0; index < type.hardpoints.size(); ++index) {
            const units::Hardpoint& source = type.hardpoints[index];
            HardpointUi hardpoint;
            hardpoint.texture = std::string(ui::hardpoint_reticle_texture(trim(source.type_name)));
            hardpoint.targetable = source.targetable;
            if (profile != nullptr) {
                for (const auto& point : profile->hardpoints) {
                    if (point.hardpoint != index) continue;
                    hardpoint.local = {to_float(point.position.x), to_float(point.position.y), to_float(point.position.z)};
                }
            }
            if (health != nullptr && index < health->hardpoints.size()) {
                hardpoint.max_health = to_float(health->hardpoints[index].max_health);
            }
            if (!hardpoint.texture.empty() && !reticles_.contains(hardpoint.texture)) {
                reticles_[hardpoint.texture] = standalone(filesystem, hardpoint.texture);
                reticles_[hardpoint.texture + "_tracked"] = standalone(filesystem, hardpoint.texture + "_tracked");
            }
            ui_type.hardpoints.push_back(std::move(hardpoint));
        }
        types_.emplace(id, std::move(ui_type));
    }
}

} // namespace eawr::presentation::godot_backend
