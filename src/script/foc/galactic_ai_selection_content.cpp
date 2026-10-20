#include "galactic_ai_selection_content.hpp"
#include "galactic_ai_selection_work.hpp"

#include <algorithm>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace eawr::script::foc::ai {
namespace {
namespace gc = sim::galactic;
thread_local GalacticSelectionWork selection_work;
using ContentTypeRow = std::map<gc::TypeId, GalacticSelectionContentType>::value_type;
struct SelectionContentIndex {
    struct Traits { Real combat_power; std::uint64_t category_bits{}; };
    std::map<std::string, gc::TypeId, std::less<>> names;
    std::map<gc::TypeId, Traits> traits;
    explicit SelectionContentIndex(const GalacticSelectionContent& content) {
        ++selection_work.content_indices_built;
        // Match the original first result: lowest content type ID for names,
        // and first authored AI row for duplicate type IDs. Copied keys and
        // traits let expired cache entries release without retaining content.
        for (const auto& row : content.types) {
            ++selection_work.content_index_rows;
            names.emplace(row.second.name, row.first);
        }
        for (const auto& row : content.ai.types) {
            ++selection_work.trait_index_rows;
            traits.emplace(row.type_id, Traits{row.combat_power, row.category_bits});
        }
    }
};
std::shared_ptr<const SelectionContentIndex> prepare_content_index(
    const std::shared_ptr<const GalacticSelectionContent>& content) {
    if (!content) return {};
    // The runtime reconstructs the provider for every query. Reuse by exact
    // immutable content identity across those bindings, without keeping the
    // content alive. Index values own their data; expired entries are pruned.
    struct Entry {
        std::weak_ptr<const GalacticSelectionContent> owner;
        std::shared_ptr<const SelectionContentIndex> index;
    };
    static std::mutex mutex;
    static std::vector<Entry> entries;
    std::scoped_lock lock(mutex);
    std::shared_ptr<const SelectionContentIndex> retained;
    for (auto at = entries.begin(); at != entries.end();) {
        const auto owner = at->owner.lock();
        if (!owner) { at = entries.erase(at); continue; }
        if (owner.get() == content.get() && !owner.owner_before(content) && !content.owner_before(owner)) retained = at->index;
        ++at;
    }
    if (retained) return retained;
    auto index = std::make_shared<const SelectionContentIndex>(*content);
    entries.push_back({content, index});
    return index;
}
core::Diagnostic unavailable(std::string message) {
    return detail::make_error(authoritative::codes::missing_api, std::move(message));
}
Real distance(const galactic_ai::TargetInput& a, const galactic_ai::TargetInput& b) {
    const auto x = to_single(numeric::from_fixed(a.position.x) - numeric::from_fixed(b.position.x));
    const auto y = to_single(numeric::from_fixed(a.position.y) - numeric::from_fixed(b.position.y));
    const auto z = to_single(numeric::from_fixed(a.position.z) - numeric::from_fixed(b.position.z));
    return to_single(square_root(to_single(to_single(x * x) + to_single(y * y) + to_single(z * z))));
}
}
GalacticSelectionWork galactic_selection_work() noexcept { return selection_work; }

