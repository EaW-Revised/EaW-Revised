#include "eawr/presentation/ui/world_ui.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/data/tag_trace.hpp"

#include <algorithm>
#include <cmath>
#include <charconv>
#include <limits>
#include <utility>

namespace eawr::presentation::ui {

double WaypointDashClock::sample(const bool shown, const double presented_tick,
                                const double velocity, const double period) noexcept {
    if (!shown) { start_tick_.reset(); return 0.0; }
    if (!start_tick_ || presented_tick < *start_tick_) start_tick_ = presented_tick;
    if (period <= 0.0) return 0.0;
    return std::fmod((presented_tick - *start_tick_) * velocity, period);
}

std::array<std::uint32_t, 2> world_ability_overlays(const WorldOverlayUnit& unit, const AbilityState& state) {
    std::array<std::uint32_t, 2> result{};
    if (!unit.ally || !unit.visible || !unit.on_screen || (!unit.squadron && !unit.bracket)) return result;
    for (std::size_t slot = 0; slot < unit.abilities.size(); ++slot) {
        const auto ability = unit.abilities[slot];
        if (ability == ability_none) continue;
        const auto current = state.state(unit.entity, ability);
        if (!current || current->status != AbilityStatus::active) continue;
        result[slot] = ability;
        if (unit.squadron) break;
    }
    return result;
}

float bracket_ability_y(const float bracket_y, const float bar_height_pixels,
                       const float icon_height_pixels, const float ui_scale) noexcept {
    return bracket_y - (bar_height_pixels + icon_height_pixels) * 0.5F - 3.0F * ui_scale;
}

WorldAbilityRect world_ability_rect(const std::array<float, 2> centre,
    const std::array<float, 2> native_size, const std::array<float, 2> effect_offset,
    const float component_scale, const float ui_scale) noexcept {
    const float width = native_size[0] * component_scale * ui_scale;
    const float height = native_size[1] * component_scale * ui_scale;
    return {centre[0] + effect_offset[0] * ui_scale - width * 0.5F,
            centre[1] - effect_offset[1] * ui_scale - height * 0.5F, width, height};
}

std::optional<float> selection_circle_side(const float select_box_scale, const float scale_factor) noexcept {
    // WU-02: no blob unless Select_Box_Scale is positive.
    if (!(select_box_scale > 0.0F) || !std::isfinite(select_box_scale) || !std::isfinite(scale_factor)) return std::nullopt;
    const float side = select_box_scale * scale_factor;
    if (!(side > 0.0F)) return std::nullopt;
    return side;
}

BarSize bar_size(const std::string_view ship_class, const std::optional<int> gui_bracket_size) noexcept {
    if (gui_bracket_size) {
        if (*gui_bracket_size == 0) return BarSize::small;
        if (*gui_bracket_size == 1) return BarSize::medium;
        if (*gui_bracket_size == 2) return BarSize::large;
    }
    if (ship_class == "fighter" || ship_class == "bomber") return BarSize::small;
    if (ship_class == "frigate" || ship_class == "capital_ship" || ship_class == "super") return BarSize::large;
    return BarSize::medium;
}

float bar_width(const BarSize size) noexcept {
    switch (size) {
    case BarSize::small: return 34.0F;
    case BarSize::medium: return 68.0F;
    case BarSize::large: return 102.0F;
    }
    return 34.0F;
}

float bar_scale(const float camera_distance) noexcept {
    if (!(camera_distance > 0.0F) || !std::isfinite(camera_distance)) return min_health_bar_scale;
    return std::max(min_health_bar_scale, health_bar_scale / camera_distance);
}

int bar_level(const float fraction) noexcept {
    const float clamped = std::isfinite(fraction) ? std::clamp(fraction, 0.0F, 1.0F) : 0.0F;
    return static_cast<int>(std::ceil(10.0F * clamped));
}

Rgb health_bar_colour(const int level) noexcept {
    if (level <= 0) return {0, 0, 0};
    if (level <= 3) return {255, 0, 0};
    if (level <= 5) return {255, 147, 0};
    if (level <= 7) return {255, 234, 0};
    if (level <= 9) return {204, 255, 0};
    return {0, 255, 0};
}

BarVisibility bar_visibility(const BarUnit& unit) noexcept {
    if (!unit.admitted || unit.neutral) return {}; // WSU-50; WSU-63 owner policy
    // A selected craft of a squadron counts as not selected, and only an unselected unit is shown
    // by the pointer or critical health (below 10 %). #502: hovering the squadron's icon shows
    // only the icon's own bar (WU-22), never its craft's.
    const bool selected = unit.selected && !unit.squadron_member;
    const bool shown = !unit.selected && (unit.hovered || (unit.health > 0.0F && unit.health < 0.1F));
    if (unit.fogged || (!selected && !shown)) return {};
    // WU-11, WU-17: the bars of GUI_Hide_Health_Bar types and of squadron craft are hidden; the health bar
    // comes back while the unit is shown by the pointer, the shield bar does not.
    const bool hide = unit.hide_health_bar || unit.squadron_member;
    return {unit.has_health && !(hide && !shown), unit.shielded && !hide};
}

bool bar_candidate(const bool hovered, const bool selected) noexcept {
    return hovered || selected;
}

float bar_anchor_lift(const std::array<float, 3>& half_extent, const std::array<float, 3>& camera_up,
                      const float gui_bounds_scale) noexcept {
    float lift = 0.0F;
    if (std::abs(gui_bounds_scale - 1.0F) <= 0.01F) {
        for (int corner = 0; corner < 8; ++corner) {
            float along = 0.0F;
            for (int axis = 0; axis < 3; ++axis) {
                const float sign = ((corner >> axis) & 1) != 0 ? -1.0F : 1.0F;
                along += sign * half_extent[static_cast<std::size_t>(axis)] * camera_up[static_cast<std::size_t>(axis)];
            }
            lift = std::max(lift, along);
        }
    } else {
        lift = std::sqrt(half_extent[0] * half_extent[0] + half_extent[1] * half_extent[1] + half_extent[2] * half_extent[2]);
    }
    return lift * gui_bounds_scale;
}

bool place_squadron_arrival_icon(SquadronIconAnchor& anchor,
    const std::optional<std::array<float, 3>> landing, const std::array<float, 3> presented) noexcept {
    if (!landing && !anchor.arriving) return false;
    anchor.position = landing.value_or(presented);
    anchor.speed = 0.0F;
    anchor.arriving = landing.has_value();
    return true;
}

void slide_squadron_icon(SquadronIconAnchor& anchor, const std::array<float, 3> desired,
    const float thrust, const float fastest, const bool in_grid, const bool fast_forward) noexcept {
    std::array<float, 3> delta{};
    float squared = 0.0F;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        delta[axis] = desired[axis] - anchor.position[axis];
        squared += delta[axis] * delta[axis];
    }
    const float distance = std::sqrt(squared);
    if (distance == 0.0F) {
        if (in_grid) anchor.speed = 0.0F;
    } else if (distance < anchor.speed || fast_forward) {
        anchor.position = desired;
    } else {
        for (std::size_t axis = 0; axis < 3; ++axis) anchor.position[axis] += delta[axis] * anchor.speed / distance;
        const bool braking = in_grid && thrust > 0.0F && anchor.speed * anchor.speed / (2.0F * thrust) >= distance;
        anchor.speed = std::clamp(anchor.speed + (braking ? -thrust : thrust), 0.0F, std::max(0.0F, fastest));
    }
}

