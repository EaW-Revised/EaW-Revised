#include "galactic_ai_world.hpp"
#include "eawr/core/gc_profile.hpp"
#include "eawr/sim/galactic/production.hpp"

#include <stdexcept>

namespace eawr::script::foc::ai {
namespace {
template<class Rows, class Id, class Key>
const auto* find_row(const Rows& rows, Id id, Key key) {
    const auto found = std::lower_bound(rows.begin(), rows.end(), id,
        [&](const auto& row, Id value) { return key(row) < value; });
    return found != rows.end() && key(*found) == id ? &*found : nullptr;
}
}

// Derived preparation only: admission and queue clocks always read the incoming snapshot.
// Retaining the exact source snapshot makes equal-input reuse safe across rollback/restore.
struct GalacticWorld::PreparedProducerOffers {
    struct Offer { galactic::ProductionOffer effective; std::uint64_t frames{}; };
    using Prices = std::vector<Offer>;
    struct PriceCache {
        mutable std::function<core::Result<std::shared_ptr<const Prices>>()> prepare;
        mutable std::once_flag once;
        mutable std::optional<core::Result<std::shared_ptr<const Prices>>> value;
        mutable std::atomic<bool> ready{};
        mutable std::map<galactic::TypeId, std::vector<std::size_t>> by_type;
        core::Result<std::shared_ptr<const Prices>> get() const {
            std::call_once(once, [&] {
                value = prepare();
                if (*value) {
                    const auto& offers = *value->value();
                    for (std::size_t index = 0; index < offers.size(); ++index)
                        by_type[offers[index].effective.type].push_back(index);
                }
                // Drop the preparer's snapshot after its immutable values exist.
                prepare = {};
                ready.store(true, std::memory_order_release);
            });
            return *value;
        }
    };
    struct Job {
        tactical::PlayerId player{};
        galactic::PlanetId planet{};
        std::size_t source_owner{}, source_location{};
        std::shared_ptr<const std::vector<Offer>> offers;
        std::shared_ptr<const std::vector<BuildRow>> rows;
        std::shared_ptr<PriceCache> prices;
        galactic::ProductionReservations reservations;
    };
    std::shared_ptr<const galactic::GalacticSnapshot> source;
    std::vector<galactic::ProductionReductionSource> reductions;
    std::shared_ptr<const std::vector<galactic::ProductionReductionSource>> price_reductions;
    std::vector<Job> jobs;
    bool unique_jobs{true};
    sim::EntityId next_id{};
    std::size_t live_objects{};
    mutable std::atomic<std::uint64_t> queried_offers{};
};

core::Result<std::vector<galactic_ai::ProducerInput>> GalacticWorld::read_producers(
    const PreparedProducerOffers& catalog, std::uint32_t logical_fps, tactical::PlayerId player,
    galactic::PlanetId planet_id, std::span<const galactic::TypeId> types, bool first_allowed, const sim::PartitionExecutor& executor) {
    using Result = core::Result<std::vector<galactic_ai::ProducerInput>>;
    std::vector<galactic_ai::ProducerInput> rows;
    const auto& snapshot = *catalog.source;
    if (!snapshot.production() || !snapshot.player_economy()) return Result::success({});
    const auto& production = *snapshot.production();
    std::vector<const PreparedProducerOffers::Job*> jobs;
    for (const auto& job : catalog.jobs)
        if ((!player || job.player == player) && (!planet_id || job.planet == planet_id)) jobs.push_back(&job);
    std::sort(jobs.begin(), jobs.end(), [](const auto* a, const auto* b) {
        return std::tie(a->source_owner, a->source_location) < std::tie(b->source_owner, b->source_location);
    });
    std::vector<std::vector<galactic_ai::ProducerInput>> staged(jobs.size());
    std::vector<std::optional<core::Diagnostic>> errors(jobs.size());
    const auto prepare_job = [&](std::size_t index) -> core::Result<void> {
        const auto& job = *jobs[index]; auto& output = staged[index];
        const auto* owner = &production.players[job.source_owner];
        const auto* location = &production.planets[job.source_location];
        const auto* account = find_row(snapshot.player_economy()->players, job.player, [](const auto& p) { return p.player; });
        const auto* population = find_row(snapshot.population(), job.player, [](const auto& p) { return p.player; });
        const auto* at = find_row(snapshot.planets(), job.planet, [](const auto& p) { return p.id; });
        if (!owner || !location || !account || !at || !job.prices) return core::Result<void>::success();
        auto prices = job.prices->get(); if (!prices) return core::Result<void>::failure(prices.error());
        std::array<math::Fixed, 2> queue_seconds{};
        for (std::size_t channel = 0; channel < queue_seconds.size(); ++channel) for (const auto& entry : location->queues[channel]) {
            auto total = math::add(queue_seconds[channel], entry.last_remaining);
            if (!total) return core::Result<void>::failure(total.error());
            queue_seconds[channel] = total.value();
        }
        std::vector<std::size_t> selected;
        if (!types.empty()) {
            for (const auto type : types) if (const auto found = job.prices->by_type.find(type); found != job.prices->by_type.end())
                selected.insert(selected.end(), found->second.begin(), found->second.end());
            // Preserve original offer registration order, including duplicate
            // offer identities, while repeated requested types do not add rows.
            std::sort(selected.begin(), selected.end());
            selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
        }
        const auto count = types.empty() ? prices.value()->size() : selected.size();
        for (std::size_t offer_index = 0; offer_index < count; ++offer_index) {
            const auto& prepared = (*prices.value())[types.empty() ? offer_index : selected[offer_index]];
            const auto& offer = prepared.effective;
            catalog.queried_offers.fetch_add(1, std::memory_order_relaxed);
            if (prepared.frames > std::numeric_limits<std::uint32_t>::max()) continue;
            const galactic::ProductionAdmissionInputs inputs{production, *location, *owner, *account,
                snapshot.fleets(), at->owner, population, catalog.next_id, catalog.live_objects, &job.reservations};
            const auto admission = galactic::production_admission(inputs, &offer, offer.domain, !owner->human);
            if (first_allowed && admission != galactic::ProductionAdmission::allowed) continue;
            galactic_ai::ProducerInput row;
            row.producer = row.planet = job.planet; row.player = job.player; row.type = offer.type;
            row.domain = offer.domain; row.admission = admission; row.cost = offer.price; row.build_seconds = offer.seconds;
            const auto channel = offer.domain == galactic::ProductionDomain::ground ? 0U : 1U;
            row.queue_seconds = queue_seconds[channel]; row.logical_fps = logical_fps;
            row.base_level = static_cast<std::uint32_t>(channel == 0 ? location->ground_level : location->space_level);
            row.required_level = static_cast<std::uint32_t>(channel == 0 ? offer.required_ground_level : offer.required_space_level);
            output.push_back(row);
            if (first_allowed) return core::Result<void>::success();
        }
        return core::Result<void>::success();
    };
    if (jobs.size() == 1) {
        if (auto prepared = prepare_job(0); !prepared) return Result::failure(prepared.error());
    } else if (!jobs.empty()) {
        const auto phase = executor.execute_phase("galactic-ai-demand-producers", sim::tick_partition_count, [&](std::size_t partition) {
            const auto range = sim::partition_range(partition, jobs.size());
            for (auto index = range.begin; index < range.end; ++index)
                if (auto prepared = prepare_job(index); !prepared) errors[index] = prepared.error();
        });
        if (!phase) return Result::failure(phase.error());
    }
    for (std::size_t index = 0; index < jobs.size(); ++index) {
        if (errors[index]) return Result::failure(*errors[index]);
        rows.insert(rows.end(), staged[index].begin(), staged[index].end());
        if (first_allowed && !rows.empty()) break;
    }
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        return std::tie(a.player, a.planet, a.domain, a.type) < std::tie(b.player, b.planet, b.domain, b.type);
    });
    return Result::success(std::move(rows));
}

