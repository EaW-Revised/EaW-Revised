#include "eawr/presentation/ui/minimap.hpp"
#include "eawr/data/tag_trace.hpp"

#include "eawr/data/xml.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>

namespace eawr::presentation::ui {
namespace {

constexpr std::string_view radar_map_path = "data/xml/radarmap.xml";
constexpr std::string_view constants_path = "data/xml/gameconstants.xml";
constexpr std::string_view factions_path = "data/xml/factions.xml";

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n')) {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] bool same_name(const std::string_view a, const std::string_view b) noexcept {
    const auto lower = [](const char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; };
    return a.size() == b.size()
        && std::equal(a.begin(), a.end(), b.begin(), [&](const char x, const char y) { return lower(x) == lower(y); });
}

// The last child of that name wins, as later XML entries do.
[[nodiscard]] const data::XmlNode* last_child(const data::XmlNode& root, const std::string_view name) noexcept {
    const data::XmlNode* found = nullptr;
    for (const data::XmlNode& child : root.children) {
        if (same_name(child.name, name)) found = &child;
    }
    data::tag_trace::used(found);
    return found;
}

[[nodiscard]] std::optional<bool> boolean(const std::string_view text) noexcept {
    const std::string_view value = trim(text);
    if (same_name(value, "yes") || same_name(value, "true") || value == "1") return true;
    if (same_name(value, "no") || same_name(value, "false") || value == "0") return false;
    return std::nullopt;
}

[[nodiscard]] std::optional<double> number(std::string_view text) noexcept {
    text = trim(text);
    if (!text.empty() && (text.back() == 'f' || text.back() == 'F')) text.remove_suffix(1);
    double value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value)) return std::nullopt;
    return value;
}

// Numbers separated by commas and/or white space.
[[nodiscard]] std::vector<double> numbers(const std::string_view text) {
    std::vector<double> out;
    std::size_t start = 0;
    while (start < text.size()) {
        while (start < text.size() && (text[start] == ',' || text[start] == ' ' || text[start] == '\t'
                                       || text[start] == '\r' || text[start] == '\n')) {
            ++start;
        }
        std::size_t end = start;
        while (end < text.size() && text[end] != ',' && text[end] != ' ' && text[end] != '\t' && text[end] != '\r'
               && text[end] != '\n') {
            ++end;
        }
        if (end > start) {
            const auto value = number(text.substr(start, end - start));
            if (!value) return {};
            out.push_back(*value);
        }
        start = end;
    }
    return out;
}

[[nodiscard]] std::optional<data::ui::Rgba8> colour(const std::string_view text) {
    const std::vector<double> parts = numbers(text);
    if (parts.size() != 3 && parts.size() != 4) return std::nullopt;
    std::array<std::uint8_t, 4> bytes{0, 0, 0, 255};
    for (std::size_t index = 0; index < parts.size(); ++index) {
        if (parts[index] < 0.0 || parts[index] > 255.0 || parts[index] != std::floor(parts[index])) return std::nullopt;
        bytes[index] = static_cast<std::uint8_t>(parts[index]);
    }
    return data::ui::Rgba8{bytes[0], bytes[1], bytes[2], bytes[3]};
}

void fallback(MinimapSettings& settings, const std::string_view path, const std::string_view tag, const std::string_view why) {
    core::Diagnostic diagnostic;
    diagnostic.code = std::string(diagnostic_codes::minimap_settings);
    diagnostic.severity = core::Severity::warning;
    diagnostic.message = std::string(tag) + " " + std::string(why) + "; the FoC value is used";
    diagnostic.logical_path = std::string(path);
    settings.diagnostics.push_back(std::move(diagnostic));
}

} // namespace

