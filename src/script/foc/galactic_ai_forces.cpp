#include "galactic_ai_world.hpp"
#include "eawr/core/gc_profile.hpp"
#include "eawr/script/numeric/binary64.hpp"

namespace eawr::script::foc::ai {
namespace {
Real distance(const math::Vec3& a, const math::Vec3& b) {
    // WGA-192: the neighbourhood reads single-precision 3D distance.
    const auto x = to_single(numeric::from_fixed(a.x) - numeric::from_fixed(b.x));
    const auto y = to_single(numeric::from_fixed(a.y) - numeric::from_fixed(b.y));
    const auto z = to_single(numeric::from_fixed(a.z) - numeric::from_fixed(b.z));
    const auto squared = to_single(to_single(to_single(x * x) + to_single(y * y)) + to_single(z * z));
    Real out; out.repr = numeric::binary64::square_root(squared.repr); return to_single(out);
}
Real coefficient_distance(const math::Vec3& a, const math::Vec3& b) {
    // WGA-490: retail positions are binary32 before vector subtraction.
    const auto delta = [](auto x, auto y) { return to_single(to_single(numeric::from_fixed(x)) - to_single(numeric::from_fixed(y))); };
    const auto x = delta(a.x, b.x), y = delta(a.y, b.y), z = delta(a.z, b.z);
    const auto squared = to_single(to_single(to_single(x * x) + to_single(y * y)) + to_single(z * z));
    Real out; out.repr = numeric::binary64::square_root(squared.repr); return to_single(out);
}
std::optional<Real> number(const std::vector<Binding>& bindings, std::string_view parameter) {
    for (const auto& binding : bindings) if (binding.parameter == parameter)
        if (const auto* value = std::get_if<Real>(&binding.value)) return *value;
    return {};
}
std::uint64_t category(const std::vector<Binding>& bindings) {
    const auto value = number(bindings, "PARAMETER_CATEGORY");
    return value ? static_cast<std::uint64_t>(*value) : ~std::uint64_t{};
}
bool text_match(const std::vector<Binding>& bindings, std::string_view parameter, std::string_view value) {
    for (const auto& binding : bindings) if (binding.parameter == parameter)
        if (const auto* text = std::get_if<std::string>(&binding.value); text && upper_case(*text) == value) return true;
    return false;
}
bool has_parameter(const std::vector<Binding>& bindings, std::string_view parameter) {
    return std::any_of(bindings.begin(), bindings.end(), [&](const auto& binding) { return binding.parameter == parameter; });
}
Real normalized(Real value, Real denominator) {
    // WGA-188: zero numerator is zero; otherwise clamp the quotient to [0,1].
    if (value == Real{}) return Real{};
    if (denominator < value) return real(1);
    const auto result = value / denominator;
    return std::max(Real{}, std::min(real(1), result));
}
}

core::Result<std::vector<PathCoefficient>> build_path_coefficients(const PathCoefficientInput& input, std::vector<std::string>* diagnostics) {
    using Result = core::Result<std::vector<PathCoefficient>>;
    const auto invalid = [](std::string message) {
        return Result::failure(detail::make_error(authoritative::codes::invalid_request, std::move(message)));
    };
    const auto n = input.planets.size();
    if ((n && n > std::numeric_limits<std::size_t>::max() / n)
        || !numeric::binary64::is_finite(input.maximum_distance.repr)
        || !numeric::binary64::is_finite(input.trade_movement_factor.repr))
        return invalid("path coefficients require representable geometry and finite constants");
    std::set<galactic::PlanetId> identities;
    for (const auto& planet : input.planets)
        if (!planet.planet || !identities.insert(planet.planet).second)
            return invalid("path coefficients require unique nonzero planet identities");
    std::set<std::pair<galactic::PlanetId, galactic::PlanetId>> trades;
    for (const auto& [from, to] : input.trades) {
        // WGA-490 queries trade existence only between actual planet objects.
        if (!identities.contains(from) || !identities.contains(to)) {
            if (diagnostics) diagnostics->push_back("path coefficient trade edge skipped: endpoint outside snapshot "
                + std::to_string(from) + " -> " + std::to_string(to));
            continue;
        }
        trades.emplace(from, to); trades.emplace(to, from);
    }
    std::vector<Real> edges(n * n, real(-1));
    std::vector<bool> incident(n);
    for (std::size_t from = 0; from < n; ++from) {
        edges[from * n + from] = Real{};
        if (!input.planets[from].allied) continue;
        for (std::size_t to = 0; to < n; ++to) {
            if (from == to || (input.internal && !input.planets[to].allied)) continue;
            auto cost = coefficient_distance(input.planets[from].position, input.planets[to].position);
            if (trades.contains({input.planets[from].planet, input.planets[to].planet}))
                cost = cost / to_single(input.trade_movement_factor);
            else if (to_single(input.maximum_distance) < cost) continue;
            if (!(Real{} <= cost)) continue;
            edges[from * n + to] = cost;
            incident[from] = incident[to] = true;
        }
    }
    std::vector<std::size_t> nodes;
    for (std::size_t i = 0; i < n; ++i) if (incident[i]) nodes.push_back(i);
    const auto count = nodes.size();
    std::vector<Real> costs(count * count);
    std::vector<std::size_t> previous(count * count, count);
    for (std::size_t from = 0; from < count; ++from) for (std::size_t to = 0; to < count; ++to) {
        const auto cost = input.internal && to < from ? costs[to * count + from] : edges[nodes[from] * n + nodes[to]];
        costs[from * count + to] = cost;
        if (Real{} < cost) previous[from * count + to] = from;
    }
    // Strict improvements preserve the first equal-cost path in retained node order.
    for (std::size_t via = 0; via < count; ++via) for (std::size_t from = 0; from < count; ++from) {
        if (costs[from * count + via] < Real{}) continue;
        for (std::size_t to = 0; to < count; ++to) {
            if (costs[via * count + to] < Real{}) continue;
            const auto candidate = costs[from * count + via] + costs[via * count + to];
            auto& current = costs[from * count + to];
            if (current < Real{} || candidate < current) {
                current = candidate;
                previous[from * count + to] = previous[via * count + to];
            }
        }
    }
    std::vector<std::uint32_t> uses(count);
    std::vector<std::size_t> path; path.reserve(count);
    for (std::size_t from = 0; from < count; ++from) for (std::size_t to = 0; to < count; ++to) {
        if (from == to || (input.internal && to < from)) continue;
        auto at = previous[from * count + to];
        if (at == count) continue;
        path.clear();
        while (at != from) {
            if (at >= count || path.size() >= count) break;
            path.push_back(at);
            at = previous[from * count + at];
        }
        if (at != from) {
            // Project boundary: retail's NIL-chain walk is unqualified. Keep other pairs usable.
            if (diagnostics) diagnostics->push_back("path coefficient pair skipped: unqualified predecessor chain "
                + std::to_string(input.planets[nodes[from]].planet) + " -> " + std::to_string(input.planets[nodes[to]].planet));
            continue;
        }
        for (const auto intermediate : path) ++uses[intermediate];
    }
    const auto maximum = uses.empty() ? 0U : *std::max_element(uses.begin(), uses.end());
    std::vector<PathCoefficient> output;
    for (std::size_t i = 0; i < count; ++i)
        output.push_back({input.planets[nodes[i]].planet, maximum ? Real(uses[i]) / Real(maximum) : Real{}});
    return Result::success(std::move(output));
}

core::Result<void> GalacticWorld::publish_path_coefficients(const sim::PartitionExecutor& executor, bool force) {
    if (!setup_.path_coefficients_requested) return core::Result<void>::success();
    if (!bound().snapshot_ || !bound().snapshot_->control() || !setup_.maximum_fleet_distance || !setup_.trade_movement_factor) {
        path_coefficients_.clear(); // missing owning data stays unavailable, never a numeric fallback
        return core::Result<void>::success();
    }
    // WGA-490: actual current allegiance must be positive as well as allied.
    std::map<galactic::PlanetId, std::pair<tactical::PlayerId, bool>> allegiance;
    for (const auto& row : bound().snapshot_->control()->state)
        allegiance.emplace(row.planet, std::pair{row.current_player, row.current.raw() > 0});
    struct Job { tactical::PlayerId player{}; std::size_t graph{}; PathCoefficientInput input; };
    std::vector<Job> jobs;
    const auto same = [](const auto& a, const auto& b) {
        return a.internal == b.internal && a.maximum_distance.repr == b.maximum_distance.repr
            && a.trade_movement_factor.repr == b.trade_movement_factor.repr && a.planets == b.planets && a.trades == b.trades;
    };
    for (const auto& owner : setup_.players) for (std::size_t graph = 0; graph < 2; ++graph) {
        PathCoefficientInput input;
        input.maximum_distance = setup_.path_maximum_distance.value_or(*setup_.maximum_fleet_distance);
        input.trade_movement_factor = *setup_.trade_movement_factor;
        input.internal = graph == 0;
        for (const auto& planet : bound().snapshot_->planets()) {
            const auto at = allegiance.find(planet.id);
            if (at == allegiance.end()) {
                path_coefficients_.clear(); return core::Result<void>::success();
            }
            input.planets.push_back({planet.id, planet.position, at->second.second && allied(owner.player, at->second.first)});
        }
        for (const auto& trade : bound().snapshot_->routes()) input.trades.emplace_back(trade.from, trade.to);
        const auto old = path_coefficients_.find(owner.player);
        if (force || old == path_coefficients_.end() || !same(input, old->second.inputs[graph]))
            jobs.push_back({owner.player, graph, std::move(input)});
    }
    if (jobs.empty()) return core::Result<void>::success();
    std::vector<std::vector<PathCoefficient>> staged(jobs.size());
    std::vector<std::vector<std::string>> diagnostics(jobs.size());
    std::vector<std::optional<core::Diagnostic>> errors(jobs.size());
    const auto prepared = executor.execute_phase("galactic-ai-path-coefficients", sim::tick_partition_count, [&](std::size_t partition) {
        const auto range = sim::partition_range(partition, jobs.size());
        for (auto i = range.begin; i < range.end; ++i) {
            auto built = build_path_coefficients(jobs[i].input, &diagnostics[i]);
            if (built) staged[i] = std::move(built).value(); else errors[i] = built.error();
        }
    });
    if (!prepared) return prepared;
    for (const auto& error : errors) if (error) return core::Result<void>::failure(*error);
    // WGA-493: ordered publication at the existing refresh seam; cadence is unqualified (#4786).
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        for (const auto& note : diagnostics[i]) path_coefficient_diagnostics_.push_back("player "
            + std::to_string(jobs[i].player) + (jobs[i].graph == 0 ? " internal: " : " external: ") + note);
        auto& publication = path_coefficients_[jobs[i].player];
        auto& values = publication.values[jobs[i].graph]; values.clear();
        for (const auto& row : staged[i]) values.emplace(row.planet, row.value);
        publication.inputs[jobs[i].graph] = std::move(jobs[i].input);
    }
    return core::Result<void>::success();
}

