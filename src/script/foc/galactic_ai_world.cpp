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
        // Live catalogs share one immutable record per owner instead of copying it into every job.
        std::shared_ptr<const galactic::ProductionReservations> owner_reservation;
        [[nodiscard]] const galactic::ProductionReservations* reserved() const noexcept {
            return owner_reservation ? owner_reservation.get() : &reservations;
        }
    };
    std::shared_ptr<const galactic::GalacticSnapshot> source;
    std::vector<galactic::ProductionReductionSource> reductions;
    std::shared_ptr<const std::vector<galactic::ProductionReductionSource>> price_reductions;
    std::vector<Job> jobs;
    // Live catalogs publish an immutable job list that an unchanged refresh shares with its successor.
    std::shared_ptr<const std::vector<Job>> live_jobs;
    std::vector<std::shared_ptr<const galactic::ProductionReservations>> owner_reservations;
    [[nodiscard]] const std::vector<Job>& job_list() const noexcept { return live_jobs ? *live_jobs : jobs; }
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
    for (const auto& job : catalog.job_list())
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
                snapshot.fleets(), at->owner, population, catalog.next_id, catalog.live_objects, job.reserved()};
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
    // Equal keys (duplicate offers) keep their registration order, whatever the row count or standard library.
    std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        return std::tie(a.player, a.planet, a.domain, a.type) < std::tie(b.player, b.planet, b.domain, b.type);
    });
    return Result::success(std::move(rows));
}