bool settle_squadron_icon(SquadronIconAnchor& anchor, const std::array<float, 3> desired,
    const float snap_distance, const bool already_settled) noexcept {
    float squared = 0.0F;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const float delta = desired[axis] - anchor.position[axis];
        squared += delta * delta;
    }
    if (!already_settled && std::sqrt(squared) > snap_distance) return false;
    anchor.position = desired;
    return true;
}

SquadronIconRect squadron_icon_rect(const std::array<float, 2> centre, const float side,
    const bool pixel_align) noexcept {
    const float left = centre[0] - side * 0.5F, top = centre[1] - side * 0.5F;
    if (!pixel_align) return {left, top, side, side};
    // WSU-60: source UI +Y is up; snapping its upper edge downward in that
    // coordinate system becomes ceil(top) in screen +Y-down. Keep the scaled size.
    return {std::floor(left), std::ceil(top), side, side};
}

float display_health(const sim::tactical::InstanceDurability& durability,
    const sim::tactical::DurabilityProfile* const profile) noexcept {
    const auto max_hull = durability.max_hull.raw();
    if (max_hull <= 0) return 0.0F;
    double health = std::clamp(static_cast<double>(durability.hull.raw()) / static_cast<double>(max_hull), 0.0, 1.0);
    if (profile == nullptr || !profile->destroyed_with_hardpoints) return static_cast<float>(health);
    std::int64_t maximum = 0;
    std::int64_t current = 0;
    for (std::size_t index = 0; index < profile->hardpoints.size() && index < durability.hardpoints.size(); ++index) {
        if (!profile->hardpoints[index].destroyable) continue;
        maximum += profile->hardpoints[index].max_health.raw();
        current += std::max<std::int64_t>(durability.hardpoints[index].health.raw(), 0);
    }
    if (maximum > 0) health = std::min(health, std::clamp(static_cast<double>(current) / static_cast<double>(maximum), 0.0, 1.0));
    return static_cast<float>(health);
}

