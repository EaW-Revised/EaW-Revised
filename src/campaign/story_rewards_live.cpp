#include "eawr/campaign/story_rewards_live.hpp"
#include "eawr/campaign/starting_forces.hpp"
#include "eawr/campaign/tactical_return_live.hpp"
#include "eawr/sim/math/fixed.hpp"
#include "eawr/sim/math/geometry.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace eawr::campaign {
namespace gc = sim::galactic;
namespace st = sim::galactic::story;
using Outcome = GameSession::StoryRewardOutcome;
using Fixed = sim::math::Fixed;

namespace {
// Journal payloads of native effect kind 20 (story_reward): an operation byte, then little-endian fields.
constexpr std::uint8_t op_spawn = 1;   // uint32 player, uint64 planet, uint64 type
constexpr std::uint8_t op_credits = 2; // uint32 player, int64 Q24 raw amount

template<class T> core::Result<T> failure(std::string message) {
    core::Diagnostic d; d.code = std::string(gc::diagnostic_codes::invalid_setup);
    d.message = "story rewards: " + std::move(message); return core::Result<T>::failure(std::move(d));
}
core::Result<Outcome> declined(std::string note) { return core::Result<Outcome>::success(Outcome{false, std::move(note)}); }
core::Result<Outcome> applied() { return core::Result<Outcome>::success(Outcome{true, {}}); }

std::string upper(std::string_view text) {
    std::string out(text);
    for (auto& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}
std::string_view trimmed(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return text;
}
std::optional<std::int64_t> integer(std::string_view text) {
    text = trimmed(text);
    std::int64_t value{};
    const auto end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), end, value);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != end) return std::nullopt;
    return value;
}
void put(std::vector<std::uint8_t>& bytes, std::uint64_t value, unsigned size) {
    for (unsigned i = 0; i < size; ++i) { bytes.push_back(static_cast<std::uint8_t>(value)); value >>= 8U; }
}
struct Cursor {
    std::span<const std::uint8_t> bytes;
    std::size_t at{};
    bool read(std::uint64_t& value, unsigned size) {
        if (size > bytes.size() - at) return false;
        value = 0;
        for (unsigned i = 0; i < size; ++i) value |= static_cast<std::uint64_t>(bytes[at++]) << (i * 8U);
        return true;
    }
    [[nodiscard]] bool done() const noexcept { return at == bytes.size(); }
};

// The first token of one reward parameter slot (WST-05), or empty.
std::string_view token(const st::Triggered& record, std::size_t slot) {
    return record.reward_params[slot].empty() ? std::string_view{} : trimmed(record.reward_params[slot].front());
}

struct Rewards {
    CampaignSetupPlan plan;
    std::map<std::string, gc::TypeId> types;    // upper-case original name -> runtime type (every registered original)
    std::map<std::string, gc::PlanetId> planets; // upper-case planet name -> its first instance (lowest id)
    std::string good_side;                       // GameConstants Good_Side_Name (WST-64)
    std::shared_ptr<const StartingForceInputs> force; // rebuilt from the live authoring on first use

    [[nodiscard]] std::optional<gc::TypeId> type(std::string_view name) const {
        const auto at = types.find(upper(trimmed(name)));
        return at == types.end() ? std::nullopt : std::optional(at->second);
    }
    [[nodiscard]] const std::string* faction(gc::PlayerId player) const {
        for (const auto& row : plan.player_bindings()) if (row.player.player_id == player) return &row.faction;
        return nullptr;
    }
    [[nodiscard]] std::optional<gc::PlayerId> player_of(std::string_view faction_name) const {
        for (const auto& row : plan.player_bindings())
            if (row.active && upper(row.faction) == upper(trimmed(faction_name))) return row.player.player_id;
        return std::nullopt;
    }
    core::Result<void> ensure_force(const gc::GalacticSnapshot& snapshot) {
        if (force) return core::Result<void>::success();
        auto rebuilt = rebuild_starting_force_inputs(snapshot, plan);
        if (!rebuilt) return core::Result<void>::failure(rebuilt.error());
        force = std::move(rebuilt).value();
        return core::Result<void>::success();
    }
    [[nodiscard]] bool hostile(gc::PlayerId player, gc::PlayerId other) const {
        if (!force) return false;
        return std::any_of(force->relations.begin(), force->relations.end(),
            [&](const auto& row) { return row.player == player && row.other == other && row.enemy; });
    }

