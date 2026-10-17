#include "galactic_ai_world.hpp"

#include <algorithm>
#include <tuple>

namespace eawr::script::foc::ai {
core::Result<void> GalacticWorld::publish_execution_routes(bool available, std::uint64_t revision,
    std::vector<GalacticExecutionRoute> routes) {
    const auto invalid = [](std::string message) {
        core::Diagnostic error; error.code = std::string(authoritative::codes::invalid_request); error.message = std::move(message);
        return core::Result<void>::failure(std::move(error));
    };
    if (!snapshot_ || (!available && !routes.empty()) || (available && !planets_.empty() && routes.empty()))
        return invalid("execution publication requires current world and consistent availability");
    const auto key = [](const auto& row) { return std::tuple{row.player, row.source, row.candidate}; };
    std::sort(routes.begin(), routes.end(), [&](const auto& a, const auto& b) { return key(a) < key(b); });
    std::map<tactical::PlayerId, std::size_t> counts;
    for (std::size_t index = 0; index < routes.size(); ++index) {
        const auto& row = routes[index];
        if (!player(row.player) || !planet(row.source) || !planet(row.candidate)
            || (index && key(routes[index - 1]) == key(row))) return invalid("execution publication has unresolved or duplicate route identity");
        if (!row.empty && (!numeric::binary64::is_finite(row.distance.repr) || !numeric::binary64::is_finite(row.threat.repr)
            || !numeric::binary64::is_finite(row.prefix_threat.repr) || row.distance < Real{} || row.threat < Real{}
            || row.prefix_threat < Real{} || row.threat < row.prefix_threat)) return invalid("execution publication has invalid decoded costs");
        ++counts[row.player];
    }
    for (const auto& [owner, count] : counts) {
        static_cast<void>(owner);
        if (count != planets_.size() * planets_.size()) return invalid("execution publication requires a complete player matrix");
    }
    auto next = std::make_shared<GalacticConnectivity>(*connectivity_);
    next->execution_available = available; next->execution_routes = std::move(routes); next->player_routes.clear();
    next->revision = revision; work_.prepared_routes = next->execution_routes.size();
    connectivity_ = std::move(next);
    return core::Result<void>::success();
}
core::Result<void> GalacticWorld::publish_execution_route_generations(bool available, std::uint64_t revision,
    std::map<tactical::PlayerId, std::shared_ptr<const std::vector<GalacticExecutionRoute>>> generations) {
    const auto invalid = [](std::string message) {
        return core::Result<void>::failure(detail::make_error(authoritative::codes::invalid_request, std::move(message)));
    };
    if (!snapshot_ || (available && !planets_.empty() && generations.empty()))
        return invalid("execution generations require a current world and complete availability");
    std::uint64_t count{};
    for (const auto& [owner, rows] : generations) {
        if (!player(owner) || !rows || rows->size() != planets_.size() * planets_.size())
            return invalid("execution generation requires a complete player matrix");
        const auto previous = connectivity_->player_routes.find(owner);
        if (previous == connectivity_->player_routes.end() || previous->second != rows) {
            std::optional<std::pair<galactic::PlanetId, galactic::PlanetId>> last;
            for (const auto& row : *rows) {
                const auto key = std::pair{row.source, row.candidate};
                if (row.player != owner || !planet(row.source) || !planet(row.candidate) || (last && key <= *last))
                    return invalid("execution generation has unresolved, unordered or duplicate route identity");
                if (!row.empty && (!numeric::binary64::is_finite(row.distance.repr) || !numeric::binary64::is_finite(row.threat.repr)
                    || !numeric::binary64::is_finite(row.prefix_threat.repr) || row.distance < Real{} || row.threat < Real{}
                    || row.prefix_threat < Real{} || row.threat < row.prefix_threat))
                    return invalid("execution generation has invalid decoded costs");
                last = key;
            }
        }
        count += rows->size();
    }
    auto next = std::make_shared<GalacticConnectivity>(*connectivity_);
    next->execution_available = available; next->revision = revision;
    next->execution_routes.clear(); next->player_routes = std::move(generations);
    connectivity_ = std::move(next); work_.prepared_routes = count;
    return core::Result<void>::success();
}
const Engine* GalacticFindTargetQueries::manager(FindTargetManager mode) const {
    return mode == FindTargetManager::galactic ? &galactic_ : current_;
}
const Target* GalacticFindTargetQueries::source_target(const Engine& selected, sim::EntityId object) const {
    if (&selected == &galactic_) {
        const auto id = world_.containing_planet(object);
        return id ? selected.target_of_object(*id) : nullptr;
    }
    if (&selected != current_) return nullptr;
    return current_queries_ ? current_queries_->source_target(selected, object) : selected.target_of_object(object);
}
std::optional<FindTargetPath> GalacticFindTargetQueries::execution_path(const Engine& selected,
    tactical::PlayerId player, const Target& source, const Target& candidate) const {
    if (&selected != &galactic_) {
        if (&selected != current_) return std::nullopt;
        if (current_queries_) return current_queries_->execution_path(selected, player, source, candidate);
        if (current_mode_ == galactic_ai::Mode::land) {
            FindTargetPath missing; missing.service = FindTargetService::land; missing.source_valid = false;
            return missing; // named land-provider dependency, not a space bypass
        }
        return std::nullopt; // FT-12 space targets have no reachability service
    }
    const auto source_planet = world_.containing_planet(source.object);
    const auto candidate_planet = world_.containing_planet(candidate.object);
    FindTargetPath out;
    out.source_valid = source_planet && candidate_planet && *source_planet == source.object && *candidate_planet == candidate.object;
    if (!out.source_valid) return out;
    out.same_target = *source_planet == *candidate_planet;
    if (out.same_target) return out; // FT-08
    if (const auto* route = world_.execution_route(player, *source_planet, *candidate_planet)) {
        out.empty = route->empty; out.distance = route->distance;
        out.threat = route->threat; out.prefix_threat = route->prefix_threat;
    } else {
        // This is a missing capability, not a fabricated empty execution route.
        if (world_.execution_available()) out.empty = true; // FT-15; binding owns fallback decoding
        else out.source_valid = false;
    }
    return out;
}
const GalacticExecutionRoute* GalacticWorld::execution_route(tactical::PlayerId owner,
    galactic::PlanetId source, galactic::PlanetId candidate) const {
    if (!planet(source) || !planet(candidate)) return nullptr;
    if (const auto generation = connectivity_->player_routes.find(owner); generation != connectivity_->player_routes.end()) {
        const auto& rows = *generation->second;
        const auto key = std::tuple{owner, source, candidate};
        const auto found = std::lower_bound(rows.begin(), rows.end(), key, [](const auto& row, const auto& value) {
            return std::tuple{row.player, row.source, row.candidate} < value;
        });
        return found != rows.end() && std::tuple{found->player, found->source, found->candidate} == key ? &*found : nullptr;
    }
    for (const auto& route : connectivity_->execution_routes)
        if (route.player == owner && route.source == source && route.candidate == candidate) return &route;
    return nullptr;
}
std::optional<bool> GalacticWorld::goal_reachable(tactical::PlayerId owner,
    galactic::PlanetId source, galactic::PlanetId candidate, std::string_view policy) const {
    if (!planet(source) || !planet(candidate) || !player(owner)) return false;
    const auto canonical = upper_case(trimmed(policy));
    static const std::set<std::string> accepted{"FRIENDLY_ONLY", "FRIENDLY_IGNORE_THREAT", "ENEMY_DESTINATION",
        "ENEMY_UNDEFENDED", "SINGLE_HOP_DISCONNECTED", "ANY"};
    if (!accepted.contains(canonical)) return false;
    if (source == candidate) return true; // WGA-18
    for (const auto& relation : connectivity_->goal_reach)
        if (relation.player == owner && relation.source == source && upper_case(relation.policy) == canonical)
            return std::find(relation.production_sites.begin(), relation.production_sites.end(), candidate) != relation.production_sites.end();
    return {}; // named production-site producer dependency, never an execution-cost substitute
}
} // namespace eawr::script::foc::ai