float squadron_health(const std::span<const float> live_craft_health) noexcept {
    if (live_craft_health.empty()) return 0.0F;
    double sum = 0.0;
    for (const float health : live_craft_health) sum += std::isfinite(health) ? std::clamp(health, 0.0F, 1.0F) : 0.0F;
    return static_cast<float>(sum / static_cast<double>(live_craft_health.size()));
}

float squadron_icon_screen_offset(const float screen_height) noexcept {
    return screen_height * squadron_icon_screen_offset_fraction;
}

CombatCell combat_cell_of(const std::array<float, 2> point, const std::array<float, 2> origin) noexcept {
    CombatCell cell;
    // WDF-05: truncated row and column from the box's low corner; an odd row's x is clamped to it.
    cell.y = static_cast<int>(std::trunc((point[1] - origin[1]) / combat_cell_size));
    float x = point[0];
    if ((cell.y & 1) != 0) x -= combat_cell_size * 0.5F;
    x = std::max(x, origin[0]);
    cell.x = static_cast<int>(std::trunc((x - origin[0]) / combat_cell_size));
    return cell;
}

std::array<float, 2> combat_cell_point(const CombatCell cell, const std::array<float, 2> origin) noexcept {
    std::array<float, 2> point{origin[0] + (static_cast<float>(cell.x) + 0.5F) * combat_cell_size,
                               origin[1] + (static_cast<float>(cell.y) + 0.5F) * combat_cell_size};
    if ((cell.y & 1) != 0) point[0] += combat_cell_size * 0.5F;
    return point;
}

std::array<float, 2> combat_grid_slot(const std::size_t index, const std::size_t count) noexcept {
    if (count == 0) return {};
    const auto columns = static_cast<std::size_t>(std::ceil(std::sqrt(static_cast<double>(count))));
    const float first_row = static_cast<float>(std::min(count, columns));
    return {-first_row * combat_grid_step * 0.5F + static_cast<float>(index % columns) * combat_grid_step,
            static_cast<float>(index / columns) * combat_grid_step};
}

std::optional<CombatCell> CombatGrid::cell_of(const sim::EntityId squadron) const noexcept {
    for (const Occupied& occupied : cells_) {
        if (std::find(occupied.squadrons.begin(), occupied.squadrons.end(), squadron) != occupied.squadrons.end()) {
            return occupied.cell;
        }
    }
    return std::nullopt;
}

std::optional<CombatCell> CombatGrid::record_of(const sim::EntityId squadron) const noexcept {
    const auto found = std::lower_bound(records_.begin(), records_.end(), squadron,
        [](const std::pair<sim::EntityId, CombatCell>& entry, const sim::EntityId value) { return entry.first < value; });
    if (found == records_.end() || found->first != squadron) return std::nullopt;
    return found->second;
}

CombatCell CombatGrid::search(const std::array<float, 2> target, const std::array<float, 2> origin) const noexcept {
    const CombatCell centre = combat_cell_of(target, origin);
    const auto joined = [this](const CombatCell cell) {
        return std::any_of(cells_.begin(), cells_.end(), [&](const Occupied& occupied) { return occupied.cell == cell; });
    };
    CombatCell best_cell = centre;
    float best = std::numeric_limits<float>::infinity();
    for (int y = centre.y - 1; y <= centre.y + 1; ++y) {
        for (int x = centre.x - 1; x <= centre.x + 1; ++x) {
            const CombatCell cell{x, y};
            const auto point = combat_cell_point(cell, origin);
            const float dx = point[0] - target[0];
            const float dy = point[1] - target[1];
            const float score = joined(cell) ? 0.0F : dx * dx + dy * dy;
            if (score < best) {
                best_cell = cell;
                best = score;
            }
        }
    }
    return best_cell;
}