    core::Result<Outcome> apply(gc::GalacticSession&, const st::Triggered&, const sim::PartitionExecutor&);
    core::Result<void> replay(gc::GalacticSession&, const gc::NativeEffect&, const sim::PartitionExecutor&);

private:
    core::Result<Outcome> spawn(gc::GalacticSession&, const st::Triggered&, const sim::PartitionExecutor&);
    core::Result<Outcome> buildable(gc::GalacticSession&, const st::Triggered&);
    core::Result<Outcome> lock(gc::GalacticSession&, const st::Triggered&);
    core::Result<Outcome> tech(gc::GalacticSession&, const st::Triggered&, bool maximum_only);
    core::Result<Outcome> credits(gc::GalacticSession&, const st::Triggered&);
    core::Result<std::optional<gc::PlanetId>> destination(const gc::GalacticSnapshot&, gc::PlayerId,
        std::optional<gc::PlanetId> named, std::string& reason) const;
    core::Result<bool> spawn_one(gc::GalacticSession&, gc::PlayerId, gc::PlanetId, gc::TypeId, const sim::PartitionExecutor&);
    core::Result<void> credit(gc::GalacticSession&, gc::PlayerId, Fixed);
};

core::Result<Outcome> Rewards::apply(gc::GalacticSession& world, const st::Triggered& record, const sim::PartitionExecutor& executor) {
    const std::string_view kind = record.reward_type;
    if (kind == "SPAWN_HERO" || kind == "UNIQUE_UNIT") return spawn(world, record, executor);
    if (kind == "BUILDABLE_UNIT") return buildable(world, record);
    if (kind == "LOCK_UNIT") return lock(world, record);
    if (kind == "SET_TECH_LEVEL") return tech(world, record, false);
    if (kind == "SET_MAX_TECH_LEVEL") return tech(world, record, true);
    if (kind == "CREDITS") return credits(world, record);
    return declined("this reward type has no live owner yet");
}

// WST-40 placement: the named planet's first instance; when the galaxy has no such planet, the first planet the
// owner holds (ascending id: the enumeration order is unverified, WST-U15, interim). The anchor then gives way to the
// nearest planet the owner holds (the anchor itself when held); an enemy fleet at that orbit sends the unit to the
// nearest planet whose orbit has none. Nearest = squared distance, then lowest id.
core::Result<std::optional<gc::PlanetId>> Rewards::destination(const gc::GalacticSnapshot& snapshot, gc::PlayerId owner,
    std::optional<gc::PlanetId> named, std::string& reason) const {
    using Result = core::Result<std::optional<gc::PlanetId>>;
    const auto planets_now = snapshot.planets();
    const auto held = [&](const gc::Planet& planet) { return planet.owner == owner; };
    const auto find = [&](gc::PlanetId id) {
        return std::find_if(planets_now.begin(), planets_now.end(), [&](const auto& row) { return row.id == id; });
    };
    auto anchor = named ? find(*named) : planets_now.end();
    if (anchor == planets_now.end()) anchor = std::find_if(planets_now.begin(), planets_now.end(), held);
    if (anchor == planets_now.end()) { reason = "the owner holds no planet (WST-40)"; return Result::success(std::nullopt); }
    const auto nearest = [&](const gc::Planet& from, const auto& allowed) -> core::Result<const gc::Planet*> {
        const gc::Planet* best = nullptr; Fixed best_distance;
        for (const auto& planet : planets_now) {
            if (!allowed(planet)) continue;
            const auto x = sim::math::subtract(planet.position.x, from.position.x);
            const auto y = sim::math::subtract(planet.position.y, from.position.y);
            const auto z = sim::math::subtract(planet.position.z, from.position.z);
            if (!x || !y || !z) return failure<const gc::Planet*>("planet distance overflows");
            const sim::math::Vec3 delta{x.value(), y.value(), z.value()};
            const auto distance = sim::math::dot(delta, delta);
            if (!distance) return core::Result<const gc::Planet*>::failure(distance.error());
            if (!best || distance.value() < best_distance) { best = &planet; best_distance = distance.value(); }
        }
        return core::Result<const gc::Planet*>::success(best);
    };
    const gc::Planet* site = &*anchor;
    if (!held(*site)) {
        auto owned = nearest(*anchor, held);
        if (!owned) return Result::failure(owned.error());
        site = owned.value();
    }
    if (!site) { reason = "the owner holds no planet (WST-40)"; return Result::success(std::nullopt); }
    const auto occupied = [&](gc::PlanetId id) {
        const auto fleets = snapshot.fleets();
        return std::any_of(fleets.begin(), fleets.end(), [&](const auto& fleet) {
            return fleet.orbit == id && !fleet.motion.transit && fleet.owner != owner && hostile(owner, fleet.owner);
        });
    };
    if (occupied(site->id)) {
        auto safe = nearest(*site, [&](const gc::Planet& planet) { return !occupied(planet.id); });
        if (!safe) return Result::failure(safe.error());
        site = safe.value();
        if (!site) { reason = "no planet has a safe orbit (WST-40)"; return Result::success(std::nullopt); }
    }
    return Result::success(std::optional(site->id));
}

