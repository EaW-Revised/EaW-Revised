#include "hero_tray_view.hpp"

#include "eawr/presentation/ui/command_bar_placement.hpp"
#include "eawr/presentation/ui/unit_cards.hpp"

#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>

#include <algorithm>
#include <cmath>
#include <sstream>
#include <utility>

using namespace godot;

namespace eawr::presentation::godot_backend {
namespace {

namespace model = presentation::ui;

[[nodiscard]] Rect2 rect2(const model::PixelRect& rect) {
    return Rect2(static_cast<float>(rect.x), static_cast<float>(rect.y), static_cast<float>(rect.width),
                 static_cast<float>(rect.height));
}

// A quad of texel size times `scale` centred on `centre`, in shell units.
[[nodiscard]] data::ui::ReferenceRect centred(const assets::Vec2f centre, const Vector2 texels, const float scale) {
    const float width = texels.x * scale;
    const float height = texels.y * scale;
    return {centre.x - width / 2.0F, centre.y - height / 2.0F, width, height};
}

// WSU-60: bars keep Pixel_Align's default; Godot uses pixel edges, so the half-texel adjustment becomes
// an integral origin. The extent is rounded independently of the position to keep equal heights.
[[nodiscard]] Rect2 aligned_bar(Rect2 rect) {
    rect.position.x = std::floor(rect.position.x);
    rect.position.y = std::floor(rect.position.y);
    rect.size.x = std::round(rect.size.x);
    rect.size.y = std::round(rect.size.y);
    return rect;
}

[[nodiscard]] std::string json_string(const std::string_view value) {
    std::string out{"\""};
    for (const char character : value) {
        if (character == '"' || character == '\\') out += '\\';
        out += static_cast<unsigned char>(character) < 0x20U ? ' ' : character;
    }
    return out + "\"";
}

[[nodiscard]] std::string rect_json(const Rect2& rect) {
    std::ostringstream output;
    output << "[" << rect.position.x << ", " << rect.position.y << ", " << rect.size.x << ", " << rect.size.y << "]";
    return output.str();
}

constexpr const char* tray_names[2] = {"friendly", "hostile"};

} // namespace

EawrHeroTray::EawrHeroTray() {
    set_mouse_filter(MOUSE_FILTER_STOP);
    set_focus_mode(FOCUS_NONE);
    set_anchors_and_offsets_preset(PRESET_FULL_RECT);
    // The frame art's UVs repeat like the main shell's.
    set_texture_repeat(TEXTURE_REPEAT_ENABLED);
    set_texture_filter(TEXTURE_FILTER_LINEAR);
}

EawrHeroTray::~EawrHeroTray() = default;

void EawrHeroTray::setup(Setup setup) {
    setup_ = std::move(setup);
    icons_ = {};
    hovered_.reset();
    laid_out_ = Vector2(-1.0F, -1.0F);
    set_process(true);
    queue_redraw();
}

void EawrHeroTray::set_levels(const Tray tray, std::vector<Icon>& icons) const {
    const auto& slots = frame(tray).slots;
    for (std::size_t slot = 0; slot < icons.size(); ++slot) {
        const std::int32_t levels = slot < slots.size() && slots[slot].health ? slots[slot].health->max_level : 10;
        icons[slot].health_level = model::health_level(icons[slot].health, levels);
    }
}

void EawrHeroTray::show(std::vector<Icon> friendly, std::vector<Icon> hostile) {
    ++shows_;
    set_levels(Tray::friendly, friendly);
    set_levels(Tray::hostile, hostile);
    if (friendly == icons_[0] && hostile == icons_[1]) return;
    ++changes_;
    icons_[0] = std::move(friendly);
    icons_[1] = std::move(hostile);
    if (hovered_ && hovered_->second >= icons_[static_cast<std::size_t>(hovered_->first)].size()) hovered_.reset();
    queue_redraw();
}

const model::HudHeroFrame& EawrHeroTray::frame(const Tray tray) const noexcept {
    const bool friendly = tray == Tray::friendly;
    const bool good = (setup_.friendly_side == model::HeroSide::good) == friendly;
    return setup_.frames[good ? 0 : 1];
}

model::ShellPlacement EawrHeroTray::frame_placement(const Tray tray) const {
    const model::HudHeroFrame& art = frame(tray);
    const model::ReferenceSpace space = setup_.space ? setup_.space() : model::ReferenceSpace{};
    // The frame is a manual shell around its centred origin: its caller supplies the translation, which
    // puts the first slot's bone at its distance from the top and from the safe area's right (friendly) or
    // left (hostile) edge (retail capture 10-20, WHE-70: the local team's heroes sit top right, the
    // opposing team's top left); the art and the other slots follow the bones.
    model::CommandBarPlacement policy = art.placement;
    model::ReferencePoint& translation = policy.manual_translation;
    if (!art.slots.empty()) {
        const assets::Vec2f first = art.slots.front().button.origin;
        const double left_edge = space.safe_left - space.width / 2.0;
        const double right_edge = left_edge + space.safe_width;
        translation.x = (tray == Tray::hostile ? left_edge + setup_.placement.edge_inset
                                               : right_edge - setup_.placement.edge_inset) - first.x;
        translation.y = space.height / 2.0 - setup_.placement.top_inset - first.y;
    }
    return model::place_command_bar(space, 0.0, policy);
}

std::size_t EawrHeroTray::variant(const Tray tray, const std::size_t count) const noexcept {
    const auto& meshes = frame(tray).meshes;
    // The art carries one variant per icon of the first row: ALT0 for one icon up to the last for six or more.
    return meshes.empty() || count == 0 ? 0 : std::min(count - 1, meshes.size() - 1);
}

std::optional<Rect2> EawrHeroTray::slot_rect(const Tray tray, const std::size_t slot) const {
    const auto& icons = icons_[static_cast<std::size_t>(tray)];
    const auto& slots = frame(tray).slots;
    if (slot >= icons.size() || slot >= slots.size() || !setup_.space) return std::nullopt;
    return rect2(model::shell_to_screen(slots[slot].button.rect, frame_placement(tray)));
}

std::optional<std::pair<EawrHeroTray::Tray, std::size_t>> EawrHeroTray::slot_at(const Vector2& point) const {
    for (const Tray tray : {Tray::friendly, Tray::hostile}) {
        for (std::size_t slot = 0; slot < icons_[static_cast<std::size_t>(tray)].size(); ++slot) {
            const auto rect = slot_rect(tray, slot);
            if (rect && rect->has_point(point)) return std::pair{tray, slot};
        }
    }
    return std::nullopt;
}

bool EawrHeroTray::_has_point(const Vector2& point) const { return slot_at(point).has_value(); }

void EawrHeroTray::_gui_input(const Ref<InputEvent>& event) {
    if (const auto* motion = Object::cast_to<InputEventMouseMotion>(event.ptr())) {
        const auto slot = slot_at(motion->get_position());
        if (slot != hovered_) {
            hovered_ = slot;
            queue_redraw();
        }
        return;
    }
    const auto* button = Object::cast_to<InputEventMouseButton>(event.ptr());
    if (button == nullptr) return;
    const auto index = button->get_button_index();
    // An icon takes the click so it never reaches the world behind it.
    if ((index == MOUSE_BUTTON_LEFT || index == MOUSE_BUTTON_RIGHT) && slot_at(button->get_position())) accept_event();
}

void EawrHeroTray::_notification(const int what) {
    if (what == NOTIFICATION_RESIZED) {
        queue_redraw();
    } else if (what == NOTIFICATION_MOUSE_EXIT) {
        if (hovered_) {
            hovered_.reset();
            queue_redraw();
        }
    } else if (what == NOTIFICATION_PROCESS) {
        if (get_size() != laid_out_) {
            laid_out_ = get_size();
            queue_redraw();
        }
    }
}

void EawrHeroTray::draw_bar(const model::HudBar& bar, const model::ShellPlacement& placement, const std::int32_t level,
                            const double percent) {
    if (!setup_.texture) return;
    const auto screen = [&](const data::ui::ReferenceRect& rect) { return rect2(model::shell_to_screen(rect, placement)); };
    const assets::Vec2f centre{bar.origin.x + bar.offset.x, bar.origin.y + bar.offset.y};
    if (!bar.back.empty()) {
        const Ref<Texture2D> back = setup_.texture(bar.back[std::min<std::size_t>(bar.back.size() - 1, bar.max_level)]);
        if (back.is_valid()) draw_texture_rect(back, aligned_bar(screen(centred(centre, back->get_size(), bar.scale))), false);
    }
    if (bar.overlay.empty()) return;
    const std::size_t index = std::min<std::size_t>(bar.overlay.size() - 1, static_cast<std::size_t>(std::max(0, level)));
    const Ref<Texture2D> overlay = setup_.texture(bar.overlay[index]);
    if (overlay.is_null()) return;
    Rect2 quad = aligned_bar(screen(centred(centre, overlay->get_size(), bar.scale)));
    // A smooth bar's overlay narrows to the percent and keeps its left edge (CARD-4).
    if (bar.smooth) quad.size.x *= static_cast<float>(std::clamp(percent, 0.0, 1.0));
    if (quad.size.x <= 0.0F || quad.size.y <= 0.0F) return;
    draw_texture_rect(overlay, quad, false);
}

void EawrHeroTray::draw_frame(const Tray tray) {
    const auto& icons = icons_[static_cast<std::size_t>(tray)];
    if (icons.empty()) return;
    const model::HudHeroFrame& art = frame(tray);
    const model::ShellPlacement placement = frame_placement(tray);
    if (setup_.texture && !art.meshes.empty()) {
        for (const model::HudShellMesh& mesh : art.meshes[variant(tray, icons.size())]) {
            const Ref<Texture2D> texture = setup_.texture(mesh.texture);
            if (texture.is_null()) continue;
            for (const data::ui::ShellTriangle& triangle : mesh.triangles) {
                PackedVector2Array points;
                PackedVector2Array uvs;
                PackedColorArray colours;
                for (const auto& vertex : triangle.vertices) {
                    const model::ReferencePoint point = model::shell_point_to_screen(vertex.position.x, vertex.position.y, placement);
                    points.push_back(Vector2(static_cast<float>(point.x), static_cast<float>(point.y)));
                    uvs.push_back(Vector2(vertex.uv.x, vertex.uv.y));
                    colours.push_back(Color(1, 1, 1, 1));
                }
                draw_polygon(points, colours, uvs, texture);
            }
        }
    }
    for (std::size_t slot = 0; slot < icons.size() && slot < art.slots.size(); ++slot) {
        const model::HudHeroSlot& look = art.slots[slot];
        const Icon& icon = icons[slot];
        const auto quad = [&](const Ref<Texture2D>& texture) {
            const Vector2 size = texture->get_size();
            return rect2(model::shell_to_screen(model::button_quad(look.button, size.x, size.y), placement));
        };
        // HCB-12g: the Blank_Texture_Name ring backs the icon, then the portrait, then the hover art.
        if (setup_.texture && !look.button.blank.empty()) {
            const Ref<Texture2D> ring = setup_.texture(look.button.blank);
            if (ring.is_valid()) draw_texture_rect(ring, quad(ring), false);
        }
        if (setup_.texture && setup_.icon) {
            std::string name = setup_.icon(icon.type);
            if (name.empty()) name = "i_button_temporary.tga";
            const Ref<Texture2D> portrait = setup_.texture(name);
            if (portrait.is_valid()) draw_texture_rect(portrait, quad(portrait), false);
        }
        const bool hovered = hovered_ && hovered_->first == tray && hovered_->second == slot;
        if ((hovered || icon.automatic) && setup_.texture && !look.button.mouse_over.empty()) {
            const Ref<Texture2D> over = setup_.texture(look.button.mouse_over);
            if (over.is_valid()) draw_texture_rect(over, quad(over), false);
        }
        if (look.health) {
            const std::int32_t levels = std::max(1, look.health->max_level);
            draw_bar(*look.health, placement, icon.health_level, static_cast<double>(icon.health_level) / levels);
        }
    }
}

void EawrHeroTray::_draw() {
    if (!setup_.space) return;
    ++draws_;
    draw_frame(Tray::friendly);
    draw_frame(Tray::hostile);
}

std::string EawrHeroTray::report_json() const {
    std::ostringstream output;
    output << "{\"friendly\": " << icons_[0].size() << ", \"hostile\": " << icons_[1].size()
           << ", \"shows\": " << shows_ << ", \"changes\": " << changes_ << ", \"draws\": " << draws_
           << ", \"friendly_side\": " << json_string(model::to_string(setup_.friendly_side)) << ", \"frames\": [";
    for (std::size_t side = 0; side < setup_.frames.size(); ++side) {
        const model::HudHeroFrame& art = setup_.frames[side];
        output << (side == 0 ? "" : ", ") << "{\"side\": " << json_string(model::to_string(art.side))
               << ", \"model\": " << json_string(art.model) << ", \"variants\": " << art.meshes.size()
               << ", \"slots\": " << art.slots.size() << "}";
    }
    output << "], \"icons\": [";
    bool first = true;
    for (const Tray tray : {Tray::friendly, Tray::hostile}) {
        const auto& icons = icons_[static_cast<std::size_t>(tray)];
        for (std::size_t slot = 0; slot < icons.size(); ++slot) {
            const Icon& icon = icons[slot];
            output << (first ? "" : ", ") << "{\"tray\": \"" << tray_names[static_cast<std::size_t>(tray)] << "\", \"slot\": " << slot
                   << ", \"type\": " << json_string(icon.type) << ", \"health_level\": " << icon.health_level
                   << ", \"automatic\": " << (icon.automatic ? "true" : "false") << ", \"hero\": " << icon.hero
                   << ", \"represented\": " << icon.represented;
            if (const auto rect = slot_rect(tray, slot)) output << ", \"rect\": " << rect_json(*rect);
            output << "}";
            first = false;
        }
    }
    output << "]}";
    return output.str();
}

} // namespace eawr::presentation::godot_backend
