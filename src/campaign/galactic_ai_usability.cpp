#include "galactic_ai_usability.hpp"
#include "galactic_ai_connecting_heap.hpp"
#include "eawr/script/foc/galactic_ai_state.hpp"
#include "../script/foc/galactic_ai_execution_state.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>

namespace eawr::campaign {
namespace gc = sim::galactic;
namespace ai = script::foc::ai;
namespace ga = script::foc::galactic_ai;
namespace {
template<class T> core::Result<T> refuse(std::string message) {
    core::Diagnostic error; error.code = "EAWR-GCAI-USABILITY-0001"; error.message = std::move(message);
    return core::Result<T>::failure(std::move(error));
}
template<class Rows, class Id, class Key> const auto* find(const Rows& rows, Id id, Key key) {
    const auto at = std::lower_bound(rows.begin(), rows.end(), id,
        [&](const auto& row, auto value) { return key(row) < value; });
    return at != rows.end() && key(*at) == id ? &*at : nullptr;
}
bool multiplayer(const gc::GalacticSnapshot& snapshot) {
    // The pinned mode is authoritative. Seat count alone cannot identify a
    // multiplayer save whose second human has dropped out (WGA-127/264).
    if (snapshot.victory()) return snapshot.victory()->context.multiplayer;
    return std::count_if(snapshot.seats().begin(), snapshot.seats().end(),
        [](const auto& seat) { return seat.kind == gc::SeatKind::human; }) > 1;
}
using Real = ai::Real;
Real single(Real value) { return ai::to_single(value); }
Real separation(const sim::math::Vec3& a, const sim::math::Vec3& b, bool squared = false) {
    const auto x = single(single(script::numeric::from_fixed(a.x)) - single(script::numeric::from_fixed(b.x)));
    const auto y = single(single(script::numeric::from_fixed(a.y)) - single(script::numeric::from_fixed(b.y)));
    const auto z = single(single(script::numeric::from_fixed(a.z)) - single(script::numeric::from_fixed(b.z)));
    const auto value = single(single(single(x * x) + single(y * y)) + single(z * z));
    return squared ? value : single(ai::square_root(value));
}
std::optional<Real> token(const GalacticAiUsabilityProvider::World& world,
    const GalacticAiUsabilityProvider::Engine& engine, gc::PlayerId owner, gc::PlanetId planet,
    std::vector<std::string> tokens, const ai::DerivedExecutionRestore* reconstruction = nullptr) {
    ai::Equation equation; equation.name = "B_OWNING_INPUT";
    equation.lookups.push_back({std::move(tokens), {}});
    ai::Node node; node.kind = ai::Node::Kind::lookup; equation.program.push_back(node);
    ai::Target target; target.object = planet; target.name = "OBJECT_" + std::to_string(planet);
    ai::PlayerAi selected; selected.player = owner;
    auto context = world.goal_context(selected, &target);
    ai::AiRandom random;
    return reconstruction ? world.reconstruction_perception(*reconstruction, engine, equation, context, random)
        : world.perception(engine, equation, context, random);
}
bool defense(const gc::PathPlanet& row, const gc::PathOptions& options) {
    if (row.starbase_relation) return *row.starbase_relation != gc::PathRelation::allied;
    return options.ai_player ? row.ai_enemy_space_force.value_or(true)
        : row.fleet_presence_visible && row.nonallied_fleet;
}
bool bypass(const gc::PathPlanet& row, const gc::PathOptions& options) {
    return options.benefits_from_corruption && row.corrupted;
}
bool expands(const gc::PathPlanet& row, const gc::PathOptions& options) {
    if (options.ai_player && options.ai_restrictions && !row.ai_usable) return false;
    bool allegiance = row.relation == gc::PathRelation::allied || bypass(row, options);
    if (!allegiance) {
        if (options.ai_player && !row.ai_classification_succeeded) allegiance = true;
        else {
            const auto relation = options.ai_player ? row.ai_relation.value_or(gc::PathRelation::neutral)
                : row.owner_visible ? row.relation : gc::PathRelation::neutral;
            allegiance = relation == gc::PathRelation::neutral ? options.allow_neutral : options.allow_hostile;
        }
    }
    if (allegiance) return options.allow_defended || bypass(row, options) || !defense(row, options);
    return options.allow_bad_if_no_force && (!defense(row, options) || bypass(row, options));
}
constexpr std::array policies{"Friendly_Only", "Friendly_Ignore_Threat", "Enemy_Destination",
    "Enemy_Undefended", "Single_Hop_Disconnected", "Any"};
gc::PathOptions reach_options(const ga::NativeActions::MovementInput& movement, std::size_t policy) {
    gc::PathOptions out; out.ai_player = movement.ai_player; out.multiplayer = movement.multiplayer;
    out.benefits_from_corruption = movement.benefits_from_corruption;
    out.allow_hostile = out.allow_neutral = policy == 5;
    out.allow_defended = policy == 1 || policy == 5;
    out.allow_bad_final = policy > 1; out.allow_bad_if_no_force = policy > 2;
    return out;
}
std::vector<gc::PlanetId> production_sites(const gc::PlanetGraph& graph, gc::PlanetId target,
    const ga::NativeActions::MovementInput& movement, std::size_t policy) {
    const auto options = reach_options(movement, policy);
    const auto row = [&](gc::PlanetId id) -> const gc::PathPlanet& {
        return *find(movement.access, id, [](const auto& at) { return at.id; });
    };
    const auto& end = row(target);
    if (!options.allow_bad_final && end.relation != gc::PathRelation::allied) return {};
    // WGA-259: root inclusion is a separate test, not expansion admission.
    const bool root = end.relation == gc::PathRelation::allied
        && (options.allow_defended || bypass(end, options) || !defense(end, options));
    std::set<gc::PlanetId> visited;
    const auto flood = [&](gc::PlanetId start) {
        std::vector<gc::PlanetId> queue{start}, result; visited.insert(start);
        for (std::size_t at = 0; at < queue.size(); ++at) {
            const auto id = queue[at];
            if (row(id).relation == gc::PathRelation::allied) result.push_back(id);
            for (const auto& edge : graph.node(id)->edges)
                if (visited.insert(edge.to).second && expands(row(edge.to), options)) queue.push_back(edge.to);
        }
        return result;
    };
    if (root) return flood(target);
    std::vector<gc::PlanetId> best;
    for (const auto& edge : graph.node(target)->edges) {
        if (visited.contains(edge.to)) continue;
        if (!expands(row(edge.to), options)) { visited.insert(edge.to); continue; }
        auto sites = flood(edge.to);
        if (policy == 4) best.insert(best.end(), sites.begin(), sites.end());
        else if (best.size() < sites.size()) best = std::move(sites);
    }
    return best;
}
} // namespace

core::Result<std::shared_ptr<const GalacticAiUsabilityContent>> prepare_stock_galactic_ai_usability(
    const data::galactic::GalacticData& data, const CampaignSetupPlan& plan,
    const ai::GalacticPerceptionSetup& perception) {
    auto content = std::make_shared<GalacticAiUsabilityContent>();
    content->players = perception.players; content->uses_fog = perception.uses_fog;
    content->logical_fps = perception.logical_fps;
    std::sort(content->players.begin(), content->players.end(), [](const auto& a, const auto& b) { return a.player < b.player; });
    for (const auto& location : plan.locations()) if (location.planet) {
        const auto* authored = data.planet(location.original_type);
        if (!authored || !authored->has_planetary_behavior())
            return refuse<std::shared_ptr<const GalacticAiUsabilityContent>>("WGA-127: campaign planet lacks planetary data");
        content->planet_order.push_back(location.planet->id);
    }
    if (content->planet_order.size() != plan.galaxy().planets.size() || !content->logical_fps)
        return refuse<std::shared_ptr<const GalacticAiUsabilityContent>>("incomplete campaign usability identities");
    return core::Result<std::shared_ptr<const GalacticAiUsabilityContent>>::success(std::move(content));
}
GalacticAiUsabilityProvider::GalacticAiUsabilityProvider(std::shared_ptr<const GalacticAiUsabilityContent> content)
    : content_(std::move(content)) {
    if (!content_ || !content_->logical_fps || content_->players.empty())
        throw std::invalid_argument("usability requires immutable campaign identities");
}
core::Result<bool> GalacticAiUsabilityProvider::usability(gc::PlayerId player,
    const gc::GalacticSnapshot& snapshot, gc::PlanetId planet) const {
    const auto* selected = find(content_->players, player, [](const auto& row) { return row.player; });
    if (!selected) return refuse<bool>("unknown AI usability player");
    if (!selected->ai || multiplayer(snapshot)) return core::Result<bool>::success(true);
    const auto* state = snapshot.planet_access();
    if (!state || !snapshot.observer()) return refuse<bool>("WGA-127: live planet access/observer owner is absent");
    const auto* access = find(state->planets, planet, [](const auto& row) { return row.planet; });
    const auto* facts = find(snapshot.observer()->planets, planet, [](const auto& row) { return row.planet; });
    if (!access || !facts) return refuse<bool>("WGA-127: planetary data is absent");
    if (access->restricted) return core::Result<bool>::success(false);
    if (facts->automatic_reveal) return core::Result<bool>::success(true);
    if (access->local_revealed) return core::Result<bool>::success(*access->local_revealed);
    // WGA-127/264: use the recorded session local observer, independent of the AI.
    const auto key = std::pair{snapshot.observer()->local_player, planet};
    const auto* record = find(snapshot.observer()->records, key,
        [](const auto& row) { return std::pair{row.player, row.planet}; });
    if (!record) return refuse<bool>("WGA-127: recorded local reveal predicate is absent");
    return core::Result<bool>::success(record->discovered);
}

core::Result<void> GalacticAiUsabilityProvider::prepare_stealth(const gc::GalacticSnapshot& snapshot) {
    if (live_stealth_unavailable_) return refuse<void>("WGA-185: live fleet stealth owner is unavailable");
    if (stealth_generation_ == snapshot.observer_input_generation()) return core::Result<void>::success();
    std::set<sim::EntityId> staged;
    for (const auto& fleet : snapshot.fleets()) {
        const auto skip = snapshot.fleet_combat_skip(fleet.id);
        if (!skip) return core::Result<void>::failure(skip.error());
        if (skip.value()) staged.insert(fleet.id);
    }
    stealth_fleets_ = std::move(staged); stealth_generation_ = snapshot.observer_input_generation();
    ++work_.stealth_preparations;
    return core::Result<void>::success();
}
core::Result<const GalacticAiUsabilityProvider::Cache*> GalacticAiUsabilityProvider::refresh(
    gc::PlayerId owner, const gc::GalacticSnapshot& snapshot, const World& world,
    const Engine& engine, const sim::PartitionExecutor& executor, const ai::DerivedExecutionRestore* reconstruction, bool force) {
    if (auto prepared = prepare_stealth(snapshot); !prepared) return core::Result<const Cache*>::failure(prepared.error());
    if (story_path_unavailable_) return refuse<const Cache*>("B-U02/WST-52: live story path predicate owner is unavailable");
    if (!world.available() || world.observer_input_generation() != snapshot.observer_input_generation())
        return refuse<const Cache*>("B perception facts are not bound to the current completed world");
    if (world.stealth_fleets() != stealth_fleets_)
        return refuse<const Cache*>("WGA-185: live fleet stealth projection is not bound to the current world");
    const auto* selected = find(content_->players, owner, [](const auto& row) { return row.player; });
    if (!selected || !snapshot.observer() || !snapshot.planet_access())
        return refuse<const Cache*>("B live player, observer and access owners are required");
    const auto generation = snapshot.observer_input_generation();
    const auto access_generation = snapshot.planet_access()->generation;
    const auto old = caches_.find(owner);
    if (!force && old != caches_.end() && !old->second.source_changed && old->second.observer_generation == generation
        && old->second.access_generation == access_generation
        && old->second.perception_revision == world.movement_input_revision()
        && Real(world.synchronized_frame()) <= old->second.valid_through)
        return core::Result<const Cache*>::success(&old->second);
    const auto* observer = find(snapshot.observer()->players, owner, [](const auto& row) { return row.player; });
    if (!observer) return refuse<const Cache*>("B selected observer is absent");
    const auto allied = [&](gc::PlayerId other) {
        return other == owner || std::binary_search(observer->allies.begin(), observer->allies.end(), other);
    };
    const auto relation = [&](gc::PlayerId other) {
        if (allied(other)) return gc::PathRelation::allied;
        const auto* player = find(content_->players, other, [](const auto& row) { return row.player; });
        return !player || player->neutral ? gc::PathRelation::neutral : gc::PathRelation::hostile;
    };
    Cache next; next.observer_generation = generation; next.access_generation = access_generation;
    next.movement.ai_player = selected->ai; next.movement.multiplayer = multiplayer(snapshot);
    next.movement.benefits_from_corruption = observer->benefits_from_corruption;
    next.movement.access.resize(snapshot.planets().size());
    std::vector<std::optional<core::Diagnostic>> errors(snapshot.planets().size());
    // The planet tokens below query perception in every partition: refresh
    // its copied economy input once here, never from a partition (#2971).
    // Reconstruction reads archived values and refreshes nothing.
    std::optional<World::PartitionedReads> reads;
    if (!reconstruction) reads.emplace(world.partitioned_reads());
    auto prepared = executor.execute_phase("galactic-ai-usability", sim::tick_partition_count, [&](std::size_t part) {
        const auto range = sim::partition_range(part, snapshot.planets().size());
        for (auto index = range.begin; index < range.end; ++index) {
            const auto id = snapshot.planets()[index].id;
            const auto* facts = find(snapshot.observer()->planets, id, [](const auto& row) { return row.planet; });
            const auto usable = usability(owner, snapshot, id);
            if (!usable) { errors[index] = usable.error(); continue; }
            if (!facts) { errors[index] = refuse<void>("B live planetary observer facts are absent").error(); continue; }
            auto& row = next.movement.access[index]; row.id = id; row.ai_usable = usable.value();
            row.owner_visible = false;
            row.relation = relation(facts->allegiance); row.corrupted = facts->corrupted;
            const auto* local = find(snapshot.observer()->records, std::pair{snapshot.observer()->local_player, id},
                [](const auto& at) { return std::pair{at.player, at.planet}; });
            row.revealed = local && local->discovered;
            for (const auto& knowledge : snapshot.observations())
                if (knowledge.player == snapshot.observer()->local_player && knowledge.planet == id) {
                    row.owner_visible = knowledge.knowledge.ownership;
                    row.fleet_presence_visible = knowledge.knowledge.fleet_presence; break;
                }
            if (facts->space_level != 0) row.starbase_relation = relation(facts->space_owner);
            for (const auto& fleet : snapshot.fleets()) if (fleet.orbit == id && !fleet.motion.transit && !allied(fleet.owner))
                row.nonallied_fleet = true;
            if (selected->ai) {
                const auto classified = token(world, engine, owner, id, {"VARIABLE_TARGET", "TARGETPLAYERID"}, reconstruction);
                row.ai_classification_succeeded = classified.has_value();
                if (classified) {
                    const auto* player = find(content_->players, static_cast<gc::PlayerId>(*classified), [](const auto& at) { return at.player; });
                    row.ai_relation = !player || player->neutral ? gc::PathRelation::neutral : gc::PathRelation::hostile;
                }
                if (!row.starbase_relation) {
                    const auto force = token(world, engine, owner, id, {"VARIABLE_TARGET", "ENEMYFORCE", "HASSPACEFORCE"}, reconstruction);
                    if (force) row.ai_enemy_space_force = *force != Real{};
                }
            }
        }
    });
    if (!prepared) return core::Result<const Cache*>::failure(prepared.error());
    for (const auto& error : errors) if (error) return core::Result<const Cache*>::failure(*error);
    if (snapshot.control()) for (const auto& at : snapshot.control()->state)
        next.movement.orbital_occupants.emplace(at.planet, at.orbital_player);
    else if (snapshot.production()) for (const auto& at : snapshot.production()->planets)
        next.movement.orbital_occupants.emplace(at.planet, at.orbit_occupier);
    if (next.movement.orbital_occupants.size() != snapshot.planets().size())
        return refuse<const Cache*>("WGM-66: complete live orbital occupant owner is absent");
    next.perception_revision = world.movement_input_revision();
    next.valid_through = reconstruction ? prepared_through_.at(owner).through
        : world.force_cache_deadline(owner, content_->logical_fps, content_->uses_fog);
    next.source_changed = reconstruction && (prepared_through_.at(owner).pending_changes & 1U) != 0;
    const PreparedInput prepared_input{next.valid_through, 0};
    if (!reconstruction && selected->ai && (!prepared_through_.contains(owner) || prepared_through_.at(owner) != prepared_input)) {
        prepared_through_[owner] = prepared_input; mark_state_changed();
    }
    ++work_.generations; work_.planets += snapshot.planets().size();
    auto [at, inserted] = caches_.insert_or_assign(owner, std::move(next)); static_cast<void>(inserted);
    return core::Result<const Cache*>::success(&at->second);
}
core::Result<GalacticAiUsabilityProvider::MovementInput> GalacticAiUsabilityProvider::movement(
    gc::PlayerId player, const gc::GalacticSnapshot& snapshot, const World& world,
    const Engine& engine, const sim::PartitionExecutor& executor) {
    auto cached = refresh(player, snapshot, world, engine, executor);
    if (!cached) return core::Result<MovementInput>::failure(cached.error());
    return core::Result<MovementInput>::success(cached.value()->movement);
}

core::Result<ai::GalacticConnectivity> GalacticAiUsabilityProvider::connectivity(
    const gc::GalacticSession& session, World& world, const Engine& engine, const sim::PartitionExecutor& executor) {
    observed_session_ = &session; observed_world_ = &world;
    const auto snapshot = session.snapshot(); const auto* graph = session.graph();
    if (!graph || !snapshot->observer() || !snapshot->planet_access())
        return refuse<ai::GalacticConnectivity>("B complete live graph/observer/access owners are required");
    if (auto prepared = prepare_stealth(*snapshot); !prepared)
        return core::Result<ai::GalacticConnectivity>::failure(prepared.error());
    // WGA-185/360: force presence consumes the qualified live aggregate before
    // any selected-player tokens; strength and observation masks keep their owners.
    if (!world.available() || world.observer_input_generation() != snapshot->observer_input_generation()
        || world.stealth_fleets() != stealth_fleets_) {
        auto inputs = published_; inputs.stealth_fleets = stealth_fleets_;
        auto rebound = world.bind(snapshot, std::move(inputs), executor, session.completed_tick(), session.next_entity_id());
        if (!rebound) return core::Result<ai::GalacticConnectivity>::failure(rebound.error());
    }
    if (reconstructed_) {
        // B-U05: load rebuilds derived output, never services producer cadence.
        // The already bound World supplies archived movement getters without
        // publishing parent caches, even when those rows or site lists expired.
        ai::GalacticConnectivity output; output.revision = publication_revision_; output.stealth_fleets = stealth_fleets_;
        const ai::DerivedExecutionRestore reconstruction;
        for (const auto& player : content_->players) if (player.ai && !retained_.empty()) {
            auto refreshed = refresh(player.player, *snapshot, world, engine, executor, &reconstruction);
            if (!refreshed) { caches_.clear(); return core::Result<ai::GalacticConnectivity>::failure(refreshed.error()); }
            auto& cache = caches_.at(player.player);
            cache.goal_reach.clear(); cache.islands.clear();
            cache.connectivity_through = std::numeric_limits<std::uint64_t>::max();
            for (const auto& planet : snapshot->planets()) {
                const auto& saved = retained_.at({player.player, planet.id});
                for (std::size_t policy = 0; policy < policies.size(); ++policy) {
                    cache.goal_reach.push_back({player.player, planet.id, policies[policy], saved.sites[policy]});
                    const auto through = saved.frames[policy]
                        ? *saved.frames[policy] + std::uint64_t(content_->logical_fps) * 10 : 0;
                    cache.connectivity_through = std::min(cache.connectivity_through, through);
                }
                cache.islands.push_back({player.player, planet.id, saved.least_force, saved.largest});
            }
            cache.connectivity_ready = (prepared_through_.at(player.player).pending_changes & 2U) == 0;
            output.goal_reach.insert(output.goal_reach.end(), cache.goal_reach.begin(), cache.goal_reach.end());
            output.island_paths.insert(output.island_paths.end(), cache.islands.begin(), cache.islands.end());
        }
        published_ = output; reconstructed_ = false;
        return core::Result<ai::GalacticConnectivity>::success(std::move(output));
    }
    world.set_synchronized_frame(session.completed_tick());
    if (!world.available() || world.observer_input_generation() != snapshot->observer_input_generation()) {
        auto rebound = world.bind(snapshot, published_, executor, session.completed_tick(), session.next_entity_id());
        if (!rebound) return core::Result<ai::GalacticConnectivity>::failure(rebound.error());
    }
    // This is the synchronization owner clock, not a camera or elapsed wall clock.
    const auto frame = world.synchronized_frame();
    bool dirty = live_stealth_unavailable_ || story_path_unavailable_ || reconstructed_
        || published_.stealth_fleets != stealth_fleets_;
    const bool source_changed = std::any_of(caches_.begin(), caches_.end(), [](const auto& row) { return row.second.source_changed; });
    dirty = dirty || source_changed;
    for (const auto& player : content_->players) if (player.ai) {
        const auto at = caches_.find(player.player);
        dirty = dirty || at == caches_.end() || !at->second.connectivity_ready
            || at->second.observer_generation != snapshot->observer_input_generation()
            || at->second.access_generation != snapshot->planet_access()->generation
            || at->second.perception_revision != world.movement_input_revision()
            || frame > at->second.connectivity_through || Real(frame) > at->second.valid_through;
    }
    if (!dirty) return core::Result<ai::GalacticConnectivity>::success(published_);
    struct CacheTransaction {
        std::function<void()> discard;
        bool committed{};
        ~CacheTransaction() { if (!committed) discard(); }
    } transaction{[this, prepared = prepared_through_]() mutable {
        caches_.clear(); prepared_through_ = std::move(prepared);
    }};
    auto staged = retained_; ai::GalacticConnectivity output; output.stealth_fleets = stealth_fleets_;
    for (const auto& player : content_->players) if (player.ai) {
        const auto prior = caches_.find(player.player);
        const bool pending = prior != caches_.end() && prior->second.source_changed;
        auto refreshed = refresh(player.player, *snapshot, world, engine, executor, nullptr, pending);
        if (!refreshed) return core::Result<ai::GalacticConnectivity>::failure(refreshed.error());
        auto& cache = caches_.at(player.player);
        if (!cache.connectivity_ready || frame > cache.connectivity_through) {
            const auto& movement = cache.movement;
            std::vector<std::array<std::vector<gc::PlanetId>, 6>> sites(snapshot->planets().size());
            std::vector<std::array<bool, 6>> updated(snapshot->planets().size());
            auto phase = executor.execute_phase("galactic-ai-production-sites", sim::tick_partition_count, [&](std::size_t part) {
                const auto range = sim::partition_range(part, snapshot->planets().size());
                for (auto index = range.begin; index < range.end; ++index) {
                    const auto id = snapshot->planets()[index].id;
                    const auto saved = staged.find({player.player, id});
                    for (std::size_t policy = 0; policy < policies.size(); ++policy) {
                        const bool valid = saved != staged.end() && saved->second.frames[policy]
                            && frame <= *saved->second.frames[policy] + std::uint64_t(content_->logical_fps) * 10;
                        if (valid) sites[index][policy] = saved->second.sites[policy];
                        else { sites[index][policy] = production_sites(*graph, id, movement, policy); updated[index][policy] = true; }
                    }
                }
            });
            if (!phase) return core::Result<ai::GalacticConnectivity>::failure(phase.error());
            const auto index_of = [&](gc::PlanetId id) {
                return static_cast<std::size_t>(std::lower_bound(snapshot->planets().begin(), snapshot->planets().end(), id,
                    [](const auto& row, auto key) { return row.id < key; }) - snapshot->planets().begin());
            };
            const auto access = [&](gc::PlanetId id) -> const gc::PathPlanet& { return movement.access[index_of(id)]; };
            std::vector<gc::PlanetId> representatives;
            for (const auto id : content_->planet_order) {
                const auto* facts = find(snapshot->observer()->planets, id, [](const auto& at) { return at.planet; });
                const auto* owner = facts ? find(content_->players, facts->allegiance, [](const auto& at) { return at.player; }) : nullptr;
                if (!owner || owner->faction != player.faction) continue;
                bool represented = false;
                for (const auto representative : representatives) {
                    const auto& component = sites[index_of(representative)][1];
                    if (std::find(component.begin(), component.end(), id) != component.end()) { represented = true; break; }
                }
                if (!represented) {
                    representatives.push_back(id);
                    // GFS-A14: changed connectivity forces component representatives;
                    // reconstruction retains the archived producer lists/deadlines.
                    if (!reconstructed_) { sites[index_of(id)][1] = production_sites(*graph, id, movement, 1); updated[index_of(id)][1] = true; }
                }
            }
            for (std::size_t index = 0; index < sites.size(); ++index) {
                auto& saved = staged[{player.player, snapshot->planets()[index].id}];
                saved.shortest = false;
                for (std::size_t policy = 0; policy < policies.size(); ++policy) {
                    saved.sites[policy] = sites[index][policy];
                    if (updated[index][policy]) { saved.frames[policy] = frame; ++work_.searches; }
                }
            }
            // Connecting paths use selected manager cached raw space+ground totals,
            // ordinary world edges, and their own single-precision hop policy.
            if (representatives.size() > 1) {
                for (const auto& row : snapshot->planets()) if (access(row.id).relation != gc::PathRelation::allied) {
                    auto space = token(world, engine, player.player, row.id, {"VARIABLE_TARGET", "ENEMYFORCE", "SPACETOTALUNNORMALIZED"});
                    auto ground = token(world, engine, player.player, row.id, {"VARIABLE_TARGET", "ENEMYFORCE", "GROUNDTOTALUNNORMALIZED"});
                    if (!space || !ground) return refuse<ai::GalacticConnectivity>("WGA-261: qualified island force perception is unavailable for player "
                        + std::to_string(player.player) + " planet " + std::to_string(row.id)
                        + (space ? " (ground; WGA-184/273 qualified authored force inputs)" : " (space; WGA-181..190 force owner)"));
                    cache.force[row.id] = {single(*space), single(*ground)};
                }
                const auto position = [&](gc::PlanetId id) -> const sim::math::Vec3& { return graph->node(id)->position; };
                const auto connect = [&](const std::vector<gc::PlanetId>& reps, std::size_t target_index,
                    bool shortest, std::set<gc::PlanetId>& connectors) -> core::Result<void> {
                    const auto target = reps[target_index]; auto nearest = gc::PlanetId{}; auto nearest_distance = Real(std::numeric_limits<std::int64_t>::max());
                    for (const auto other : reps) if (other != target) {
                        const auto distance = separation(position(other), position(target), true);
                        if (distance < nearest_distance) { nearest = other; nearest_distance = distance; }
                    }
                    const auto& goal = sites[index_of(target)][0];
                    const auto is_goal = [&](gc::PlanetId id) { return id == target || std::find(goal.begin(), goal.end(), id) != goal.end(); };
                    struct Label { Real cost; gc::PlanetId previous{}; };
                    struct Entry { Real priority{}; gc::PlanetId planet{}; Real cost{}; };
                    GalacticAiConnectingHeap<Entry> open;
                    std::map<gc::PlanetId, Label> labels; labels.emplace(nearest, Label{}); open.push({Real{}, nearest, Real{}});
                    std::set<gc::PlanetId> closed;
                    std::size_t allocated = 1;
                    const auto capacity = snapshot->planets().size() * 3; // WGA-325 fixed header/component storage.
                    ++work_.searches;
                    while (!open.empty()) {
                        const auto [priority, current, cost] = open.pop(); static_cast<void>(priority);
                        if (labels.at(current).cost != cost) continue;
                        if (!closed.insert(current).second) continue;
                        if (is_goal(current)) {
                            for (auto id = current; id != 0; id = labels.at(id).previous)
                                if (access(id).relation != gc::PathRelation::allied) connectors.insert(id);
                            return core::Result<void>::success();
                        }
                        for (const auto& edge : graph->node(current)->edges) {
                            ++work_.edges;
                            // WGA-324: connecting search skips closed neighbors,
                            // even for a lower signed cost. It never reopens them.
                            if (closed.contains(edge.to)) continue;
                            Real hop{};
                            const bool free = access(edge.to).relation == gc::PathRelation::allied || connectors.contains(edge.to);
                            if (!access(current).ai_usable || !access(edge.to).ai_usable)
                                hop = single(Real(1000000000000000000LL)); // WGA-322/331: finite sentinel is still enqueued.
                            else if (!free) {
                                hop = separation(position(current), position(edge.to));
                                if (edge.route) {
                                    const auto* route = find(snapshot->routes(), edge.route, [](const auto& at) { return at.id; });
                                    hop = single(hop / single(script::numeric::from_fixed(route->hs_speed_factor)));
                                }
                                if (shortest) hop = single(hop + Real(100));
                                else { hop = single(hop + cache.force.at(edge.to).first); hop = single(hop + cache.force.at(edge.to).second); }
                            }
                            const auto next = single(cost + hop); const auto old = labels.find(edge.to);
                            if (old != labels.end() && old->second.cost <= next) continue;
                            if (allocated == capacity)
                                return refuse<void>("B-U06/WGA-325: connecting path exhausted the original three-times-node storage ceiling");
                            ++allocated;
                            labels.insert_or_assign(edge.to, Label{next, current});
                            Real estimate{};
                            if (!is_goal(edge.to)) {
                                estimate = separation(position(edge.to), position(target));
                                if (graph->largest_route_factor()) estimate = single(estimate / single(script::numeric::from_fixed(*graph->largest_route_factor())));
                                if (free) estimate = single(estimate / Real(2));
                            }
                            // WGA-327: strict total priority, with heap-derived tie order.
                            open.push({single(next + estimate), edge.to, next});
                        }
                    }
                    return core::Result<void>::success();
                };
                std::set<gc::PlanetId> shortest, least, largest;
                for (std::size_t i = 0; i < representatives.size(); ++i) {
                    auto result = connect(representatives, i, true, shortest);
                    if (!result) return core::Result<ai::GalacticConnectivity>::failure(result.error());
                }
                for (std::size_t i = 0; i < representatives.size(); ++i) {
                    auto result = connect(representatives, i, false, least);
                    if (!result) return core::Result<ai::GalacticConnectivity>::failure(result.error());
                }
                auto ordered = representatives;
                std::stable_sort(ordered.begin(), ordered.end(), [&](auto a, auto b) { return sites[index_of(a)][1].size() > sites[index_of(b)][1].size(); });
                ordered.resize(2); auto result = connect(ordered, 0, true, largest);
                if (!result) return core::Result<ai::GalacticConnectivity>::failure(result.error());
                for (const auto id : shortest) staged.at({player.player, id}).shortest = true;
                for (const auto id : least) staged.at({player.player, id}).least_force = true;
                for (const auto id : largest) staged.at({player.player, id}).largest = true;
            }
            cache.goal_reach.clear(); cache.islands.clear(); cache.connectivity_through = std::numeric_limits<std::uint64_t>::max();
            for (const auto& row : snapshot->planets()) {
                const auto& saved = staged.at({player.player, row.id});
                for (std::size_t policy = 0; policy < policies.size(); ++policy) {
                    cache.goal_reach.push_back({player.player, row.id, policies[policy], saved.sites[policy]});
                    cache.connectivity_through = std::min(cache.connectivity_through, *saved.frames[policy] + std::uint64_t(content_->logical_fps) * 10);
                }
                cache.islands.push_back({player.player, row.id, saved.least_force, saved.largest});
            }
            cache.connectivity_ready = true; cache.perception_revision = world.movement_input_revision();
            cache.valid_through = world.force_cache_deadline(player.player, content_->logical_fps, content_->uses_fog);
            if (prepared_through_.at(player.player).through != cache.valid_through) {
                prepared_through_[player.player].through = cache.valid_through; mark_state_changed();
            }
        }
        output.goal_reach.insert(output.goal_reach.end(), cache.goal_reach.begin(), cache.goal_reach.end());
        output.island_paths.insert(output.island_paths.end(), cache.islands.begin(), cache.islands.end());
    }
    if (retained_ != staged || published_.stealth_fleets != output.stealth_fleets) {
        if (publication_revision_ == std::numeric_limits<std::uint64_t>::max())
            return refuse<ai::GalacticConnectivity>("B publication revision is exhausted");
        retained_ = std::move(staged); ++publication_revision_; mark_state_changed();
    }
    reconstructed_ = false;
    // Each player's cache has now consumed this immutable boundary. Recording
    // the final shared perception revision prevents another player's initial
    // cache fills from invalidating an already completed generation.
    const auto perception_revision = world.movement_input_revision();
    for (auto& [player, cache] : caches_) { static_cast<void>(player); cache.perception_revision = perception_revision; }
    for (auto& [player, prepared] : prepared_through_) {
        static_cast<void>(player);
        if (prepared.pending_changes) { prepared.pending_changes = 0; mark_state_changed(); }
    }
    output.revision = publication_revision_; published_ = output;
    transaction.committed = true;
    return core::Result<ai::GalacticConnectivity>::success(std::move(output));
}
core::Result<ga::NativeState> GalacticAiUsabilityProvider::capture() const {
    std::vector<std::uint8_t> bytes;
    const auto put = [&](std::uint64_t value, unsigned width) {
        for (unsigned i = 0; i < width; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (8U * i)));
    };
    // Save the same pending invalidation that an uninterrupted owner observes.
    // A whole-world fingerprint includes unrelated motion/queue clocks and
    // can falsely spend a producer refresh on the first resumed service.
    const auto current = observed_session_ ? observed_session_->snapshot() : nullptr;
    const auto perception = observed_world_ ? observed_world_->movement_input_revision() : 0;
    // WGA-329/GFS-A14: exact flags, ordered site lists and producer deadlines.
    put(3, 4); put(publication_revision_, 8); put(prepared_through_.size(), 4);
    for (const auto& [player, prepared] : prepared_through_) {
        put(player, 4); put(prepared.through.repr, 8);
        std::uint64_t pending = prepared.pending_changes;
        if (current && observed_world_) {
            const auto cached = caches_.find(player);
            if (cached == caches_.end() || !current->planet_access()) pending |= 3U;
            if (cached != caches_.end() && current->planet_access()) {
                if (cached->second.source_changed
                    || cached->second.observer_generation != current->observer_input_generation()
                    || cached->second.access_generation != current->planet_access()->generation
                    || cached->second.perception_revision != perception) pending |= 1U;
                if (!cached->second.connectivity_ready) pending |= 2U;
            }
        }
        put(pending, 1);
    }
    put(retained_.size(), 4);
    for (const auto& [key, row] : retained_) {
        put(key.first, 4); put(key.second, 8);
        put(row.shortest, 1); put(row.least_force, 1); put(row.largest, 1);
        for (std::size_t policy = 0; policy < policies.size(); ++policy) {
            put(row.frames[policy].has_value(), 1);
            if (row.frames[policy]) put(*row.frames[policy], 8);
            if (policy == 4) continue; // WGA-329: saved stamp, unsaved Single_Hop_Disconnected list.
            put(row.sites[policy].size(), 4);
            for (const auto id : row.sites[policy]) put(id, 8);
        }
    }
    ga::NativeState out; out.components.emplace("GALACTIC_USABILITY", std::move(bytes));
    if (auto encoded = ga::encode_native_state(out); !encoded) return core::Result<ga::NativeState>::failure(encoded.error());
    return core::Result<ga::NativeState>::success(std::move(out));
}
core::Result<void> GalacticAiUsabilityProvider::restore(const ga::NativeState& state) {
    const auto found = state.components.find("GALACTIC_USABILITY");
    if (state.version != ga::native_state_version || found == state.components.end()
        || found->second.size() > ga::native_state_max_bytes) return refuse<void>("required B native component is absent or unsupported");
    std::span<const std::uint8_t> bytes = found->second; bool bad = false;
    const auto get = [&](unsigned width) {
        if (width > bytes.size()) { bad = true; return std::uint64_t{}; }
        std::uint64_t value{};
        for (unsigned i = 0; i < width; ++i) value |= std::uint64_t(bytes[i]) << (8U * i);
        bytes = bytes.subspan(width); return value;
    };
    const auto flag = [&]() { const auto value = get(1); if (value > 1) bad = true; return value != 0; };
    if (get(4) != 3) return refuse<void>("unsupported B native component version");
    const auto revision = get(8); const auto deadlines = get(4);
    const auto players = static_cast<std::size_t>(std::count_if(content_->players.begin(), content_->players.end(), [](const auto& row) { return row.ai; }));
    if (deadlines > players || deadlines > bytes.size() / 13) return refuse<void>("invalid B prepared expiry domain");
    decltype(prepared_through_) prepared; gc::PlayerId previous_player{};
    for (std::uint64_t i = 0; i < deadlines; ++i) {
        const auto owner = static_cast<gc::PlayerId>(get(4)); Real through{}; through.repr = get(8);
        const auto* player = find(content_->players, owner, [](const auto& row) { return row.player; });
        if (!player || !player->ai || owner <= previous_player || through < Real{}
            || (through.repr & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL)
            return refuse<void>("invalid B prepared expiry value");
        previous_player = owner; PreparedInput input; input.through = through;
        input.pending_changes = static_cast<std::uint8_t>(get(1));
        if (input.pending_changes > 3) bad = true;
        prepared.emplace(owner, input);
    }
    const auto count = get(4);
    const auto expected = players * content_->planet_order.size();
    if (!revision || (count != 0 && (count != expected || deadlines != players)) || count > bytes.size() / 41)
        return refuse<void>("B native component domain differs from selected campaign");
    std::set<gc::PlanetId> planets(content_->planet_order.begin(), content_->planet_order.end());
    decltype(retained_) staged; std::pair<gc::PlayerId, gc::PlanetId> previous{};
    for (std::uint64_t i = 0; i < count && !bad; ++i) {
        const auto owner = static_cast<gc::PlayerId>(get(4)); const auto planet = get(8);
        const auto key = std::pair{owner, planet};
        const auto* player = find(content_->players, owner, [](const auto& row) { return row.player; });
        if (!player || !player->ai || !planets.contains(planet) || (i && key <= previous)) { bad = true; break; }
        previous = key; Reach row;
        row.shortest = flag(); row.least_force = flag(); row.largest = flag();
        for (std::size_t policy = 0; policy < policies.size() && !bad; ++policy) {
            if (flag()) {
                row.frames[policy] = get(8);
                if (*row.frames[policy] > std::uint64_t(std::numeric_limits<std::int64_t>::max())) bad = true;
            }
            if (policy == 4) continue;
            const auto sites = get(4);
            if (sites > planets.size() || sites > bytes.size() / 8 || (!row.frames[policy] && sites)) { bad = true; break; }
            std::set<gc::PlanetId> unique;
            for (std::uint64_t j = 0; j < sites; ++j) {
                const auto id = get(8);
                if (!planets.contains(id) || !unique.insert(id).second) bad = true;
                row.sites[policy].push_back(id);
            }
        }
        staged.emplace(key, std::move(row));
    }
    if (bad || !bytes.empty()) return refuse<void>("malformed B native component");
    retained_ = std::move(staged); prepared_through_ = std::move(prepared);
    publication_revision_ = revision; caches_.clear(); published_ = {};
    stealth_generation_.reset(); stealth_fleets_.clear();
    observed_session_ = nullptr; observed_world_ = nullptr;
    reconstructed_ = true; mark_state_changed();
    return core::Result<void>::success();
}
core::Result<std::string> GalacticAiUsabilityProvider::state_hash() const {
    const auto saved = capture(); if (!saved) return core::Result<std::string>::failure(saved.error());
    return ga::native_state_hash(saved.value());
}
} // namespace eawr::campaign