core::Result<bool> Rewards::spawn_one(gc::GalacticSession& world, gc::PlayerId player, gc::PlanetId planet, gc::TypeId type,
    const sim::PartitionExecutor& executor) {
    std::vector<std::uint8_t> bytes{op_spawn};
    put(bytes, player, 4); put(bytes, planet, 8); put(bytes, type, 8);
    if (auto recorded = world.append_native_effect(gc::NativeEffectKind::story_reward, std::move(bytes)); !recorded)
        return core::Result<bool>::failure(recorded.error());
    const gc::GalacticSession::NativeProductionRequest request{player, planet, type, {}};
    auto placed = world.place_due_hero(request, stock_native_force_builder(force, request), executor);
    if (!placed) return core::Result<bool>::failure(placed.error());
    return core::Result<bool>::success(placed.value().primary != 0);
}

core::Result<void> Rewards::credit(gc::GalacticSession& world, gc::PlayerId player, Fixed amount) {
    auto one = Fixed::from_integer(1);
    if (!one) return core::Result<void>::failure(one.error());
    auto added = world.add_credits(player, amount, one.value());
    return added ? core::Result<void>::success() : core::Result<void>::failure(added.error());
}

// WST-40: SPAWN_HERO and UNIQUE_UNIT run the same code (1 type, 2 planet, 3 count, default 1).
core::Result<Outcome> Rewards::spawn(gc::GalacticSession& world, const st::Triggered& record, const sim::PartitionExecutor& executor) {
    if (token(record, 0).empty()) return declined("names no unit type (WST-40)");
    const auto unit = type(token(record, 0));
    if (!unit) return declined("unit type " + std::string(token(record, 0)) + " is not a registered original type");
    // The event gives up when it names no planet and was triggered without an object; plot triggers carry none.
    if (token(record, 1).empty()) return declined("names no planet and was triggered without an object (WST-40)");
    std::int64_t count = 1;
    if (!token(record, 2).empty()) {
        const auto parsed = integer(token(record, 2));
        if (!parsed || *parsed < 1 || *parsed > 1024) return declined("count " + std::string(token(record, 2)) + " is not a number from 1 to 1024");
        count = *parsed;
    }
    auto snapshot = world.snapshot();
    if (auto ready = ensure_force(*snapshot); !ready) return core::Result<Outcome>::failure(ready.error());
    if (std::none_of(force->bindings.begin(), force->bindings.end(), [&](const auto& row) { return row.id == *unit; }))
        return declined("unit type " + std::string(token(record, 0)) + " is not part of the live creation inputs");
    std::optional<gc::PlanetId> named;
    if (const auto at = planets.find(upper(token(record, 1))); at != planets.end()) named = at->second;
    std::string reason;
    auto site = destination(*snapshot, record.owner, named, reason);
    if (!site) return core::Result<Outcome>::failure(site.error());
    if (!site.value()) return declined(reason);
    for (std::int64_t i = 0; i < count; ++i) {
        auto placed = spawn_one(world, record.owner, *site.value(), *unit, executor);
        if (!placed) return core::Result<Outcome>::failure(placed.error());
        if (!placed.value()) return declined("the placement produced no unit (WGM-132)");
    }
    return applied();
}

// WST-64: the unlock type of parameter 1, then the lock type of parameter 2 (an empty or unresolved one skips its
// half), then the faction post-check, which runs even when both are absent.
core::Result<Outcome> Rewards::buildable(gc::GalacticSession& world, const st::Triggered& record) {
    gc::GalacticSession::ProductionLockWrite write;
    write.player = record.owner;
    if (const auto unlock = token(record, 0).empty() ? std::nullopt : type(token(record, 0))) write.unlock = *unlock;
    if (const auto lock_type = token(record, 1).empty() ? std::nullopt : type(token(record, 1))) write.lock = *lock_type;
    const auto* faction_name = faction(record.owner);
    write.slicer_check = faction_name && !good_side.empty() && upper(*faction_name) == upper(good_side);
    auto written = world.apply_production_lock(write);
    if (!written) return core::Result<Outcome>::failure(written.error());
    return applied();
}

core::Result<Outcome> Rewards::lock(gc::GalacticSession& world, const st::Triggered& record) {
    const auto locked = token(record, 0).empty() ? std::nullopt : type(token(record, 0));
    if (!locked) return declined("names no registered original type (WST-40)");
    auto written = world.apply_production_lock({record.owner, 0, *locked, false});
    if (!written) return core::Result<Outcome>::failure(written.error());
    return applied();
}