void CombatGrid::lift(const sim::EntityId squadron) {
    for (Occupied& occupied : cells_) std::erase(occupied.squadrons, squadron);
    std::erase_if(cells_, [](const Occupied& occupied) { return occupied.squadrons.empty(); });
    std::erase_if(records_, [squadron](const auto& entry) { return entry.first == squadron; });
}

void CombatGrid::update(const std::span<const Fight> fights, const std::array<float, 2> origin) {
    // A squadron with no fight this frame leaves its cell and forgets it.
    std::vector<sim::EntityId> gone;
    for (const auto& [squadron, cell] : records_) {
        if (std::none_of(fights.begin(), fights.end(), [&](const Fight& fight) { return fight.squadron == squadron; })) {
            gone.push_back(squadron);
        }
    }
    for (const sim::EntityId squadron : gone) lift(squadron);
    const auto record = [this](const sim::EntityId squadron, const CombatCell cell) {
        const auto at = std::lower_bound(records_.begin(), records_.end(), squadron,
            [](const std::pair<sim::EntityId, CombatCell>& entry, const sim::EntityId value) { return entry.first < value; });
        records_.insert(at, {squadron, cell});
    };
    for (const Fight& fight : fights) {
        // WU-25: a target that is not a squadron, or a leader closing from beyond the strafe reach
        // on a target whose cell it does not already share, ends the dogfight.
        const auto own = record_of(fight.squadron);
        const auto theirs = fight.target_squadron != sim::invalid_entity_id ? record_of(fight.target_squadron)
                                                                              : std::nullopt;
        const bool sharing = own && theirs && *own == *theirs;
        if (fight.target_squadron == sim::invalid_entity_id || (fight.closing && !sharing)) {
            lift(fight.squadron);
            continue;
        }
        if (theirs) {
            // Join the target's cell at the end of its list (every frame, so the list keeps the
            // frame's service order).
            const CombatCell cell = *theirs;
            lift(fight.squadron);
            const auto found = std::find_if(cells_.begin(), cells_.end(),
                [&](const Occupied& occupied) { return occupied.cell == cell; });
            if (found != cells_.end()) {
                found->squadrons.push_back(fight.squadron);
            } else {
                cells_.push_back({cell, {fight.squadron}});
            }
            record(fight.squadron, cell);
            continue;
        }
        // WU-25a: search before leaving, so the squadron's own cell still counts as joined.
        const CombatCell cell = search(fight.target, origin);
        lift(fight.squadron);
        record(fight.squadron, cell);
    }
}

void CombatGrid::adopt(const std::span<const Record> records) {
    cells_.clear();
    records_.clear();
    for (const Record& entry : records) {
        const auto at = std::lower_bound(records_.begin(), records_.end(), entry.squadron,
            [](const std::pair<sim::EntityId, CombatCell>& held, const sim::EntityId value) { return held.first < value; });
        records_.insert(at, {entry.squadron, entry.cell});
        if (!entry.joined) continue;
        const auto found = std::find_if(cells_.begin(), cells_.end(),
            [&](const Occupied& occupied) { return occupied.cell == entry.cell; });
        if (found != cells_.end()) {
            found->squadrons.push_back(entry.squadron);
        } else {
            cells_.push_back({entry.cell, {entry.squadron}});
        }
    }
}

void CombatIconGrid::update(const std::span<const Cell> cells) {
    std::erase_if(held_, [&](const Held& held) {
        return std::none_of(cells.begin(), cells.end(), [&](const Cell& cell) {
            return cell.cell == held.cell && !cell.squadrons.empty();
        });
    });
    for (const auto& cell : cells) {
        if (cell.squadrons.empty()) continue;
        auto found = std::find_if(held_.begin(), held_.end(), [&](const Held& held) { return held.cell == cell.cell; });
        if (found == held_.end()) {
            held_.push_back({cell.cell, cell.height, {}});
            found = held_.end() - 1;
        }
        auto& held = *found;
        // Reserve all current owners before assigning newcomers: an earlier-ID
        // newcomer must never take a still-held slot from a later-ID squadron.
        for (auto& slot : held.slots) {
            slot.active = std::find(cell.squadrons.begin(), cell.squadrons.end(), slot.squadron) != cell.squadrons.end();
        }
        for (const auto squadron : cell.squadrons) {
            if (std::any_of(held.slots.begin(), held.slots.end(),
                [&](const Owner& slot) { return slot.squadron == squadron; })) continue;
            const auto vacant = std::find_if(held.slots.begin(), held.slots.end(), [](const Owner& slot) { return !slot.active; });
            if (vacant != held.slots.end()) *vacant = {squadron, true};
            else held.slots.push_back({squadron, true});
        }
    }
}