core::Result<gc::TypeId> galactic_starbase_proposal(const GalacticSelectionContent& content,
    const gc::PlanetProduction* target, gc::TypeId authored) {
    using Result = core::Result<gc::TypeId>;
    const auto marker = content.types.find(authored);
    if (marker == content.types.end()) return Result::failure(unavailable("base proposal original type missing"));
    if (!marker->second.starbase || !target) return Result::success(authored);
    gc::TypeId next{};
    if (target->starbase_type) {
        const auto current = content.types.find(target->starbase_type);
        if (current == content.types.end() || !current->second.original)
            return Result::failure(unavailable("current base original type missing"));
        next = current->second.next_base;
    } else if (const auto first = content.initial_starbase_types.find(target->target_owner);
        first != content.initial_starbase_types.end()) next = first->second;
    if (!next) return Result::success(authored);
    const auto type = content.types.find(next);
    if (type == content.types.end() || !type->second.original)
        return Result::failure(unavailable("next base proposal facts missing"));
    // WGEP-51/481: queued station work and the live cap govern the next-level
    // query, independently of later faction/tech/blockade production admission.
    auto level = target->target_owner && target->space_level < 5
        ? target->starbase_owner ? target->space_level + 1 : std::int64_t{1} : std::int64_t{};
    level = std::min(level, std::min(target->maximum_space_level, std::int64_t{5}));
    for (const auto& queue : target->queues) for (const auto& entry : queue) {
        const auto queued = content.types.find(entry.type);
        if (queued == content.types.end()) return Result::failure(unavailable("queued base type facts missing"));
        if (queued->second.starbase) level = 0;
    }
    return Result::success(type->second.original->base_level.value_or(0) <= level ? next : authored);
}
core::Result<std::vector<gc::PlanetId>> galactic_force_stages(const PlanDef& plan,
    std::span<const galactic_goals::GalacticForceConstraints> constraints, gc::PlanetId target,
    std::string_view reachability, std::span<const gc::PlanetId> sites) {
    using Result = core::Result<std::vector<gc::PlanetId>>;
    if (constraints.size() != plan.taskforces.size()) return Result::failure(unavailable("force staging requires complete authored constraints"));
    std::vector<gc::PlanetId> stages(plan.taskforces.size());
    if (plan.allow_free_store && !plan.magic && target) for (std::size_t force = 0; force < stages.size(); ++force) {
        const auto& options = constraints[force]; gc::PlanetId selected{};
        if (upper_case(reachability) == "SINGLE_HOP_DISCONNECTED") selected = target;
        else for (const auto site : sites) {
            const auto prior = std::span(stages).first(force);
            if (options.unique_stage && std::find(prior.begin(), prior.end(), site) != prior.end()) continue;
            if (!options.unique_stage && options.matching_stage && std::find(prior.begin(), prior.end(), site) == prior.end()) continue;
            selected = site; break;
        }
        stages[force] = selected ? selected : target;
    }
    return Result::success(std::move(stages));
}
static core::Result<bool> read_allowed_production_site(const galactic_ai::WorldInput& input, tactical::PlayerId player, gc::PlanetId planet) {
    auto rows = input.producers.read(player, planet, {}, true);
    if (!rows) return core::Result<bool>::failure(rows.error());
    return core::Result<bool>::success(!rows.value().empty());
}
bool allowed_production_site(const galactic_ai::WorldInput& input, tactical::PlayerId player, gc::PlanetId planet) {
    auto result = read_allowed_production_site(input, player, planet);
    return result && result.value();
}
core::Result<std::vector<GalacticSelectionContrast>> galactic_target_contrast(const GalacticWorld& world,
    const Context& context, std::span<const std::uint64_t> categories, Real classified_scale) {
    using Result = core::Result<std::vector<GalacticSelectionContrast>>;
    std::vector<GalacticSelectionContrast> out;
    for (const auto channel : {gc::ProductionDomain::ground, gc::ProductionDomain::space}) {
        const auto total = world.target_enemy_force(context, channel);
        if (!total) return Result::failure(unavailable("target contrast requires cached enemy force and actual defensive structures"));
        out.push_back({channel, 0, *total, false});
    }
    for (const auto channel : {gc::ProductionDomain::ground, gc::ProductionDomain::space}) {
        const auto total = out[channel == gc::ProductionDomain::ground ? 0U : 1U].force;
        for (const auto category : categories) {
            if (!category) return Result::failure(unavailable("classified target contrast category must be nonzero"));
            Real force{};
            if (Real{} < total) {
                const auto value = world.target_enemy_force(context, channel, category);
                if (!value) return Result::failure(unavailable("classified target contrast force is unavailable"));
                force = *value * classified_scale;
            }
            out.push_back({channel, category, force, false});
        }
    }
    return Result::success(std::move(out));
}
namespace {
core::Result<std::vector<GalacticSelectionGarrison>> proposal_garrisons(
    const GalacticSelectionContent& content, gc::TypeId type, std::uint32_t tech, gc::PlanetId target,
    const SelectionContentIndex* prepared) {
    using Result = core::Result<std::vector<GalacticSelectionGarrison>>;
    const auto source = content.types.find(type);
    if (source == content.types.end() || !source->second.original)
        return Result::failure(unavailable("proposal garrison expansion requires its original type"));
    const auto& levels = source->second.original->starting_spawned_units;
    if (tech >= levels.size()) return Result::failure(unavailable("proposal garrison tech is outside the authored level table"));
    auto level = tech;
    while (level && levels[level].empty()) --level;
    const auto& spawned = levels[level];
    const auto planet = content.planets.find(target);
    if (target && planet == content.planets.end()) return Result::failure(unavailable("proposal garrison target terrain is unavailable"));
    std::vector<GalacticSelectionGarrison> out;
    const auto append = [&](std::string_view name) -> core::Result<void> {
        const auto canonical = upper_case(name);
        const ContentTypeRow* unit{};
        if (prepared) {
            ++selection_work.garrison_name_lookups;
            const auto found = prepared->names.find(canonical);
            if (found != prepared->names.end()) {
                const auto row = content.types.find(found->second);
                if (row != content.types.end()) unit = &*row;
            }
        } else {
            const auto found = std::find_if(content.types.begin(), content.types.end(),
                [&](const auto& row) { return row.second.name == canonical; });
            if (found != content.types.end()) unit = &*found;
        }
        if (!unit) return core::Result<void>::failure(unavailable("proposal garrison type is unbound: " + canonical));
        const Real* combat_power{};
        if (prepared) {
            ++selection_work.garrison_trait_lookups;
            const auto found = prepared->traits.find(unit->first);
            if (found != prepared->traits.end()) combat_power = &found->second.combat_power;
        } else {
            const auto found = std::find_if(content.ai.types.begin(), content.ai.types.end(),
                [&](const auto& row) { return row.type_id == unit->first; });
            if (found != content.ai.types.end()) combat_power = &found->combat_power;
        }
        if (!combat_power) return core::Result<void>::failure(unavailable("proposal garrison combat metric is unavailable"));
        Real terrain = real(1);
        if (planet != content.planets.end()) {
            if (const auto environment = content.terrain.find(planet->second.terrain); environment != content.terrain.end())
                if (const auto movement = environment->second.find(unit->second.movement_class); movement != environment->second.end()) terrain = movement->second;
        }
        const auto fallback = unit->second.company || unit->second.ground_base ? gc::ProductionDomain::ground : gc::ProductionDomain::space;
        out.push_back({unit->first, fallback, to_single(*combat_power), terrain, true, unit->second.value.population});
        return core::Result<void>::success();
    };
    for (const auto& spawn : spawned) {
        // The original count getter selects the first entry for this type,
        // even when the authored list contains the same type again.
        const auto first = std::find_if(spawned.begin(), spawned.end(),
            [&](const auto& row) { return upper_case(row.type) == upper_case(spawn.type); });
        for (std::int32_t count = 0; count < first->count; ++count)
            if (const auto added = append(spawn.type); !added) return Result::failure(added.error());
    }
    for (const auto& escort : source->second.space_escorts)
        if (const auto added = append(escort); !added) return Result::failure(added.error());
    return Result::success(std::move(out));
}
}
core::Result<std::vector<GalacticSelectionGarrison>> galactic_proposal_garrisons(
    const GalacticSelectionContent& content, gc::TypeId type, std::uint32_t tech, gc::PlanetId target) {
    return proposal_garrisons(content, type, tech, target, nullptr);
}
bool galactic_hero_stage_applies(bool authored_hero, bool named_hero, bool special_weapon, bool authors_script) noexcept {
    return authored_hero && (named_hero || special_weapon || authors_script);
}
GalacticSelectionProvider stock_selection_provider(std::shared_ptr<const GalacticSelectionContent> content,
    std::shared_ptr<const galactic_goals::MaterializedPlanCatalog> plans, std::shared_ptr<const GalacticWorld> world,
    std::shared_ptr<const Engine> engine, std::function<std::shared_ptr<const gc::GalacticSnapshot>()> snapshot,
    GalacticHeroSelectionProvider heroes, GalacticFreestoreSelectionProvider freestore) {
    auto prepared = prepare_content_index(content);
    return [content = std::move(content), prepared = std::move(prepared), plans = std::move(plans), world = std::move(world),
            engine = std::weak_ptr<const Engine>(engine), snapshot = std::move(snapshot), heroes = std::move(heroes), freestore = std::move(freestore)]
        (tactical::PlayerId player, gc::PlanetId target, const GoalType& goal, const PlanDef& plan)
        -> core::Result<std::shared_ptr<const GalacticSelectionInput>> {
        using Result = core::Result<std::shared_ptr<const GalacticSelectionInput>>;
        const auto manager = engine.lock(); const auto state = snapshot ? snapshot() : nullptr;
        const auto input = world ? world->copy_input() : nullptr;
        if (!manager || !content || !plans || !world || !state || !input || !state->production() || !state->player_economy())
            return Result::failure(unavailable("stock selection requires completed world, content and production owners"));
        auto out = std::make_shared<GalacticSelectionInput>();
        const auto authored = plans->constraints.find(plan.name);
        if (authored == plans->constraints.end()) return Result::failure(unavailable("stock plan constraints missing"));
        if (authored->second.additional_cost.value_or(Real{}) != Real{})
            out->missing_facts.push_back("stock nonzero AdditionalCost retained roster and paid-cost initialization");
        if (plan.magic) out->missing_facts.push_back("stock magic live lock, unlock, disable and named space hero admission");
        if (target && !plan.ignore_target && !plan.galactic_contrast_qualified)
            out->missing_facts.push_back("stock plan-authored target contrast lists");
        if (plan.allow_free_store && !plan.magic) {
            if (!freestore) out->missing_facts.push_back("stock freestore membership, AI usability, loss state and path producer");
            else {
                const auto copied = freestore(player, target, plan);
                if (!copied) return Result::failure(copied.error());
                out->freestore = copied.value();
            }
        }
        if (!out->missing_facts.empty()) return Result::success(std::move(out));
        if (target && !plan.ignore_target) {
            PlayerAi selected; selected.player = player;
            const auto context = world->goal_context(selected, manager->target_of_object(target));
            const auto contrast = galactic_target_contrast(*world, context, plan.galactic_contrast_order, real(1));
            if (!contrast) return Result::failure(contrast.error());
            out->contrast = contrast.value(); out->unscaled_domain_totals = true;
        }
        out->minimum_cost = authored->second.minimum_cost.value_or(Real{}); out->maximum_cost = Real{}; // No authored maximum-plan-cost global is applied.
        for (const auto& force : authored->second.forces)
            out->attachment_allowed.emplace_back(!force.deny_heroes, !force.deny_special_weapons && plan.allow_special_weapons);
        const auto account = std::find_if(state->player_economy()->players.begin(), state->player_economy()->players.end(), [&](const auto& row) { return row.player == player; });
        if (account == state->player_economy()->players.end()) return Result::failure(unavailable("stock selection player account missing"));
        out->player_tech = static_cast<std::uint32_t>(account->current_tech);
        if (!content->population_check) return Result::failure(unavailable("stock selection global population gate unqualified"));
        out->population_check = *content->population_check;
        const auto population = std::find_if(state->population().begin(), state->population().end(), [&](const auto& row) { return row.player == player; });
        if (out->population_check && population == state->population().end()) return Result::failure(unavailable("stock selection population census missing"));
        if (population != state->population().end()) out->remaining_population = to_single(numeric::from_fixed(population->capacity) - numeric::from_fixed(population->used));
        const auto destination = std::find_if(input->targets.begin(), input->targets.end(), [&](const auto& row) { return row.planet == target; });
        if (target && destination == input->targets.end()) return Result::failure(unavailable("stock selection target absent"));
        for (const auto& site : input->targets) {
            const auto allowed = read_allowed_production_site(*input, player, site.planet);
            if (!allowed) return Result::failure(allowed.error());
            if (!allowed.value()) continue;
            const auto reach = target ? world->goal_reachable(player, target, site.planet, goal.reachability) : std::optional{true};
            if (!reach) return Result::failure(unavailable("stock production reachability owner unavailable"));
            if (*reach) out->production_sites.push_back(site.planet);
        }
        out->reachable = !target || !out->production_sites.empty();
        // WGA-254: the freestore marshaller initializes staging only for an
        // ordinary freestore-enabled plan. Missing staging falls back to target.
        const auto stages = galactic_force_stages(plan, authored->second.forces, target, goal.reachability, out->production_sites);
        if (!stages) { return Result::failure(stages.error()); }
        out->force_stages = stages.value();
        const gc::PlanetProduction* location{};
        for (const auto& row : state->production()->planets) if (row.planet == target) location = &row;
        std::set<gc::TypeId> proposals;
        for (const auto& force : plan.taskforces) for (const auto& team : force.teams) for (const auto type : team.types) {
            const auto resolved = galactic_starbase_proposal(*content, location, type);
            if (!resolved) return Result::failure(resolved.error());
            if (resolved.value() != type) out->proposal_substitutions.emplace(type, resolved.value());
            proposals.insert(resolved.value());
        }
        const std::vector<gc::TypeId> requested(proposals.begin(), proposals.end());
        // Only rows of the player's reachable production sites survive the filter below, so read exactly those
        // planets, in ascending planet order: the same rows, in the order of the player-wide read, without
        // evaluating the admission of every other planet's offers.
        std::vector<gc::PlanetId> site_order = out->production_sites;
        std::sort(site_order.begin(), site_order.end());
        std::vector<galactic_ai::ProducerInput> site_rows;
        if (!requested.empty()) for (const auto site : site_order) {
            auto read = input->producers.read(player, site, requested);
            if (!read) return Result::failure(read.error());
            site_rows.insert(site_rows.end(), read.value().begin(), read.value().end());
        }
        for (auto producer : site_rows) {
            const auto type = content->types.find(producer.type); if (type == content->types.end()) continue;
            producer.reachable = true; // read for a reachable production site only
            if (target) {
                const auto source = std::find_if(input->targets.begin(), input->targets.end(), [&](const auto& row) { return row.planet == producer.planet; });
                if (source == input->targets.end()) return Result::failure(unavailable("stock production source absent"));
                producer.distance = distance(*source, *destination);
            }
            producer.hyperspace_speed = type->second.hyperspace_speed;
            const auto factor = content->faction_speed.find(player);
            if (factor == content->faction_speed.end()) return Result::failure(unavailable("stock production faction speed missing"));
            producer.faction_speed_factor = factor->second;
            if (!content->technology_weight && !type->second.value.structure && producer.base_level != producer.required_level)
                return Result::failure(unavailable("stock production technology estimate constant unqualified"));
            producer.technology_weight = content->technology_weight.value_or(Real{});
            out->producers.push_back(producer);
        }
        if (location) {
            for (std::size_t channel = 0; channel < 2; ++channel) {
                const auto occupied = std::count_if(location->objects.begin(), location->objects.end(), [&](const auto& object) {
                    return object.kind == gc::ProductionKind::structure && static_cast<std::size_t>(object.domain) == channel;
                });
                const auto slots = channel == 0 ? location->ground_slots : location->space_slots;
                out->remaining_structure_slots_by_channel[channel] = static_cast<std::uint32_t>(std::max<std::int64_t>(slots - occupied, 0));
            }
        }
        const auto planet = content->planets.find(target);
        for (const auto id : proposals) {
            const auto original = content->types.find(id);
            if (original == content->types.end()) return Result::failure(unavailable("stock proposal original type missing"));
            const auto& source = original->second;
            auto facts = source.value; facts.survival_type = source.survival_type;
            // WGA-489: a generic hero candidate whose type authors no object script is an ordinary unit (no hero stage,
            // no hero gates). Named heroes keep the hero stage whatever they author.
            facts.hero = galactic_hero_stage_applies(facts.hero, source.named_hero, source.special_weapon, !source.lua_script.empty() || source.member_script);
            // WGA-482: both base kinds use target admission/quotes, without
            // the special-structure instance and slot checks.
            facts.base = source.starbase || source.ground_base;
            facts.contrast_fallback_channel = source.company || source.ground_base ? gc::ProductionDomain::ground : gc::ProductionDomain::space;
            if (target && !plan.ignore_target) {
                const auto garrisons = proposal_garrisons(*content, id, out->player_tech, target, prepared.get());
                if (!garrisons) return Result::failure(garrisons.error());
                facts.garrisons = garrisons.value();
            }
            facts.terrain_allowed = true; facts.terrain_effectiveness = real(1);
            ++selection_work.proposal_trait_lookups;
            const auto found_type = prepared->traits.find(id);
            const auto* type = found_type == prepared->traits.end() ? nullptr : &found_type->second;
            if (!type) return Result::failure(unavailable("stock original AI type missing"));
            if (planet != content->planets.end()) {
                facts.terrain_allowed = (planet->second.restricted_categories & type->category_bits) == 0
                    && std::find(planet->second.restricted_types.begin(), planet->second.restricted_types.end(), id) == planet->second.restricted_types.end();
                if (const auto terrain = content->terrain.find(planet->second.terrain); terrain != content->terrain.end())
                    if (const auto movement = terrain->second.find(source.movement_class); movement != terrain->second.end()) facts.terrain_effectiveness = movement->second;
            }
            facts.instance_allowed = true; facts.instance_remaining = std::numeric_limits<std::uint32_t>::max();
            if (facts.structure) {
                facts.instance_allowed = false;
                if (location) for (const auto& offer : location->offers) if (offer.type == id) {
                    const auto count = std::count_if(location->objects.begin(), location->objects.end(), [&](const auto& object) { return object.type == id; });
                    facts.instance_allowed = offer.planet_limit < 0 || count < offer.planet_limit;
                    facts.instance_remaining = offer.planet_limit < 0 ? std::numeric_limits<std::uint32_t>::max()
                        : static_cast<std::uint32_t>(std::max<std::int64_t>(offer.planet_limit - count, 0));
                }
            }
            if (facts.structure || facts.base) for (const auto& producer : out->producers)
                if (producer.type == id && producer.planet == target && producer.admission == gc::ProductionAdmission::allowed) {
                    facts.target_cost = numeric::from_fixed(producer.cost);
                    facts.target_build_seconds = numeric::from_fixed(producer.build_seconds);
                    break;
                }
            if (facts.hero && heroes) {
                const auto weights = heroes(player, target, id, plan); if (!weights) return Result::failure(weights.error());
                facts.hero_weight = weights.value().target_weight; facts.escort_weight = weights.value().escort_weight;
                facts.hero_minimum_cost = weights.value().minimum_cost; facts.hero_maximum_cost = weights.value().maximum_cost;
                facts.hero_containment_allowed = weights.value().containment_allowed;
            } // Absent hero owner rejects that candidate through containment, never a guessed weight.
            out->types.emplace(id, std::move(facts));
        }
        out->complete = true;
        return Result::success(std::move(out));
    };
}
}
