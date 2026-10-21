#include "session_impl.hpp"
#include "session_diagnostic.hpp"

#include <algorithm>
#include <optional>

namespace eawr::sim::galactic {
void GalacticSession::defer_history_service(bool deferred) noexcept { impl_->history_service_deferred = deferred; }
const HistoryArchive& GalacticSession::history_archive() const noexcept { return impl_->history_archive; }

core::Result<void> GalacticSession::service_admitted_history(std::uint64_t active_tick, const PartitionExecutor& executor) {
    auto& impl = *impl_;
    if (active_tick > galactic_max_ticks)
        return session_detail::fail<void>(diagnostic_codes::invalid_setup, "History active tick exceeds the simulation limit");
    struct Contribution { PlayerId owner{}; HistoryValue space{}, ground{}; bool known{true}; };
    const bool sampling = active_tick < 3 || active_tick % 30 == 0;
    std::vector<Contribution> contributions;
    const auto actors = impl.actor_inputs();
    const auto planets = sampling ? impl.sorted_planets() : std::vector<Planet>{};
    const auto fleets = sampling ? impl.sorted_fleets() : std::vector<Fleet>{};
    const auto contents = sampling && impl.movement_inputs ? impl.movement_inputs->contents : std::vector<MovementContents>{};
    const auto type = [&](TypeId id) -> const CensusType* {
        if (!impl.setup.census) return nullptr;
        const auto& rows = impl.setup.census->types;
        const auto at = std::lower_bound(rows.begin(), rows.end(), id, [](const auto& row, auto key) { return row.type < key; });
        return at != rows.end() && at->type == id ? &*at : nullptr;
    };
    const auto metric = [](const CensusType* row) -> std::optional<HistoryValue> {
        if (!row || !row->history) return {};
        return HistoryValue::from_fixed(row->history->power);
    };
    const auto actor = [&](EntityId id) -> const CensusObject* {
        const auto at = std::lower_bound(actors.begin(), actors.end(), id, [](const auto& row, auto key) { return row.id < key; });
        return at != actors.end() && at->id == id ? &*at : nullptr;
    };
    const auto power = [&](const CensusObject& object, bool space) -> std::optional<HistoryValue> {
        // WGP-191: representation supplies ordinary power. A company transport
        // retains its exact original company binding for the ground query.
        const bool planetary = std::binary_search(planets.begin(), planets.end(), Planet{object.id},
            [](const auto& a, const auto& b) { return a.id < b.id; });
        const auto* current = type(object.current_type ? object.current_type : planetary ? object.original_type : 0);
        const auto* original = type(object.original_type);
        if (!current || !current->history) return {};
        if (!space && current->history->transport && original && original->company) return metric(original);
        if (!current->history->fleet) return metric(current);
        HistoryValue total{}; bool known = true;
        const auto add = [&](const FleetMember& member) {
            const auto* child = actor(member.id);
            if (!child) { known = false; return; }
            const auto* child_type = type(child->current_type);
            if (!child_type || !child_type->history) { known = false; return; }
            std::optional<HistoryValue> value;
            if (child_type->history->fleet) {
                // Direct nested squadron contributes its bound dummy type to
                // the space query; the ground query ignores nested fleets.
                if (space) {
                    const auto* squadron = type(child->original_type);
                    if (squadron && squadron->history && squadron->history->squadron) value = metric(squadron);
                    else known = false;
                }
            } else if (space) value = metric(child_type);
            else if (child_type->history->transport) {
                const auto* company = type(child->original_type);
                value = metric(company && company->company ? company : child_type);
            }
            if (value) total = total + *value;
        };
        // WGP-192: direct breakdown only; cargo and nested squadron children
        // are not recursively flattened by this reader.
        const auto fleet = std::lower_bound(fleets.begin(), fleets.end(), object.id, [](const auto& row, auto id) { return row.id < id; });
        if (fleet != fleets.end() && fleet->id == object.id) for (const auto& member : fleet->members) add(member);
        else {
            const auto& rows = contents;
            const auto at = std::lower_bound(rows.begin(), rows.end(), object.id, [](const auto& row, auto id) { return row.container < id; });
            if (at != rows.end() && at->container == object.id) for (const auto& member : at->children) add(member);
        }
        return known ? std::optional(total) : std::nullopt;
    };
    if (sampling) {
        contributions.resize(actors.size());
        const auto phase = executor.execute_phase("galactic-history-objects", tick_partition_count, [&](std::size_t partition) {
            const auto range = partition_range(partition, actors.size());
            for (auto i = range.begin; i < range.end; ++i) {
                const auto& object = actors[i]; auto& out = contributions[i];
                // WGP-191: membership in this owned-record list is the input;
                // the chart reader adds no local death/deletion predicate.
                out.owner = object.owner;
                const auto space = power(object, true), ground = power(object, false);
                out.known = space.has_value() && ground.has_value();
                if (out.known) { out.space = *space; out.ground = *ground; }
            }
        });
        if (!phase) return core::Result<void>::failure(phase.error());
    }
    std::map<PlayerId, HistorySample> samples;
    std::map<PlayerId, bool> known;
    for (const auto& player : impl.setup.players) {
        samples[player.player_id].frame = active_tick;
        known[player.player_id] = impl.economy.has_value() && impl.setup.census.has_value();
    }
    if (sampling) {
        if (impl.economy) for (const auto& account : impl.economy->players) {
            auto& row = samples.at(account.player); row.technology = account.current_tech;
            row.economy = HistoryValue::from_fixed(account.credits);
        }
        for (const auto& planet : planets) ++samples.at(planet.owner).planets;
        // Ordered stable-object reduction preserves single-precision additions
        // across worker counts; workers never share a floating accumulator.
        for (const auto& value : contributions) if (samples.contains(value.owner)) {
            auto& row = samples.at(value.owner);
            row.military = (row.military + value.space) + value.ground;
            known.at(value.owner) = known.at(value.owner) && value.known;
        }
    }
    bool changed{};
    for (auto& [player, rows] : impl.history_archive) {
        const auto old_size = rows.size();
        const auto& sample = samples.at(player);
        if (!sample.economy.finite() || !sample.military.finite())
            return session_detail::fail<void>(diagnostic_codes::resource_limit, "History sample exceeds finite floating range");
        service_history(rows, sample, known.at(player));
        changed = changed || old_size != rows.size();
    }
    if (changed) {
        impl.dirty[static_cast<std::size_t>(StateSection::history)] = true;
        if (impl.history_service_deferred) {
            const auto events = impl.snapshot->events();
            impl.snapshot = impl.make_snapshot({events.begin(), events.end()}, &executor);
        }
    }
    return core::Result<void>::success();
}
} // namespace eawr::sim::galactic