MinimapSettings minimap_settings(const data::XmlNode* radar_map, const data::XmlNode* game_constants,
                                 const data::XmlNode* factions) {
    MinimapSettings settings;
    if (factions != nullptr) {
        for (const data::XmlNode& faction : factions->children) {
            if (!same_name(faction.name, "Faction")) continue;
            const auto name = std::find_if(faction.attributes.begin(), faction.attributes.end(),
                [](const data::XmlAttribute& attribute) { return same_name(attribute.name, "Name"); });
            const data::XmlNode* node = last_child(faction, "Color");
            if (name == faction.attributes.end()) continue;
            data::tag_trace::used_attribute(faction, name->name);
            // MM-07/WNO-40: later faction definitions replace the earlier registry winner.
            std::erase_if(settings.faction_colours, [&](const auto& entry) { return same_name(entry.first, name->value); });
            std::erase_if(settings.faction_no_colorization, [&](const auto& entry) { return same_name(entry.first, name->value); });
            if (node != nullptr) {
                if (const auto parsed = colour(node->raw_text)) settings.faction_colours.emplace_back(name->value, *parsed);
            }
            if (const auto* unassigned = last_child(faction, "No_Colorization_Color")) {
                if (const auto parsed = colour(unassigned->raw_text)) settings.faction_no_colorization.emplace_back(name->value, *parsed);
            }
        }
    }
    const data::XmlNode* space = radar_map != nullptr ? last_child(*radar_map, "RadarMapSettings") : nullptr;
    if (radar_map != nullptr && space == nullptr) fallback(settings, radar_map_path, "RadarMapSettings", "is absent");
    if (space != nullptr) {
        const auto read_colour = [&](const std::string_view tag, data::ui::Rgba8& destination) {
            if (const auto* node = last_child(*space, tag)) {
                if (const auto parsed = colour(node->raw_text)) destination = *parsed;
                else fallback(settings, radar_map_path, tag, "is not a colour");
            }
        };
        read_colour("Space_Asteroid_Field_Color", settings.field);
        read_colour("Space_Asteroid_Field_Border_Color", settings.field_border);
        if (const data::XmlNode* node = last_child(*space, "Space_Backdrop_Texture_Name")) {
            if (!trim(node->raw_text).empty()) settings.backdrop = std::string(trim(node->raw_text));
        }
        if (const data::XmlNode* node = last_child(*space, "Space_FOW_Color")) {
            if (const auto parsed = colour(node->raw_text)) settings.fog = *parsed;
            else fallback(settings, radar_map_path, "Space_FOW_Color", "is not a colour");
        }
        if (const data::XmlNode* node = last_child(*space, "Space_Is_Guide_Rectangle")) {
            if (const auto parsed = boolean(node->raw_text)) settings.guide_rectangle = *parsed;
            else fallback(settings, radar_map_path, "Space_Is_Guide_Rectangle", "is not Yes or No");
        }
        // MM-14: the space surface colour fills the background layer; the last entry wins.
        for (const data::XmlNode& child : space->children) {
            if (!same_name(child.name, "Color")) continue;
            const auto name = std::find_if(child.attributes.begin(), child.attributes.end(),
                [](const data::XmlAttribute& attribute) { return same_name(attribute.name, "name"); });
            if (name == child.attributes.end() || !same_name(name->value, "space")) continue;
            data::tag_trace::used(child);
            data::tag_trace::used_attribute(child, name->name);
            if (const auto parsed = colour(child.raw_text)) settings.background = *parsed;
            else fallback(settings, radar_map_path, "Color name=\"space\"", "is not a colour");
        }
    }
    if (game_constants != nullptr) {
        if (const auto* node = last_child(*game_constants, "Nebula_Effect_Color")) {
            if (const auto parsed = colour(node->raw_text)) settings.nebula = *parsed;
            else fallback(settings, constants_path, "Nebula_Effect_Color", "is not a colour");
        }
        if (const data::XmlNode* node = last_child(*game_constants, "Radar_Colorize_Selected_Units")) {
            if (const auto parsed = boolean(node->raw_text)) settings.colorize_selected = *parsed;
            else fallback(settings, constants_path, "Radar_Colorize_Selected_Units", "is not Yes or No");
        }
        if (const data::XmlNode* node = last_child(*game_constants, "Radar_Selected_Units_Color")) {
            if (const auto parsed = colour(node->raw_text)) settings.selected = *parsed;
            else fallback(settings, constants_path, "Radar_Selected_Units_Color", "is not a colour");
        }
    }
    return settings;
}

