#include "eawr/skirmish/attract.hpp"

#include "eawr/data/tag_trace.hpp"
#include "skirmish_internal.hpp"

#include <algorithm>
#include <charconv>
#include <span>
#include <string_view>
#include <utility>

namespace eawr::skirmish {

namespace {

constexpr std::string_view constants_path = "data/xml/gameconstants.xml";
constexpr std::string_view maps_directory = "data/art/maps/";

[[nodiscard]] std::optional<std::int64_t> whole(const std::string_view text) {
    std::int64_t value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return std::nullopt;
    return value;
}

// The game keeps its clocks and delays in 32 bits: a delay in milliseconds and an elapsed time
// both wrap there (MAT-04), which makes a negative delay "never".
[[nodiscard]] std::uint32_t delay_ms(const std::int64_t seconds) noexcept {
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(seconds) * 1000U);
}

[[nodiscard]] std::uint32_t elapsed(const std::uint64_t now_ms, const std::uint64_t since_ms) noexcept {
    return static_cast<std::uint32_t>(now_ms) - static_cast<std::uint32_t>(since_ms);
}

} // namespace

AttractSettings attract_settings(const data::XmlNode& constants_root) {
    AttractSettings settings;
    for (const auto& child : constants_root.children) {
        const auto is = [&](const std::string_view tag) { return detail::iequals(child.name, tag); };
        if (is("Main_Menu_Demo_Attract_Mode")) {
            data::tag_trace::used(child);
            settings.menu_attract = detail::retail_flag(child.raw_text, settings.menu_attract);
        } else if (is("Demo_Attract_Maps")) {
            data::tag_trace::used(child);
            for (auto& name : detail::tokens(child.raw_text)) settings.maps.push_back(std::move(name));
        } else if (is("Demo_Attract_Map_Cycle_Delay_Seconds")) {
            data::tag_trace::used(child);
            for (const auto& token : detail::tokens(child.raw_text)) {
                if (const auto value = whole(token)) settings.cycle_delays.push_back(*value);
            }
        } else if (is("Demo_Attract_Start_Timeout_Seconds")) {
            data::tag_trace::used(child);
            if (const auto value = whole(detail::trim(child.raw_text))) settings.start_timeout_seconds = *value;
        }
    }
    return settings;
}

core::Result<AttractSettings> read_attract_settings(const vfs::Vfs& filesystem) {
    auto constants = data::load_document(filesystem, constants_path);
    if (!constants) return core::Result<AttractSettings>::failure(constants.error());
    data::tag_trace::document(constants.value().root);
    return core::Result<AttractSettings>::success(attract_settings(constants.value().root));
}

std::vector<std::pair<std::uint32_t, std::string>> attract_ai_players() {
    return {{0U, "BasicRebel"}, {6U, "AI_Player_Underworld"}};
}

std::string attract_map_path(const std::string& name) {
    const auto trimmed = detail::trim(name);
    if (trimmed.empty()) return {};
    auto canonical = vfs::canonicalize(std::string(maps_directory) + trimmed);
    if (!canonical || !canonical.value().starts_with(maps_directory)) return {};
    return std::move(canonical).value();
}

namespace {

// One mini of a chunk payload: its id byte, its size byte, then that many bytes. The first of an id
// wins (the same as the other chunk readers).
[[nodiscard]] std::optional<std::span<const std::byte>> mini_of(const std::vector<std::byte>& payload,
                                                                const std::uint8_t id) {
    std::size_t offset = 0;
    while (offset + 2 <= payload.size()) {
        const auto mini_id = static_cast<std::uint8_t>(payload[offset]);
        const auto size = static_cast<std::size_t>(static_cast<std::uint8_t>(payload[offset + 1]));
        offset += 2;
        if (offset + size > payload.size()) return std::nullopt;
        if (mini_id == id) return std::span<const std::byte>(payload.data() + offset, size);
        offset += size;
    }
    return std::nullopt;
}

// The binary32 bit pattern of the index-th value of a mini, little-endian; nullopt for a short
// mini, an infinity or a NaN.
[[nodiscard]] std::optional<std::uint32_t> binary32_bits(const std::span<const std::byte> bytes,
                                                         const std::size_t index = 0) {
    if (bytes.size() < (index + 1U) * 4U) return std::nullopt;
    std::uint32_t bits{};
    for (std::size_t byte = 0; byte < 4; ++byte) {
        bits |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[index * 4U + byte])) << (8U * byte);
    }
    if (((bits >> 23U) & 0xFFU) == 0xFFU) return std::nullopt;
    return bits;
}

// A positive normal number: sign clear, exponent neither zero nor all ones.
[[nodiscard]] bool positive_normal(const std::uint32_t bits) noexcept {
    return (bits >> 31U) == 0U && ((bits >> 23U) & 0xFFU) != 0U;
}

} // namespace

