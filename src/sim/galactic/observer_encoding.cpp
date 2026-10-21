#include "encoding.hpp"
#include "../replay_internal.hpp"

#include <tuple>
#include <type_traits>

namespace eawr::sim::galactic::detail {
namespace {
using Reader = sim::detail::Reader;
template<class T> auto fields(T& v) {
    using V = std::remove_cv_t<T>;
    if constexpr (std::is_same_v<V, ObserverPlayer>)
        return std::tie(v.player, v.ai, v.benefits_from_corruption, v.default_intelligence, v.minimum_visible_base_level, v.allies, v.canonical_empire);
    else if constexpr (std::is_same_v<V, ObserverLandedTransport>)
        return std::tie(v.id, v.owner, v.delete_pending, v.dead);
    else if constexpr (std::is_same_v<V, ObserverPlanet>)
        return std::tie(v.planet, v.allegiance, v.target_owner, v.space_owner, v.ground_owner, v.space_level, v.ground_level,
            v.corrupted, v.space_stealth, v.ground_stealth, v.automatic_reveal, v.landed_transports, v.political_control, v.current_owner, v.current_controller);
    else if constexpr (std::is_same_v<V, ObserverType>)
        return std::tie(v.type, v.stealth_object, v.stealth_hero, v.superweapon);
    else if constexpr (std::is_same_v<V, IntelligenceSource>)
        return std::tie(v.source, v.ability, v.text_identity, v.mask, v.final_frame);
    else if constexpr (std::is_same_v<V, ObserverRecord>)
        return std::tie(v.player, v.planet, v.discovered, v.reveal_counter, v.refresh_frame, v.mask, v.sources, v.apparent_control, v.apparent_owner);
    else if constexpr (std::is_same_v<V, ObserverSetup>)
        return std::tie(v.human_observers, v.solo_reference_human, v.reveal_delay_seconds, v.automatic_reveal_range, v.players, v.planets, v.types, v.records, v.logical_fps, v.planet_spies, v.history_players);
    else if constexpr (std::is_same_v<V, PlanetSpy>)
        return std::tie(v.planet, v.ability, v.text_identity, v.mask, v.duration, v.influence_range, v.style, v.enabled, v.despawning);
    else
        return std::tie(v.action, v.player, v.previous_player, v.planet, v.gameplay_frame, v.source, v.duration, v.influence_range, v.facts, v.clear_local, v.spy_effect, v.style, v.enabled, v.despawning, v.owner_destroyed, v.logical_fps);
}
template<class T> void encode(std::vector<std::uint8_t>& out, const T& v) {
    if constexpr (std::is_same_v<T, bool>) put_u8(out, v ? 1 : 0);
    else if constexpr (std::is_same_v<T, std::uint32_t>) put_u32(out, v);
    else if constexpr (std::is_same_v<T, std::uint64_t>) put_u64(out, v);
    else if constexpr (std::is_same_v<T, std::int64_t>) put_i64(out, v);
    else if constexpr (std::is_same_v<T, math::Fixed>) put_i64(out, v.raw());
    else if constexpr (std::is_same_v<T, ObserverAction>) put_u8(out, static_cast<std::uint8_t>(v));
    else if constexpr (std::is_same_v<T, SpyStyle>) put_u8(out, static_cast<std::uint8_t>(v));
    else if constexpr (std::is_same_v<T, std::string>) {
        put_u32(out, static_cast<std::uint32_t>(v.size()));
        for (const auto ch : v) put_u8(out, static_cast<std::uint8_t>(ch));
    } else if constexpr (requires { typename T::value_type; v.size(); }) {
        put_u32(out, static_cast<std::uint32_t>(v.size()));
        for (const auto& row : v) encode(out, row);
    } else std::apply([&](const auto&... values) { (encode(out, values), ...); }, fields(v));
}
template<class T> bool decode(Reader& in, T& v) {
    if constexpr (std::is_same_v<T, bool>) { std::uint8_t raw{}; if (!in.read_u8(raw) || raw > 1) return false; v = raw != 0; return true; }
    else if constexpr (std::is_same_v<T, std::uint32_t>) return in.read_u32(v);
    else if constexpr (std::is_same_v<T, std::uint64_t>) return in.read_u64(v);
    else if constexpr (std::is_same_v<T, std::int64_t>) return in.read_i64(v);
    else if constexpr (std::is_same_v<T, math::Fixed>) { std::int64_t raw{}; if (!in.read_i64(raw)) return false; v = math::Fixed::from_raw(raw); return true; }
    else if constexpr (std::is_same_v<T, ObserverAction>) { std::uint8_t raw{}; if (!in.read_u8(raw) || raw > 7) return false; v = static_cast<ObserverAction>(raw); return true; }
    else if constexpr (std::is_same_v<T, SpyStyle>) { std::uint8_t raw{}; if (!in.read_u8(raw) || raw > 2) return false; v = static_cast<SpyStyle>(raw); return true; }
    else if constexpr (std::is_same_v<T, std::string>) {
        std::uint32_t count{}; if (!in.read_u32(count) || count > 1024 || count > in.remaining()) return false;
        for (std::uint32_t i = 0; i < count; ++i) { std::uint8_t raw{}; if (!in.read_u8(raw)) return false; v.push_back(static_cast<char>(raw)); }
        return true;
    } else if constexpr (requires { typename T::value_type; v.size(); }) {
        std::uint32_t count{}; if (!in.read_u32(count) || count > galactic_max_objects || count > in.remaining()) return false;
        for (std::uint32_t i = 0; i < count; ++i) { typename T::value_type row{}; if (!decode(in, row)) return false; v.push_back(std::move(row)); }
        return true;
    } else return std::apply([&](auto&... values) { return (decode(in, values) && ...); }, fields(v));
}
template<class T> std::optional<T> payload(std::span<const std::uint8_t> bytes) {
    Reader in(bytes); T v;
    if (!decode(in, v) || in.remaining()) return std::nullopt;
    return v;
}
}
void encode_observer(std::vector<std::uint8_t>& bytes, const ObserverSetup& v) { put_u16(bytes, 3); encode(bytes, v); }
std::optional<ObserverSetup> decode_observer(std::span<const std::uint8_t> bytes) {
    Reader in(bytes); std::uint16_t schema{};
    ObserverSetup value;
    if (!in.read_u16(schema) || schema != 3 || !decode(in, value) || in.remaining()) return std::nullopt;
    return value;
}
void encode_observer_command(std::vector<std::uint8_t>& bytes, const ObserverCommand& v) { encode(bytes, v); }
std::optional<ObserverCommand> decode_observer_command(std::span<const std::uint8_t> bytes) {
    auto v = payload<ObserverCommand>(bytes);
    if (!v || !v->logical_fps || (v->source.mask & ~((1U << 19) - 1)) || v->source.text_identity.size() > 1024) return std::nullopt;
    return v;
}
} // namespace eawr::sim::galactic::detail