MinimapSettings minimap_settings(const vfs::Vfs& filesystem) {
    auto radar = data::load_document(filesystem, radar_map_path);
    auto constants = data::load_document(filesystem, constants_path);
    // WNO-40/43: expansion and mod factions share the authored registry, including Hutts.
    const auto registry = data::load_document(filesystem, "data/xml/factionfiles.xml");
    data::XmlNode faction_definitions;
    std::vector<std::pair<std::string, std::string>> missing_factions;
    if (registry) {
        for (const auto& entry : registry.value().root.children) {
            if (!same_name(entry.name, "File")) continue;
            data::tag_trace::used(&entry);
            const auto path = data::registry_include_path("data/xml/factionfiles.xml", trim(entry.raw_text));
            auto document = data::load_document(filesystem, path);
            if (!document) {
                missing_factions.emplace_back(path, document.error().message);
                continue;
            }
            for (auto& faction : document.value().root.children) faction_definitions.children.push_back(std::move(faction));
        }
    } else {
        auto factions = data::load_document(filesystem, factions_path);
        if (factions) faction_definitions = std::move(factions).value().root;
        else missing_factions.emplace_back(std::string(factions_path), factions.error().message);
    }
    MinimapSettings settings = minimap_settings(radar ? &radar.value().root : nullptr,
                                                constants ? &constants.value().root : nullptr,
                                                &faction_definitions);
    if (!radar) fallback(settings, radar_map_path, radar_map_path, "cannot be read (" + radar.error().message + ")");
    if (!constants) fallback(settings, constants_path, constants_path, "cannot be read (" + constants.error().message + ")");
    for (const auto& [path, why] : missing_factions) fallback(settings, path, "Faction", "cannot be read (" + why + ")");
    return settings;
}

std::optional<data::ui::Rgba8> faction_colour(const MinimapSettings& settings, const std::string_view faction,
                                            const bool no_colorization) {
    for (const auto& [name, value] : no_colorization ? settings.faction_no_colorization : settings.faction_colours) {
        if (same_name(name, faction)) return value;
    }
    return std::nullopt;
}

MinimapTypeLooks minimap_type_looks(const std::string_view type, const data::Catalog* objects) {
    MinimapTypeLooks looks;
    if (objects == nullptr || objects->find(type, data::Category::game_object) == nullptr) return looks;
    auto resolved = objects->resolve(type, data::Category::game_object);
    if (!resolved) return looks;
    const auto text = [&](const std::string_view tag) -> std::optional<std::string_view> {
        const data::EffectiveValue* value = resolved.value().value(tag);
        if (value == nullptr) return std::nullopt;
        return trim(value->value.raw_text);
    };
    if (const auto value = text("Is_Visible_On_Radar")) looks.visible = boolean(*value).value_or(looks.visible);
    if (const auto value = text("Visible_On_Radar_When_Fogged")) looks.visible_when_fogged = boolean(*value).value_or(false);
    if (const auto value = text("No_Colorization_Color")) looks.no_colorization = colour(*value);
    if (const auto value = text("Is_Visible_On_Enemy_Radar")) {
        looks.visible_to_enemy = boolean(*value).value_or(looks.visible_to_enemy);
    }
    // MM-16: an explicit empty name selects the point layer; an omitted name keeps the default icon.
    if (const auto value = text("Radar_Icon_Name")) looks.icon = std::string(*value);
    if (const auto value = text("Radar_Icon_Size")) {
        const std::vector<double> parts = numbers(*value);
        if (parts.size() == 2 && parts[0] > 0.0 && parts[1] > 0.0) {
            looks.size = {static_cast<float>(parts[0]), static_cast<float>(parts[1])};
        }
    }
    if (const auto value = text("Radar_Show_Facing")) looks.show_facing = boolean(*value).value_or(looks.show_facing);
    if (const auto value = text("Radar_Rotate_Icon")) looks.rotate_icon = boolean(*value).value_or(looks.rotate_icon);
    if (const auto value = text("Radar_Blip_Size")) {
        if (const auto parsed = number(*value); parsed && *parsed >= 0.0 && *parsed < 4294967296.0) {
            looks.point_size = static_cast<float>(*parsed);
        }
    }
    if (const auto value = text("Radar_Draw_To_Scale")) looks.draw_to_scale = boolean(*value).value_or(looks.draw_to_scale);
    if (const auto value = text("Radar_Icon_Scale_Space")) looks.space_scale = number(*value).value_or(looks.space_scale);
    std::uint8_t kind = 1;
    for (const auto tag : {"Is_Asteroid_Field", "Is_Ion_Storm", "Is_Nebula"}) {
        if (const auto value = text(tag); value && boolean(*value).value_or(false)) {
            looks.hazard = true;
            looks.hazard_kind = static_cast<std::uint8_t>(looks.hazard_kind | kind);
        }
        kind = static_cast<std::uint8_t>(kind << 1U);
    }
    return looks;
}