std::optional<AttractCamera> attract_camera(const assets::Map& map) {
    const assets::RawChunk* camera = nullptr;
    for (const auto& root : map.chunks) {
        if (root.id != 1U) continue;
        for (const auto& child : root.children) {
            if (child.id == 261U && !child.group) camera = &child;
        }
    }
    if (camera == nullptr) return std::nullopt;
    const auto pitch = mini_of(camera->payload, 0);
    const auto yaw = mini_of(camera->payload, 1);
    const auto look = mini_of(camera->payload, 2);
    const auto distance = mini_of(camera->payload, 3);
    const auto fov = mini_of(camera->payload, 8);
    if (!pitch || !yaw || !look || !distance || !fov) return std::nullopt;
    AttractCamera result;
    const auto read = [](const std::optional<std::uint32_t> value, std::uint32_t& into) {
        if (!value) return false;
        into = *value;
        return true;
    };
    if (!read(binary32_bits(*pitch), result.pitch_degrees_bits) || !read(binary32_bits(*yaw), result.yaw_degrees_bits)
        || !read(binary32_bits(*look, 0), result.look_at_bits[0]) || !read(binary32_bits(*look, 1), result.look_at_bits[1])
        || !read(binary32_bits(*look, 2), result.look_at_bits[2]) || !read(binary32_bits(*distance), result.distance_bits)
        || !read(binary32_bits(*fov), result.fov_degrees_bits)) {
        return std::nullopt;
    }
    if (!positive_normal(result.distance_bits) || !positive_normal(result.fov_degrees_bits)) return std::nullopt;
    return result;
}

void AttractManager::init(const std::uint64_t now_ms) noexcept {
    initialized_ = true;
    idle_ms_ = now_ms;
    map_ms_ = now_ms;
}

std::optional<std::size_t> AttractManager::advance(const std::uint64_t now_ms) noexcept {
    if (!initialized_ || settings_.maps.empty()) return std::nullopt;
    active_ = true;
    ++index_;
    if (index_ < 0 || static_cast<std::size_t>(index_) >= settings_.maps.size()) index_ = 0;
    map_ms_ = now_ms;
    return static_cast<std::size_t>(index_);
}

std::int64_t AttractManager::delay_seconds() const noexcept {
    if (index_ >= 0 && static_cast<std::size_t>(index_) < settings_.cycle_delays.size()) {
        return settings_.cycle_delays[static_cast<std::size_t>(index_)];
    }
    return attract_default_delay_seconds;
}

bool AttractManager::map_expired(const std::uint64_t now_ms) const noexcept {
    return delay_ms(delay_seconds()) < elapsed(now_ms, map_ms_);
}

AttractManager::Service AttractManager::service(const AttractFrame& frame) noexcept {
    Service result;
    if (!initialized_) return result;
    paused_ = delay_ms(settings_.start_timeout_seconds) < elapsed(frame.now_ms, idle_ms_);
    result.paused = paused_;
    if (!active_ || paused_) return result;
    if (!frame.dialog_active) {
        if (map_expired(frame.now_ms)) result.action = AttractAction::advance;
        return result;
    }
    // MAT-04: with a dialog up the same test also waits for the player to stop touching things
    // and for the window to be in the foreground.
    if (!frame.foreground) return result;
    const std::uint64_t wait = frame.main_menu_active ? attract_menu_idle_ms : attract_dialog_idle_ms;
    if (wait < elapsed(frame.now_ms, idle_ms_) && map_expired(frame.now_ms)) result.action = AttractAction::advance;
    return result;
}

AttractManager::KeyResult AttractManager::key(const AttractKey pressed, const AttractFrame& frame) noexcept {
    KeyResult result;
    idle_ms_ = frame.now_ms;
    if (!initialized_ || !active_ || frame.dialog_active) return result;
    switch (pressed) {
    case AttractKey::backspace:
        // The index steps back, then the advance steps it forward: the current map starts again.
        --index_;
        result.consumed = true;
        result.action = AttractAction::advance;
        break;
    case AttractKey::enter:
        result.consumed = true;
        result.action = AttractAction::advance;
        break;
    case AttractKey::escape:
        shutdown();
        result.consumed = true;
        result.action = AttractAction::stop;
        break;
    case AttractKey::minus: {
        // MAT-07: the map's remaining time is skipped by moving its clock back one delay.
        const std::uint64_t delay = delay_ms(delay_seconds());
        map_ms_ = delay < map_ms_ ? map_ms_ - delay : 1U;
        result.consumed = true;
        break;
    }
    case AttractKey::other: break;
    }
    return result;
}

void AttractManager::pointer_moved(const std::uint64_t now_ms, const bool main_menu_active) noexcept {
    if ((initialized_ && !active_ && main_menu_active) || active_) idle_ms_ = now_ms;
}

void AttractManager::shutdown() noexcept {
    if (!initialized_) return;
    initialized_ = false;
    active_ = false;
}

} // namespace eawr::skirmish