std::optional<CombatIconGrid::Position> CombatIconGrid::position(const CombatCell cell,
    const sim::EntityId squadron) const noexcept {
    const auto held = std::find_if(held_.begin(), held_.end(), [&](const Held& entry) { return entry.cell == cell; });
    if (held == held_.end()) return std::nullopt;
    const auto slot = std::find_if(held->slots.begin(), held->slots.end(), [&](const Owner& owner) {
        return owner.active && owner.squadron == squadron;
    });
    if (slot == held->slots.end()) return std::nullopt;
    const auto index = static_cast<std::size_t>(slot - held->slots.begin());
    // WSU-36 deliberate deviation: a fixed width prevents both re-packing and
    // first-join growth from moving existing icons. Keep the retail 30px pitch.
    constexpr std::size_t columns = 4;
    return Position{{-static_cast<float>(columns) * combat_grid_step * 0.5F
        + static_cast<float>(index % columns) * combat_grid_step,
        static_cast<float>(index / columns) * combat_grid_step}, held->height};
}

Rgb hardpoint_reticle_tint(const float health_fraction, const bool disabled) noexcept {
    if (disabled) return {128, 128, 128};
    if (health_fraction >= 0.66F) return {32, 255, 32};
    if (health_fraction >= 0.33F) return {255, 255, 32};
    return {255, 32, 32};
}

HardpointReticleArtTable hardpoint_reticle_art(const vfs::Vfs& filesystem) {
    HardpointReticleArtTable result;
    const auto document = data::load_document(filesystem, "data/xml/gameconstants.xml");
    if (!document) return result;
    const auto trim = [](std::string_view text) {
        const auto first = text.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos) return std::string{};
        return std::string(text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1));
    };
    const auto upper = [](std::string text) {
        for (char& c : text) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        return text;
    };
    for (const auto& node : document.value().root.children) {
        std::vector<std::string> HardpointReticleArt::* member{};
        const auto name = upper(node.name);
        if (name == "HARDPOINT_TARGET_RETICLE_ENEMY_TEXTURE") member = &HardpointReticleArt::enemy;
        else if (name == "HARDPOINT_TARGET_RETICLE_ENEMY_TRACKED_TEXTURE") member = &HardpointReticleArt::enemy_tracked;
        else if (name == "HARDPOINT_TARGET_RETICLE_FRIENDLY_TEXTURE") member = &HardpointReticleArt::friendly;
        else if (name == "HARDPOINT_TARGET_RETICLE_FRIENDLY_TRACKED_TEXTURE") member = &HardpointReticleArt::friendly_tracked;
        else if (name == "HARDPOINT_TARGET_RETICLE_FRIENDLY_REPAIRING_TEXTURE") member = &HardpointReticleArt::repairing;
        else if (name == "HARDPOINT_TARGET_RETICLE_FRIENDLY_DISABLED_TEXTURE") member = &HardpointReticleArt::disabled;
        else if (name == "HARDPOINT_TARGET_RETICLE_FRIENDLY_DISABLED_TRACKED_TEXTURE") member = &HardpointReticleArt::disabled_tracked;
        if (member == nullptr) continue;
        data::tag_trace::used(node);
        const auto comma = node.raw_text.find(',');
        if (comma == std::string::npos) continue;
        const auto type = upper(trim(std::string_view(node.raw_text).substr(0, comma)));
        if (type.empty()) continue;
        std::vector<std::string> textures;
        std::string_view rest(node.raw_text);
        rest.remove_prefix(comma + 1);
        while (true) {
            const auto first = rest.find_first_not_of(", \t\r\n");
            if (first == std::string_view::npos) break;
            rest.remove_prefix(first);
            const auto end = rest.find_first_of(", \t\r\n");
            textures.emplace_back(rest.substr(0, end));
            if (end == std::string_view::npos) break;
            rest.remove_prefix(end);
        }
        result[type].*member = std::move(textures);
    }
    return result;
}