MinimapExtents minimap_extents(const double min_x, const double max_x, const double min_y, const double max_y) noexcept {
    MinimapExtents extents;
    const double centre_x = (min_x + max_x) / 2.0;
    const double centre_y = (min_y + max_y) / 2.0;
    const double half = std::max({std::abs(max_x - min_x) / 2.0, std::abs(max_y - min_y) / 2.0, 1.0});
    extents.min_x = centre_x - half;
    extents.max_x = centre_x + half;
    extents.min_y = centre_y - half;
    extents.max_y = centre_y + half;
    extents.playable_min_x = std::min(min_x, max_x);
    extents.playable_max_x = std::max(min_x, max_x);
    extents.playable_min_y = std::min(min_y, max_y);
    extents.playable_max_y = std::max(min_y, max_y);
    return extents;
}

MinimapPoint minimap_point(const MinimapExtents& extents, const double x, const double y) noexcept {
    const double width = extents.max_x - extents.min_x;
    const double height = extents.max_y - extents.min_y;
    return {((x - extents.min_x) / width - 0.5) * 2.0, ((y - extents.min_y) / height - 0.5) * 2.0};
}

std::array<double, 2> minimap_world(const MinimapExtents& extents, const MinimapPoint point) noexcept {
    const double width = extents.max_x - extents.min_x;
    const double height = extents.max_y - extents.min_y;
    return {extents.min_x + (point.x / 2.0 + 0.5) * width, extents.min_y + (point.y / 2.0 + 0.5) * height};
}

std::optional<MinimapSquadronPose> minimap_squadron_pose(
    const std::span<const MinimapSquadronMember> members) noexcept {
    if (members.empty() || !members.front().visible) return std::nullopt;
    MinimapSquadronPose pose;
    for (const auto& member : members) {
        pose.x += member.x;
        pose.y += member.y;
    }
    const auto count = static_cast<double>(members.size());
    pose.x /= count;
    pose.y /= count;
    pose.yaw_degrees = members.front().yaw_degrees;
    return pose;
}