core::Result<void> GalacticWorld::refresh_live_producers(
    std::shared_ptr<const galactic::GalacticSnapshot> snapshot, sim::EntityId frontier, std::size_t objects) {
    build_options_.clear();
    if (!snapshot->production() || !snapshot->player_economy()) {
        prepared_producer_offers_.reset();
        return core::Result<void>::success();
    }
    const auto& production = *snapshot->production();
    const auto* income = snapshot->income() ? &snapshot->income()->inputs : nullptr;
    auto next = std::make_shared<PreparedProducerOffers>();
    next->source = snapshot; next->next_id = frontier; next->live_objects = objects;
    next->reductions = income ? galactic::production_reduction_sources(*income, snapshot->planets())
        : std::vector<galactic::ProductionReductionSource>{};
    const auto* previous = prepared_producer_offers_.get();
    const auto* old_income = previous && previous->source->income() ? &previous->source->income()->inputs : nullptr;
    const auto reduction_key = [](const auto& row) {
        return std::tie(row.type, row.price, row.id, row.planet, row.planet_owner, row.planet_source,
            row.intimidation, row.galaxy_wide, row.category, row.reduction);
    };
    const bool same_prices = previous && previous->unique_jobs && previous->price_reductions
        && previous->source->production() && bool(income) == bool(old_income)
        && (!income || income->offers == old_income->offers)
        && std::equal(next->reductions.begin(), next->reductions.end(), previous->reductions.begin(), previous->reductions.end(),
            [&](const auto& a, const auto& b) { return reduction_key(a) == reduction_key(b); });
    next->price_reductions = same_prices ? previous->price_reductions
        : std::make_shared<const std::vector<galactic::ProductionReductionSource>>(next->reductions);
    next->jobs.reserve(production.players.size() * production.planets.size());
    for (std::size_t owner_index = 0; owner_index < production.players.size(); ++owner_index) {
        const auto& owner = production.players[owner_index];
        const auto reservations = galactic::production_reservations(production, owner.player);
        for (std::size_t location_index = 0; location_index < production.planets.size(); ++location_index) {
            const auto& location = production.planets[location_index];
            PreparedProducerOffers::Job job;
            job.player = owner.player; job.planet = location.planet;
            job.source_owner = owner_index; job.source_location = location_index; job.reservations = reservations;
            if (find_row(snapshot->player_economy()->players, owner.player, [](const auto& row) { return row.player; })
                && find_row(snapshot->planets(), location.planet, [](const auto& row) { return row.id; })) {
                // Preserve the atomic binder's checked queue-clock refusal even
                // when no service requests this producer on the current frame.
                for (const auto& queue : location.queues) {
                    math::Fixed seconds;
                    for (const auto& entry : queue) {
                        auto total = math::add(seconds, entry.last_remaining);
                        if (!total) return core::Result<void>::failure(total.error());
                        seconds = total.value();
                    }
                }
                if (same_prices) {
                    const auto* old_location = find_row(previous->source->production()->planets, location.planet,
                        [](const auto& row) { return row.planet; });
                    const auto* old_job = find_row(previous->jobs, std::pair{owner.player, location.planet},
                        [](const auto& row) { return std::pair{row.player, row.planet}; });
                    if (old_location && old_job && old_location->offers == location.offers) job.prices = old_job->prices;
                }
                if (!job.prices) {
                    job.prices = std::make_shared<PreparedProducerOffers::PriceCache>();
                    job.prices->prepare = [source = snapshot, reductions = next->price_reductions,
                        location_index, selected = owner.player]() -> core::Result<std::shared_ptr<const PreparedProducerOffers::Prices>> {
                        using Result = core::Result<std::shared_ptr<const PreparedProducerOffers::Prices>>;
                        const auto& at = source->production()->planets[location_index];
                        const auto* inputs = source->income() ? &source->income()->inputs : nullptr;
                        auto offers = std::make_shared<PreparedProducerOffers::Prices>(); offers->reserve(at.offers.size());
                        for (const auto& offer : at.offers) {
                            auto effective = galactic::prepared_production_offer(offer, inputs, *reductions, at.planet, selected);
                            if (!effective) return Result::failure(effective.error());
                            auto frames = galactic::production_frames(effective.value().seconds);
                            if (!frames) return Result::failure(frames.error());
                            offers->push_back({std::move(effective).value(), frames.value()});
                        }
                        return Result::success(std::move(offers));
                    };
                }
            }
            next->jobs.push_back(std::move(job));
        }
    }
    std::sort(next->jobs.begin(), next->jobs.end(), [](const auto& a, const auto& b) {
        return std::pair{a.player, a.planet} < std::pair{b.player, b.planet};
    });
    next->unique_jobs = std::adjacent_find(next->jobs.begin(), next->jobs.end(), [](const auto& a, const auto& b) {
        return a.player == b.player && a.planet == b.planet;
    }) == next->jobs.end();
    prepared_producer_offers_ = std::move(next);
    return core::Result<void>::success();
}

galactic_ai::ProducerInputs::Query GalacticWorld::producer_query() const {
    using Result = core::Result<galactic_ai::ProducerInputs::Rows>;
    // This memo belongs to one immutable completed input. A subsequent bind,
    // including a clock-only delta, publishes a fresh memo; retained inputs
    // keep their own source and results. No memo data is authoritative.
    struct Memo {
        using Key = std::tuple<tactical::PlayerId, galactic::PlanetId, std::vector<galactic::TypeId>, bool>;
        std::mutex mutex;
        std::map<Key, Result> rows;
    };
    return [memo = std::make_shared<Memo>(), catalog = prepared_producer_offers_, fps = setup_.logical_fps,
        workers = read_workers_, supplier = producer_executor_](auto owner, auto planet, auto types, bool first_allowed) -> Result {
        std::vector<galactic::TypeId> requested(types.begin(), types.end());
        std::sort(requested.begin(), requested.end());
        requested.erase(std::unique(requested.begin(), requested.end()), requested.end());
        const Memo::Key key{owner, planet, std::move(requested), first_allowed};
        // Admission performs no recursive producer reads. Serializing publication
        // guarantees one evaluation even when service partitions share a key.
        std::scoped_lock lock(memo->mutex);
        if (const auto found = memo->rows.find(key); found != memo->rows.end()) return found->second;
        static const sim::InlineExecutor inline_executor;
        const auto* active = supplier ? supplier() : nullptr;
        auto result = read_producers(*catalog, fps, owner, planet, std::get<2>(key), first_allowed,
            active ? *active : workers ? *workers : inline_executor);
        return memo->rows.emplace(key, std::move(result)).first->second;
    };
}

std::uint64_t GalacticWorld::producer_demand_offer_checks() const noexcept {
    return prepared_producer_offers_ ? prepared_producer_offers_->queried_offers.load(std::memory_order_relaxed) : 0;
}