core::Result<void> GalacticWorld::prepare_forces(std::vector<PlanetRow>& rows,
    const galactic::GalacticSnapshot& snapshot, const GalacticConnectivity& connectivity,
    const sim::PartitionExecutor& executor, bool retained_nearby) const {
    gc_profile::Scope profile_scope(gc_profile::Phase::forces);
    if (rows.empty()) return core::Result<void>::success();
    // Ordered input indexing keeps fleet and surface-list summation order. All
    // independent planet preparation runs in disjoint partition staging slots.
    std::map<galactic::PlanetId, std::vector<const galactic::Fleet*>> fleets;
    for (const auto& fleet : snapshot.fleets()) if (fleet.orbit && !fleet.motion.transit) fleets[fleet.orbit].push_back(&fleet);
    std::map<galactic::PlanetId, const galactic::ControlPlanet*> surfaces;
    if (snapshot.control()) for (const auto& surface : snapshot.control()->planets) surfaces.emplace(surface.planet, &surface);
    std::map<sim::EntityId, const galactic::MovementContents*> contents;
    if (snapshot.movement()) for (const auto& payload : snapshot.movement()->contents) contents.emplace(payload.container, &payload);
    std::map<galactic::PlanetId, std::set<galactic::PlanetId>> direct_routes;
    if (!retained_nearby) for (const auto& route : snapshot.routes()) {
        direct_routes[route.from].insert(route.to); direct_routes[route.to].insert(route.from);
    }
    // WGA-192: unchanged authored positions/topology reuse the derived neighbourhood.
    const bool reuse_nearby = !retained_nearby && bound().snapshot_ && bound().planets_.size() == snapshot.planets().size()
        && bound().snapshot_->planets().size() == snapshot.planets().size()
        && std::equal(bound().snapshot_->planets().begin(), bound().snapshot_->planets().end(), snapshot.planets().begin(),
            [](const auto& a, const auto& b) { return a.id == b.id && a.position == b.position; })
        && bound().snapshot_->routes().size() == snapshot.routes().size()
        && std::equal(bound().snapshot_->routes().begin(), bound().snapshot_->routes().end(), snapshot.routes().begin());
    const auto type = [&](tactical::TypeId id) -> const GalacticForceType* {
        const auto found = std::lower_bound(setup_.force_types.begin(), setup_.force_types.end(), id,
            [](const auto& row, auto key) { return row.type < key; });
        return found != setup_.force_types.end() && found->type == id ? &*found : nullptr;
    };
    const auto tech = [&](tactical::PlayerId id) -> std::optional<std::size_t> {
        if (snapshot.player_economy()) for (const auto& account : snapshot.player_economy()->players)
            if (account.player == id && account.current_tech >= 0 && account.current_tech <= 5) return static_cast<std::size_t>(account.current_tech);
        return {};
    };
    return executor.execute_phase("galactic-ai-forces", sim::tick_partition_count, [&](std::size_t partition) {
        const auto range = sim::partition_range(partition, rows.size());
        for (auto index = range.begin; index < range.end; ++index) {
            auto& row = rows[index];
            row.space.clear(); row.ground.clear(); row.starbase.clear();
            row.space_units.clear(); row.ground_units.clear();
            row.space_presence.clear(); row.ground_presence.clear();
            row.forces_available = row.ground_units_available = true;
            row.neighbourhood_pairs = 0;
            const auto append = [&](auto& output, tactical::TypeId id, tactical::PlayerId owner) {
                const auto* facts = type(id);
                if (!facts) { row.forces_available = false; return; }
                output.push_back({owner, facts->categories, to_single(facts->power)});
            };
            const auto spawns = [&](auto& output, tactical::TypeId id, tactical::PlayerId credited, tactical::PlayerId owner) {
                const auto* facts = type(id);
                if (!facts) { row.forces_available = false; return; }
                const auto level = tech(owner);
                if (!level) {
                    if (std::any_of(facts->starting_spawns.begin(), facts->starting_spawns.end(), [](const auto& spawn) { return !spawn.empty(); })) row.forces_available = false;
                    return;
                }
                auto selected = *level;
                while (selected > 0 && facts->starting_spawns[selected].empty()) --selected; // WAR-77 / WGA-183
                const auto& spawned = facts->starting_spawns[selected];
                for (const auto& spawn : spawned) {
                    if (!spawn.type) continue; // RGC-24: a null entry contributes nothing and does not make the estimate unavailable
                    const auto* unit = type(spawn.type);
                    const auto first = std::find_if(spawned.begin(), spawned.end(), [&](const auto& entry) { return entry.type == spawn.type; });
                    if (!unit || first->count < 0) { row.forces_available = false; continue; }
                    output.push_back({credited, unit->categories, to_single(real(first->count) * to_single(unit->power))});
                }
            };
            if (const auto found = fleets.find(row.planet.id); found != fleets.end()) for (const auto* fleet : found->second) {
                if (!connectivity.stealth_fleets.contains(fleet->id)) row.space_presence.push_back(fleet->owner); // WGA-185
                for (const auto& member : fleet->members) {
                    append(row.space, member.type, fleet->owner); spawns(row.space, member.type, fleet->owner, fleet->owner); // WGA-182/183
                    const auto* facts = type(member.type);
                    if (facts && !facts->transport) row.space_units.push_back({fleet->owner, member.type});
                    if (const auto payload = contents.find(member.id); payload != contents.end())
                        for (const auto& child : payload->second->children) row.space_units.push_back({fleet->owner, child.type}); // WGA-187
                }
            }
            if (const auto found = surfaces.find(row.planet.id); found != surfaces.end()) for (const auto& force : found->second->landed) {
                append(row.ground, force.type, force.owner); row.ground_presence.push_back(force.owner); // WGA-184/186
                if (const auto payload = contents.find(force.id); payload != contents.end())
                    for (const auto& child : payload->second->children) row.ground_units.push_back({force.owner, child.type});
                else row.ground_units_available = false;
            }
            if (row.production) {
                if (row.production->starbase_type) {
                    append(row.starbase, row.production->starbase_type, row.planet.owner); // WGA-190
                    spawns(row.space, row.production->starbase_type, row.planet.owner, row.production->starbase_owner);
                }
                for (const auto& object : row.production->objects) if (object.kind == galactic::ProductionKind::structure) {
                    if (object.domain == galactic::ProductionDomain::ground) spawns(row.ground, object.type, row.planet.owner, object.owner);
                    const auto* facts = type(object.type);
                    if (!facts) { row.forces_available = false; continue; }
                    if (object.domain == galactic::ProductionDomain::space || facts->space_weapon) {
                        if (!facts->space_structure_categories) { row.forces_available = false; continue; } // named authoring seam
                        row.space.push_back({row.planet.owner, *facts->space_structure_categories, to_single(facts->power)});
                    }
                }
            }
            // Native layers retain the completed topology/neighbourhood in row.
            if (!retained_nearby && setup_.maximum_fleet_distance && reuse_nearby) row.nearby = bound().planets_[index].nearby;
            else if (!retained_nearby && setup_.maximum_fleet_distance) for (const auto& other : snapshot.planets()) {
                ++row.neighbourhood_pairs;
                const auto separation = distance(row.planet.position, other.position);
                const auto routes = direct_routes.find(row.planet.id);
                const bool trade = routes != direct_routes.end() && routes->second.contains(other.id);
                if (separation <= *setup_.maximum_fleet_distance || trade) row.nearby.push_back({other.id, separation, trade}); // WGA-192 includes self
            }
        }
    });
}

