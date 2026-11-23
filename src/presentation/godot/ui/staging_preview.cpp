#include "ui/staging_preview.hpp"
#include "ui/kit.hpp"
#include "ui/theme_builder.hpp"
#include "eawr/data/ui/dialog_catalog.hpp"
#include "eawr/presentation/ui/skirmish_setup.hpp"
#include "eawr/skirmish/setup.hpp"
#include "eawr/vfs/vfs.hpp"

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/texture_rect.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string_view>
#include <utility>

namespace eawr::presentation::godot_backend {
using namespace godot;
namespace model = presentation::ui;
namespace {
std::optional<assets::DeclaredExtents> preview_extents(const vfs::Vfs& filesystem,
                                                     const std::string& path) {
    const auto record = filesystem.stat(path);
    // Same pre-allocation TED budget as the full map loader, including the fallback read.
    if (!record || record.value().size > 512ULL * 1024ULL * 1024ULL) return std::nullopt;
    auto source = assets::source_from(record.value());
    source.stored_size = 0; // A header prefix intentionally does not cover the full stored asset.
    // Root/header only: preview decoration never needs the placements or terrain.
    if (auto bytes = filesystem.open_prefix(path, 64U * 1024U)) {
        if (auto header = assets::load_map_header(bytes.value(), source)) return header.value().declared_extents;
    }
    if (auto bytes = filesystem.open(path)) {
        if (auto header = assets::load_map_header(bytes.value(), source)) return header.value().declared_extents;
    }
    return std::nullopt;
}
} // namespace

std::string preview_font_role(const std::string& statement, const std::string& class_name) {
    data::ui::DialogControl control;
    control.statement = statement;
    control.class_name = class_name;
    return std::string(data::ui::to_string(data::ui::font_role(control)));
}

void StagingPreview::clear() {
    for (const auto& marker : markers_) {
        marker.backing->hide();
        marker.backing->queue_free();
    }
    markers_.clear();
    icons_.clear();
}

void StagingPreview::rebuild(TextureRect& preview, const skirmish::SetupMap& map,
                            const vfs::Vfs& filesystem,
                            const std::span<const skirmish::SetupFaction> factions,
                            const assets::MegaTextureAtlas* atlas, UiTextures* textures,
                            const std::string_view font_role,
                            const std::string_view variation) {
    clear();
    if (map.metadata.new_markers != true || !map.metadata.start_positions) return;
    const auto extents = preview_extents(filesystem, map.path);
    if (!extents || !std::isfinite(extents->first) || !std::isfinite(extents->second)
        || extents->first <= 0 || extents->second <= 0) return;
    if (map.domain == sim::tactical::BattleDomain::space && atlas && textures) {
        for (const auto& faction : factions) {
            if (faction.preview_icon.empty()) continue;
            const auto slot = model::resolve_ui_texture(faction.preview_icon, &atlas->directory, {});
            // WSS-63 resolves an atlas entry; a missing icon leaves the start number.
            if (slot.origin != model::TextureOrigin::atlas) continue;
            if (auto icon = textures->texture(slot, 1.0, 1.0); icon.is_valid())
                icons_.emplace(faction.name, std::move(icon));
        }
    }
    const auto size = preview.get_size();
    for (std::size_t i = 0; i < map.metadata.start_positions->size(); ++i) {
        const auto& point = map.metadata.start_positions->at(i);
        if (!std::isfinite(point.x) || !std::isfinite(point.y)) continue;
        const float x = std::clamp(0.5F + point.x / extents->first, 0.0F, 1.0F);
        const float y = std::clamp(0.5F - point.y / extents->second, 0.0F, 1.0F);
        auto* backing = memnew(Control); // No backdrop: the emblem sits directly on the preview.
        backing->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        backing->set_position(Vector2(size.x * (0.1F + x * 0.8F) - 10,
            size.y * (0.1F + y * 0.8F) - 10));
        backing->set_size(Vector2(20, 20));
        preview.add_child(backing);
        auto* emblem = memnew(TextureRect);
        emblem->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        emblem->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
        backing->add_child(emblem);
        // The number takes the preview gadget's font role (with that role's outline and emboss), in
        // white, and its measured text block is centred on the emblem's centre both ways, per the
        // debug-build walk.
        auto* number = memnew(EawrUiLabel);
        number->set_theme_type_variation(String::utf8(variation.data(), static_cast<int64_t>(variation.size())));
        number->set_role(String::utf8(font_role.data(), static_cast<int64_t>(font_role.size())));
        number->set_text(String::num_uint64(i + 1));
        number->set_alignment(HORIZONTAL_ALIGNMENT_CENTER);
        number->set_wrap(false);
        number->set_centre_vertically(true);
        number->set_ink(Color(1, 1, 1, 1));
        number->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        number->set_size(Vector2(20, 20));
        backing->add_child(number); // Draw the number after the white, untinted emblem.
        markers_.push_back({static_cast<std::uint32_t>(i), backing, emblem, number});
    }
}

void StagingPreview::update(const std::span<const StagingPreviewSeat> occupied) {
    std::vector<model::SetupRow> rows;
    rows.reserve(occupied.size());
    for (const auto& seat : occupied) rows.push_back({seat.team, seat.faction});
    for (auto& marker : markers_) {
        const auto faction = model::setup_preview_faction(rows, marker.team);
        const auto icon = icons_.find(faction);
        const Ref<Texture2D> texture = icon != icons_.end() ? icon->second : Ref<Texture2D>();
        marker.emblem->set_texture(texture);
        marker.emblem->set_visible(texture.is_valid());
        if (texture.is_valid()) {
            // WSS-63 uses the entry's native screen-pixel dimensions even when the
            // dialog's parent transform scales its reference coordinate space.
            const auto scale = marker.backing->get_global_transform_with_canvas().get_scale();
            const Vector2 size(static_cast<float>(texture->get_width()) / (scale.x != 0 ? std::abs(scale.x) : 1.0F),
                static_cast<float>(texture->get_height()) / (scale.y != 0 ? std::abs(scale.y) : 1.0F));
            marker.emblem->set_size(size);
            marker.emblem->set_position((Vector2(20, 20) - size) * 0.5F);
        }
    }
}

std::size_t StagingPreview::emblem_count() const noexcept {
    return static_cast<std::size_t>(std::count_if(markers_.begin(), markers_.end(),
        [](const auto& marker) { return marker.emblem->is_visible(); }));
}
std::vector<Rect2> StagingPreview::emblem_rects() const {
    std::vector<Rect2> rects;
    for (const auto& marker : markers_) {
        if (!marker.emblem->is_visible()) continue;
        const auto transform = marker.emblem->get_global_transform_with_canvas();
        rects.emplace_back(transform.get_origin(), marker.emblem->get_size() * transform.get_scale());
    }
    return rects;
}

namespace {
// The text block the label draws: the role's measured string width by its line height.
Vector2 number_block(const Control& label, const String& text, const String& role) {
    const CharString utf8 = role.utf8();
    const KitTextStyle style = kit_text_style(label, std::string_view(utf8.get_data(), utf8.length()));
    if (style.font.is_null() || style.size <= 0) return {};
    const float line = style.cell_ascent > 0 ? static_cast<float>(style.cell_ascent + style.cell_descent)
                                             : style.font->get_height(style.size);
    return Vector2(style.font->get_string_size(text, HORIZONTAL_ALIGNMENT_LEFT, -1, style.size).x, line);
}
} // namespace

std::vector<Rect2> StagingPreview::number_rects() const {
    std::vector<Rect2> rects;
    for (const auto& marker : markers_) {
        const auto* label = static_cast<const EawrUiLabel*>(marker.number);
        const auto block = number_block(*label, label->get_text(), label->role());
        const auto transform = label->get_global_transform_with_canvas();
        const auto scale = transform.get_scale();
        const Vector2 size = block * scale;
        rects.emplace_back(transform.get_origin() + (label->get_size() * scale - size) * 0.5F, size);
    }
    return rects;
}

bool StagingPreview::number_outline() const {
    if (markers_.empty()) return false;
    const auto* label = static_cast<const EawrUiLabel*>(markers_.front().number);
    const CharString role = label->role().utf8();
    return kit_text_style(*label, std::string_view(role.get_data(), role.length())).outline;
}

bool StagingPreview::number_emboss() const {
    if (markers_.empty()) return false;
    const auto* label = static_cast<const EawrUiLabel*>(markers_.front().number);
    const CharString role = label->role().utf8();
    return kit_text_style(*label, std::string_view(role.get_data(), role.length())).emboss;
}
} // namespace eawr::presentation::godot_backend