std::vector<MinimapBlip> minimap_blips(const std::span<const MinimapUnit> units,
    const std::function<const MinimapTypeLooks&(std::string_view)>& looks, const MinimapExtents& extents,
    const MinimapSettings& settings, const std::span<const MinimapMemory> memories) {
    std::vector<MinimapBlip> blips;
    blips.reserve(units.size() + memories.size());
    const auto neutral = faction_colour(settings, "Neutral").value_or(data::ui::Rgba8{100, 100, 100, 255});
    const auto append = [&](const MinimapUnit* unit, const bool remembered) {
        const MinimapTypeLooks& type = looks(unit->type);
        const MinimapPoint centre = minimap_point(extents, unit->x, unit->y);
        if (!std::isfinite(centre.x) || !std::isfinite(centre.y)
            || std::abs(centre.x) > 1.0 || std::abs(centre.y) > 1.0) return;
        MinimapBlip blip;
        blip.id = unit->id;
        blip.icon = type.icon;
        blip.centre = centre;
        blip.half_size = {type.size[0], type.size[1]};
        // MM-17: fixed icon size wins unless draw-to-scale was explicitly enabled.
        if (!type.icon.empty() && type.draw_to_scale) {
            const double x_extent = unit->team ? type.space_scale : unit->world_half_size[0] * type.space_scale;
            const double y_extent = unit->team ? type.space_scale : unit->world_half_size[1] * type.space_scale;
            blip.half_size = {x_extent * 2.0 / (extents.max_x - extents.min_x),
                y_extent * 2.0 / (extents.max_y - extents.min_y)};
        }
        blip.point_pixels = type.point_size >= 2.0F && type.point_size < 3.0F ? 2U : 1U;
        // MM-08: a facing icon turns by the facing less a quarter turn; the engine skips the turn for a
        // facing of exactly zero, as it does for a type without Radar_Show_Facing.
        if (type.show_facing && unit->yaw_degrees != 0.0) blip.rotation_degrees = unit->yaw_degrees - 90.0;
        blip.rotate_icon = type.rotate_icon;
        // MM-07: the owner's colour; the selection's colour for a selected unit.
        blip.colour = unit->selected && settings.colorize_selected ? settings.selected : unit->owner_colour;
        // WNO-43: capture override follows selected recolouring, even under raw fog.
        if (unit->capture) blip.colour = minimap_capture_colour(*unit->capture, neutral);
        blip.remembered = remembered;
        blips.push_back(std::move(blip));
    };
    for (auto unit = units.rbegin(); unit != units.rend(); ++unit) {
        const auto& type = looks(unit->type);
        const auto& radar = unit->radar;
        // WNO-41: the authored flag/replay bypass exactly two early presentation gates.
        if (!type.visible || type.hazard || !radar.alive || radar.limbo) continue;
        if (!radar.capital_layer && (unit->x < extents.playable_min_x || unit->x > extents.playable_max_x
            || unit->y < extents.playable_min_y || unit->y > extents.playable_max_y)) continue;
        if (unit->hostile && (!type.visible_to_enemy || unit->in_nebula || radar.jammed || radar.stealthed
            || (!radar.display_enemies && !unit->capture))) continue;
        if (!type.visible_when_fogged && !radar.replay && (radar.model_hidden || radar.radar_faded)) continue;
        if (!radar.locally_visible && !radar.interdicted) continue;
        append(&*unit, false);
    }
    // WNO-44: separate stored-model pass; live visibility and fogged-radar flags do not apply.
    for (auto memory = memories.rbegin(); memory != memories.rend(); ++memory) {
        const auto& type = looks(memory->type);
        // WNO-44: remembered admission does not repeat the live radar-type switch.
        if (!memory->previously_revealed || !memory->retained_model || type.hazard
            || (memory->hostile && (!type.visible_to_enemy || !memory->display_enemies))) continue;
        if (memory->x < extents.playable_min_x || memory->x > extents.playable_max_x
            || memory->y < extents.playable_min_y || memory->y > extents.playable_max_y) continue;
        MinimapUnit stored;
        stored.id = memory->id;
        stored.type = memory->type;
        stored.owner_colour = memory->capture_point ? neutral : memory->owner_colour;
        stored.x = memory->x;
        stored.y = memory->y;
        stored.yaw_degrees = memory->yaw_degrees;
        stored.world_half_size = memory->world_half_size;
        append(&stored, true);
    }
    return blips;
}

data::ui::Rgba8 minimap_capture_colour(const MinimapCaptureColour& capture, const data::ui::Rgba8 neutral) noexcept {
    if (capture.raw_local_fog) return neutral;
    const double progress = std::isfinite(capture.progress) ? std::clamp(capture.progress, 0.0, 1.0) : 0.0;
    const auto channel = [progress](const std::uint8_t old, const std::uint8_t next) {
        return static_cast<std::uint8_t>(std::lround(old + (static_cast<double>(next) - old) * progress));
    };
    return {channel(capture.old_colour.r, capture.new_colour.r), channel(capture.old_colour.g, capture.new_colour.g),
        channel(capture.old_colour.b, capture.new_colour.b), channel(capture.old_colour.a, capture.new_colour.a)};
}

