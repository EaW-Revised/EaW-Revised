#include "galactic_object_perception.hpp"

#include "eawr/core/sha256.hpp"
#include "eawr/vfs/vfs.hpp"

#include "../script/foc/galactic_ai.hpp"

#include <algorithm>
#include <cstring>
#include <map>

namespace eawr::campaign {
namespace {
namespace ai = script::foc::ai;
namespace gc = sim::galactic;

std::string join(const std::vector<std::string>& tokens) {
    std::string text;
    for (const auto& token : tokens) text += (text.empty() ? "" : ".") + token;
    return text;
}

// The lookups an object script's equations may ask (GOL-17), read from the completed world with no fog of war: an
// object script runs for the owner of the object it is attached to.
struct WorldResolver final : ai::LookupResolver {
    const ai::EquationSet& equations;
    const gc::GalacticSnapshot& snapshot;
    gc::PlayerId player;
    std::optional<gc::PlanetId> target;
    std::uint64_t seed;
    std::string& why;
    std::uint64_t& visited;
    unsigned depth;
    WorldResolver(const ai::EquationSet& set, const gc::GalacticSnapshot& world, gc::PlayerId who, std::optional<gc::PlanetId> at,
        std::uint64_t world_seed, std::string& reason, std::uint64_t& work, unsigned nesting = 0)
        : equations(set), snapshot(world), player(who), target(at), seed(world_seed), why(reason), visited(work), depth(nesting) {}

    [[nodiscard]] bool allied(gc::PlayerId other) const {
        if (other == player) return true;
        const auto& control = snapshot.control();
        if (!control) return false;
        for (const auto& relation : control->relations)
            if (relation.player == player && relation.other == other) return relation.allied;
        return false;
    }
    [[nodiscard]] bool in_roster(gc::PlayerId id) const {
        const auto players = snapshot.players();
        return std::any_of(players.begin(), players.end(), [&](const auto& row) { return row.player_id == id; });
    }
    // WGA-185: a fleet that skips combat (stealth) is not a presence; presence is independent of strength and category.
    std::optional<ai::Real> has_space_force(bool friendly) {
        for (const auto& fleet : snapshot.fleets()) {
            ++visited;
            if (fleet.orbit != *target || fleet.members.empty() || !in_roster(fleet.owner)) continue;
            if (friendly != allied(fleet.owner)) continue;
            const auto skip = snapshot.fleet_combat_skip(fleet.id);
            if (!skip) { why = "fleet presence is unavailable: " + skip.error().message; return {}; }
            if (!skip.value()) return ai::real(1);
        }
        return ai::real(0);
    }
    std::optional<ai::Real> resolve(const ai::Lookup& lookup, const std::vector<ai::Binding>& bindings) override {
        const auto& tokens = lookup.tokens;
        if (tokens.size() == 2 && tokens[0].starts_with("FUNCTION_") && tokens[1] == "EVALUATE") {
            const auto* nested = depth >= 64 ? nullptr : equations.find(tokens[0].substr(9));
            if (!nested) { why = "function " + tokens[0] + " is not an equation"; return {}; }
            ai::AiRandom random;
            random.set_seed(static_cast<std::uint32_t>(seed) + (player + 1U) * 0x83U + 0x12345678U + ai::crc32(nested->name));
            WorldResolver inner(equations, snapshot, player, target, seed, why, visited, depth + 1);
            return ai::run(*nested, inner, random);
        }
        const bool targeted = !tokens.empty() && tokens.front() == "VARIABLE_TARGET";
        if (targeted && !target) { why = "lookup " + join(tokens) + " needs a target planet"; return {}; }
        if (targeted && tokens.size() == 3 && (tokens[1] == "FRIENDLYFORCE" || tokens[1] == "ENEMYFORCE") && tokens[2] == "HASSPACEFORCE") {
            if (!bindings.empty()) { why = "lookup " + join(tokens) + " takes no parameters here"; return {}; }
            return has_space_force(tokens[1] == "FRIENDLYFORCE");
        }
        if (targeted && tokens.size() == 2 && tokens[1] == "STARBASELEVELUNNORMALIZED") {
            const auto& production = snapshot.production();
            if (!production) { why = "lookup " + join(tokens) + " has no production data"; return {}; }
            for (const auto& row : production->planets)
                if (row.planet == *target) return ai::real(row.space_level);
            why = "lookup " + join(tokens) + " has no production row for the target planet";
            return {};
        }
        why = "lookup " + join(tokens) + " is not available to object scripts";
        return {};
    }
};
} // namespace

core::Result<std::shared_ptr<const GalacticObjectPerception>> GalacticObjectPerception::load(const vfs::Vfs& files) {
    using Result = core::Result<std::shared_ptr<const GalacticObjectPerception>>;
    auto out = std::make_shared<GalacticObjectPerception>();
    std::map<std::string, std::string, std::less<>> xml;
    const auto read = [&](const std::string& path) -> std::optional<std::string> {
        auto bytes = files.open(path);
        if (!bytes) return std::nullopt;
        std::string text(bytes.value().size(), '\0');
        if (!text.empty()) std::memcpy(text.data(), bytes.value().data(), bytes.value().size());
        return text;
    };
    for (const auto* path : {"data/xml/enum/gameobjectcategorytype.xml", "data/xml/enum/gameobjectpropertiestype.xml",
                             "data/xml/enum/aigoalcategorytype.xml"})
        if (auto text = read(path)) xml.emplace(path, std::move(*text));
    std::string identity = "eawr-object-perception-v1";
    const auto assets = files.enumerate("data/xml/ai/perceptualequations", ".xml");
    if (!assets) return Result::failure(assets.error());
    for (const auto& asset : assets.value())
        if (auto text = read(asset.canonical_path)) {
            identity += std::to_string(asset.canonical_path.size()) + ":" + asset.canonical_path + std::to_string(text->size()) + ":" + *text;
            xml.emplace(asset.canonical_path, std::move(*text));
        }
    auto equations = script::foc::galactic_ai::load_perception_equations(xml, out->notes_);
    if (!equations) return Result::failure(equations.error());
    out->equations_ = std::move(equations).value();
    out->digest_ = core::sha256_hex(std::span(reinterpret_cast<const std::uint8_t*>(identity.data()), identity.size()));
    return Result::success(std::move(out));
}

std::optional<ai::Real> GalacticObjectPerception::evaluate(std::string_view name, gc::PlayerId player,
    std::optional<gc::PlanetId> target, const gc::GalacticSnapshot& snapshot, std::uint64_t seed, std::string& why,
    std::uint64_t& visited) const {
    const auto* equation = equations_.find(name);
    if (!equation) { why = "no perception equation is named " + std::string(name); return {}; }
    if (target) {
        const auto planets = snapshot.planets();
        if (std::none_of(planets.begin(), planets.end(), [&](const auto& row) { return row.id == *target; })) {
            why = "the target is not a planet";
            return {};
        }
    }
    ai::AiRandom random;
    random.set_seed(static_cast<std::uint32_t>(seed) + (player + 1U) * 0x83U + 0x12345678U + ai::crc32(equation->name)
        + (target ? static_cast<std::uint32_t>(*target) : 0U));
    WorldResolver resolver(equations_, snapshot, player, target, seed, why, visited);
    auto value = ai::run(*equation, resolver, random);
    if (!value && why.empty()) why = "equation " + equation->name + " did not evaluate";
    return value;
}

} // namespace eawr::campaign
