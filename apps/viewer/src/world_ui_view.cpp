#include "world_ui_view.hpp"

#include "ui/theme_builder.hpp"

#include "eawr/assets/assets.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/skirmish/start.hpp"

#include <godot_cpp/classes/image.hpp>
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

namespace eawr::presentation::godot_backend {

using namespace godot;

namespace {

using Vec3 = std::array<float, 3>;

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

[[nodiscard]] float to_float(const sim::math::Fixed value) {
    return static_cast<float>(static_cast<double>(value.raw()) / static_cast<double>(sim::math::Fixed::scale));
}

[[nodiscard]] Color colour(const ui::Rgb& rgb, const float alpha = 1.0F) {
    return {static_cast<float>(rgb[0]) / 255.0F, static_cast<float>(rgb[1]) / 255.0F, static_cast<float>(rgb[2]) / 255.0F,
            alpha};
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

[[nodiscard]] Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
[[nodiscard]] Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
[[nodiscard]] Vec3 normalized(const Vec3& value) {
    const float length = std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
    if (!(length > 1.0e-6F)) return {0.0F, 0.0F, 1.0F};
    return {value[0] / length, value[1] / length, value[2] / length};
}

// The camera's up axis in the source basis (+Z up); the render basis is source (x, z, -y).
[[nodiscard]] Vec3 camera_up_source(const camera::TacticalFrame& frame) {
    const Vec3 forward = normalized(sub(frame.target, frame.eye));
    const Vec3 right = normalized(cross(forward, frame.up));
    const Vec3 up = cross(right, forward);
    return {up[0], -up[2], up[1]};
}

// The world-space bounds of a unit's pick box (FoC: the model's world-space bounds).
struct Bounds final {
    Vec3 centre{};
    Vec3 half{};
};
[[nodiscard]] Bounds world_bounds(const ui::UnitBox& box) {
    Vec3 low{1.0e30F, 1.0e30F, 1.0e30F};
    Vec3 high{-1.0e30F, -1.0e30F, -1.0e30F};
    const auto& m = box.model_to_world;
    for (int corner = 0; corner < 8; ++corner) {
        const Vec3 local{(corner & 1) ? box.high[0] : box.low[0], (corner & 2) ? box.high[1] : box.low[1],
                         (corner & 4) ? box.high[2] : box.low[2]};
        for (std::size_t row = 0; row < 3; ++row) {
            const float world = m[row * 4] * local[0] + m[row * 4 + 1] * local[1] + m[row * 4 + 2] * local[2] + m[row * 4 + 3];
            low[row] = std::min(low[row], world);
            high[row] = std::max(high[row], world);
        }
    }
    Bounds bounds;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        bounds.centre[axis] = 0.5F * (low[axis] + high[axis]);
        bounds.half[axis] = 0.5F * (high[axis] - low[axis]);
    }
    return bounds;
}

[[nodiscard]] const sim::tactical::TacticalInstance* instance_of(const sim::tactical::TacticalSnapshot* snapshot,
                                                                 const sim::EntityId entity) {
    if (snapshot == nullptr) return nullptr;
    const auto instances = snapshot->instances();
    const auto found = std::lower_bound(instances.begin(), instances.end(), entity,
        [](const sim::tactical::TacticalInstance& instance, const sim::EntityId value) { return instance.entity_id < value; });
    return found != instances.end() && found->entity_id == entity ? &*found : nullptr;
}

// WU-12, WU-14: a one-pixel outlined bar: the black back over the full width, the
// filled part from the left, and a black outline around the back.
void bar(const RID item, const float centre_x, const float centre_y, const float width, const float height,
         const float fraction, const ui::Rgb& fill) {
    RenderingServer* rendering = RenderingServer::get_singleton();
    const float left = centre_x - width * 0.5F;
    const float top = centre_y - height * 0.5F;
    const Color black(0.0F, 0.0F, 0.0F, 1.0F);
    rendering->canvas_item_add_rect(item, Rect2(left - 1.0F, top - 1.0F, width + 2.0F, height + 2.0F), black);
    const float filled = width * std::clamp(fraction, 0.0F, 1.0F);
    if (filled > 0.0F) rendering->canvas_item_add_rect(item, Rect2(left, top, filled, height), colour(fill));
}

} // namespace

WorldUiView::WorldUiView(Node3D& host) : host_(&host) {
    if (host.get_world_3d().is_valid()) scenario_ = host.get_world_3d()->get_scenario();
}

WorldUiView::~WorldUiView() {
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (rendering == nullptr) return;
    for (const RID& circle : circles_) {
        if (circle.is_valid()) rendering->free_rid(circle);
    }
}

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
    // WU-01 (debug build; Factions.xml, ShadowBlobMaterials.xml): the local player's
    // faction names its space selection material; the ring is that material's second texture,
    // tinted by the faction's Selection_Blob_RGBA.
    const std::string faction = live.local_faction();
    std::string material = "Selection_Rebel_Space";
    if (auto object = catalog.resolve(faction)) {
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
        ui_type.name = type.id;
        ui_type.shielded = type.shielded;
        ui_type.layer_z = to_float(type.movement.layer_z_adjust.value_or(sim::math::Fixed{}));
        std::string ship_class;
        std::optional<int> bracket;
        if (auto object = catalog.resolve(type.id)) {
            const auto scale = type.scale_factor ? to_float(*type.scale_factor) : number(tag(object.value(), "Scale_Factor")).value_or(1.0F);
            if (const auto box = number(tag(object.value(), "Select_Box_Scale"))) {
                ui_type.circle_side = ui::selection_circle_side(*box, scale);
            }
            ship_class = lower(tag(object.value(), "Ship_Class"));
            if (const auto size = number(tag(object.value(), "GUI_Bracket_Size"))) bracket = static_cast<int>(*size);
            ui_type.bounds_scale = number(tag(object.value(), "GUI_Bounds_Scale")).value_or(1.0F);
            ui_type.hide_health_bar = yes(tag(object.value(), "GUI_Hide_Health_Bar"));
            ui_type.icon = lower(tag(object.value(), "Icon_Name"));
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

void WorldUiView::place_circles(const Frame& frame) {
    std::vector<std::pair<Vec3, float>> circles;
    if (ring_material_.is_valid()) {
        const auto& squadrons = frame.live->squadron_members();
        for (const sim::EntityId selected : frame.selection->units()) {
            // A selected squadron: every craft carries its own small circle (WU-03).
            const bool squadron = squadrons.contains(selected);
            for (const ui::BattleUnit& unit : *frame.units) {
                if (unit.entity != selected) continue;
                sim::tactical::TypeId type = unit.type;
                if (squadron) {
                    const auto view = frame.live->unit_frame(unit.part);
                    if (!view) continue;
                    type = view->type;
                }
                const auto found = types_.find(type);
                if (found == types_.end() || !found->second.circle_side) continue;
                Vec3 at = unit.position;
                if (squadron) {
                    const auto view = frame.live->unit_frame(unit.part);
                    at = {static_cast<float>(view->position[0]), static_cast<float>(view->position[1]),
                          static_cast<float>(view->position[2])};
                }
                circles.emplace_back(at, *found->second.circle_side);
            }
        }
    }
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (rendering == nullptr || !scenario_.is_valid() || ring_mesh_.is_null()) {
        circles.clear();
    } else {
        while (circles_.size() < circles.size()) {
            const RID instance = rendering->instance_create2(ring_mesh_->get_rid(), scenario_);
            if (!instance.is_valid()) break;
            rendering->instance_geometry_set_cast_shadows_setting(instance, RenderingServer::SHADOW_CASTING_SETTING_OFF);
            circles_.push_back(instance);
        }
        for (std::size_t index = 0; index < circles_.size(); ++index) {
            if (index >= circles.size()) {
                rendering->instance_set_visible(circles_[index], false);
                continue;
            }
            const auto& [at, side] = circles[index];
            // WU-02: flat at the unit's position (render basis: source x, z, -y), side x side.
            const Transform3D transform(Basis().scaled(Vector3(side, 1.0F, side)), Vector3(at[0], at[2], -at[1]));
            rendering->instance_set_transform(circles_[index], transform);
            rendering->instance_set_visible(circles_[index], true);
        }
        circles.resize(std::min(circles.size(), circles_.size()));
    }
    circles_drawn_ = circles.size();
    max_circles_ = std::max(max_circles_, circles_drawn_);
}

void WorldUiView::draw_bars(const Frame& frame, const RID canvas_item, const float ui_scale) {
    health_bars_ = 0;
    shield_bars_ = 0;
    bar_rows_.clear();
    if (!frame.camera) return;
    const auto snapshot = frame.live->battle_frame().latest;
    const auto& squadron_of = frame.live->squadron_of();
    // WU-16 (#502): the unit under the pointer (for a squadron, the craft), then the selection.
    // Hovering a squadron's icon adds none of its craft (its own bar, drawn with the icon, already
    // shows the squadron's health).
    struct Candidate final {
        const ui::BattleUnit* unit{};
        sim::EntityId entity{};
    };
    std::vector<Candidate> candidates;
    const auto add = [&candidates](const ui::BattleUnit& unit, const sim::EntityId entity) {
        if (std::none_of(candidates.begin(), candidates.end(), [entity](const Candidate& c) { return c.entity == entity; })) {
            candidates.push_back({&unit, entity});
        }
    };
    const auto& units = *frame.units;
    if (frame.hovered && *frame.hovered < units.size()) {
        const ui::BattleUnit& unit = units[*frame.hovered];
        add(unit, unit.part != sim::invalid_entity_id ? unit.part : unit.entity);
    }
    for (const ui::BattleUnit& unit : units) {
        // WU-16 (#502): only these reach the bar rules; an unhovered, unselected unit shows none
        // even at critical health, and hovering a squadron's icon adds none of its craft.
        const bool craft = unit.part != sim::invalid_entity_id;
        const bool selected = !craft && frame.selection->contains(unit.entity);
        if (ui::bar_candidate(false, selected)) add(unit, craft ? unit.part : unit.entity);
    }
    const Vec3 up = camera_up_source(*frame.camera);
    const Vec3 eye{frame.camera->eye[0], -frame.camera->eye[2], frame.camera->eye[1]};
    for (const Candidate& candidate : candidates) {
        const ui::BattleUnit& unit = *candidate.unit;
        const auto* instance = instance_of(snapshot.get(), candidate.entity);
        if (instance == nullptr || !instance->durability) continue;
        const sim::tactical::InstanceDurability& durability = *instance->durability;
        const auto type = types_.find(instance->type_id);
        if (type == types_.end()) continue;
        const bool craft = squadron_of.contains(candidate.entity);
        ui::BarUnit rules;
        rules.selected = !craft && frame.selection->contains(candidate.entity);
        rules.hovered = frame.hovered && *frame.hovered < units.size()
            && (units[*frame.hovered].part != sim::invalid_entity_id ? units[*frame.hovered].part : units[*frame.hovered].entity)
                == candidate.entity;
        rules.squadron_member = craft;
        const float max_hull = to_float(durability.max_hull);
        rules.has_health = max_hull > 0.0F;
        rules.health = rules.has_health ? to_float(durability.hull) / max_hull : 0.0F;
        rules.shielded = type->second.shielded && durability.max_shields && to_float(*durability.max_shields) > 0.0F;
        rules.hide_health_bar = type->second.hide_health_bar;
        const ui::BarVisibility shown = ui::bar_visibility(rules);
        if (!shown.health && !shown.shield) continue;
        // WU-18: the anchor over the unit's bounds along the camera's up axis. A craft of a
        // squadron uses its own pick volume.
        const Bounds bounds = world_bounds(unit.box);
        const float lift = ui::bar_anchor_lift(bounds.half, up, type->second.bounds_scale);
        const Vec3 anchor{bounds.centre[0] + up[0] * lift, bounds.centre[1] + up[1] * lift, bounds.centre[2] + up[2] * lift};
        const auto screen = frame.project(anchor);
        if (!screen) continue;
        const float distance = std::sqrt((unit.position[0] - eye[0]) * (unit.position[0] - eye[0])
            + (unit.position[1] - eye[1]) * (unit.position[1] - eye[1]) + (unit.position[2] - eye[2]) * (unit.position[2] - eye[2]));
        const float scale = ui::bar_scale(distance) * ui_scale;
        const float width = ui::bar_width(type->second.bar) * scale;
        const float height = ui::bar_height * scale;
        float y = (*screen)[1];
        std::string row = std::to_string(candidate.entity) + ":";
        if (shown.shield) {
            const float max_shields = to_float(*durability.max_shields);
            const float shields = durability.shields ? to_float(*durability.shields) / max_shields : 0.0F;
            bar(canvas_item, (*screen)[0], y, width, height, shields, ui::shield_bar_colour);
            y += height * ui::health_bar_spacing;
            ++shield_bars_;
            row += "s" + std::to_string(ui::bar_level(shields));
        }
        if (shown.health) {
            const int level = ui::bar_level(rules.health);
            bar(canvas_item, (*screen)[0], y, width, height, rules.health, ui::health_bar_colour(level));
            ++health_bars_;
            row += "h" + std::to_string(level);
        }
        bar_rows_.push_back(std::move(row));
    }
    max_health_bars_ = std::max(max_health_bars_, health_bars_);
}

void WorldUiView::update_grid(const Frame& frame) {
    // WU-25 (#457): the sim flies the dogfights over its combat cells (space-fighters FD-01 to
    // FD-03) and publishes the cell each squadron records; the joined ones hold their icons, in
    // ascending squadron ID (the sim's service order). The grid starts at the world origin.
    const auto snapshot = frame.live->battle_frame().latest;
    std::vector<ui::CombatGrid::Record> records;
    if (snapshot != nullptr) {
        for (const sim::tactical::SquadronTarget& row : snapshot->squadron_targets()) {
            if (row.recorded) records.push_back({row.squadron, ui::CombatCell{row.cell_x, row.cell_y}, row.joined});
        }
    }
    grid_.adopt(records);
}

void WorldUiView::draw_icons(const Frame& frame, const RID canvas_item, const float ui_scale) {
    icons_.clear();
    icon_rows_.clear();
    flag_rows_.clear();
    RenderingServer* rendering = RenderingServer::get_singleton();
    const auto snapshot = frame.live->battle_frame().latest;
    const sim::tactical::PlayerId local = frame.live->local_player();
    update_grid(frame);
    grid_icons_ = 0;
    for (const auto& [squadron, members] : frame.live->squadron_members()) {
        // WU-20: every squadron the local player sees has its icon; the pointer can grab it.
        std::vector<const ui::BattleUnit*> seen;
        for (const ui::BattleUnit& unit : *frame.units) {
            if (unit.entity == squadron && unit.part != sim::invalid_entity_id) seen.push_back(&unit);
        }
        if (seen.empty()) continue;
        Vec3 at{};
        if (const auto container = frame.live->unit_frame(squadron)) {
            at = {static_cast<float>(container->position[0]), static_cast<float>(container->position[1]),
                  static_cast<float>(container->position[2])};
        } else {
            for (const ui::BattleUnit* unit : seen) {
                for (std::size_t axis = 0; axis < 3; ++axis) at[axis] += unit->position[axis] / static_cast<float>(seen.size());
            }
        }
        auto screen = frame.project(at);
        // WU-26: a dogfighting squadron's icon takes its slot in its combat cell's grid, laid out
        // from the cell point's screen position (the point at the squadron's height).
        std::string gridded;
        if (const auto cell = grid_.cell_of(squadron)) {
            const auto point = ui::combat_cell_point(*cell, {0.0F, 0.0F});
            const auto occupied = std::find_if(grid_.cells().begin(), grid_.cells().end(),
                [&](const ui::CombatGrid::Occupied& entry) { return entry.cell == *cell; });
            const auto place = std::find(occupied->squadrons.begin(), occupied->squadrons.end(), squadron);
            // WU-26: one point per cell, at the Layer_Z_Adjust of the craft type of the cell's first
            // squadron: a constant, so the grid holds still while the fighters pitch (#564). The type
            // is its first live craft's (#635), so a dead first craft doesn't drop the grid to 0.
            float height = 0.0F;
            const auto first_members = frame.live->squadron_members().find(occupied->squadrons.front());
            if (first_members != frame.live->squadron_members().end()) {
                for (const sim::EntityId member : first_members->second) {
                    const auto* craft = instance_of(snapshot.get(), member);
                    if (craft == nullptr) continue;
                    if (const auto type = types_.find(craft->type_id); type != types_.end()) height = type->second.layer_z;
                    break;
                }
            }
            if (const auto base = frame.project({point[0], point[1], height})) {
                const auto slot = ui::combat_grid_slot(static_cast<std::size_t>(place - occupied->squadrons.begin()),
                                                       occupied->squadrons.size());
                screen = std::array<float, 2>{(*base)[0] + slot[0] * ui_scale, (*base)[1] + slot[1] * ui_scale};
                gridded = ":grid" + std::to_string(cell->x) + "," + std::to_string(cell->y);
                ++grid_icons_;
            }
        }
        if (!screen) continue;
        // WU-24 (#500): FoC's gripper always asks CommandBar to add this share of the screen
        // height to the icon's Y after projecting, so the icon hovers below the squadron instead
        // of covering it.
        (*screen)[1] += ui::squadron_icon_screen_offset(frame.viewport[1]);
        // WU-22: the squadron's health over its craft's (lost craft count as empty).
        float health = 0.0F;
        float max_health = 0.0F;
        const auto* container = instance_of(snapshot.get(), squadron);
        for (const sim::EntityId member : members) {
            const auto* craft = instance_of(snapshot.get(), member);
            const auto profile_type = frame.live->unit_frame(member);
            if (craft != nullptr && craft->durability) {
                health += to_float(craft->durability->hull);
                max_health += to_float(craft->durability->max_hull);
            } else if (const sim::tactical::DurabilityTable* table = frame.live->durability()) {
                // A lost craft: its type is the squadron's member type in roster order.
                static_cast<void>(profile_type);
                for (const ui::BattleUnit* unit : seen) {
                    const auto* alive = instance_of(snapshot.get(), unit->part);
                    if (alive == nullptr) continue;
                    if (const auto* profile = table->find(alive->type_id)) max_health += to_float(profile->max_hull);
                    break;
                }
            }
        }
        const float fraction = ui::squadron_health(health, max_health);
        const int level = ui::bar_level(fraction);
        const bool selected = frame.selection->contains(squadron);
        const bool hovered = frame.hovered_icon && *frame.hovered_icon == squadron;
        const float side = ui::squadron_frame_side * ui_scale;
        const float x = (*screen)[0];
        const float y = (*screen)[1];
        const Rect2 frame_rect(x - side * 0.5F, y - side * 0.5F, side, side);
        // WU-21: the gripper frame tinted by the owner's colour, or the yellow select frame while
        // selected or under the pointer; the squadron's icon inside it.
        const sim::tactical::PlayerId owner = container != nullptr ? container->owner : local;
        const ui::Rgb tint = frame.live->player_colour(owner).value_or(ui::Rgb{255, 255, 255});
        const std::string frame_name = selected || hovered ? "i_button_unit_frame_gripper_select.tga"
                                                           : "i_button_unit_frame_gripper.tga";
        if (atlas_.is_valid()) {
            if (const auto region = atlas_region(frame_name)) {
                rendering->canvas_item_add_texture_rect_region(canvas_item, frame_rect, atlas_->get_rid(), *region,
                                                               selected || hovered ? Color(1, 1, 1, 1) : colour(tint));
            }
            const auto type = types_.find(container != nullptr ? container->type_id : sim::tactical::TypeId{});
            if (type != types_.end() && !type->second.icon.empty()) {
                if (const auto region = atlas_region(type->second.icon)) {
                    const float icon = 50.0F * 0.6F * ui_scale;
                    rendering->canvas_item_add_texture_rect_region(canvas_item,
                        Rect2(x - icon * 0.5F, y - icon * 0.5F, icon, icon), atlas_->get_rid(), *region);
                }
            }
        }
        const float bar_scale = ui::squadron_bar_scale * ui_scale;
        bar(canvas_item, x, y + ui::squadron_bar_offset * ui_scale, ui::bar_width(ui::BarSize::small) * bar_scale,
            ui::bar_height * bar_scale, fraction, ui::health_bar_colour(level));
        // WU-37, WU-38 (#632): a launched squadron's icon carries FoC's small white flag, on an
        // ally's icon only.
        if (atlas_.is_valid() && frame.live->squadron_launched(squadron) && frame.live->is_ally_of_local(owner)) {
            if (const auto region = atlas_region("i_garrison_flag.tga")) {
                const float flag_w = ui::garrison_flag_width * ui_scale;
                const float flag_h = ui::garrison_flag_height * ui_scale;
                const float flag_x = x + ui::garrison_flag_offset_x * ui_scale - flag_w * 0.5F;
                const float flag_y = y + ui::garrison_flag_offset_y * ui_scale - flag_h * 0.5F;
                rendering->canvas_item_add_texture_rect_region(canvas_item, Rect2(flag_x, flag_y, flag_w, flag_h),
                                                               atlas_->get_rid(), *region);
                flag_rows_.push_back(std::to_string(squadron) + ":x=" + std::to_string(std::lround(flag_x))
                                     + ",y=" + std::to_string(std::lround(flag_y)));
            }
        }
        icons_.push_back({squadron, container != nullptr ? container->type_id : sim::tactical::TypeId{}, owner == local,
                          frame_rect.position.x, frame_rect.position.y, frame_rect.position.x + side,
                          frame_rect.position.y + side});
        // The icon's own screen-space Y (WU-24's offset applied, gridded or not): evidence for a
        // test that the offset reaches a dogfighting squadron's icon the same as any other's.
        icon_rows_.push_back(std::to_string(squadron) + ":" + (selected ? "selected" : hovered ? "hovered" : "normal") + ":"
                             + std::to_string(level) + ":y=" + std::to_string(std::lround(y)) + gridded);
    }
    max_grid_icons_ = std::max(max_grid_icons_, grid_icons_);
    for (const std::string& row : flag_rows_) flags_seen_.insert(row.substr(0, row.find(':')));
}

void WorldUiView::draw_reticles(const Frame& frame, const RID canvas_item) {
    reticles_drawn_ = 0;
    tracked_reticles_ = 0;
    if (!frame.hovered || *frame.hovered >= frame.units->size()) return;
    const ui::BattleUnit& unit = (*frame.units)[*frame.hovered];
    if (unit.part != sim::invalid_entity_id) return;  // a squadron has no hardpoints
    const auto snapshot = frame.live->battle_frame().latest;
    const auto* instance = instance_of(snapshot.get(), unit.entity);
    const auto view = frame.live->unit_frame(unit.entity);
    const auto type = types_.find(unit.type);
    if (instance == nullptr || !instance->durability || !view || type == types_.end()) return;
    RenderingServer* rendering = RenderingServer::get_singleton();
    const Vec3 position{static_cast<float>(view->position[0]), static_cast<float>(view->position[1]),
                        static_cast<float>(view->position[2])};
    struct Placed final {
        std::size_t index{};
        Rect2 rect;
    };
    std::vector<Placed> placed;
    const auto& statuses = instance->durability->hardpoints;
    for (std::size_t index = 0; index < type->second.hardpoints.size() && index < statuses.size(); ++index) {
        const HardpointUi& hardpoint = type->second.hardpoints[index];
        // WU-30: every targetable hardpoint that still stands.
        if (!hardpoint.targetable || hardpoint.texture.empty()
            || statuses[index].state == sim::tactical::HardpointState::destroyed) {
            continue;
        }
        // WU-34: on the attachment point as the ship is drawn, bank and pitch included.
        const Vec3 world = ui::hardpoint_reticle_anchor(position, static_cast<float>(view->yaw_degrees),
                                                        static_cast<float>(view->pitch_degrees),
                                                        static_cast<float>(view->roll_degrees), hardpoint.local);
        const auto screen = frame.project(world);
        if (!screen) continue;
        // WU-31: a fixed share of the screen, whatever the camera distance.
        const ui::ReticleRect rect = ui::hardpoint_reticle_rect(*screen, frame.viewport);
        placed.push_back({index, Rect2(rect.x, rect.y, rect.width, rect.height)});
    }
    // WU-32: the hardpoint under the pointer shows the tracked art (the last reticle drawn over
    // the pointer, as the later one is on top).
    std::optional<std::size_t> tracked;
    if (frame.pointer) {
        for (const Placed& entry : placed) {
            if (entry.rect.has_point(Vector2((*frame.pointer)[0], (*frame.pointer)[1]))) tracked = entry.index;
        }
    }
    for (const Placed& entry : placed) {
        const HardpointUi& hardpoint = type->second.hardpoints[entry.index];
        const bool is_tracked = tracked && *tracked == entry.index;
        const auto texture = reticles_.find(is_tracked ? hardpoint.texture + "_tracked" : hardpoint.texture);
        if (texture == reticles_.end() || texture->second.is_null()) continue;
        const float health = hardpoint.max_health > 0.0F ? to_float(statuses[entry.index].health) / hardpoint.max_health : 1.0F;
        rendering->canvas_item_add_texture_rect(canvas_item, entry.rect, texture->second->get_rid(), false,
                                                colour(ui::hardpoint_reticle_tint(health, false)));
        ++reticles_drawn_;
        if (is_tracked) ++tracked_reticles_;
        reticle_size_ = {entry.rect.size.x, entry.rect.size.y};
    }
    max_reticles_ = std::max(max_reticles_, reticles_drawn_);
}

void WorldUiView::draw(const Frame& frame, const RID canvas_item) {
    if (frame.units == nullptr || frame.selection == nullptr || frame.live == nullptr || !frame.project) return;
    place_circles(frame);
    // The UI reference scale (layout.hpp UI-L1): screen height over 768 at 4:3 and wider.
    const float ui_scale = frame.viewport[1] > 0.0F
        ? std::min(frame.viewport[1] / 768.0F, frame.viewport[0] / 1024.0F * (frame.viewport[0] / frame.viewport[1] >= 4.0F / 3.0F ? 1.0e9F : 1.0F))
        : 1.0F;
    if (frame.brackets) {
        draw_bars(frame, canvas_item, ui_scale);
    } else {
        health_bars_ = 0;
        shield_bars_ = 0;
        bar_rows_.clear();
    }
    draw_reticles(frame, canvas_item);
    draw_icons(frame, canvas_item, ui_scale);
}

std::optional<sim::EntityId> WorldUiView::icon_at(const std::array<float, 2> point) const {
    std::optional<sim::EntityId> found;
    for (const Icon& icon : icons_) {
        if (point[0] >= icon.min_x && point[0] <= icon.max_x && point[1] >= icon.min_y && point[1] <= icon.max_y) {
            found = icon.squadron;
        }
    }
    return found;
}

std::optional<std::array<float, 2>> WorldUiView::icon_centre(const sim::EntityId squadron) const {
    for (const Icon& icon : icons_) {
        if (icon.squadron == squadron) return std::array<float, 2>{0.5F * (icon.min_x + icon.max_x), 0.5F * (icon.min_y + icon.max_y)};
    }
    return std::nullopt;
}

std::vector<ui::BattleUnit> WorldUiView::icon_units() const {
    std::vector<ui::BattleUnit> units;
    units.reserve(icons_.size());
    for (const Icon& icon : icons_) {
        ui::BattleUnit unit;
        unit.entity = icon.squadron;
        unit.type = icon.type;
        unit.own = icon.own;
        unit.screen = std::array<float, 2>{0.5F * (icon.min_x + icon.max_x), 0.5F * (icon.min_y + icon.max_y)};
        units.push_back(unit);
    }
    return units;
}

void WorldUiView::write_report(std::ostream& output) const {
    const auto list = [&output](const std::vector<std::string>& rows) {
        output << "[";
        for (std::size_t index = 0; index < rows.size(); ++index) output << (index ? ", " : "") << "\"" << rows[index] << "\"";
        output << "]";
    };
    output << "  \"world_ui\": {\"ring\": \"" << ring_source_ << "\", \"ring_loaded\": " << (ring_.is_valid() ? "true" : "false")
           << ", \"atlas_loaded\": " << (atlas_.is_valid() ? "true" : "false") << ", \"types\": " << types_.size()
           << ", \"circles\": " << circles_drawn_ << ", \"max_circles\": " << max_circles_
           << ", \"health_bars\": " << health_bars_ << ", \"shield_bars\": " << shield_bars_
           << ", \"max_health_bars\": " << max_health_bars_ << ", \"reticles\": " << reticles_drawn_
           << ", \"tracked_reticles\": " << tracked_reticles_ << ", \"max_reticles\": " << max_reticles_
           << ", \"reticle_size\": [" << reticle_size_[0] << ", " << reticle_size_[1] << "]"
           << ", \"icons\": " << icons_.size() << ", \"grid_icons\": " << grid_icons_
           << ", \"max_grid_icons\": " << max_grid_icons_ << ", \"combat_cells\": " << grid_.cells().size()
           << ", \"bar_rows\": ";
    list(bar_rows_);
    output << ", \"icon_rows\": ";
    list(icon_rows_);
    // #632: the flags drawn last frame (squadron:x=,y= of the flag's corner), and every squadron that ever showed one.
    output << ", \"flag_rows\": ";
    list(flag_rows_);
    output << ", \"flags_seen\": ";
    list(std::vector<std::string>(flags_seen_.begin(), flags_seen_.end()));
    output << ", \"unresolved\": ";
    std::vector<std::string> unresolved;
    for (const std::string& row : unresolved_) {
        std::string clean;
        for (const char character : row) clean += character == '"' || character == '\\' ? '\'' : character;
        unresolved.push_back(clean);
    }
    list(unresolved);
    output << "},\n";
}

} // namespace eawr::presentation::godot_backend