data::ui::Rgba8 minimap_community_colour(const std::span<const MinimapPlayerColour> players,
    const sim::tactical::PlayerId owner, const sim::tactical::PlayerId local, const bool multiplayer,
    const data::ui::Rgba8 fallback) noexcept {
    const auto owned = std::find_if(players.begin(), players.end(), [owner](const auto& p) { return p.player == owner; });
    if (!multiplayer || owned == players.end() || owned->neutral || owned->team == 0xffffffffU) return fallback;
    const auto own = std::find_if(players.begin(), players.end(), [local](const auto& p) { return p.player == local; });
    if (own != players.end() && own->team == owned->team) return own->colour;
    const MinimapPlayerColour* representative = &*owned;
    for (const auto& player : players) {
        if (!player.neutral && player.team == owned->team && player.player < representative->player) representative = &player;
    }
    return representative->colour;
}

std::optional<MinimapPixelRect> minimap_point_pixels(const MinimapBlip& blip,
    const std::uint32_t width, const std::uint32_t height) noexcept {
    if (width == 0 || height == 0 || width > 4096 || height > 4096 || !blip.icon.empty()
        || !std::isfinite(blip.centre.x) || !std::isfinite(blip.centre.y)) return std::nullopt;
    const double x = std::trunc((blip.centre.x + 1.0) * 0.5 * width);
    const double y = std::trunc((1.0 - blip.centre.y) * 0.5 * height);
    if (x < 0.0 || y < 0.0 || x >= width || y >= height) return std::nullopt;
    const auto column = static_cast<std::uint32_t>(x), row = static_cast<std::uint32_t>(y);
    const auto size = blip.point_pixels == 2 ? 2U : 1U;
    return MinimapPixelRect{column, row, std::min(size, width - column), std::min(size, height - row)};
}

