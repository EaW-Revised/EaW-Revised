#include "eawr/presentation/animation/unit_clips.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace eawr::presentation::animation {
namespace {

static_assert(clip_type_names.back() == "HEAL", "the retail clip table has 119 types");

char lower(const char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
}

std::string lowered(const std::string_view text) {
    std::string result(text);
    for (char& value : result) value = value == '\\' ? '/' : lower(value);
    return result;
}

bool equals_ignoring_case(const std::string_view left, const std::string_view right) noexcept {
    return left.size() == right.size()
        && std::equal(left.begin(), left.end(), right.begin(),
                      [](const char a, const char b) { return lower(a) == lower(b); });
}

std::string_view trimmed(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}

} // namespace

std::vector<std::string> model_clip_paths(const std::string_view model_path, const std::size_t type,
                                          const std::function<bool(const std::string&)>& exists,
                                          const std::string_view anim_override) {
    std::vector<std::string> paths;
    const std::string model = lowered(model_path);
    if (type >= clip_type_names.size() || !model.ends_with(".alo") || !exists) return paths;
    const std::size_t slash = model.rfind('/');
    const std::string directory = slash == std::string::npos ? std::string{} : model.substr(0, slash + 1);
    std::string base = model.substr(directory.size(), model.size() - directory.size() - 4);
    if (!anim_override.empty()) {
        std::string name = lowered(anim_override);
        if (const std::size_t cut = name.rfind('/'); cut != std::string::npos) name.erase(0, cut + 1);
        if (name.ends_with(".alo")) name.resize(name.size() - 4);
        base = std::move(name);
    }
    const std::string stem = directory + base + "_" + lowered(clip_type_names[type]) + "_";
    for (std::uint32_t index = 0; index < 100; ++index) {
        const char digits[3]{static_cast<char>('0' + index / 10), static_cast<char>('0' + index % 10), '\0'};
        std::string path = stem + digits + ".ala";
        if (!exists(path)) break;
        paths.push_back(std::move(path));
    }
    return paths;
}

std::vector<ClipName> parse_clip_name(const std::string_view ala_path) {
    std::vector<ClipName> names;
    const std::string path = lowered(ala_path);
    if (!path.ends_with(".ala")) return names;
    const std::size_t slash = path.rfind('/');
    const std::string directory = slash == std::string::npos ? std::string{} : path.substr(0, slash + 1);
    const std::string_view stem = std::string_view(path).substr(directory.size(), path.size() - directory.size() - 4);
    // <base>_<TYPE>_NN: the index is the last three characters.
    if (stem.size() < 6 || stem[stem.size() - 3] != '_') return names;
    const char tens = stem[stem.size() - 2];
    const char ones = stem[stem.size() - 1];
    if (tens < '0' || tens > '9' || ones < '0' || ones > '9') return names;
    const auto index = static_cast<std::uint32_t>((tens - '0') * 10 + (ones - '0'));
    const std::string_view head = stem.substr(0, stem.size() - 3);
    for (std::size_t type = 0; type < clip_type_names.size(); ++type) {
        const std::string_view name = clip_type_names[type];
        if (head.size() < name.size() + 2) continue;
        const std::string_view tail = head.substr(head.size() - name.size());
        if (!equals_ignoring_case(tail, name) || head[head.size() - name.size() - 1] != '_') continue;
        const std::string_view base = head.substr(0, head.size() - name.size() - 1);
        names.push_back({directory + std::string(base) + ".alo", type, index});
    }
    std::stable_sort(names.begin(), names.end(), [](const ClipName& left, const ClipName& right) {
        return left.model_path.size() > right.model_path.size();
    });
    return names;
}

std::optional<std::size_t> clip_type_index(const std::string_view name) {
    const std::string_view wanted = trimmed(name);
    for (std::size_t index = 0; index < clip_type_names.size(); ++index) {
        if (equals_ignoring_case(clip_type_names[index], wanted)) return index;
    }
    return std::nullopt;
}

std::optional<std::size_t> death_clip_variant(const std::size_t variants, const std::optional<std::uint32_t> index,
                                              const std::uint64_t draw) noexcept {
    if (variants == 0) return std::nullopt;
    if (index) return *index < variants ? std::optional<std::size_t>(*index) : std::nullopt;
    return static_cast<std::size_t>(draw % variants);
}

DeathStart death_start(const bool clip_started, const bool remove_upon_death) noexcept {
    if (clip_started) return DeathStart::clip;
    return remove_upon_death ? DeathStart::removed : DeathStart::pose;
}

std::optional<std::string> death_clone_type(const std::span<const std::string> entries) {
    for (const std::string_view damage : {std::string_view("Damage_Normal"), std::string_view("Damage_Misc")}) {
        for (const std::string& entry : entries) {
            const std::size_t comma = entry.find(',');
            if (comma == std::string::npos) continue;
            const std::string_view key = trimmed(std::string_view(entry).substr(0, comma));
            const std::string_view type = trimmed(std::string_view(entry).substr(comma + 1));
            if (equals_ignoring_case(key, damage) && !type.empty()) return std::string(type);
        }
    }
    return std::nullopt;
}

std::optional<std::uint64_t> seconds_to_ticks(const double seconds, const std::uint32_t ticks_per_second) noexcept {
    if (!std::isfinite(seconds) || seconds < 0.0) return std::nullopt;
    const double ticks = std::round(seconds * static_cast<double>(ticks_per_second));
    if (ticks >= 9.0e15) return std::nullopt;
    return static_cast<std::uint64_t>(ticks);
}

std::optional<DeathFrame> death_frame(const DeathPlayback& playback, const std::uint32_t playable_frames,
                                      const std::uint32_t frames_per_second, const std::uint64_t tick,
                                      const std::uint32_t ticks_per_second) noexcept {
    if (frames_per_second == 0 || ticks_per_second == 0) return std::nullopt;
    constexpr std::uint64_t most = std::numeric_limits<std::uint64_t>::max();
    DeathFrame frame;
    frame.position.subdivisions = ticks_per_second;
    // One frame is ticks_per_second positions; a tick moves the clip by fps.
    const std::uint64_t end = static_cast<std::uint64_t>(playable_frames) * ticks_per_second;
    const std::uint64_t end_tick = end / frames_per_second + (end % frames_per_second != 0 ? 1U : 0U);
    frame.position.position = tick >= end_tick ? end : tick * frames_per_second;
    frame.blend_from = tick >= playback.blend_ticks
        ? 0.0F
        : 1.0F - static_cast<float>(static_cast<double>(tick) / static_cast<double>(playback.blend_ticks));
    if (playback.persistence_ticks) {
        if (*playback.persistence_ticks > most - end_tick
            || playback.fade_ticks > most - end_tick - *playback.persistence_ticks) {
            return std::nullopt;
        }
        frame.shown = tick < end_tick + *playback.persistence_ticks + playback.fade_ticks;
    }
    return frame;
}

core::Result<Pose> sample_death_frame(const Player& player, const DeathFrame& frame) {
    auto pose = player.sample_position(frame.position.position, frame.position.subdivisions);
    if (pose && frame.blend_from > 0.0F) {
        // The clip blends in from the bind pose (DeathBehavior's 0.5 s blend).
        const float fps = player.frames_per_second();
        const float seconds = static_cast<float>(static_cast<double>(frame.position.position)
            / static_cast<double>(frame.position.subdivisions) / static_cast<double>(fps));
        if (auto blended = player.sample({seconds, PlaybackMode::clamp, frame.blend_from})) pose = std::move(blended);
    }
    return pose;
}

} // namespace eawr::presentation::animation