core::Result<void> GalacticWorld::prepare_catalog_slice(const sim::PartitionExecutor& executor, std::uint64_t frame) {
    if (!live_deltas_ || !prepared_producer_offers_ || last_catalog_slice_ == frame) return core::Result<void>::success();
    const auto& jobs = prepared_producer_offers_->jobs;
    const auto count = jobs.size() / perception_period_ + (jobs.size() % perception_period_ != 0 ? 1 : 0);
    const auto slice = frame % perception_period_;
    // Fixed sim-frame slices: no wall-clock budgets, worker timing or catch-up.
    if (count && slice < (jobs.size() + count - 1) / count) {
        const auto begin = static_cast<std::size_t>(slice) * count;
        const auto end = std::min(jobs.size(), begin + count);
        std::vector<std::optional<core::Diagnostic>> errors(end - begin);
        std::vector<std::uint64_t> prepared(end - begin);
        const auto phase = executor.execute_phase("galactic-ai-perception-catalog", sim::tick_partition_count, [&](std::size_t partition) {
            const auto range = sim::partition_range(partition, end - begin);
            for (auto index = range.begin; index < range.end; ++index) if (const auto& cache = jobs[begin + index].prices) {
                const bool ready = cache->ready.load(std::memory_order_acquire);
                auto result = cache->get();
                if (!result) errors[index] = result.error();
                else if (!ready) prepared[index] = result.value()->size();
            }
        });
        if (!phase) return phase;
        for (std::size_t index = 0; index < errors.size(); ++index) {
            if (errors[index]) return core::Result<void>::failure(*errors[index]);
            work_.producer_offer_preparations += prepared[index];
        }
    }
    last_catalog_slice_ = frame;
    return core::Result<void>::success();
}