std::vector<std::uint8_t> minimap_hazards(const std::span<const MinimapHazard> hazards,
    const MinimapExtents& extents, const MinimapSettings& settings, const std::uint32_t width, const std::uint32_t height) {
    if (width == 0 || height == 0 || width > 4096 || height > 4096) return {};
    const auto count = static_cast<std::size_t>(width) * height;
    std::vector<std::uint8_t> fill(count), edge(count), rgba(count * 4);
    for (const auto& hazard : hazards) {
        if (!std::isfinite(hazard.x) || !std::isfinite(hazard.y) || !std::isfinite(hazard.x_extent)
            || !std::isfinite(hazard.y_extent) || hazard.x_extent <= 0 || hazard.y_extent <= 0) continue;
        const auto centre = minimap_point(extents, hazard.x, hazard.y);
        const double cx = std::trunc((centre.x + 1.0) * 0.5 * width);
        const double cy = std::trunc((1.0 - centre.y) * 0.5 * height);
        // The registered box extent is projected as a displacement, then halved for the ellipse.
        const double rx = std::max(1.0, hazard.x_extent * width / (extents.max_x - extents.min_x) * 0.5);
        const double ry = std::max(1.0, hazard.y_extent * height / (extents.max_y - extents.min_y) * 0.5);
        for (std::uint32_t y = 0; y < height; ++y) {
            const double dy = (y - cy) / ry;
            if (std::abs(dy) > 1.0) continue;
            const double reach = rx * std::sqrt(std::max(0.0, 1.0 - dy * dy));
            for (std::uint32_t x = 0; x < width; ++x) {
                if (std::abs(x - cx) <= reach) fill[static_cast<std::size_t>(y) * width + x] = 255;
            }
        }
    }
    const auto sample = [&](const std::uint32_t x, const std::uint32_t y) { return fill[static_cast<std::size_t>(y) * width + x]; };
    // WHZ-72: evaluate the predecessor differences in both vertical orientations, then OR.
    for (std::uint32_t y = 0; y < height; ++y) for (std::uint32_t x = 0; x < width; ++x) {
        const int value = sample(x, y);
        const int horizontal = x > 0 ? std::abs(value - sample(x - 1, y)) : 0;
        const auto threshold = [&](const int vertical) {
            const auto difference = static_cast<std::uint8_t>(horizontal + vertical);
            return difference > (2 * 128 / 3) ? difference : std::uint8_t{};
        };
        edge[static_cast<std::size_t>(y) * width + x] = static_cast<std::uint8_t>(
            threshold(y > 0 ? std::abs(value - sample(x, y - 1)) : 0)
            | threshold(y + 1 < height ? std::abs(value - sample(x, y + 1)) : 0));
    }
    const auto gaussian = [&](std::vector<std::uint8_t>& mask) {
        std::vector<std::uint8_t> output(count);
        constexpr std::array<unsigned, 3> weights{1, 2, 1};
        for (std::uint32_t y = 1; y + 1 < height; ++y) for (std::uint32_t x = 1; x + 1 < width; ++x) {
            unsigned sum = 0;
            for (std::uint32_t row = 0; row < 3; ++row) for (std::uint32_t column = 0; column < 3; ++column) {
                // The image filter's horizontal window starts at x; its centre is x + 1.
                const auto index = static_cast<std::size_t>(y + row - 1) * width + x + column;
                if (index < count) sum += mask[index] * weights[row] * weights[column];
            }
            output[static_cast<std::size_t>(y) * width + x] = static_cast<std::uint8_t>(sum / 16);
        }
        mask = std::move(output);
    };
    gaussian(fill);
    gaussian(edge);
    const std::array<std::uint8_t, 4> a{settings.field.r, settings.field.g, settings.field.b, settings.field.a};
    const std::array<std::uint8_t, 4> b{settings.field_border.r, settings.field_border.g, settings.field_border.b, settings.field_border.a};
    for (std::size_t index = 0; index < count; ++index) {
        if (fill[index] == 0 && edge[index] == 0) continue;
        const double coverage = fill[index] / 255.0;
        for (std::size_t channel = 0; channel < 4; ++channel) {
            double value;
            if (fill[index] && edge[index]) value = std::trunc(coverage * a[channel]) + (1.0 - coverage) * b[channel];
            else {
                const auto colour = fill[index] ? a[channel] : b[channel];
                value = channel == 3 ? colour * (fill[index] ? fill[index] : edge[index]) / 255.0 : colour;
            }
            rgba[index * 4 + channel] = static_cast<std::uint8_t>(std::clamp(value, 0.0, 255.0));
        }
    }
    return rgba;
}

std::array<MinimapPoint, 4> minimap_guide(const std::array<std::array<double, 2>, 4>& ground,
                                          const MinimapExtents& extents, const bool rectangle) {
    std::array<std::array<double, 2>, 4> corners = ground;
    if (rectangle) {
        double min_x = ground[0][0];
        double max_x = ground[0][0];
        double min_y = ground[0][1];
        double max_y = ground[0][1];
        for (const auto& corner : ground) {
            min_x = std::min(min_x, corner[0]);
            max_x = std::max(max_x, corner[0]);
            min_y = std::min(min_y, corner[1]);
            max_y = std::max(max_y, corner[1]);
        }
        corners = {{{min_x, max_y}, {max_x, max_y}, {max_x, min_y}, {min_x, min_y}}};
    }
    std::array<MinimapPoint, 4> out{};
    for (std::size_t index = 0; index < corners.size(); ++index) {
        out[index] = minimap_point(extents, corners[index][0], corners[index][1]);
    }
    return out;
}

std::optional<std::array<MinimapPoint, 2>> minimap_clip(const MinimapPoint a, const MinimapPoint b) noexcept {
    // Liang-Barsky against the square -1..1.
    double from = 0.0;
    double to = 1.0;
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const std::array<std::pair<double, double>, 4> edges{
        {{-dx, a.x + 1.0}, {dx, 1.0 - a.x}, {-dy, a.y + 1.0}, {dy, 1.0 - a.y}}};
    for (const auto& [p, q] : edges) {
        if (p == 0.0) {
            if (q < 0.0) return std::nullopt;
            continue;
        }
        const double t = q / p;
        if (p < 0.0) {
            if (t > to) return std::nullopt;
            from = std::max(from, t);
        } else {
            if (t < from) return std::nullopt;
            to = std::min(to, t);
        }
    }
    return std::array<MinimapPoint, 2>{MinimapPoint{a.x + from * dx, a.y + from * dy}, MinimapPoint{a.x + to * dx, a.y + to * dy}};
}