std::optional<Real> GalacticWorld::force_token(const GalacticQueryKey& key, ReadMode mode) const {
    const auto* at = planet(key.planet);
    const auto token = key.tokens.back();
    if (key.tokens.size() == 2 && key.tokens.front() == "VARIABLE_TARGET") {
        if (!at) return {};
        if (token == "CONNECTSISOLATEDPLANETSBYFORCE" || token == "CONNECTSLARGESTISLANDS") {
            for (const auto& path : bound().connectivity_->island_paths) if (path.player == key.player && path.planet == key.planet)
                return real((token == "CONNECTSLARGESTISLANDS" ? path.largest : path.least_force) ? 1 : 0); // WGA-194
            return {}; // producer must publish known false as well as true
        }
        if (token != "ISCONNECTEDTO" || !setup_.maximum_fleet_distance
            || (!has_parameter(key.bindings, "PARAMETER_TYPE") && !has_parameter(key.bindings, "PARAMETER_FACTION"))) return {};
        const auto attenuation = number(key.bindings, "PARAMETER_ATTENUATOR").value_or(Real{});
        Real result{};
        for (const auto& nearby : at->nearby) {
            if (nearby.planet == key.planet) continue; // WGA-193: connectivity excludes the source planet
            const auto* other = planet(nearby.planet); if (!other) return {};
            const auto* owner = player(other->planet.owner);
            if (!text_match(key.bindings, "PARAMETER_TYPE", other->facts.name)
                && !(owner && text_match(key.bindings, "PARAMETER_FACTION", owner->faction))) continue;
            if (attenuation <= Real{}) return real(1);
            if (*setup_.maximum_fleet_distance == Real{}) return {};
            const auto separation = nearby.trade_route ? Real{} : nearby.distance;
            result = std::max(result, real(1) - attenuation * (separation / *setup_.maximum_fleet_distance));
        }
        return result; // WGA-193: maximum matching contribution with zero floor
    }
    if (key.tokens.size() != 3 || (key.tokens.front() != "VARIABLE_TARGET" && key.tokens.front() != "GAME")) return {};
    const bool friendly = key.tokens[1] == "FRIENDLYFORCE";
    const auto matches = [&](tactical::PlayerId owner) {
        if (!player(owner)) return false;
        if (bound().snapshot_->control()) for (const auto& relation : bound().snapshot_->control()->relations) if (relation.player == key.player && relation.other == owner)
            return friendly ? relation.allied : !relation.allied;
        return friendly ? allied(key.player, owner) : !allied(key.player, owner);
    }; // WGA-181: copied explicit relation; typed team fixture otherwise
    const auto mask = category(key.bindings);
    const bool space = token.find("SPACE") != std::string::npos;
    const bool starbase = token == "STARBASETOTALUNNORMALIZED";
    const auto raw = [&](const PlanetRow& row) -> std::optional<Real> {
        if (!row.forces_available) return {};
        Real result{};
        const auto& forces = starbase ? row.starbase : space ? row.space : row.ground;
        for (const auto& force : forces) if (matches(force.owner) && (starbase || (force.categories & mask) != 0)) result += force.power;
        if (!friendly && !space && !starbase && Real{} < result) {
            if (row.facts.indigenous_potential) {
                // WGA-273/274: authored type qualifies once if any explicitly
                // enemy player has an affiliated faction; neutral is not !ally.
                for (const auto& potential : *row.facts.indigenous_potential) {
                    if (!bound().snapshot_->production() || !bound().snapshot_->production()->nested_creation) return {};
                    const auto& relations = bound().snapshot_->production()->nested_creation->relations;
                    bool enemy{};
                    for (const auto affiliation : potential.affiliations) {
                        if (!player(affiliation)) return {};
                        const auto relation = std::find_if(relations.begin(), relations.end(), [&](const auto& candidate) {
                            return candidate.player == key.player && candidate.other == affiliation;
                        });
                        if (relation == relations.end()) return {};
                        enemy = enemy || relation->enemy;
                    }
                    if (enemy && (potential.categories & mask) != 0) result += to_single(potential.power);
                }
            } else {
                if (!row.facts.enemy_indigenous) return {}; // qualified synthetic fixture only
                for (const auto& [bits, power] : *row.facts.enemy_indigenous) if ((bits & mask) != 0) result += power;
            }
        }
        return result;
    };
    if (key.tokens.front() == "GAME") {
        if (token != "SPACETOTALUNNORMALIZED" && token != "GROUNDTOTALUNNORMALIZED") return {};
        Real total{}; auto nested = copy_key(key); nested.tokens.front() = "VARIABLE_TARGET";
        nested.bindings.clear(); // WGA-189: rebuilt planet contexts omit incoming bindings
        for (const auto& row : bound().planets_) { nested.planet = row.planet.id; const auto value = read(nested, mode); if (!value) return {}; total += *value; }
        return total; // WGA-189
    }
    if (!at) return {};
    if (token == "HASSPACEFORCE" || token == "HASGROUNDFORCE") {
        const auto& owners = space ? at->space_presence : at->ground_presence;
        for (const auto owner : owners) if (matches(owner)) {
            if (!has_parameter(key.bindings, "PARAMETER_FACTION")) return real(1);
            const auto* who = player(owner); if (who && text_match(key.bindings, "PARAMETER_FACTION", who->faction)) return real(1);
        }
        return Real{}; // presence is independent of strength, category and structures
    }
    if (token == "HASSPACEUNITSBITFIELD" || token == "HASGROUNDUNITSBITFIELD") {
        if (!space && !at->ground_units_available) return {};
        std::vector<std::string> names; std::vector<std::uint64_t> categories;
        for (const auto& binding : key.bindings) {
            if (binding.parameter == "PARAMETER_TYPE") { if (const auto* name = std::get_if<std::string>(&binding.value)) names.push_back(upper_case(*name)); else return {}; }
            if (binding.parameter == "PARAMETER_CATEGORY") { if (const auto* value = std::get_if<Real>(&binding.value)) categories.push_back(static_cast<std::uint64_t>(*value)); else return {}; }
        }
        if (names.size() + categories.size() == 0 || names.size() + categories.size() > 32) return {};
        std::uint32_t bits{};
        const auto& units = space ? at->space_units : at->ground_units;
        for (const auto& unit : units) if (matches(unit.owner)) {
            const auto found = std::lower_bound(setup_.types.begin(), setup_.types.end(), unit.type, [](const auto& row, auto id) { return row.type_id < id; });
            if (found == setup_.types.end() || found->type_id != unit.type) return {};
            for (std::size_t i = 0; i < names.size(); ++i) if (upper_case(found->name) == names[i]) { bits |= std::uint32_t{1} << i; break; }
            for (std::size_t i = 0; i < categories.size(); ++i) if (categories[i] == 0 || (found->category_bits & categories[i]) != 0) bits |= std::uint32_t{1} << (names.size() + i);
        }
        return real(static_cast<std::int32_t>(bits)); // WGA-187: signed 32-bit result
    }
    if (token == "STARBASETOTALUNNORMALIZED" || token == "SPACETOTALUNNORMALIZED" || token == "GROUNDTOTALUNNORMALIZED") return raw(*at);
    const bool nearby = token == "NEARBYSPACETOTAL" || token == "NEARBYGROUNDTOTAL";
    if (!nearby && token != "SPACETOTAL" && token != "GROUNDTOTAL" && token != "SPACETOTALNBTD" && token != "GROUNDTOTALNBTD"
        && token != "SPACETOTALNBTT" && token != "GROUNDTOTALNBTT" && token != "SPACETOTALNBPTM" && token != "GROUNDTOTALNBPTM") return {};
    const auto raw_token = space ? "SPACETOTALUNNORMALIZED" : "GROUNDTOTALUNNORMALIZED";
    auto nested = copy_key(key); nested.tokens.back() = raw_token;
    const auto numerator = read(nested, mode); if (!numerator) return {};
    Real denominator{};
    if (token.ends_with("NBTT")) {
        nested.bindings.clear(); const auto value = read(nested, mode); if (!value) return {}; denominator = *value;
    } else {
        const bool both = nearby || token == "SPACETOTAL" || token == "GROUNDTOTAL";
        const bool maximum = token.ends_with("NBPTM");
        // WGA-189: game force rebuilds planet contexts without parameter bindings.
        nested.bindings.clear();
        for (const auto& row : bound().planets_) {
            nested.planet = row.planet.id;
            const auto value = read(nested, mode); if (!value) return {};
            denominator = maximum ? std::max(denominator, *value) : denominator + *value;
        }
        if (both) {
            nested.tokens[1] = friendly ? "ENEMYFORCE" : "FRIENDLYFORCE";
            for (const auto& row : bound().planets_) { nested.planet = row.planet.id; const auto value = read(nested, mode); if (!value) return {}; denominator += *value; }
        }
    }
    if (!nearby) return normalized(*numerator, denominator);
    if (!setup_.maximum_fleet_distance || *setup_.maximum_fleet_distance <= Real{}) return {};
    Real total{};
    const auto attenuation = number(key.bindings, "PARAMETER_ATTENUATOR").value_or(real(1));
    nested = copy_key(key); nested.tokens.back() = raw_token;
    for (const auto& other : at->nearby) {
        nested.planet = other.planet; const auto value = read(nested, mode); if (!value) return {};
        total += (real(1) - (real(1) - attenuation) * (other.distance / *setup_.maximum_fleet_distance)) * *value;
    }
    if (denominator == Real{}) return total == Real{} ? std::optional<Real>{Real{}} : std::nullopt;
    return total / denominator; // WGA-192: nearby ratio is not clamped
}
} // namespace eawr::script::foc::ai