core::Result<void> GalacticWorld::refresh_live_producers(
    std::shared_ptr<const galactic::GalacticSnapshot> snapshot, sim::EntityId frontier, std::size_t objects,
    bool inputs_unchanged) {
    bound().build_options_.clear();
    if (!snapshot->production() || !snapshot->player_economy()) {
        bound().prepared_producer_offers_.reset();
        return core::Result<void>::success();
    }
    const auto& production = *snapshot->production();
    const auto* income = snapshot->income() ? &snapshot->income()->inputs : nullptr;
    auto next = std::make_shared<PreparedProducerOffers>();
    next->source = snapshot; next->next_id = frontier; next->live_objects = objects;
    const auto* previous = bound().prepared_producer_offers_.get();
    const auto* old_income = previous && previous->source->income() ? &previous->source->income()->inputs : nullptr;
    const bool previous_usable = previous && previous->unique_jobs && previous->price_reductions && previous->source->production();
    // The caller proves the price inputs equal (same income inputs and planets) relative to the retained source.
    bool same_prices = previous_usable && inputs_unchanged && bool(income) == bool(old_income);
    if (same_prices) next->price_reductions = previous->price_reductions;
    else {
        auto fresh = income ? galactic::production_reduction_sources(*income, snapshot->planets())
            : std::vector<galactic::ProductionReductionSource>{};
        const auto reduction_key = [](const auto& row) {
            return std::tie(row.type, row.price, row.id, row.planet, row.planet_owner, row.planet_source,
                row.intimidation, row.galaxy_wide, row.category, row.reduction);
        };
        same_prices = previous_usable && bool(income) == bool(old_income)
            && (!income || income->offers == old_income->offers)
            && std::equal(fresh.begin(), fresh.end(), previous->price_reductions->begin(), previous->price_reductions->end(),
                [&](const auto& a, const auto& b) { return reduction_key(a) == reduction_key(b); });
        next->price_reductions = same_prices ? previous->price_reductions
            : std::make_shared<const std::vector<galactic::ProductionReductionSource>>(std::move(fresh));
    }
    // One record per owner. An equal record keeps the previous publication's identity.
    next->owner_reservations.reserve(production.players.size());
    bool reservations_equal = same_prices && inputs_unchanged
        && previous->owner_reservations.size() == production.players.size();
    for (std::size_t owner_index = 0; owner_index < production.players.size(); ++owner_index) {
        auto fresh = galactic::production_reservations(production, production.players[owner_index].player);
        if (reservations_equal && previous->owner_reservations[owner_index]
            && previous->owner_reservations[owner_index]->population == fresh.population
            && previous->owner_reservations[owner_index]->counts == fresh.counts)
            next->owner_reservations.push_back(previous->owner_reservations[owner_index]);
        else {
            reservations_equal = false;
            next->owner_reservations.push_back(std::make_shared<const galactic::ProductionReservations>(std::move(fresh)));
        }
    }
    const auto* old_production = previous_usable ? &*previous->source->production() : nullptr;
    // The checked queue clocks are refused here even when no service requests the producer. They depend on the
    // location only, so each location is summed once, when some owner would reach it.
    const bool owner_has_account = std::any_of(production.players.begin(), production.players.end(),
        [&](const auto& owner) { return find_row(snapshot->player_economy()->players, owner.player, [](const auto& row) { return row.player; }); });
    const auto check_queues = [&](const galactic::PlanetProduction& location) -> core::Result<void> {
        if (!owner_has_account || !find_row(snapshot->planets(), location.planet, [](const auto& row) { return row.id; }))
            return core::Result<void>::success();
        for (const auto& queue : location.queues) {
            math::Fixed seconds;
            for (const auto& entry : queue) {
                auto total = math::add(seconds, entry.last_remaining);
                if (!total) return core::Result<void>::failure(total.error());
                seconds = total.value();
            }
        }
        return core::Result<void>::success();
    };
    const bool share_jobs = reservations_equal && inputs_unchanged
        && previous->job_list().size() == production.players.size() * production.planets.size();
    if (share_jobs) {
        // Players, planets, offers and prices equal: only queue clocks, credits and the frontier moved.
        for (const auto& location : production.planets) if (auto checked = check_queues(location); !checked) return checked;
        next->live_jobs = previous->live_jobs;
        next->unique_jobs = previous->unique_jobs;
        bound().prepared_producer_offers_ = std::move(next);
        return core::Result<void>::success();
    }
    // Offer lists are owner-independent: compare each location once.
    std::vector<signed char> offers_same(production.planets.size(), -1);
    const auto planet_offers_same = [&](std::size_t location_index) {
        auto& cached = offers_same[location_index];
        if (cached < 0) {
            const auto* old_location = old_production ? find_row(old_production->planets, production.planets[location_index].planet,
                [](const auto& row) { return row.planet; }) : nullptr;
            cached = old_location && old_location->offers == production.planets[location_index].offers ? 1 : 0;
        }
        return cached == 1;
    };
    std::vector<signed char> queues_checked(production.planets.size(), 0);
    std::vector<PreparedProducerOffers::Job> jobs;
    jobs.reserve(production.players.size() * production.planets.size());
    const std::vector<PreparedProducerOffers::Job>* old_jobs = previous_usable ? &previous->job_list() : nullptr;
    for (std::size_t owner_index = 0; owner_index < production.players.size(); ++owner_index) {
        const auto& owner = production.players[owner_index];
        for (std::size_t location_index = 0; location_index < production.planets.size(); ++location_index) {
            const auto& location = production.planets[location_index];
            PreparedProducerOffers::Job job;
            job.player = owner.player; job.planet = location.planet;
            job.source_owner = owner_index; job.source_location = location_index;
            job.owner_reservation = next->owner_reservations[owner_index];
            if (find_row(snapshot->player_economy()->players, owner.player, [](const auto& row) { return row.player; })
                && find_row(snapshot->planets(), location.planet, [](const auto& row) { return row.id; })) {
                // Preserve the atomic binder's checked queue-clock refusal even
                // when no service requests this producer on the current frame.
                if (!queues_checked[location_index]) {
                    queues_checked[location_index] = 1;
                    if (auto checked = check_queues(location); !checked) return checked;
                }
                if (same_prices && old_jobs && planet_offers_same(location_index)) {
                    const auto* old_job = find_row(*old_jobs, std::pair{owner.player, location.planet},
                        [](const auto& row) { return std::pair{row.player, row.planet}; });
                    if (old_job) job.prices = old_job->prices;
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
            jobs.push_back(std::move(job));
        }
    }
    std::sort(jobs.begin(), jobs.end(), [](const auto& a, const auto& b) {
        return std::pair{a.player, a.planet} < std::pair{b.player, b.planet};
    });
    next->unique_jobs = std::adjacent_find(jobs.begin(), jobs.end(), [](const auto& a, const auto& b) {
        return a.player == b.player && a.planet == b.planet;
    }) == jobs.end();
    next->live_jobs = std::make_shared<const std::vector<PreparedProducerOffers::Job>>(std::move(jobs));
    bound().prepared_producer_offers_ = std::move(next);
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
    return [memo = std::make_shared<Memo>(), catalog = bound().prepared_producer_offers_, fps = setup_.logical_fps,
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
    return bound().prepared_producer_offers_ ? bound().prepared_producer_offers_->queried_offers.load(std::memory_order_relaxed) : 0;
}

core::Result<void> GalacticWorld::prepare_catalog_slice(const sim::PartitionExecutor& executor, std::uint64_t frame) {
    if (!bound().live_deltas_ || !bound().prepared_producer_offers_ || bound().last_catalog_slice_ == frame) return core::Result<void>::success();
    const auto& jobs = bound().prepared_producer_offers_->job_list();
    const auto count = jobs.size() / perception_period_ + (jobs.size() % perception_period_ != 0 ? 1 : 0);
    const auto slice = frame % perception_period_;
    // Fixed sim-frame slices: no wall-clock budgets, worker timing or catch-up.
    if (count && slice < (jobs.size() + count - 1) / count) {
        const auto begin = static_cast<std::size_t>(slice) * count;
        const auto end = std::min<std::size_t>(jobs.size(), begin + count);
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
            bound().work_.producer_offer_preparations += prepared[index];
        }
    }
    bound().last_catalog_slice_ = frame;
    return core::Result<void>::success();
}

thread_local const GalacticWorld* GalacticWorld::frontier_world_ = nullptr;
thread_local GalacticWorld::BoundState* GalacticWorld::active_frontier_ = nullptr;

GalacticWorld::PlayerFrontier::PlayerFrontier(GalacticWorld& world, tactical::PlayerId player)
    : previous_world_(frontier_world_), previous_frontier_(active_frontier_) {
    if (!std::binary_search(world.prepared_frontier_players_.begin(), world.prepared_frontier_players_.end(), player))
        throw std::logic_error("galactic player frontier was not prepared at the barrier");
    lock_ = std::unique_lock<std::recursive_mutex>(*world.shards_.at(player).query_mutex);
    active_frontier_ = &world.player_frontiers_.at(player); frontier_world_ = &world;
}
GalacticWorld::PlayerFrontier::~PlayerFrontier() {
    frontier_world_ = previous_world_; active_frontier_ = previous_frontier_;
}
core::Result<void> GalacticWorld::prepare_player_frontiers(const sim::PartitionExecutor& executor,
    std::span<const tactical::PlayerId> players) {
    const auto reads = owner_reads();
    if (frontier_world_ == this || !bound_state_.snapshot_ || !std::is_sorted(players.begin(), players.end())
        || std::adjacent_find(players.begin(), players.end()) != players.end()
        || std::any_of(players.begin(), players.end(), [&](auto who) { return !who || !player(who); }))
        return core::Result<void>::failure(detail::make_error(authoritative::codes::invalid_request,
            "galactic player frontiers require ordered known owners and the serial barrier"));
    prepared_frontier_players_.assign(players.begin(), players.end());
    for (const auto who : players) player_frontiers_.try_emplace(who);
    return executor.execute_phase("galactic-player-frontiers", sim::tick_partition_count, [&](std::size_t partition) {
        const auto range = sim::partition_range(partition, players.size());
        for (auto index = range.begin; index < range.end; ++index)
            player_frontiers_.at(players[index]) = bound_state_;
    });
}

GalacticWorld::GalacticWorld(GalacticPerceptionSetup setup) : setup_(std::move(setup)) {
    if (setup_.logical_fps == 0 || setup_.players.size() > 64)
        throw std::invalid_argument("invalid galactic perception logical rate/player count");
    std::sort(setup_.players.begin(), setup_.players.end(), [](const auto& a, const auto& b) { return a.player < b.player; });
    for (const auto& row : setup_.players) shards_[row.player]; // S9a: one perception shard per player, from load
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
    for (const auto& row : setup_.force_types)
        type_traits_.push_back({row.type, row.object_traits.has_value(), row.object_traits && row.object_traits->hero,
            row.object_traits && row.object_traits->named_hero});
    for (const auto& row : setup_.players) teams_.push_back({row.player, row.team});
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
    return bind_publication(std::move(snapshot), std::make_shared<const GalacticConnectivity>(std::move(connectivity)),
        executor, synchronized_frame, next_entity_id, perception_due);
}

core::Result<void> GalacticWorld::bind_native_frontier(tactical::PlayerId player,
    std::shared_ptr<const galactic::GalacticSnapshot> snapshot, const sim::PartitionExecutor& executor,
    sim::EntityId next_entity_id) {
    if (!snapshot || !player || !next_entity_id)
        return core::Result<void>::failure(detail::make_error(authoritative::codes::invalid_request,
            "native movement requires its player frontier"));
    if (bound().native_frontier_work_.barrier != snapshot->completed_tick()) {
        bound().native_frontier_work_ = {}; bound().native_frontier_work_.barrier = snapshot->completed_tick();
    }
    ++bound().native_frontier_work_.requests;
    // Native layers cache their immutable publication until another real write.
    // The player key also keeps independent player frontiers distinct.
    if (bound().native_frontier_player_ == player && bound().snapshot_ == snapshot && bound().next_id_ == next_entity_id)
        return core::Result<void>::success();
    std::set<sim::EntityId> stealth;
    for (const auto& fleet : snapshot->fleets()) {
        const auto skipped = snapshot->fleet_combat_skip(fleet.id);
        if (!skipped) return core::Result<void>::failure(skipped.error());
        if (skipped.value()) stealth.insert(fleet.id);
    }
    auto publication = bound().connectivity_;
    if (publication->stealth_fleets != stealth) {
        auto updated = std::make_shared<GalacticConnectivity>(*publication);
        updated->stealth_fleets = std::move(stealth); publication = std::move(updated);
    }
    const auto frame = snapshot->completed_tick();
    auto rebound = bind_publication_impl(std::move(snapshot), std::move(publication), executor, frame, next_entity_id,
        false, sparse_native_frontiers_);
    if (!rebound) return rebound;
    bound().native_frontier_player_ = player;
    ++bound().native_frontier_work_.binds;
    bound().native_frontier_work_.prepared_planets += bound().work_.prepared_planets;
    bound().native_frontier_work_.prepared_force_planets += bound().work_.prepared_force_planets;
    bound().native_frontier_work_.force_terms += bound().work_.force_terms;
    return core::Result<void>::success();
}

core::Result<void> GalacticWorld::bind_publication(std::shared_ptr<const galactic::GalacticSnapshot> snapshot,
    std::shared_ptr<const GalacticConnectivity> publication, const sim::PartitionExecutor& executor,
    std::uint64_t synchronized_frame, sim::EntityId next_entity_id, bool perception_due) {
    return bind_publication_impl(std::move(snapshot), std::move(publication), executor, synchronized_frame,
        next_entity_id, perception_due, false);
}

core::Result<void> GalacticWorld::bind_publication_impl(std::shared_ptr<const galactic::GalacticSnapshot> snapshot,
    std::shared_ptr<const GalacticConnectivity> publication, const sim::PartitionExecutor& executor,
    std::uint64_t synchronized_frame, sim::EntityId next_entity_id, bool perception_due, bool native_sparse) {
    if (!publication) return core::Result<void>::failure(detail::make_error(
        authoritative::codes::invalid_request, "galactic perception needs an immutable connectivity publication"));
    const auto& connectivity = *publication;
    gc_profile::Scope profile_scope(gc_profile::Phase::world_bind);
    if (!snapshot || !next_entity_id) return core::Result<void>::failure(detail::make_error(
        authoritative::codes::invalid_request, "galactic perception needs a completed snapshot and identity frontier"));
    // Any ordinary publication bind ends the previous native-reader cache key.
    // bind_native_frontier installs its player only after a successful bind.
    bound().native_frontier_player_ = 0;
    bound().admission_preparations_ = 0;
    // The owning publisher invalidates this record for physical/economic writers.
    // A clock-only snapshot changes neither forces nor production admission. Keep
    // its producer data and ordered rows; only publish the new boundary clocks.
    // Unqualified synthetic snapshots have no generation and take the full path.
    const auto& generation = snapshot->economy_input_generation();
    // The same retained rows or object compare equal without a walk.
    const auto equal_rows = [](const auto& left, const auto& right) {
        return (left.size() == right.size() && left.data() == right.data())
            || std::equal(left.begin(), left.end(), right.begin(), right.end());
    };
    const auto equal_optional_pointer = [](const auto* left, const auto* right) {
        return left == right || (bool(left) == bool(right) && (!left || *left == *right));
    };
    bool queues_changed{};
    const auto same_production = [&](const auto& left, const auto& right) {
        gc_profile::Scope production_scope(gc_profile::Phase::compare_production);
        // The session shares an unchanged published production state by pointer: the same object has no change.
        if (&left == &right) return true;
        if (bool(left) != bool(right)) return false;
        if (!left) return true;
        if (left->maximum_human_queue != right->maximum_human_queue || left->players != right->players
            || left->fleet_blockers != right->fleet_blockers || left->nested_creation != right->nested_creation
            || left->technology_types != right->technology_types || left->planets.size() != right->planets.size()) return false;
        // Per-planet equality is independent: compare in a partitioned prepass, then fold in planet order so
        // the first mismatching planet and the queue flag read exactly as the serial walk would leave them.
        const auto count = left->planets.size();
        std::vector<unsigned char> nonqueue_same(count), queues_same(count);
        const auto compare_range = [&](std::size_t begin, std::size_t end) {
            for (auto index = begin; index < end; ++index) {
                const auto& a = left->planets[index]; const auto& b = right->planets[index];
                // Every nonqueue producer fact still qualifies the physical publication.
                // Current queues are published separately for admission/economy reads.
                nonqueue_same[index] = std::tie(a.planet, a.target_owner, a.starbase_owner, a.ground_occupier, a.orbit_occupier,
                    a.starbase_type, a.ground_level, a.space_level, a.maximum_space_level, a.ground_slots,
                    a.space_slots, a.landed_transports, a.maximum_ground_forces, a.enabled, a.surface_accessible,
                    a.starbase_blocked, a.offers, a.objects)
                    == std::tie(b.planet, b.target_owner, b.starbase_owner, b.ground_occupier, b.orbit_occupier,
                    b.starbase_type, b.ground_level, b.space_level, b.maximum_space_level, b.ground_slots,
                    b.space_slots, b.landed_transports, b.maximum_ground_forces, b.enabled, b.surface_accessible,
                    b.starbase_blocked, b.offers, b.objects);
                queues_same[index] = a.queues == b.queues;
            }
        };
        if (!executor.execute_phase("galactic-ai-compare", sim::tick_partition_count, [&](std::size_t partition) {
                const auto range = sim::partition_range(partition, count);
                compare_range(range.begin, range.end);
            })) compare_range(0, count);
        for (std::size_t index = 0; index < count; ++index) {
            if (!nonqueue_same[index]) return false;
            queues_changed = queues_changed || !queues_same[index];
        }
        return true;
    };
    bool credits_changed{};
    const auto same_accounts = [&](const auto& left, const auto& right) {
        if (&left == &right) return true; // one shared published ledger: no balance moved
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
    std::optional<gc_profile::Scope> compare_scope;
    compare_scope.emplace(gc_profile::Phase::world_compare);
    if (!perception_due && bound().live_deltas_ && bound().bound_source_ && generation && bound().bound_source_->economy_input_generation()) {
        const auto& old = *bound().bound_source_;
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
            && (&snapshot->observer() == &old.observer() || snapshot->observer() == old.observer())
            && equal_rows(snapshot->observations(), old.observations())
            && snapshot->maximum_movement_distance() == old.maximum_movement_distance()
            && std::equal(snapshot->fleets().begin(), snapshot->fleets().end(), old.fleets().begin(), old.fleets().end(),
                [](const auto& a, const auto& b) {
                    return a.id == b.id && a.owner == b.owner && a.orbit == b.orbit
                        && bool(a.motion.transit) == bool(b.motion.transit) && a.members == b.members;
                });
    }
    compare_scope.reset();
    if (!perception_due && bound().bound_source_ && generation && (same_projection || (generation == bound().bound_source_->economy_input_generation()
        && snapshot->observer_input_generation() != 0
        && snapshot->observer_input_generation() == bound().bound_source_->observer_input_generation()))
        && next_entity_id == bound().next_id_ && connectivity.stealth_fleets == bound().connectivity_->stealth_fleets) {
        std::optional<gc_profile::Scope> copy_scope;
        copy_scope.emplace(gc_profile::Phase::world_view);
        // The spare buffer is two publications old: when it has the same roster only positions can differ.
        bool aligned{};
        auto view = bound().reader_.take_buffer(aligned);
        if (aligned) {
            for (std::size_t index = 0; index < view->units.size(); ++index) view->units[index].position = bound().reader_.view()->units[index].position;
        } else {
            view->units = bound().reader_.view()->units; view->players = bound().reader_.view()->players; view->capture_candidates = bound().reader_.view()->capture_candidates;
        }
        view->tick = snapshot->completed_tick();
        if (same_projection) {
            gc_profile::Scope moved_scope(gc_profile::Phase::view_moved);
            // First insertion of an identity keeps its position; passes over the contents rows repeat until
            // no payload is added (nested payloads inherit the container's position).
            FlatIdMap slots(snapshot->fleets().size() * 4 + 16);
            std::vector<std::pair<sim::EntityId, math::Vec3>> moved;
            const auto place = [&](sim::EntityId id, const math::Vec3& position) {
                const bool inserted = slots.insert(id).second;
                if (inserted) moved.emplace_back(id, position);
                return inserted;
            };
            for (const auto& fleet : snapshot->fleets()) if (fleet.motion.transit) {
                place(fleet.id, fleet.motion.position);
                for (const auto& member : fleet.members) place(member.id, fleet.motion.position);
            }
            if (snapshot->movement() && !moved.empty()) {
                bool appended = true;
                while (appended) {
                    appended = false;
                    for (const auto& contents : snapshot->movement()->contents) if (const auto parent = slots.find(contents.container); parent != FlatIdMap::npos) {
                        const auto position = moved[parent].second;
                        for (const auto& child : contents.children) appended = place(child.id, position) || appended;
                    }
                }
            }
            std::sort(moved.begin(), moved.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            // Both lists ascend by identity: one pass moves every listed unit that exists.
            auto unit = view->units.begin();
            for (const auto& [id, position] : moved) {
                unit = std::lower_bound(unit, view->units.end(), id, [](const auto& row, sim::EntityId key) { return row.id < key; });
                if (unit == view->units.end()) break;
                if (unit->id == id) unit->position = position;
            }
        }
        copy_scope.reset();
        const bool economy_changed = queues_changed || credits_changed;
        if (economy_changed) {
            {
                gc_profile::Scope producers_scope(gc_profile::Phase::world_producers);
                if (auto refreshed = refresh_live_producers(snapshot, next_entity_id, view->units.size() + snapshot->routes().size(), same_projection); !refreshed) return refreshed;
            }
            for (auto& row : bound().planets_) row.production = snapshot->production()
                ? find_row(snapshot->production()->planets, row.planet.id, [](const auto& at) { return at.planet; }) : nullptr;
            // Do not qualify a partially refreshed publication after refusal.
            // bound().snapshot_ below retains every new borrowed producer pointer.
            bound().bound_source_.reset();
            bound().accounts_.clear();
            if (snapshot->player_economy()) for (const auto& row : snapshot->player_economy()->players) {
                tactical::EconomyView account; account.player = row.player; account.credits = row.credits;
                account.tech_level = static_cast<std::uint32_t>(row.current_tech);
                bound().accounts_.push_back(std::move(account));
            }
        }
        copy_scope.emplace(gc_profile::Phase::world_copy);
        auto input = std::make_shared<galactic_ai::WorldInput>(*bound().input_);
        input->clocks = {synchronized_frame, snapshot->clock().galactic_frame};
        if (economy_changed) {
            input->credits.clear();
            for (const auto& row : bound().accounts_) input->credits.emplace(row.player, row.credits);
        }
        if (bound().live_deltas_ && bound().prepared_producer_offers_) input->producers.set_query(producer_query());
        bound().snapshot_ = std::move(snapshot); bound().synchronized_frame_ = synchronized_frame;
        bound().reader_.publish(std::move(view)); bound().work_ = {};
        copy_scope.reset();
        {
            gc_profile::Scope release_scope(gc_profile::Phase::world_release);
            bound().input_ = std::move(input);
        }
        if (economy_changed) {
            gc_profile::Scope economy_scope(gc_profile::Phase::world_economy);
            if (auto prepared_economy = refresh_economy_queues(executor, queues_changed); !prepared_economy) return prepared_economy;
        }
        bind_connectivity(std::move(publication));
        gc_profile::Scope catalog_scope(gc_profile::Phase::world_catalog);
        auto prepared_catalog = prepare_catalog_slice(executor, synchronized_frame);
        if (prepared_catalog && economy_changed) bound().bound_source_ = bound().snapshot_;
        return prepared_catalog;
    }
    // Only staged native publications can prove local writes. Shared root identity
    // also admits rewinding to another player's retained frontier: compare BOTH
    // revision maps so removed writes are prepared again. Unknown publishers use
    // the full oracle, regardless of equal counters or planet counts.
    std::optional<std::vector<std::size_t>> sparse_planets;
    if (native_sparse && bound().bound_source_ && bound().snapshot_ && snapshot->native_planet_revisions()
        && bound().snapshot_->native_root() == snapshot->native_root()
        && (bound().snapshot_.get() == bound().snapshot_->native_root() || bound().snapshot_->native_planet_revisions())) {
        bool same_tech = bool(bound().snapshot_->player_economy()) == bool(snapshot->player_economy());
        if (same_tech && snapshot->player_economy()) {
            const auto& old = bound().snapshot_->player_economy()->players;
            const auto& fresh = snapshot->player_economy()->players;
            same_tech = old.size() == fresh.size() && std::equal(old.begin(), old.end(), fresh.begin(),
                [](const auto& a, const auto& b) { return a.player == b.player && a.current_tech == b.current_tech; });
        }
        if (same_tech) {
            sparse_planets.emplace();
            const auto* before = bound().snapshot_->native_planet_revisions();
            const auto& after = *snapshot->native_planet_revisions();
            for (std::size_t index = 0; index < bound().planets_.size(); ++index) {
                const auto id = bound().planets_[index].planet.id;
                const auto old = before ? before->find(id) : after.end();
                const auto fresh = after.find(id);
                const auto previous = before && old != before->end() ? old->second : nullptr;
                const auto current = fresh != after.end() ? fresh->second : nullptr;
                if (previous != current) sparse_planets->push_back(index);
            }
        }
    }
    // Clock-only binds can leave production pointers in an older physical
    // publication. Keep that owner alive, and restore its proof if staging
    // refuses before anything is published.
    struct SourceRetention {
        std::shared_ptr<const galactic::GalacticSnapshot>& current;
        std::shared_ptr<const galactic::GalacticSnapshot> previous;
        bool published{};
        ~SourceRetention() { if (!published) current = std::move(previous); }
    } source_retention{bound().bound_source_, bound().bound_source_};
    bound().bound_source_.reset();
    // ADR-009: stage every independent planet in disjoint slots, then publish once.
    std::optional<gc_profile::Scope> full_scope;
    full_scope.emplace(gc_profile::Phase::world_rows);
    // Copy only changed rows into disjoint staging slots. Keep the published
    // vector and its source alive through every fallible preparation below.
    std::vector<PlanetRow> rows(sparse_planets ? sparse_planets->size() : snapshot->planets().size());
    std::map<galactic::PlanetId, std::pair<std::uint64_t, std::uint64_t>> routes;
    if (!sparse_planets) for (const auto& route : snapshot->routes()) {
        const auto* from = find_row(snapshot->planets(), route.from, [](const auto& p) { return p.id; });
        const auto* to = find_row(snapshot->planets(), route.to, [](const auto& p) { return p.id; });
        const bool active = from && to && from->owner != 0 && allied(from->owner, to->owner);
        for (const auto id : {route.from, route.to}) {
            ++routes[id].first;
            if (active) ++routes[id].second;
        }
    }
    const auto refresh_row = [&](PlanetRow& row) {
        row.production = snapshot->production()
            ? find_row(snapshot->production()->planets, row.planet.id, [](const auto& p) { return p.planet; }) : nullptr;
        row.income.reset();
        if (snapshot->economy_input_generation() && snapshot->economy_input_generation()->income) {
            const auto* income = find_row(*snapshot->economy_input_generation()->income, row.planet.id, [](const auto& p) { return p.first; });
            if (income) row.income = to_single(numeric::from_fixed(income->second));
        } else if (snapshot->economy_forecast()) {
            const auto* income = find_row(snapshot->economy_forecast()->planets, row.planet.id, [](const auto& p) { return p.planet; });
            if (income && income->actual) row.income = numeric::from_fixed(*income->actual);
        }
    };
    const auto prepared = executor.execute_phase("galactic-ai-planets", sim::tick_partition_count,
        [&](std::size_t partition) {
            const auto range = sim::partition_range(partition, rows.size());
            for (auto index = range.begin; index < range.end; ++index) {
                auto& row = rows[index];
                if (sparse_planets) {
                    const auto& previous = bound().planets_[(*sparse_planets)[index]];
                    row.planet = previous.planet; row.facts = previous.facts;
                    row.routes = previous.routes; row.active_routes = previous.active_routes;
                    row.nearby = previous.nearby;
                }
                else {
                    row.planet = snapshot->planets()[index];
                    if (const auto* authored = find_row(setup_.planets, row.planet.id, [](const auto& p) { return p.planet; })) row.facts = *authored;
                    else { row.facts.planet = row.planet.id; row.facts.name = "PLANET_" + std::to_string(row.planet.id); }
                }
                refresh_row(row);
                if (const auto found = routes.find(row.planet.id); found != routes.end())
                    std::tie(row.routes, row.active_routes) = found->second;
            }
        });
    if (!prepared) return prepared;
    full_scope.reset();
    const auto forces = prepare_forces(rows, *snapshot, connectivity, executor, sparse_planets.has_value());
    if (!forces) return forces;
    full_scope.emplace(gc_profile::Phase::world_objects);
    std::vector<ReaderPlanet> reader_planets;
    const auto& roster_rows = sparse_planets ? bound().planets_ : rows;
    reader_planets.reserve(roster_rows.size());
    for (const auto& row : roster_rows) {
        const auto* production = sparse_planets && snapshot->production()
            ? find_row(snapshot->production()->planets, row.planet.id, [](const auto& p) { return p.planet; }) : row.production;
        reader_planets.push_back({row.planet.id, row.planet.owner, row.planet.position, row.facts.type, production});
    }
    auto roster = std::make_shared<const StructuralIndex>(build_structural_index(reader_planets, *snapshot));
    bool aligned{};
    auto view = bound().reader_.take_buffer(aligned);
    view->tick = snapshot->completed_tick();
    view->players.clear(); view->capture_candidates.clear();
    for (std::size_t player_index = 0; player_index < setup_.players.size(); ++player_index)
        view->players.push_back({setup_.players[player_index].player, setup_.players[player_index].team, player_index});
    full_scope.emplace(gc_profile::Phase::world_visibility);
    std::vector<ReaderLegacyVisibility> legacy;
    if (!snapshot->observer()) {
        const auto lock = query_lock();
        for (const auto& [who, mine] : shards_) {
            static_cast<void>(who);
            for (const auto& observed : mine.visibility) if (observed.mask == 0 && observed.visible) legacy.push_back({observed.player, observed.planet});
        }
    }
    const ReaderFog fog{setup_.uses_fog, snapshot.get(), type_traits_, teams_, legacy, view->players};
    if (auto filled = fill_view_units(view->units, *roster, fog, executor); !filled) return filled;
    full_scope.reset();
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
    if (!bound().live_deltas_ && snapshot->production() && snapshot->player_economy()) {
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
        const auto* previous = bound().prepared_producer_offers_.get();
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
                    old_job = find_row(previous->job_list(), std::pair{owner.player, location.planet},
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
        producer_rows_reused = !bound().live_deltas_ && previous && previous->unique_jobs && previous->job_list().size() == prepared_offers->jobs.size();
        std::size_t staged_rows{};
        for (const auto& job : prepared_offers->jobs) {
            if (job.rows) staged_rows += job.rows->size();
            const auto* old = previous ? find_row(previous->job_list(), std::pair{job.player, job.planet},
                [](const auto& row) { return std::pair{row.player, row.planet}; }) : nullptr;
            producer_rows_reused = producer_rows_reused && old && job.rows == old->rows;
        }
        if (!producer_rows_reused) builds.reserve(staged_rows);
        for (std::size_t index = 0; index < jobs; ++index) {
            if (errors[index]) return core::Result<void>::failure(*errors[index]);
            offer_preparations += preparations[index];
            bound().admission_preparations_ += admissions[index];
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
    const bool observer_changed = !bound().snapshot_ || bound().snapshot_->observer_input_generation() != snapshot->observer_input_generation();
    markup_->retain_planets(snapshot->planets());
    if (sparse_planets) {
        for (std::size_t slot = 0; slot < sparse_planets->size(); ++slot)
            bound().planets_[(*sparse_planets)[slot]] = std::move(rows[slot]);
        rows = std::move(bound().planets_);
        // Borrowed producer pointers and globally derived income can change even
        // on untouched force planets. Refresh only these small fields at commit.
        for (auto& row : rows) refresh_row(row);
    }
    bound().snapshot_ = std::move(snapshot); bound().planets_ = std::move(rows); bound().reader_.publish(std::move(view), std::move(roster));
    source_retention.published = true;
    bound().accounts_ = std::move(accounts);
    if (!producer_rows_reused) { bound().builds_ = std::move(builds); bound().build_options_.clear(); }
    bound().next_id_ = next_entity_id; bound().synchronized_frame_ = synchronized_frame;
    if (bound().live_deltas_) {
        gc_profile::Scope producers_scope(gc_profile::Phase::world_producers);
        if (auto refreshed = refresh_live_producers(bound().snapshot_, bound().next_id_, bound().reader_.view()->units.size() + bound().snapshot_->routes().size()); !refreshed) return refreshed;
    } else bound().prepared_producer_offers_ = std::move(prepared_offers);
    const auto prepared_count = sparse_planets ? sparse_planets->size() : bound().planets_.size();
    bound().work_ = {prepared_count, bound().reader_.view()->units.size(), sparse_planets ? 0 : bound().snapshot_->routes().size(), bound().builds_.size()};
    bound().work_.prepared_force_planets = prepared_count;
    if (sparse_planets) ++bound().native_frontier_work_.sparse_binds;
    bound().work_.producer_offer_preparations = offer_preparations;
    if (bound().live_deltas_ && bound().prepared_producer_offers_)
        for (const auto& job : bound().prepared_producer_offers_->job_list())
            if (const auto* location = find_row(bound().snapshot_->production()->planets, job.planet, [](const auto& p) { return p.planet; }))
                bound().work_.producer_offers += location->offers.size();
    {
        gc_profile::Scope economy_scope(gc_profile::Phase::world_economy);
        if (auto prepared_economy = prepare_economy_tokens(executor); !prepared_economy) return prepared_economy;
    }
    // WGA-258/65: each fogged unit has its own qualified visibility category.
    // A unchanged bind does not refresh caches or reset observation ages.
    full_scope.emplace(gc_profile::Phase::world_observed);
    if (observer_changed && bound().snapshot_->observer()) for (const auto& known : bound().snapshot_->observations()) {
        const std::array values{
            std::pair{galactic::bit(galactic::Intelligence::ownership), known.knowledge.ownership},
            std::pair{galactic::bit(galactic::Intelligence::fleet_presence), known.knowledge.fleet_presence},
            std::pair{galactic::bit(galactic::Intelligence::fleet_count), known.knowledge.fleet_count},
            std::pair{galactic::bit(galactic::Intelligence::ground_count), known.knowledge.ground_count}};
        for (const auto& [mask, value] : values) {
            bool changed;
            {
                const auto lock = query_lock();
                const auto* found = visibility(known.player, known.planet, mask, false);
                changed = !found || found->visible != value;
            }
            if (changed) visibility_changed(known.player, known.planet, value, mask);
        }
    }
    full_scope.reset();
    for (std::size_t slot = 0; slot < (sparse_planets ? sparse_planets->size() : bound().planets_.size()); ++slot) {
        const auto& row = bound().planets_[sparse_planets ? (*sparse_planets)[slot] : slot];
        bound().work_.force_terms += row.space.size() + row.ground.size() + row.starbase.size();
        bound().work_.neighbourhood_pairs += row.neighbourhood_pairs;
    }
    bind_connectivity(std::move(publication));
    full_scope.emplace(gc_profile::Phase::world_input);
    auto input = std::make_shared<galactic_ai::WorldInput>();
    input->clocks = {bound().synchronized_frame_, bound().snapshot_->clock().galactic_frame};
    for (const auto& row : bound().planets_) {
        galactic_ai::TargetInput target; target.planet = row.planet.id; target.owner = row.planet.owner; target.position = row.planet.position;
        if (row.production) {
            target.tokens.emplace("GROUNDBASELEVELUNNORMALIZED", real(row.production->ground_level));
            target.tokens.emplace("STARBASELEVELUNNORMALIZED", real(row.production->space_level));
        }
        input->targets.push_back(std::move(target));
    }
    if (bound().live_deltas_ && bound().prepared_producer_offers_) {
        input->producers.set_query(producer_query());
    } else if (producer_rows_reused) input->producers = bound().input_->producers;
    else {
    input->producers.reserve(bound().builds_.size());
    for (const auto& row : bound().builds_) input->producers.push_back(row.input);
    const auto canonical = [](const auto& a, const auto& b) {
        return std::tie(a.player, a.planet, a.domain, a.type) < std::tie(b.player, b.planet, b.domain, b.type);
    };
    // Rows arrive grouped by producer job (player, planet), and a planet's offers have distinct
    // types, so sorting each group yields the one canonical order. Any input where that does not
    // give a strictly ascending list is sorted whole, from the original row order (#3003).
    for (auto group = input->producers.begin(); group != input->producers.end();) {
        const auto end = std::find_if(group, input->producers.end(),
            [&](const auto& row) { return row.player != group->player || row.planet != group->planet; });
        std::stable_sort(group, end, canonical); // equal-key offers keep bound().builds_ order on every library (#4329)
        group = end;
    }
    if (std::adjacent_find(input->producers.begin(), input->producers.end(),
        [&](const auto& a, const auto& b) { return !canonical(a, b); }) != input->producers.end()) {
        input->producers.clear();
        for (const auto& row : bound().builds_) input->producers.push_back(row.input);
        std::stable_sort(input->producers.begin(), input->producers.end(), canonical); // as above
    }
    }
    for (const auto& row : bound().accounts_) input->credits.emplace(row.player, row.credits);
    bound().input_ = std::move(input);
    full_scope.emplace(gc_profile::Phase::world_catalog);
    if (auto prepared_catalog = prepare_catalog_slice(executor, synchronized_frame); !prepared_catalog) return prepared_catalog;
    bound().bound_source_ = bound().snapshot_;
    return core::Result<void>::success();
}

void GalacticWorld::bind_connectivity(std::shared_ptr<const GalacticConnectivity> connectivity) {
    if (connectivity->revision != bound().connectivity_->revision) {
        const auto lock = query_lock();
        mark_state_changed();
        for (auto& [who, mine] : shards_) {
            static_cast<void>(who);
            // Rows before the first connecting-path row neither move nor go: a shard without one is untouched, and
            // its lookup index stays valid.
            rebuild_row_index(mine);
            if (mine.first_connected == std::numeric_limits<std::size_t>::max()) continue;
            mine.cache_index.clear();
            std::size_t kept = mine.first_connected;
            mine.force_rows.erase(std::lower_bound(mine.force_rows.begin(), mine.force_rows.end(), kept), mine.force_rows.end());
            for (std::size_t index = mine.first_connected; index < mine.cache.size(); ++index) {
                auto& row = mine.cache[index];
                const auto& tokens = token_of(mine, row.key.token).tokens;
                if (galactic_connectivity_token(tokens)) {
                    if (records_ready_) mine.records.erase(row.key.player, 1, index);
                    continue;
                }
                if (index != kept) {
                    if (records_ready_) mine.records.relocate(row.key.player, 1, index, kept);
                    mine.cache[kept] = std::move(row);
                }
                if (galactic_force_deadline_token(tokens)) mine.force_rows.push_back(static_cast<std::uint32_t>(kept));
                ++kept;
            }
            mine.cache.resize(kept);
            mine.first_connected = std::numeric_limits<std::size_t>::max();
        }
    }
    if (!(connectivity->goal_reach == bound().connectivity_->goal_reach))
        bound().goal_reach_index_ = std::make_shared<const GalacticGoalReachIndex>(GalacticGoalReachIndex::build(connectivity->goal_reach));
    bound().connectivity_ = std::move(connectivity);
}

std::shared_ptr<const galactic_ai::WorldInput> GalacticWorld::copy_input() const { return bound().input_; }

std::uint64_t GalacticWorld::completed_tick() const noexcept { return bound().snapshot_ ? bound().snapshot_->completed_tick() : 0; }
const GalacticWorld::PlanetRow* GalacticWorld::planet(galactic::PlanetId id) const { return find_row(bound().planets_, id, [](const auto& p) { return p.planet.id; }); }
const GalacticPlayerFacts* GalacticWorld::player(tactical::PlayerId id) const { return find_row(setup_.players, id, [](const auto& p) { return p.player; }); }
bool GalacticWorld::allied(tactical::PlayerId a, tactical::PlayerId b) const {
    if (bound().snapshot_ && bound().snapshot_->observer()) {
        const auto* who = find_row(bound().snapshot_->observer()->players, a, [](const auto& row) { return row.player; });
        return who && (a == b || std::binary_search(who->allies.begin(), who->allies.end(), b));
    }
    const auto* left = player(a); const auto* right = player(b);
    return left && right && !left->neutral && !right->neutral && left->team == right->team;
}
const tactical::EconomyView* GalacticWorld::account(tactical::PlayerId id) const { return find_row(bound().accounts_, id, [](const auto& p) { return p.player; }); }
bool GalacticWorld::arriving(sim::EntityId id) const { return bound().reader_.arriving(id); }
bool GalacticWorld::build_allowed(tactical::PlayerId owner, sim::EntityId producer, tactical::TypeId type) const {
    const auto* option = build_option(owner, producer, type); return option && option->available;
}
const tactical::BuildOption* GalacticWorld::build_option(tactical::PlayerId owner, sim::EntityId producer, tactical::TypeId type) const {
    if (bound().live_deltas_ && bound().input_) {
        if (producer > std::numeric_limits<galactic::PlanetId>::max()) return nullptr;
        const auto lock = query_lock();
        const auto key = std::tuple{owner, producer, type};
        if (const auto at = bound().build_options_.find(key); at != bound().build_options_.end()) return &at->second;
        const std::array types{type};
        auto rows = bound().input_->producers.read(owner, static_cast<galactic::PlanetId>(producer), types);
        if (!rows || rows.value().empty()) return nullptr;
        const auto& row = rows.value().front();
        auto frames = galactic::production_frames(row.build_seconds);
        if (!frames || frames.value() > std::numeric_limits<std::uint32_t>::max()) return nullptr;
        tactical::BuildOption option; option.type = row.type; option.price = row.cost;
        option.build_frames = option.ai_build_frames = static_cast<std::uint32_t>(frames.value());
        option.available = row.admission == galactic::ProductionAdmission::allowed;
        return &bound().build_options_.emplace(key, std::move(option)).first->second;
    }
    for (const auto& row : bound().builds_) if (row.player == owner && row.producer == producer && row.option.type == type) return &row.option;
    return nullptr;
}
tactical::ProductionCounts GalacticWorld::production_counts(tactical::PlayerId owner, tactical::TypeId type) const {
    tactical::ProductionCounts out;
    if (!bound().snapshot_ || !bound().snapshot_->production()) return out;
    for (const auto& p : bound().snapshot_->production()->players) for (const auto& count : p.counts) if (count.type == type) {
        if (p.player == owner) { out.owned_player += count.owned; out.lifetime_player += count.historical; }
        if (allied(owner, p.player)) { out.owned_allies += count.owned; out.lifetime_allies += count.historical; }
    }
    for (const auto& p : bound().snapshot_->production()->planets) for (const auto& queue : p.queues) for (const auto& entry : queue) if (entry.type == type) {
        if (entry.builder == owner) ++out.queued_player;
        if (allied(owner, entry.builder)) ++out.queued_allies;
    }
    out.current_player = out.owned_player + out.queued_player;
    out.current_allies = out.owned_allies + out.queued_allies;
    return out;
}

std::vector<Target> GalacticWorld::targets(const Engine&, const PlayerAi&) const {
    std::vector<Target> out; out.reserve(bound().planets_.size());
    for (const auto& row : bound().planets_) {
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
    const auto containing = bound().reader_.containing(object);
    return containing ? std::optional{containing} : std::nullopt;
}
} // namespace eawr::script::foc::ai