void MinimapFog::resize(const std::uint32_t width, const std::uint32_t height) {
    if (width == width_ && height == height_) return;
    width_ = width;
    height_ = height;
    const std::size_t bytes = static_cast<std::size_t>(width) * height * 4U;
    front_.assign(bytes, 0);
    back_.assign(bytes, 0);
    next_row_ = 0;
    full_ = true;
}

bool MinimapFogCells::revealed(const double x, const double y) const noexcept {
    if ((!values && !rows) || cell <= 0.0) return false;
    const double column = std::floor((x - left) / cell);
    const double row = std::floor((top - y) / cell);
    if (column < 0.0 || row < 0.0 || column >= wide || row >= tall) return false;
    if (rows) {
        const auto at = static_cast<std::size_t>(row);
        const auto offset = static_cast<std::size_t>(column);
        return at < rows->size() && (*rows)[at] && offset < (*rows)[at]->size() && (*(*rows)[at])[offset] != 0;
    }
    const std::size_t index = static_cast<std::size_t>(row) * wide + static_cast<std::size_t>(column);
    return index < values->size() && (*values)[index] != 0;
}

bool MinimapFog::advance(const MinimapExtents& extents, const std::span<const MinimapRevealer> revealers,
                         const data::ui::Rgba8 fog, const bool show) {
    return advance(extents, [revealers](const double x, const double y) {
        return std::any_of(revealers.begin(), revealers.end(), [&](const MinimapRevealer& revealer) {
            const double dx = x - revealer.x;
            const double dy = y - revealer.y;
            return dx * dx + dy * dy <= revealer.range * revealer.range;
        });
    }, fog, show);
}

bool MinimapFog::advance(const MinimapExtents& extents, const MinimapFogCells& cells, const data::ui::Rgba8 fog,
                         const bool show) {
    return advance(extents, [&cells](const double x, const double y) { return cells.revealed(x, y); }, fog, show);
}

bool MinimapFog::advance(const MinimapExtents& extents, const std::function<bool(double, double)>& revealed_at,
                         const data::ui::Rgba8 fog, const bool show) {
    if (width_ == 0 || height_ == 0) return false;
    if (next_row_ == 0) std::fill(back_.begin(), back_.end(), std::uint8_t{0});
    const std::uint32_t last = full_ ? height_ : std::min(height_, next_row_ + rows_per_frame);
    const double world_width = extents.max_x - extents.min_x;
    const double world_height = extents.max_y - extents.min_y;
    for (std::uint32_t row = next_row_; row < last; ++row) {
        // The texel's corner in the world: row 0 is the top (largest Y).
        const double y = extents.min_y + (1.0 - static_cast<double>(row) / height_) * world_height;
        for (std::uint32_t column = 0; column < width_; ++column) {
            const double x = extents.min_x + static_cast<double>(column) / width_ * world_width;
            const bool playable = x >= extents.playable_min_x && x <= extents.playable_max_x
                && y >= extents.playable_min_y && y <= extents.playable_max_y;
            if (!playable || !show) continue;
            if (revealed_at(x, y)) continue;
            std::uint8_t* texel = back_.data() + (static_cast<std::size_t>(row) * width_ + column) * 4U;
            texel[0] = fog.r;
            texel[1] = fog.g;
            texel[2] = fog.b;
            texel[3] = fog.a;
        }
    }
    next_row_ = last;
    full_ = false;
    if (next_row_ < height_) return true;
    front_.swap(back_);
    next_row_ = 0;
    ++passes_;
    return true;
}

std::size_t MinimapFog::fogged() const noexcept {
    std::size_t count = 0;
    for (std::size_t index = 3; index < front_.size(); index += 4) count += front_[index] != 0 ? 1U : 0U;
    return count;
}

} // namespace eawr::presentation::ui