GalacticWorld::GalacticWorld(GalacticPerceptionSetup setup) : setup_(std::move(setup)) {
    if (setup_.logical_fps == 0 || setup_.players.size() > 64)
        throw std::invalid_argument("invalid galactic perception logical rate/player count");
    std::sort(setup_.players.begin(), setup_.players.end(), [](const auto& a, const auto& b) { return a.player < b.player; });
    std::sort(setup_.planets.begin(), setup_.planets.end(), [](const auto& a, const auto& b) { return a.planet < b.planet; });
    std::sort(setup_.types.begin(), setup_.types.end(), [](const auto& a, const auto& b) { return a.type_id < b.type_id; });
    std::sort(setup_.force_types.begin(), setup_.force_types.end(), [](const auto& a, const auto& b) { return a.type < b.type; });
    for (auto& type : setup_.economy_types) for (auto& faction : type.affiliations) {
        faction = upper_case(faction);
        if (type.advances_tech) next_technology_types_[faction].try_emplace(type.advances_tech, type.type);
    }
    std::sort(setup_.economy_types.begin(), setup_.economy_types.end(), [](const auto& a, const auto& b) { return a.type < b.type; });
    if (std::adjacent_find(setup_.economy_types.begin(), setup_.economy_types.end(),
        [](const auto& a, const auto& b) { return a.type == b.type; }) != setup_.economy_types.end())
        throw std::invalid_argument("duplicate galactic economy type identity");
    for (const auto& type : setup_.economy_types) {
        const auto valid = [](Real value) { return (value.repr & 0x7ff0000000000000ULL) != 0x7ff0000000000000ULL; };
        if (!type.type || !valid(type.generic_price) || !valid(type.generic_seconds)
            || type.generic_price < Real{} || real(std::numeric_limits<std::int32_t>::max()) < type.generic_price
            || type.generic_seconds < Real{} || (type.maintenance && !valid(*type.maintenance)))
            throw std::invalid_argument("invalid galactic economy type metadata");
    }
    for (const auto maximum : {setup_.maximum_ground_level, setup_.maximum_space_level})
        if (maximum && (!(Real{} < *maximum) || real(std::numeric_limits<std::uint32_t>::max()) < *maximum
            || (maximum->repr & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL))
            throw std::invalid_argument("invalid galactic base normalization maximum");
    for (const auto& [id, factor] : setup_.faction_maintenance)
        if (!player(id) || (factor.repr & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL)
            throw std::invalid_argument("invalid galactic faction maintenance input");
    if (std::adjacent_find(setup_.force_types.begin(), setup_.force_types.end(), [](const auto& a, const auto& b) { return a.type == b.type; }) != setup_.force_types.end())
        throw std::invalid_argument("duplicate galactic force type identity");
    for (const auto& type : setup_.force_types) if (!type.type
        || (type.power.repr & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL)
        throw std::invalid_argument("invalid galactic force type metric");
    if (setup_.maximum_fleet_distance && (*setup_.maximum_fleet_distance < Real{}
        || (setup_.maximum_fleet_distance->repr & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL))
        throw std::invalid_argument("invalid galactic force neighbourhood distance");
    if (std::adjacent_find(setup_.players.begin(), setup_.players.end(), [](const auto& a, const auto& b) { return a.player == b.player; }) != setup_.players.end()
        || std::adjacent_find(setup_.planets.begin(), setup_.planets.end(), [](const auto& a, const auto& b) { return a.planet == b.planet; }) != setup_.planets.end()
        || std::adjacent_find(setup_.types.begin(), setup_.types.end(), [](const auto& a, const auto& b) { return a.type_id == b.type_id; }) != setup_.types.end())
        throw std::invalid_argument("duplicate galactic perception identity");
    for (const auto& row : setup_.players) if (!row.player || !row.team)
        throw std::invalid_argument("galactic perception player/team identity must be explicit");
    std::map<std::string, Real, std::less<>> intervals;
    for (const auto& [name, interval] : setup_.cache_intervals) {
        if (interval < real(-1) || (interval.repr & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL)
            throw std::invalid_argument("invalid galactic perception cache interval");
        intervals.emplace(upper_case(name), interval);
    }
    setup_.cache_intervals = std::move(intervals);
    // WGA-191: stock evaluator-specific seconds; explicit provider overrides win.
    for (const auto side : {"FRIENDLYFORCE.", "ENEMYFORCE."}) {
        for (const auto token : {"SPACETOTAL", "GROUNDTOTAL", "SPACETOTALUNNORMALIZED", "GROUNDTOTALUNNORMALIZED",
            "SPACETOTALNBTD", "GROUNDTOTALNBTD", "SPACETOTALNBTT", "GROUNDTOTALNBTT", "SPACETOTALNBPTM", "GROUNDTOTALNBPTM"})
            setup_.cache_intervals.try_emplace(std::string(side) + token, real(15));
        for (const auto token : {"HASSPACEFORCE", "HASGROUNDFORCE"}) setup_.cache_intervals.try_emplace(std::string(side) + token, real(7));
        for (const auto token : {"HASSPACEUNITSBITFIELD", "HASGROUNDUNITSBITFIELD"}) setup_.cache_intervals.try_emplace(std::string(side) + token, real(13));
        for (const auto token : {"NEARBYSPACETOTAL", "NEARBYGROUNDTOTAL"}) setup_.cache_intervals.try_emplace(std::string(side) + token, real(30));
        setup_.cache_intervals.try_emplace(std::string(side) + "STARBASETOTALUNNORMALIZED", Real{});
    }
    setup_.cache_intervals.try_emplace("ISCONNECTEDTO", real(5)); // WGA-191/193 visible-only cache
    setup_.cache_intervals.try_emplace("TARGETPLAYERID", Real{}); // WGA-258: fogged ownership getter
    for (auto& row : setup_.players) { row.faction = upper_case(row.faction); row.difficulty = upper_case(row.difficulty); }
    for (auto& row : setup_.planets) row.name = upper_case(row.name);
    for (const auto& row : setup_.players) players_.push_back({row.player, row.team, 0, 0});
    std::set<tactical::PlayerId> ids, ai_ids;
    std::set<galactic::PlanetId> planets;
    for (const auto& row : setup_.players) { ids.insert(row.player); if (row.ai) ai_ids.insert(row.player); }
    for (const auto& row : setup_.planets) planets.insert(row.planet);
    markup_ = std::make_shared<GalacticMarkup>(setup_.markup_hints, std::move(ids), std::move(ai_ids), std::move(planets));
}

core::Result<void> GalacticWorld::bind(std::shared_ptr<const galactic::GalacticSnapshot> snapshot,
    GalacticConnectivity connectivity, const sim::PartitionExecutor& executor,
    std::uint64_t synchronized_frame, sim::EntityId next_entity_id, bool perception_due) {
    gc_profile::Scope profile_scope(gc_profile::Phase::world_bind);
    if (!snapshot || !next_entity_id) return core::Result<void>::failure(detail::make_error(
        authoritative::codes::invalid_request, "galactic perception needs a completed snapshot and identity frontier"));
    admission_preparations_ = 0;
    // The owning publisher invalidates this record for physical/economic writers.
    // A clock-only snapshot changes neither forces nor production admission. Keep
    // its producer data and ordered rows; only publish the new boundary clocks.
    // Unqualified synthetic snapshots have no generation and take the full path.
    const auto& generation = snapshot->economy_input_generation();
    const auto equal_rows = [](const auto& left, const auto& right) {
        return std::equal(left.begin(), left.end(), right.begin(), right.end());
    };
    const auto equal_optional_pointer = [](const auto* left, const auto* right) {
        return bool(left) == bool(right) && (!left || *left == *right);
    };
    bool queues_changed{};
    const auto same_production = [&](const auto& left, const auto& right) {
        if (bool(left) != bool(right)) return false;
        if (!left) return true;
        if (left->maximum_human_queue != right->maximum_human_queue || left->players != right->players
            || left->fleet_blockers != right->fleet_blockers || left->nested_creation != right->nested_creation
            || left->technology_types != right->technology_types || left->planets.size() != right->planets.size()) return false;
        for (std::size_t index = 0; index < left->planets.size(); ++index) {
            const auto& a = left->planets[index]; const auto& b = right->planets[index];
            // Every nonqueue producer fact still qualifies the physical publication.
            // Current queues are published separately for admission/economy reads.
            if (std::tie(a.planet, a.target_owner, a.starbase_owner, a.ground_occupier, a.orbit_occupier,
                a.starbase_type, a.ground_level, a.space_level, a.maximum_space_level, a.ground_slots,
                a.space_slots, a.landed_transports, a.maximum_ground_forces, a.enabled, a.surface_accessible,
                a.starbase_blocked, a.offers, a.objects)
                != std::tie(b.planet, b.target_owner, b.starbase_owner, b.ground_occupier, b.orbit_occupier,
                b.starbase_type, b.ground_level, b.space_level, b.maximum_space_level, b.ground_slots,
                b.space_slots, b.landed_transports, b.maximum_ground_forces, b.enabled, b.surface_accessible,
                b.starbase_blocked, b.offers, b.objects)) return false;
            queues_changed = queues_changed || a.queues != b.queues;
        }
        return true;
    };
    bool credits_changed{};
    const auto same_accounts = [&](const auto& left, const auto& right) {
        if (bool(left) != bool(right)) return false;
        if (!left) return true;
        if (left->players.size() != right->players.size()) return false;
        for (std::size_t index = 0; index < left->players.size(); ++index) {
            const auto& a = left->players[index]; const auto& b = right->players[index];
            if (std::tie(a.player, a.credit_cap, a.current_tech, a.maximum_tech)
                != std::tie(b.player, b.credit_cap, b.current_tech, b.maximum_tech)) return false;
            credits_changed = credits_changed || a.credits != b.credits;
        }
        return true;
    };
    bool same_projection{};
    if (!perception_due && live_deltas_ && bound_source_ && generation && bound_source_->economy_input_generation()) {
        const auto& old = *bound_source_;
        const auto& old_generation = *old.economy_input_generation();
        // Equal counters in a different publisher are never an identity proof.
        const bool same_publisher = generation == old.economy_input_generation()
            || (generation->census >= old_generation.census && generation->accounts >= old_generation.accounts
                && (generation->census > old_generation.census || generation->accounts > old_generation.accounts));
        same_projection = same_publisher && generation->income == old_generation.income
            && same_production(snapshot->production(), old.production()) && same_accounts(snapshot->player_economy(), old.player_economy())
            && equal_rows(snapshot->planets(), old.planets()) && equal_rows(snapshot->routes(), old.routes())
            && equal_rows(snapshot->population(), old.population())
            && equal_optional_pointer(snapshot->control(), old.control())
            && equal_optional_pointer(snapshot->movement(), old.movement())
            && equal_optional_pointer(snapshot->income(), old.income())
            && equal_optional_pointer(snapshot->economy_corruption(), old.economy_corruption())
            && equal_rows(snapshot->commissioned_originals(), old.commissioned_originals())
            && equal_rows(snapshot->actor_records(), old.actor_records())
            && snapshot->observer() == old.observer() && equal_rows(snapshot->observations(), old.observations())
            && snapshot->maximum_movement_distance() == old.maximum_movement_distance()
            && std::equal(snapshot->fleets().begin(), snapshot->fleets().end(), old.fleets().begin(), old.fleets().end(),
                [](const auto& a, const auto& b) {
                    return a.id == b.id && a.owner == b.owner && a.orbit == b.orbit
                        && bool(a.motion.transit) == bool(b.motion.transit) && a.members == b.members;
                });
    }
    if (!perception_due && bound_source_ && generation && (same_projection || (generation == bound_source_->economy_input_generation()
        && snapshot->observer_input_generation() != 0
        && snapshot->observer_input_generation() == bound_source_->observer_input_generation()))
        && next_entity_id == next_id_ && connectivity.stealth_fleets == connectivity_.stealth_fleets) {
        auto view = std::make_shared<WorldView>(*view_);
        view->tick = snapshot->completed_tick();
        if (same_projection) {
            std::map<sim::EntityId, math::Vec3> moved;
            for (const auto& fleet : snapshot->fleets()) if (fleet.motion.transit) {
                moved.emplace(fleet.id, fleet.motion.position);
                for (const auto& member : fleet.members) moved.emplace(member.id, fleet.motion.position);
            }
            if (snapshot->movement()) {
                bool appended = true;
                while (appended) {
                    appended = false;
                    for (const auto& contents : snapshot->movement()->contents) if (const auto parent = moved.find(contents.container); parent != moved.end())
                        for (const auto& child : contents.children) appended = moved.emplace(child.id, parent->second).second || appended;
                }
            }
            if (!moved.empty()) {
                const auto motion = executor.execute_phase("galactic-ai-live-motion", sim::tick_partition_count, [&](std::size_t partition) {
                    const auto range = sim::partition_range(partition, view->units.size());
                    for (auto index = range.begin; index < range.end; ++index)
                        if (const auto at = moved.find(view->units[index].id); at != moved.end()) view->units[index].position = at->second;
                });
                if (!motion) return motion;
            }
        }
        const bool economy_changed = queues_changed || credits_changed;
        if (economy_changed) {
            if (auto refreshed = refresh_live_producers(snapshot, next_entity_id, view->units.size() + snapshot->routes().size()); !refreshed) return refreshed;
            for (auto& row : planets_) row.production = snapshot->production()
                ? find_row(snapshot->production()->planets, row.planet.id, [](const auto& at) { return at.planet; }) : nullptr;
            // Do not qualify a partially refreshed publication after refusal.
            // snapshot_ below retains every new borrowed producer pointer.
            bound_source_.reset();
            accounts_.clear();
            if (snapshot->player_economy()) for (const auto& row : snapshot->player_economy()->players) {
                tactical::EconomyView account; account.player = row.player; account.credits = row.credits;
                account.tech_level = static_cast<std::uint32_t>(row.current_tech);
                accounts_.push_back(std::move(account));
            }
        }
        auto input = std::make_shared<galactic_ai::WorldInput>(*input_);
        input->clocks = {synchronized_frame, snapshot->clock().galactic_frame};
        if (economy_changed) {
            input->credits.clear();
            for (const auto& row : accounts_) input->credits.emplace(row.player, row.credits);
        }
        if (live_deltas_ && prepared_producer_offers_) input->producers.set_query(producer_query());
        snapshot_ = std::move(snapshot); synchronized_frame_ = synchronized_frame;
        view_ = std::move(view); input_ = std::move(input); work_ = {};
        if (economy_changed) if (auto prepared_economy = refresh_economy_queues(executor, queues_changed); !prepared_economy) return prepared_economy;
        bind_connectivity(std::move(connectivity));
        auto prepared_catalog = prepare_catalog_slice(executor, synchronized_frame);
        if (prepared_catalog && economy_changed) bound_source_ = snapshot_;
        return prepared_catalog;
    }
    // A failed preparation must never make a partially published input reusable.
    bound_source_.reset();
    // ADR-009: stage every independent planet in disjoint slots, then publish once.
    std::vector<PlanetRow> rows(snapshot->planets().size());
    std::map<galactic::PlanetId, std::pair<std::uint64_t, std::uint64_t>> routes;
    for (const auto& route : snapshot->routes()) {
        const auto* from = find_row(snapshot->planets(), route.from, [](const auto& p) { return p.id; });
        const auto* to = find_row(snapshot->planets(), route.to, [](const auto& p) { return p.id; });
        const bool active = from && to && from->owner != 0 && allied(from->owner, to->owner);
        for (const auto id : {route.from, route.to}) {
            ++routes[id].first;
            if (active) ++routes[id].second;
        }
    }
    const auto prepared = executor.execute_phase("galactic-ai-planets", sim::tick_partition_count,
        [&](std::size_t partition) {
            const auto range = sim::partition_range(partition, rows.size());
            for (auto index = range.begin; index < range.end; ++index) {
                auto& row = rows[index]; row.planet = snapshot->planets()[index];
                if (const auto* authored = find_row(setup_.planets, row.planet.id, [](const auto& p) { return p.planet; })) row.facts = *authored;
                else { row.facts.planet = row.planet.id; row.facts.name = "PLANET_" + std::to_string(row.planet.id); }
                if (snapshot->production()) {
                    const auto* production = find_row(snapshot->production()->planets, row.planet.id, [](const auto& p) { return p.planet; });
                    if (production) row.production = production;
                }
                if (const auto found = routes.find(row.planet.id); found != routes.end())
                    std::tie(row.routes, row.active_routes) = found->second;
                if (snapshot->economy_input_generation() && snapshot->economy_input_generation()->income) {
                    const auto* income = find_row(*snapshot->economy_input_generation()->income, row.planet.id, [](const auto& p) { return p.first; });
                    if (income) row.income = to_single(numeric::from_fixed(income->second));
                } else if (snapshot->economy_forecast()) {
                    const auto* income = find_row(snapshot->economy_forecast()->planets, row.planet.id, [](const auto& p) { return p.planet; });
                    if (income && income->actual) row.income = numeric::from_fixed(*income->actual);
                }
            }
        });
    if (!prepared) return prepared;
    const auto forces = prepare_forces(rows, *snapshot, connectivity, executor);
    if (!forces) return forces;
    auto view = std::make_shared<WorldView>(); view->tick = snapshot->completed_tick();
    for (std::size_t index = 0; index < setup_.players.size(); ++index)
        view->players.push_back({setup_.players[index].player, setup_.players[index].team, index});
    std::map<sim::EntityId, galactic::PlanetId> containment;
    std::set<sim::EntityId> transit;
    struct ObjectInput { sim::EntityId id{}; tactical::TypeId type{}; tactical::PlayerId owner{}; math::Vec3 position; galactic::PlanetId planet{}; galactic::IntelligenceContainer container{}; bool structure{}; };
    std::vector<ObjectInput> objects;
    std::set<sim::EntityId> identities;
    const auto add = [&](sim::EntityId id, tactical::TypeId type, tactical::PlayerId owner,
                         const math::Vec3& position, galactic::PlanetId containing,
                         galactic::IntelligenceContainer container = galactic::IntelligenceContainer::none, bool structure = false) {
        if (!identities.insert(id).second) return;
        objects.push_back({id, type, owner, position, containing, container, structure});
        if (containing) containment.emplace(id, containing);
    };
    // Ordered roster indexing resolves shared persistent identities. Token/visibility
    // preparation below is partitioned; this traversal never services world entities.
    for (const auto& row : rows) add(row.planet.id, row.facts.type, row.planet.owner, row.planet.position, row.planet.id);
    for (const auto& fleet : snapshot->fleets()) {
        // In transit a fleet has no planet parent. Do not retain its former orbit as containment.
        const auto* at = find_row(rows, fleet.orbit, [](const auto& p) { return p.planet.id; });
        const auto containing = fleet.motion.transit ? galactic::PlanetId{} : fleet.orbit;
        const auto position = fleet.motion.transit || !at ? fleet.motion.position : at->planet.position;
        add(fleet.id, 0, fleet.owner, position, containing, galactic::IntelligenceContainer::orbit);
        for (const auto& member : fleet.members) {
            add(member.id, member.type, fleet.owner, position, containing, galactic::IntelligenceContainer::orbit);
            if (fleet.motion.transit) transit.insert(member.id);
        }
        if (fleet.motion.transit) transit.insert(fleet.id);
    }
    if (snapshot->control()) for (const auto& control : snapshot->control()->planets) {
        const auto* at = find_row(rows, control.planet, [](const auto& p) { return p.planet.id; });
        if (!at) continue;
        for (const auto& force : control.landed) add(force.id, force.type, force.owner, at->planet.position, control.planet, galactic::IntelligenceContainer::ground);
    }
    for (const auto& row : rows) if (row.production) for (const auto& object : row.production->objects)
        add(object.id, object.type, object.owner, row.planet.position, row.planet.id,
            object.domain == galactic::ProductionDomain::space ? galactic::IntelligenceContainer::orbit : galactic::IntelligenceContainer::ground,
            object.kind == galactic::ProductionKind::structure);
    // Nested payloads inherit the resolved parent's location, never a type-based guess.
    if (snapshot->movement()) {
        std::map<sim::EntityId, const galactic::MovementContents*> nested;
        for (const auto& contents : snapshot->movement()->contents) nested.emplace(contents.container, &contents);
        for (std::size_t index = 0; index < objects.size(); ++index) {
            const auto parent = objects[index];
            const auto found = nested.find(parent.id); if (found == nested.end()) continue;
            for (const auto& member : found->second->children) {
                if (identities.contains(member.id)) continue;
                const auto where = containment.find(parent.id);
                add(member.id, member.type, parent.owner, parent.position, where == containment.end() ? 0 : where->second, parent.container);
                if (transit.contains(parent.id)) transit.insert(member.id);
            }
        }
    }
    view->units.resize(objects.size());
    std::vector<GalacticVisibility> visibility_state;
    if (!snapshot->observer()) {
        std::scoped_lock lock(cache_mutex_);
        visibility_state = state_.visibility;
    }
    const auto object_phase = executor.execute_phase("galactic-ai-objects", sim::tick_partition_count,
        [&](std::size_t partition) {
            const auto range = sim::partition_range(partition, objects.size());
            for (auto index = range.begin; index < range.end; ++index) {
                const auto& source = objects[index]; auto& unit = view->units[index];
                unit.id = source.id; unit.type = source.type; unit.owner = source.owner; unit.position = source.position;
                if (const auto* owner_row = player(source.owner)) unit.team = owner_row->team;
                if (!setup_.uses_fog) unit.visible_to = ~std::uint64_t{}; // WGA-08
                else if (snapshot->observer()) for (const auto& viewer : view->players) {
                    const auto* observer = find_row(snapshot->observer()->players, viewer.id, [](const auto& row) { return row.player; });
                    const bool allied_owner = source.owner == viewer.id || (observer
                        && std::binary_search(observer->allies.begin(), observer->allies.end(), source.owner));
                    const auto* known = find_row(snapshot->observations(), std::pair{viewer.id, source.planet},
                        [](const auto& row) { return std::pair{row.player, row.planet}; });
                    bool visible = allied_owner;
                    if (known && !visible) {
                        // WGUI-18: scalar ownership/presence/count is not an
                        // object-details grant. The planet target is separate.
                        if (source.id == source.planet) visible = known->knowledge.ownership;
                        else {
                            const auto* type = find_row(setup_.force_types, source.type, [](const auto& row) { return row.type; });
                            const auto* live = find_row(snapshot->observer()->types, source.type, [](const auto& row) { return row.type; });
                            if (source.type == 0 || source.structure || (type && type->object_traits)) {
                                galactic::ObjectIntelligenceContext object;
                                object.planetary_system = source.planet != 0; object.structure = source.structure;
                                object.container = source.container;
                                if (type && type->object_traits) {
                                    object.hero = type->object_traits->hero; object.named_hero = type->object_traits->named_hero;
                                }
                                object.stealth_hero = live && live->stealth_hero;
                                visible = galactic::object_details_known(object, known->knowledge);
                            }
                        }
                    }
                    if (visible) unit.visible_to |= std::uint64_t{1} << viewer.snapshot_index;
                }
                else for (const auto& observed : visibility_state) {
                    // Explicit legacy fixture visibility is a separate input.
                    if (observed.mask != 0 || observed.planet != source.planet || !observed.visible) continue;
                    const auto* viewer = view->player(observed.player);
                    if (viewer) unit.visible_to |= std::uint64_t{1} << viewer->snapshot_index;
                }
            }
        });
    if (!object_phase) return object_phase;
    std::sort(view->units.begin(), view->units.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    std::vector<tactical::EconomyView> accounts;
    if (snapshot->player_economy()) for (const auto& row : snapshot->player_economy()->players) {
        tactical::EconomyView account; account.player = row.player; account.credits = row.credits;
        account.tech_level = static_cast<std::uint32_t>(row.current_tech);
        accounts.push_back(std::move(account));
    }
    // Adapt the real production admission facts to the existing mode-neutral read interface.
    std::vector<BuildRow> builds;
    bool producer_rows_reused{};
    std::shared_ptr<PreparedProducerOffers> prepared_offers;
    std::uint64_t offer_preparations{};
    if (!live_deltas_ && snapshot->production() && snapshot->player_economy()) {
        const auto& production = *snapshot->production();
        std::vector<galactic::ProductionReservations> reservations;
        for (const auto& owner : production.players) reservations.push_back(galactic::production_reservations(production, owner.player));
        // Gathered once for every player x planet x offer below, not per offer (#3003).
        const auto* income_inputs = snapshot->income() ? &snapshot->income()->inputs : nullptr;
        const auto reductions = income_inputs ? galactic::production_reduction_sources(*income_inputs, snapshot->planets())
                                              : std::vector<galactic::ProductionReductionSource>{};
        const auto jobs = production.players.size() * production.planets.size();
        prepared_offers = std::make_shared<PreparedProducerOffers>();
        prepared_offers->source = snapshot;
        prepared_offers->reductions = reductions;
        prepared_offers->jobs.resize(jobs);
        prepared_offers->next_id = next_entity_id;
        prepared_offers->live_objects = view->units.size() + snapshot->routes().size();
        const auto* previous = prepared_producer_offers_.get();
        const auto* previous_income = previous && previous->source->income() ? &previous->source->income()->inputs : nullptr;
        const auto reduction_key = [](const auto& row) {
            return std::tie(row.type, row.price, row.id, row.planet, row.planet_owner, row.planet_source,
                row.intimidation, row.galaxy_wide, row.category, row.reduction);
        };
        const bool same_adjustments = previous && previous->unique_jobs && bool(income_inputs) == bool(previous_income)
            && (!income_inputs || income_inputs->offers == previous_income->offers)
            && std::equal(reductions.begin(), reductions.end(), previous->reductions.begin(), previous->reductions.end(),
                [&](const auto& a, const auto& b) { return reduction_key(a) == reduction_key(b); });
        // Admission consumes orbit/transit membership and blocker declarations;
        // interpolation clocks and positions do not change those producer facts.
        const bool same_admission_sources = previous && previous->unique_jobs
            && previous->source->production() && previous->source->player_economy()
            && previous->next_id == prepared_offers->next_id
            && previous->live_objects == prepared_offers->live_objects
            && previous->source->production()->maximum_human_queue == production.maximum_human_queue
            && previous->source->production()->fleet_blockers == production.fleet_blockers
            && std::equal(snapshot->fleets().begin(), snapshot->fleets().end(),
                previous->source->fleets().begin(), previous->source->fleets().end(),
                [](const auto& a, const auto& b) {
                    return a.id == b.id && a.owner == b.owner && a.orbit == b.orbit
                        && bool(a.motion.transit) == bool(b.motion.transit) && a.members == b.members;
                });
        std::vector<std::vector<BuildRow>> staged(jobs);
        std::vector<std::optional<core::Diagnostic>> errors(jobs);
        std::vector<std::uint64_t> preparations(jobs);
        std::vector<std::uint64_t> admissions(jobs);
        const auto build_phase = executor.execute_phase("galactic-ai-producers", sim::tick_partition_count,
            [&](std::size_t partition) {
              const auto range = sim::partition_range(partition, jobs);
              for (auto index = range.begin; index < range.end; ++index) {
                const auto owner_index = index / production.planets.size();
                const auto& owner = production.players[owner_index];
                const auto& location = production.planets[index % production.planets.size()];
                auto& cached = prepared_offers->jobs[index];
                cached.player = owner.player; cached.planet = location.planet;
                cached.source_owner = owner_index; cached.source_location = index % production.planets.size();
                cached.reservations = reservations[owner_index];
                const auto* account = find_row(snapshot->player_economy()->players, owner.player, [](const auto& p) { return p.player; });
                if (!account) continue;
                const auto* population = find_row(snapshot->population(), owner.player, [](const auto& p) { return p.player; });
                const auto* at = find_row(snapshot->planets(), location.planet, [](const auto& p) { return p.id; });
                if (!at) continue;
                std::array<math::Fixed, 2> queue_seconds{};
                for (std::size_t channel = 0; channel < queue_seconds.size(); ++channel)
                    for (const auto& entry : location.queues[channel]) {
                        const auto total = math::add(queue_seconds[channel], entry.last_remaining);
                        if (!total) { errors[index] = total.error(); break; }
                        queue_seconds[channel] = total.value();
                    }
                if (errors[index]) continue;
                const PreparedProducerOffers::Job* old_job = nullptr;
                const galactic::PlanetProduction* old_location = nullptr;
                if (same_adjustments) {
                    old_location = find_row(previous->source->production()->planets, location.planet,
                        [](const auto& p) { return p.planet; });
                    old_job = find_row(previous->jobs, std::pair{owner.player, location.planet},
                        [](const auto& job) { return std::pair{job.player, job.planet}; });
                    if (old_location && old_job && old_location->offers == location.offers) cached.offers = old_job->offers;
                }
                if (same_admission_sources && cached.offers && old_job && old_job->rows
                    && old_location && *old_location == location) {
                    const auto* old_owner = find_row(previous->source->production()->players, owner.player, [](const auto& p) { return p.player; });
                    const auto* old_account = find_row(previous->source->player_economy()->players, owner.player, [](const auto& p) { return p.player; });
                    const auto* old_population = find_row(previous->source->population(), owner.player, [](const auto& p) { return p.player; });
                    const auto* old_planet = find_row(previous->source->planets(), location.planet, [](const auto& p) { return p.id; });
                    if (old_owner && *old_owner == owner && old_account && *old_account == *account
                        && old_planet && old_planet->owner == at->owner
                        && bool(old_population) == bool(population) && (!population || *old_population == *population)
                        && old_job->reservations.population == cached.reservations.population
                        && old_job->reservations.counts == cached.reservations.counts) {
                        cached.rows = old_job->rows;
                        continue;
                    }
                }
                if (!cached.offers) {
                    auto offers = std::make_shared<std::vector<PreparedProducerOffers::Offer>>();
                    offers->reserve(location.offers.size());
                    for (const auto& offer : location.offers) {
                        auto effective = galactic::prepared_production_offer(offer,
                            income_inputs, reductions, location.planet, owner.player);
                        if (!effective) { errors[index] = effective.error(); break; }
                        const auto frames = galactic::production_frames(effective.value().seconds);
                        if (!frames) { errors[index] = frames.error(); break; }
                        offers->push_back({std::move(effective).value(), frames.value()});
                        ++preparations[index];
                    }
                    cached.offers = std::move(offers);
                }
                if (errors[index]) continue;
                staged[index].reserve(location.offers.size());
                for (const auto& prepared_offer : *cached.offers) {
                    const auto& effective = prepared_offer.effective;
                    const galactic::ProductionAdmissionInputs inputs{production, location, owner, *account,
                        snapshot->fleets(), at->owner, population, next_entity_id, view->units.size() + snapshot->routes().size(), &reservations[owner_index]};
                    if (prepared_offer.frames > std::numeric_limits<std::uint32_t>::max()) continue;
                    tactical::BuildOption option; option.type = effective.type; option.price = effective.price;
                    option.build_frames = option.ai_build_frames = static_cast<std::uint32_t>(prepared_offer.frames);
                    // WGA-425: AI producer options publish content eligibility;
                    // native funding owns payment admission separately.
                    const auto admission = galactic::production_admission(inputs, &effective, effective.domain, !owner.human);
                    ++admissions[index];
                    option.available = admission == galactic::ProductionAdmission::allowed;
                    galactic_ai::ProducerInput producer;
                    producer.producer = producer.planet = location.planet; producer.player = owner.player;
                    producer.type = effective.type; producer.domain = effective.domain; producer.admission = admission;
                    producer.cost = effective.price; producer.build_seconds = effective.seconds;
                    const auto channel = effective.domain == galactic::ProductionDomain::ground ? 0U : 1U;
                    producer.queue_seconds = queue_seconds[channel]; producer.logical_fps = setup_.logical_fps;
                    producer.base_level = static_cast<std::uint32_t>(channel == 0 ? location.ground_level : location.space_level);
                    producer.required_level = static_cast<std::uint32_t>(channel == 0 ? effective.required_ground_level : effective.required_space_level);
                    // E/D supply target-dependent execution reach/distance and qualified
                    // composite transport speed; zero fields do not admit a fallback route.
                    staged[index].push_back({owner.player, location.planet, std::move(option), std::move(producer)});
                }
                cached.rows = std::make_shared<const std::vector<BuildRow>>(staged[index]);
              }
            });
        if (!build_phase) return build_phase;
        // Keep the canonical publication when every producer chunk is unchanged.
        // A changed chunk still contributes its current queue/account/admission
        // facts; unchanged chunks are copied only for that changed publication.
        producer_rows_reused = !live_deltas_ && previous && previous->unique_jobs && previous->jobs.size() == prepared_offers->jobs.size();
        std::size_t staged_rows{};
        for (const auto& job : prepared_offers->jobs) {
            if (job.rows) staged_rows += job.rows->size();
            const auto* old = previous ? find_row(previous->jobs, std::pair{job.player, job.planet},
                [](const auto& row) { return std::pair{row.player, row.planet}; }) : nullptr;
            producer_rows_reused = producer_rows_reused && old && job.rows == old->rows;
        }
        if (!producer_rows_reused) builds.reserve(staged_rows);
        for (std::size_t index = 0; index < jobs; ++index) {
            if (errors[index]) return core::Result<void>::failure(*errors[index]);
            offer_preparations += preparations[index];
            admission_preparations_ += admissions[index];
            if (!producer_rows_reused && prepared_offers->jobs[index].rows)
                builds.insert(builds.end(), prepared_offers->jobs[index].rows->begin(), prepared_offers->jobs[index].rows->end());
        }
        std::sort(prepared_offers->jobs.begin(), prepared_offers->jobs.end(), [](const auto& a, const auto& b) {
            return std::pair{a.player, a.planet} < std::pair{b.player, b.planet};
        });
        // A synthetic snapshot can contain duplicate producer keys. Recompute it rather
        // than selecting an ambiguous earlier preparation; normal published jobs are unique.
        prepared_offers->unique_jobs = std::adjacent_find(prepared_offers->jobs.begin(), prepared_offers->jobs.end(),
            [](const auto& a, const auto& b) { return a.player == b.player && a.planet == b.planet; }) == prepared_offers->jobs.end();
    }
    const bool observer_changed = !snapshot_ || snapshot_->observer_input_generation() != snapshot->observer_input_generation();
    markup_->retain_planets(snapshot->planets());
    snapshot_ = std::move(snapshot); planets_ = std::move(rows); view_ = std::move(view);
    containment_ = std::move(containment); transit_ = std::move(transit); accounts_ = std::move(accounts);
    if (!producer_rows_reused) { builds_ = std::move(builds); build_options_.clear(); }
    next_id_ = next_entity_id; synchronized_frame_ = synchronized_frame;
    if (live_deltas_) {
        if (auto refreshed = refresh_live_producers(snapshot_, next_id_, view_->units.size() + snapshot_->routes().size()); !refreshed) return refreshed;
    } else prepared_producer_offers_ = std::move(prepared_offers);
    work_ = {planets_.size(), view_->units.size(), snapshot_->routes().size(), builds_.size()};
    work_.producer_offer_preparations = offer_preparations;
    if (live_deltas_ && prepared_producer_offers_)
        for (const auto& job : prepared_producer_offers_->jobs)
            if (const auto* location = find_row(snapshot_->production()->planets, job.planet, [](const auto& p) { return p.planet; }))
                work_.producer_offers += location->offers.size();
    if (auto prepared_economy = prepare_economy_tokens(executor); !prepared_economy) return prepared_economy;
    // WGA-258/65: each fogged unit has its own qualified visibility category.
    // A unchanged bind does not refresh caches or reset observation ages.
    if (observer_changed && snapshot_->observer()) for (const auto& known : snapshot_->observations()) {
        const std::array values{
            std::pair{galactic::bit(galactic::Intelligence::ownership), known.knowledge.ownership},
            std::pair{galactic::bit(galactic::Intelligence::fleet_presence), known.knowledge.fleet_presence},
            std::pair{galactic::bit(galactic::Intelligence::fleet_count), known.knowledge.fleet_count},
            std::pair{galactic::bit(galactic::Intelligence::ground_count), known.knowledge.ground_count}};
        for (const auto& [mask, value] : values) {
            bool changed;
            {
                std::scoped_lock lock(cache_mutex_);
                const auto* found = visibility(known.player, known.planet, mask, false);
                changed = !found || found->visible != value;
            }
            if (changed) visibility_changed(known.player, known.planet, value, mask);
        }
    }
    for (const auto& row : planets_) {
        work_.force_terms += row.space.size() + row.ground.size() + row.starbase.size();
        work_.neighbourhood_pairs += row.neighbourhood_pairs;
    }
    bind_connectivity(std::move(connectivity));
    auto input = std::make_shared<galactic_ai::WorldInput>();
    input->clocks = {synchronized_frame_, snapshot_->clock().galactic_frame};
    for (const auto& row : planets_) {
        galactic_ai::TargetInput target; target.planet = row.planet.id; target.owner = row.planet.owner; target.position = row.planet.position;
        if (row.production) {
            target.tokens.emplace("GROUNDBASELEVELUNNORMALIZED", real(row.production->ground_level));
            target.tokens.emplace("STARBASELEVELUNNORMALIZED", real(row.production->space_level));
        }
        input->targets.push_back(std::move(target));
    }
    if (live_deltas_ && prepared_producer_offers_) {
        input->producers.set_query(producer_query());
    } else if (producer_rows_reused) input->producers = input_->producers;
    else {
    input->producers.reserve(builds_.size());
    for (const auto& row : builds_) input->producers.push_back(row.input);
    const auto canonical = [](const auto& a, const auto& b) {
        return std::tie(a.player, a.planet, a.domain, a.type) < std::tie(b.player, b.planet, b.domain, b.type);
    };
    // Rows arrive grouped by producer job (player, planet), and a planet's offers have distinct
    // types, so sorting each group yields the one canonical order. Any input where that does not
    // give a strictly ascending list is sorted whole, from the original row order (#3003).
    for (auto group = input->producers.begin(); group != input->producers.end();) {
        const auto end = std::find_if(group, input->producers.end(),
            [&](const auto& row) { return row.player != group->player || row.planet != group->planet; });
        std::sort(group, end, canonical);
        group = end;
    }
    if (std::adjacent_find(input->producers.begin(), input->producers.end(),
        [&](const auto& a, const auto& b) { return !canonical(a, b); }) != input->producers.end()) {
        input->producers.clear();
        for (const auto& row : builds_) input->producers.push_back(row.input);
        std::sort(input->producers.begin(), input->producers.end(), canonical);
    }
    }
    for (const auto& row : accounts_) input->credits.emplace(row.player, row.credits);
    input_ = std::move(input);
    if (auto prepared_catalog = prepare_catalog_slice(executor, synchronized_frame); !prepared_catalog) return prepared_catalog;
    bound_source_ = snapshot_;
    return core::Result<void>::success();
}

void GalacticWorld::bind_connectivity(GalacticConnectivity connectivity) {
    if (connectivity.revision != connectivity_.revision) {
        std::scoped_lock lock(cache_mutex_);
        mark_state_changed();
        cache_index_.clear();
        std::erase_if(state_.cache, [](const auto& row) {
            return std::any_of(row.key.tokens.begin(), row.key.tokens.end(), [](const auto& token) { return token.starts_with("ISCONNECTED") || token.starts_with("CONNECTS"); });
        });
    }
    connectivity_ = std::move(connectivity);
}

std::shared_ptr<const galactic_ai::WorldInput> GalacticWorld::copy_input() const { return input_; }

std::uint64_t GalacticWorld::completed_tick() const noexcept { return snapshot_ ? snapshot_->completed_tick() : 0; }
const GalacticWorld::PlanetRow* GalacticWorld::planet(galactic::PlanetId id) const { return find_row(planets_, id, [](const auto& p) { return p.planet.id; }); }
const GalacticPlayerFacts* GalacticWorld::player(tactical::PlayerId id) const { return find_row(setup_.players, id, [](const auto& p) { return p.player; }); }
bool GalacticWorld::allied(tactical::PlayerId a, tactical::PlayerId b) const {
    if (snapshot_ && snapshot_->observer()) {
        const auto* who = find_row(snapshot_->observer()->players, a, [](const auto& row) { return row.player; });
        return who && (a == b || std::binary_search(who->allies.begin(), who->allies.end(), b));
    }
    const auto* left = player(a); const auto* right = player(b);
    return left && right && !left->neutral && !right->neutral && left->team == right->team;
}
const tactical::EconomyView* GalacticWorld::account(tactical::PlayerId id) const { return find_row(accounts_, id, [](const auto& p) { return p.player; }); }
bool GalacticWorld::arriving(sim::EntityId id) const { return transit_.contains(id); }
bool GalacticWorld::build_allowed(tactical::PlayerId owner, sim::EntityId producer, tactical::TypeId type) const {
    const auto* option = build_option(owner, producer, type); return option && option->available;
}
const tactical::BuildOption* GalacticWorld::build_option(tactical::PlayerId owner, sim::EntityId producer, tactical::TypeId type) const {
    if (live_deltas_ && input_) {
        if (producer > std::numeric_limits<galactic::PlanetId>::max()) return nullptr;
        std::scoped_lock lock(cache_mutex_);
        const auto key = std::tuple{owner, producer, type};
        if (const auto at = build_options_.find(key); at != build_options_.end()) return &at->second;
        const std::array types{type};
        auto rows = input_->producers.read(owner, static_cast<galactic::PlanetId>(producer), types);
        if (!rows || rows.value().empty()) return nullptr;
        const auto& row = rows.value().front();
        auto frames = galactic::production_frames(row.build_seconds);
        if (!frames || frames.value() > std::numeric_limits<std::uint32_t>::max()) return nullptr;
        tactical::BuildOption option; option.type = row.type; option.price = row.cost;
        option.build_frames = option.ai_build_frames = static_cast<std::uint32_t>(frames.value());
        option.available = row.admission == galactic::ProductionAdmission::allowed;
        return &build_options_.emplace(key, std::move(option)).first->second;
    }
    for (const auto& row : builds_) if (row.player == owner && row.producer == producer && row.option.type == type) return &row.option;
    return nullptr;
}
tactical::ProductionCounts GalacticWorld::production_counts(tactical::PlayerId owner, tactical::TypeId type) const {
    tactical::ProductionCounts out;
    if (!snapshot_ || !snapshot_->production()) return out;
    for (const auto& p : snapshot_->production()->players) for (const auto& count : p.counts) if (count.type == type) {
        if (p.player == owner) { out.owned_player += count.owned; out.lifetime_player += count.historical; }
        if (allied(owner, p.player)) { out.owned_allies += count.owned; out.lifetime_allies += count.historical; }
    }
    for (const auto& p : snapshot_->production()->planets) for (const auto& queue : p.queues) for (const auto& entry : queue) if (entry.type == type) {
        if (entry.builder == owner) ++out.queued_player;
        if (allied(owner, entry.builder)) ++out.queued_allies;
    }
    out.current_player = out.owned_player + out.queued_player;
    out.current_allies = out.owned_allies + out.queued_allies;
    return out;
}

std::vector<Target> GalacticWorld::targets(const Engine&, const PlayerAi&) const {
    std::vector<Target> out; out.reserve(planets_.size());
    for (const auto& row : planets_) {
        Target target; target.object = row.planet.id; target.name = row.facts.name;
        target.x = numeric::from_fixed(row.planet.position.x); target.y = numeric::from_fixed(row.planet.position.y);
        out.push_back(std::move(target));
    }
    return out;
}
std::vector<Target> GalacticWorld::new_targets(const Engine& engine) const {
    auto out = targets(engine, {});
    std::erase_if(out, [&](const auto& target) { return engine.target_of_object(target.object) != nullptr; });
    return out;
}
bool GalacticWorld::target_alive(const Target& target) const { return target.object != 0 && planet(target.object) != nullptr; }
bool GalacticWorld::target_matches(tactical::PlayerId owner, const std::set<std::string>& flags, const Target* target) const {
    const auto* row = target ? planet(target->object) : nullptr; if (!row) return false;
    const auto* other = player(row->planet.owner);
    if (!other || other->neutral) return flags.contains("NEUTRAL");
    return flags.contains(allied(owner, other->player) ? "FRIENDLY" : "ENEMY");
}
std::optional<galactic::PlanetId> GalacticWorld::containing_planet(sim::EntityId object) const {
    const auto found = containment_.find(object); return found == containment_.end() ? std::nullopt : std::optional{found->second};
}
} // namespace eawr::script::foc::ai