// WST-41: SET_TECH_LEVEL (1 faction, 2 level) raises the maximum when needed, then sets the level;
// SET_MAX_TECH_LEVEL sets the maximum alone. Both parameters are required.
core::Result<Outcome> Rewards::tech(gc::GalacticSession& world, const st::Triggered& record, bool maximum_only) {
    if (token(record, 0).empty() || token(record, 1).empty()) return declined("needs a faction and a level (WST-41)");
    const auto player = player_of(token(record, 0));
    if (!player) return declined("no active player of faction " + std::string(token(record, 0)));
    const auto level = integer(token(record, 1));
    if (!level || *level < 0) return declined("level " + std::string(token(record, 1)) + " is not a non-negative number");
    const auto snapshot = world.snapshot();
    const gc::GalacticPlayerEconomy* account = nullptr;
    if (const auto& economy = snapshot->player_economy())
        for (const auto& row : economy->players) if (row.player == *player) account = &row;
    if (!account) return declined("the faction has no technology account");
    if (maximum_only && *level < account->current_tech) return declined("a maximum below the current level " + std::to_string(account->current_tech) + " is refused");
    gc::GalacticSession::TechnologyWrite write;
    write.player = *player; write.target = *level;
    write.process_unlocks = !maximum_only; write.request_sound = !maximum_only;
    write.maximum = maximum_only ? gc::GalacticSession::TechnologyMaximum::set_only : gc::GalacticSession::TechnologyMaximum::raise_to_target;
    auto written = world.set_technology(write);
    if (!written) return core::Result<Outcome>::failure(written.error());
    return applied();
}

core::Result<Outcome> Rewards::credits(gc::GalacticSession& world, const st::Triggered& record) {
    const auto amount = token(record, 0).empty() ? std::nullopt : integer(token(record, 0));
    if (!amount) return declined("amount " + std::string(token(record, 0)) + " is not a whole number");
    auto value = Fixed::from_integer(*amount);
    if (!value) return declined("amount " + std::to_string(*amount) + " is out of range");
    std::vector<std::uint8_t> bytes{op_credits};
    put(bytes, record.owner, 4); put(bytes, static_cast<std::uint64_t>(value.value().raw()), 8);
    if (auto recorded = world.append_native_effect(gc::NativeEffectKind::story_reward, std::move(bytes)); !recorded)
        return core::Result<Outcome>::failure(recorded.error());
    if (auto added = credit(world, record.owner, value.value()); !added) return core::Result<Outcome>::failure(added.error());
    return applied();
}

core::Result<void> Rewards::replay(gc::GalacticSession& world, const gc::NativeEffect& effect, const sim::PartitionExecutor& executor) {
    Cursor in{effect.payload};
    std::uint64_t op{}, player{}, a{}, b{};
    if (!in.read(op, 1) || !in.read(player, 4)) return failure<void>("invalid story reward journal");
    if (op == op_spawn) {
        if (!in.read(a, 8) || !in.read(b, 8) || !in.done()) return failure<void>("invalid story spawn journal");
        if (auto ready = ensure_force(*world.snapshot()); !ready) return ready;
        auto placed = spawn_one(world, static_cast<gc::PlayerId>(player), a, b, executor); // the journal is already applying
        return placed ? core::Result<void>::success() : core::Result<void>::failure(placed.error());
    }
    if (op == op_credits) {
        if (!in.read(a, 8) || !in.done()) return failure<void>("invalid story credits journal");
        return credit(world, static_cast<gc::PlayerId>(player), Fixed::from_raw(static_cast<std::int64_t>(a)));
    }
    return failure<void>("unknown story reward journal operation");
}
} // namespace

core::Result<void> bind_story_rewards(GameSession& game, const CampaignSetupPlan& plan, const data::galactic::GalacticData& data) {
    auto state = std::make_shared<Rewards>(Rewards{plan, {}, {}, {}, {}});
    for (const auto& binding : plan.type_bindings()) state->types.emplace(upper(binding.original_type), binding.id);
    for (const auto& location : plan.locations()) {
        if (!location.planet) continue;
        const auto key = upper(location.original_type);
        const auto at = state->planets.find(key);
        if (at == state->planets.end()) state->planets.emplace(key, location.planet->id);
        else at->second = std::min(at->second, location.planet->id); // WST-40: the first instance
    }
    if (const auto* side = data.constant("Good_Side_Name")) state->good_side = std::string(trimmed(side->text));
    game.set_story_rewards([state](gc::GalacticSession& world, const st::Triggered& record, const sim::PartitionExecutor& executor) {
        return state->apply(world, record, executor);
    });
    return game.configure_native_replay_owners([state](gc::GalacticSession& world) {
        world.set_native_effect_replayer(gc::NativeEffectKind::story_reward,
            [state](gc::GalacticSession& owner, const gc::NativeEffect& effect, const sim::PartitionExecutor& executor) {
                return state->replay(owner, effect, executor);
            });
        return core::Result<void>::success();
    });
}

} // namespace eawr::campaign