std::string_view hardpoint_reticle_texture(const HardpointReticleArt& art, const float health,
    const bool hostile, const bool tracked, const bool disabled, const bool repairing, const bool repair_phase) noexcept {
    const auto& textures = hostile ? (tracked ? art.enemy_tracked : art.enemy)
        : tracked ? (health < 1.0F && !repairing ? art.disabled_tracked : art.friendly_tracked)
        : repairing ? (repair_phase ? art.repairing : art.friendly)
        : disabled ? art.disabled : art.friendly;
    if (textures.empty()) return {};
    const bool single = !hostile && (tracked ? health < 1.0F && !repairing : !repairing && disabled);
    const float fraction = std::isfinite(health) ? std::clamp(health, 0.0F, 1.0F) : 0.0F;
    const auto index = single ? std::size_t{} : std::min(textures.size() - 1,
        static_cast<std::size_t>(static_cast<double>(fraction) * static_cast<double>(textures.size())));
    return textures[index];
}

std::optional<HardpointReticleSizes> hardpoint_reticle_sizes(const vfs::Vfs& filesystem) {
    const auto document = data::load_document(filesystem, "data/xml/gameconstants.xml");
    if (!document) return std::nullopt;
    HardpointReticleSizes result;
    for (const auto& [name, destination] : std::array{
        std::pair{"HardPoint_Target_Reticle_Enemy_Screen_Size", &result.enemy},
        std::pair{"HardPoint_Target_Reticle_Friendly_Screen_Size", &result.friendly}}) {
        const data::XmlNode* found{};
        for (const auto& node : document.value().root.children) {
            const std::string_view tag(name);
            if (node.name.size() == tag.size() && std::equal(node.name.begin(), node.name.end(), tag.begin(),
                [](char a, char b) { return (a >= 'A' && a <= 'Z' ? a + 'a' - 'A' : a)
                    == (b >= 'A' && b <= 'Z' ? b + 'a' - 'A' : b); })) found = &node;
        }
        if (!found) return std::nullopt;
        data::tag_trace::used(*found);
        const auto first = found->raw_text.find_first_not_of(" \t\r\n");
        const auto last = found->raw_text.find_last_not_of(" \t\r\n");
        if (first == std::string::npos) return std::nullopt;
        const auto raw = std::string_view(found->raw_text).substr(first, last - first + 1);
        float value{};
        const auto [end, error] = std::from_chars(raw.data(), raw.data() + raw.size(), value);
        if (error != std::errc{} || end != raw.data() + raw.size() || !std::isfinite(value) || value < 0) return std::nullopt;
        *destination = value;
    }
    return result;
}

ReticleRect hardpoint_reticle_rect(const std::array<float, 2> centre, const std::array<float, 2> viewport,
                                  const float screen_size) noexcept {
    // WU-31: authored screen share, 4/3 height, centred.
    const float width = screen_size * viewport[0];
    const float height = screen_size * 4.0F / 3.0F * viewport[1];
    return {centre[0] - width * 0.5F, centre[1] - height * 0.5F, width, height};
}

std::array<float, 3> hardpoint_reticle_anchor(const std::array<float, 3>& position, const float yaw_degrees,
                                              const float pitch_degrees, const float roll_degrees,
                                              const std::array<float, 3>& attachment) noexcept {
    constexpr float degrees = 3.14159265358979323846F / 180.0F;
    const float cy = std::cos(yaw_degrees * degrees);
    const float sy = std::sin(yaw_degrees * degrees);
    const float cp = std::cos(pitch_degrees * degrees);
    const float sp = std::sin(pitch_degrees * degrees);
    const float cr = std::cos(roll_degrees * degrees);
    const float sr = std::sin(roll_degrees * degrees);
    // Rx(roll), then Ry(pitch), then Rz(yaw).
    const auto [ax, ay, az] = attachment;
    const float y1 = cr * ay - sr * az;
    const float z1 = sr * ay + cr * az;
    const float x2 = cp * ax + sp * z1;
    const float z2 = -sp * ax + cp * z1;
    return {position[0] + cy * x2 - sy * y1, position[1] + sy * x2 + cy * y1, position[2] + z2};
}

} // namespace eawr::presentation::ui
