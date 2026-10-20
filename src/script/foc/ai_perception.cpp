#include "eawr/core/tick_profile.hpp"
// FoC tactical AI perception (#449, docs/behaviour/foc-tactical-ai.md "Perception", rules PG-xx
// and PE-xx): the threat grid, the goal targets and the evaluation of perceptual equations.

#include "ai_engine.hpp"
#include "tactical_ai_world.hpp"
#include "eawr/script/numeric/decimal.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <stdexcept>

namespace eawr::script::foc::ai {
namespace {

Real single(Real value) { return to_single(value); }
Real fixed(math::Fixed value) { return numeric::from_fixed(value); }
std::atomic<ThreatGrid::QueryObserver> threat_observer{};
std::atomic<Engine::InputObserver> input_observer{};

template <typename T> T clamp_to(T value, T low, T high) { return value < low ? low : (high < value ? high : value); }

// PG-08, the tactical combat power metric: the company type's AI_Combat_Power, for a squadron
// its craft's sum (PL-13); the squad-size scale is 1 for every FoC squadron.
Real power_metric(const AiType* type) { return type == nullptr ? Real{} : type->combat_power; }

// The company type of an object (PG-08): a squadron container's squadron type, else its own.
const AiType* company_type(const Host& host, const ViewUnit& unit) { return host.type(unit.type); }

bool fogged_for(const Host& host, const WorldView& view, const ViewUnit& unit, tactical::PlayerId player) {
    // SK-45: an AI player sees everything (AIUsesFogOfWarSpace = False).
    if (host.ai(player)) return false;
    const detail::ViewPlayer* entry = view.player(player);
    return entry == nullptr || (unit.visible_to & (std::uint64_t{1} << entry->snapshot_index)) == 0;
}

template <typename Row>
void prepare_rows(const sim::PartitionExecutor& executor, const std::string_view name, const std::size_t size, Row row) {
    if (size == 0) return;
    const auto prepared = executor.execute_phase(name, sim::tick_partition_count, [&](const std::size_t partition) {
        const auto range = sim::partition_range(partition, size);
        for (std::size_t index = range.begin; index < range.end; ++index) row(index);
    });
    if (!prepared) throw std::runtime_error("AI threat preparation failed: " + prepared.error().message);
}

// SAE-02: one station's exact damage/predecessor calculation. Both the
// isolated query and worker preparation use the same authored rules.
Real station_level(const Host& host, const AiType& type, sim::EntityId id) {
    const auto state = host.world->durability_state(id);
    if (state && state->hull.raw() <= 0) return Real{};
    const auto* profile = host.world->durability().find(type.type_id);
    std::uint64_t total{}, destroyed{};
    if (profile != nullptr && state) {
        for (std::size_t index = 0; index < profile->hardpoints.size(); ++index) {
            if (!profile->hardpoints[index].destroyable) continue;
            ++total;
            if (tactical::hardpoint_destroyed(*profile, *state, index)) ++destroyed;
        }
    }
    const std::uint64_t level = type.base_level;
    if (total == 0 || total < level) return real(static_cast<std::int64_t>(level));
    if (destroyed == total) return Real{};
    const auto surviving = level - level * destroyed / total;
    if (surviving == level) return real(static_cast<std::int64_t>(level));
    for (const auto previous : type.previous_base_levels) {
        if (previous == surviving) return real(previous);
    }
    return Real{};
}

} // namespace

// Immutable completed-view identities and session durability are read by
// independent rows. Only the caller merges qualifying stations in entity order.
void Engine::prepare_station_levels(const sim::PartitionExecutor* executor) {
    station_levels_.reset();
    if (!host_->world || !host_->view) return;
    const auto& units = host_->view->units;
    std::vector<std::optional<StationLevel>> rows(units.size());
    const sim::InlineExecutor inline_executor;
    prepare_rows(executor ? *executor : inline_executor, "ai-station-levels", units.size(),
        [&](const std::size_t index) {
            const auto& unit = units[index];
            const auto* type = host_->type(unit.type);
            if (type && type->star_base && type->unit_ai && !type->capture_point)
                rows[index] = StationLevel{unit.owner, station_level(*host_, *type, unit.id)};
        });
    station_levels_.emplace();
    for (const auto& row : rows) if (row) station_levels_->push_back(*row);
}

Real Engine::station_base_level(tactical::PlayerId id) const {
    if (!host_->economy(id) || !host_->world) return real(host_->setup.perception.base_level);
    // SAE-02: first qualifying station in player order then entity order;
    // a dead/damaged station still wins over any later, higher station.
    if (station_levels_) {
        for (const auto& owner : host_->world->players()) {
            if (!host_->allied(id, owner.player_id)) continue;
            for (const auto& station : *station_levels_)
                if (station.owner == owner.player_id) return station.level;
        }
        return Real{};
    }
    // Queries outside the ordered barrier retain live semantics. In particular,
    // a staged removal or content edit cannot leave a stale prepared answer.
    const auto units = host_->world->units();
    for (const auto& owner : host_->world->players()) {
        if (!host_->allied(id, owner.player_id)) continue;
        for (const auto& unit : units) {
            if (unit.owner != owner.player_id) continue;
            const auto* type = host_->type(unit.type_id);
            if (type && type->star_base && type->unit_ai && !type->capture_point)
                return station_level(*host_, *type, unit.entity_id);
        }
    }
    return Real{};
}

// ---- Threat grid ----------------------------------------------------------------------------

void ThreatGrid::clear_queries() noexcept {
    total_queries_.clear();
    force_queries_.clear();
    contributions_ready_ = false;
    for (std::size_t index = 0; index < contributions_.size(); ++index) {
        auto& storage = contribution_storage_[index];
        storage.rows.clear();
        storage.entries.clear();
        for (auto& owner : storage.owners) owner.clear();
    }
    contributions_.clear();
    contribution_owners_.clear();
    contribution_work_.clear();
}

void ThreatGrid::prepare_slots() {
    if (!slots_dirty_ && slots_.size() == tracked_.size() && cell_slots_.size() == cells_.size()) {
        // A copied grid owns a different map, even when its topology is identical.
        std::size_t slot{};
        for (const auto& [id, tracked] : tracked_) {
            if (slots_[slot].id != id) { slots_dirty_ = true; break; }
            slots_[slot++].tracked = &tracked;
        }
        if (!slots_dirty_) return;
    }
    slots_dirty_ = true; // A failed rebuild must never leave reusable partial topology.
    slots_.clear();
    for (const auto& [id, tracked] : tracked_) slots_.push_back({id, &tracked});
    cell_slots_.resize(cells_.size());
    for (std::size_t cell = 0; cell < cells_.size(); ++cell) {
        auto& output = cell_slots_[cell];
        output.clear();
        for (const auto& item : cells_[cell]) {
            const auto slot = std::lower_bound(slots_.begin(), slots_.end(), item.first,
                [](const auto& slot_row, const auto value) { return slot_row.id < value; });
            if (slot == slots_.end() || slot->id != item.first || item.second >= slot->tracked->entries.size())
                throw std::runtime_error("AI threat topology references an untracked entry");
            output.push_back(static_cast<std::size_t>(slot - slots_.begin()));
        }
    }
    slots_dirty_ = false;
}

ThreatGrid::Preparation::Preparation(ThreatGrid& grid, const sim::PartitionExecutor* executor,
    const Host& host, const WorldView& view) : grid_(grid) {
    grid_.clear_queries();
    grid_.pending_ = std::move(grid_.requested_);
    grid_.requested_.clear();
    grid_.query_counts_ = {};
    grid_.executor_ = executor;
    grid_.preparation_host_ = &host;
    grid_.preparation_view_ = &view;
}

ThreatGrid::Preparation::~Preparation() {
    // Opt-in, write-only diagnostics; these counts never enter simulation state.
    if (core::tick_profile::active) {
        if (const auto observer = threat_observer.load(std::memory_order_relaxed))
            observer(grid_.preparation_view_->tick, grid_.requested_.size(), grid_.query_counts_);
    }
    grid_.executor_ = nullptr;
    grid_.preparation_host_ = nullptr;
    grid_.preparation_view_ = nullptr;
    grid_.clear_queries();
}

void ThreatGrid::observe_queries(const QueryObserver observer) noexcept {
    threat_observer.store(observer, std::memory_order_relaxed);
}

void ThreatGrid::prepare_contributions() {
    if (!preparation_host_ || !preparation_view_) return;
    clear_queries();
    try {
        const auto& host = *preparation_host_;
        const auto& view = *preparation_view_;
        for (const auto& [key, players] : requested_) pending_.try_emplace(key, players);
        prepare_slots();
        for (const auto& owner : view.players) contribution_owners_.push_back(owner.id);
        std::sort(contribution_owners_.begin(), contribution_owners_.end());
        contribution_owners_.erase(std::unique(contribution_owners_.begin(), contribution_owners_.end()), contribution_owners_.end());
        if (contribution_storage_.size() < pending_.size()) contribution_storage_.resize(pending_.size());
        std::size_t key_index{};
        for (const auto& [key, players] : pending_) {
            static_cast<void>(players);
            auto& prepared = contribution_storage_[key_index];
            contributions_.emplace(key, key_index++);
            prepared.rows.resize(slots_.size());
            prepared.owners.resize(contribution_owners_.size());
            std::size_t entries{};
            for (std::size_t slot = 0; slot < slots_.size(); ++slot) {
                const auto& identity = slots_[slot];
                const auto* unit = view.find(identity.id);
                const auto* company = unit ? company_type(host, *unit) : nullptr;
                if (!company || (company->category_bits & key.first) == 0) continue;
                auto& contribution = prepared.rows[slot];
                contribution.unit = unit;
                contribution.offset = entries;
                contribution.count = identity.tracked->entries.size();
                entries += contribution.count;
                const auto owner = std::lower_bound(contribution_owners_.begin(), contribution_owners_.end(), unit->owner);
                if (owner != contribution_owners_.end() && *owner == unit->owner)
                    prepared.owners[static_cast<std::size_t>(owner - contribution_owners_.begin())].push_back(slot);
            }
            prepared.entries.resize(entries);
            for (std::size_t slot = 0; slot < slots_.size(); ++slot) {
                auto& contribution = prepared.rows[slot];
                // Keep a zero-length span null, including defensively decoded empty entries.
                if (contribution.unit) contribution_work_.emplace_back(&contribution, slots_[slot].tracked,
                    Real::from_repr(key.second), contribution.count ? prepared.entries.data() + contribution.offset : nullptr);
            }
        }
        query_counts_.prepared_keys = contributions_.size();
        const auto prepare = [&](const std::size_t index) {
            // ADR-009: each partition owns disjoint row ordinals/spans, hidden until successful publication.
            assert(!contributions_ready_);
            auto& [output, tracked, attenuator, entries] = contribution_work_[index];
            const auto& unit = *output->unit;
            const Real scale = single(real(1) - attenuator * single(real(1) - unit.health));
            output->total = single(power_metric(company_type(host, unit)) * scale);
            for (std::size_t entry = 0; entry < tracked->entries.size(); ++entry)
                entries[entry] = single(tracked->entries[entry].power * scale);
        };
        // Dispatch decisions depend only on a fixed work count, never measured time.
        constexpr std::size_t partition_threshold = 256;
        if (executor_ && contribution_work_.size() >= partition_threshold) {
            prepare_rows(*executor_, "ai-threat-total", contribution_work_.size(), prepare);
        } else {
            for (std::size_t index = 0; index < contribution_work_.size(); ++index) prepare(index);
        }
        contribution_work_.clear();
        pending_.clear();
        contributions_ready_ = true; // Ordered publication through contributions().
    } catch (...) {
        clear_queries();
        throw;
    }
}

const ThreatGrid::Contributions* ThreatGrid::contributions(const std::uint64_t category,
    const Real attenuator, const tactical::PlayerId player) const {
    const ContributionKey key{category, attenuator.repr};
    auto& players = requested_[key];
    ++query_counts_.queries;
    if (!players.empty()) {
        ++query_counts_.repeats;
        if (!players.contains(player)) ++query_counts_.cross_player;
    }
    players.insert(player);
    // ADR-009: this is the sole query gateway; unpublished staging stays on the inline path.
    if (!contributions_ready_) { ++query_counts_.cold; return nullptr; }
    const auto found = contributions_.find(key);
    if (found == contributions_.end()) { ++query_counts_.cold; return nullptr; }
    ++query_counts_.warm;
    return &contribution_storage_[found->second];
}

// PG-01: the grid spans the map with a cell per AI_FogCellsPerThreatCell fog cells (rounded up);
// the smallest zone radius is half the cell diagonal.
void ThreatGrid::partition(const AiBounds& bounds, std::int32_t x_cells, std::int32_t y_cells, const Constants& constants) {
    clear_queries();
    slots_dirty_ = true;
    slots_.clear();
    left_ = bounds.left;
    top_ = bounds.top;
    right_ = bounds.right;
    bottom_ = bounds.bottom;
    x_cells_ = std::max(1, x_cells);
    y_cells_ = std::max(1, y_cells);
    cell_width_ = single((right_ - left_) / real(x_cells_));
    cell_height_ = single((top_ - bottom_) / real(y_cells_));
    min_radius_ = single(square_root(cell_width_ * cell_width_ + cell_height_ * cell_height_) / real(2));
    constants_ = constants;
    cells_.assign(static_cast<std::size_t>(x_cells_) * static_cast<std::size_t>(y_cells_), {});
    tracked_.clear();
    fog_watch_.clear();
    setup_scanned_ = false;
    setup_limit_ = 0;
}

// PG-02: an object's entries are its targetable weapon hardpoints, or the object itself when it
// has none. Each entry's zone is a square of side 2r around it, r the weapon's range (the
// object's Targeting_Max_Attack_Distance), grown by speed times the look-ahead time, at least
// half a cell diagonal and at most AI_SpaceThreatRangeCap. The zone spreads the entry's combat
// power evenly: a cell holds power * cell area / (4 r^2).
void ThreatGrid::add(const ViewUnit& unit, const Host& host, Tracked& tracked) {
    slots_dirty_ = true;
    const AiType* type = host.type(unit.type);
    tracked.entries.clear();
    if (type != nullptr) {
        for (std::size_t index = 0; index < type->weapons.size(); ++index) {
            if (!type->weapons[index].targetable) continue;
            ThreatEntry entry;
            entry.weapon = static_cast<std::int32_t>(index);
            tracked.entries.push_back(entry);
        }
    }
    if (tracked.entries.empty()) tracked.entries.push_back(ThreatEntry{});
    const Real x = fixed(unit.position.x);
    const Real y = fixed(unit.position.y);
    for (std::size_t index = 0; index < tracked.entries.size(); ++index) {
        ThreatEntry& entry = tracked.entries[index];
        Real radius{};
        Real power{};
        if (entry.weapon >= 0) {
            const AiWeapon& weapon = type->weapons[static_cast<std::size_t>(entry.weapon)];
            // A destroyed destroyable hardpoint (other than the first) keeps no zone.
            if (entry.weapon > 0 && weapon.destroyable && weapon.hardpoint < unit.hardpoint_destroyed.size()
                && unit.hardpoint_destroyed[weapon.hardpoint]) {
                entry.radius = real(-1);
                continue;
            }
            radius = weapon.range;
            power = weapon.combat_power;
        } else {
            const AiType* company = company_type(host, unit);
            const AiType* reach = company;
            if (company != nullptr && company->squadron && company->squadron_unit != 0) {
                if (const AiType* craft = host.type(company->squadron_unit)) reach = craft;
            }
            radius = reach == nullptr ? Real{} : reach->max_attack_distance;
            power = power_metric(company);
        }
        if (type != nullptr && type->locomotor) radius = single(radius + type->max_speed * constants_.threat_look_ahead);
        radius = std::max(radius, min_radius_);
        radius = std::min(radius, constants_.threat_range_cap);
        entry.radius = radius;
        entry.x = single(x - left_);
        entry.y = single(top_ - y);
        entry.power = single(power * single(single(cell_width_ * cell_height_) / single(radius * real(4) * radius)));
        entry.x0 = static_cast<std::int32_t>(std::max<std::int64_t>(truncate(single((entry.x - radius) / cell_width_)), 0));
        entry.x1 = static_cast<std::int32_t>(std::min<std::int64_t>(truncate(single((entry.x + radius) / cell_width_)), x_cells_ - 1));
        entry.y0 = static_cast<std::int32_t>(std::max<std::int64_t>(truncate(single((entry.y - radius) / cell_height_)), 0));
        entry.y1 = static_cast<std::int32_t>(std::min<std::int64_t>(truncate(single((entry.y + radius) / cell_height_)), y_cells_ - 1));
        for (std::int32_t row = entry.y0; row <= entry.y1; ++row) {
            for (std::int32_t column = entry.x0; column <= entry.x1; ++column) {
                cells_[static_cast<std::size_t>(row * x_cells_ + column)].emplace_back(unit.id, index);
            }
        }
    }
}

void ThreatGrid::remove(sim::EntityId object, Tracked& tracked) {
    slots_dirty_ = true;
    for (const ThreatEntry& entry : tracked.entries) {
        if (entry.radius < Real{}) continue;
        for (std::int32_t row = entry.y0; row <= entry.y1; ++row) {
            for (std::int32_t column = entry.x0; column <= entry.x1; ++column) {
                auto& list = cells_[static_cast<std::size_t>(row * x_cells_ + column)];
                list.erase(std::remove_if(list.begin(), list.end(),
                               [&](const auto& item) { return item.first == object; }),
                    list.end());
            }
        }
    }
    tracked.entries.clear();
}

// PG-03: an object joins the grid at its first service; from then on, whenever its service frame
// comes (every 300 frames from the first), its zones are rebuilt if it moved. Dead objects leave.
void ThreatGrid::service(const WorldView& view, const Host& host, std::int64_t frame) {
    clear_queries();
    if (!ready()) return;
    if (!setup_scanned_) {
        setup_scanned_ = true;
        setup_limit_ = host.ai_world ? host.ai_world->next_entity_id() : sim::EntityId{};
    }
    const auto admitted = [&](const ViewUnit& unit) {
        const AiType* type = host.type(unit.type);
        // PG-03: a station created with the battle never joins the grid; its upgrades do.
        return !unit.planetary_weapon && type != nullptr && type->unit_ai && !(type->star_base && unit.id < setup_limit_);
    };
    for (auto iterator = tracked_.begin(); iterator != tracked_.end();) {
        const ViewUnit* current = view.find(iterator->first);
        if (current == nullptr || !admitted(*current)) {
            remove(iterator->first, iterator->second);
            iterator = tracked_.erase(iterator);
        } else {
            ++iterator;
        }
    }
    for (const ViewUnit& unit : view.units) {
        // PG-03/WSW-01: only ordinary UnitAI objects initialize combat tracking.
        if (!admitted(unit)) continue;
        const auto [tracked_it, inserted] = tracked_.try_emplace(unit.id);
        if (inserted) slots_dirty_ = true;
        Tracked& tracked = tracked_it->second;
        if (frame < tracked.next_service) continue;
        if (tracked.entries.empty()) add(unit, host, tracked);
        if (tracked.has_position && tracked.last_position == unit.position) {
            tracked.next_service += 300;
        } else {
            remove(unit.id, tracked);
            add(unit, host, tracked);
            tracked.next_service += 300;
            tracked.last_position = unit.position;
            tracked.has_position = true;
        }
    }
}

// PE-25: a threat cell is stamped when one of its fog cells reaches 0 (fully fogged). The cell is
// found from the fog cell's index, (row / k) * XCells + column / k with k fog cells per threat cell,
// as the original does, not from its position: the grid's cells are bounds / XCells wide, so on an
// extent that is not a multiple of k fog cells the two differ for the outer columns and rows. The
// sweep compares each session player's rows with the previous sweep, so only rows that changed cost.
void ThreatGrid::sweep_fog(const Host& host, const std::int64_t frame, const bool stamp) {
    if (!ready() || host.world == nullptr) return;
    const tactical::FogCells* fog = host.world->fog_cells();
    if (fog == nullptr) return;
    const auto players = host.world->players();
    const std::size_t per = static_cast<std::size_t>(std::max<std::int64_t>(1, constants_.fog_cells_per_threat_cell));
    if (fog_watch_.size() != players.size()) fog_watch_.resize(players.size());
    for (std::size_t player = 0; player < players.size(); ++player) {
        FogWatch& watch = fog_watch_[player];
        if (watch.last_seen.size() != cells_.size()) watch.last_seen.assign(cells_.size(), 0);
        auto rows = fog->value_rows(player);
        if (stamp && watch.rows.size() == rows.size()) {
            for (std::size_t row = 0; row < rows.size(); ++row) {
                if (rows[row] == watch.rows[row] || !rows[row] || !watch.rows[row]) continue;
                const auto& now = *rows[row];
                const auto& before = *watch.rows[row];
                const std::size_t columns = std::min(now.size(), before.size());
                for (std::size_t column = 0; column < columns; ++column) {
                    if (before[column] == 0 || now[column] != 0) continue;
                    const std::size_t threat_row = row / per;
                    const std::size_t threat_column = column / per;
                    if (threat_row >= static_cast<std::size_t>(y_cells_) || threat_column >= static_cast<std::size_t>(x_cells_)) continue;
                    watch.last_seen[threat_row * static_cast<std::size_t>(x_cells_) + threat_column] = frame;
                }
            }
        }
        watch.rows = std::move(rows);
    }
}

void ThreatGrid::observe_fog(const Host& host, const std::int64_t frame) { sweep_fog(host, frame, true); }

void ThreatGrid::baseline_fog(const Host& host) { sweep_fog(host, 0, false); }

// PE-25: over the threat cells the rectangle touches, each cell whose centre is fogged for the
// player adds the ticks since its last fully-fogged stamp; the total is averaged over every touched
// cell and converted to seconds. A never-seen fogged cell counts the whole game age; an unfogged
// cell counts 0. The fog is the session's real fog grid for every player, an AI player included;
// the AIUsesFogOfWarSpace setting (False in stock) is not read, so True is not modelled.
Real ThreatGrid::time_since_seen(const Host& host, const Rect& rect, const tactical::PlayerId player,
    const std::int64_t frame) const {
    if (!ready() || host.world == nullptr) return Real{};
    const tactical::FogCells* fog = host.world->fog_cells();
    if (fog == nullptr) return Real{};
    const auto players = host.world->players();
    std::size_t index = players.size();
    for (std::size_t i = 0; i < players.size(); ++i) {
        if (players[i].player_id == player) { index = i; break; }
    }
    if (index >= players.size() || index >= fog_watch_.size() || fog_watch_[index].last_seen.size() != cells_.size()) return Real{};
    const std::vector<std::int64_t>& last_seen = fog_watch_[index].last_seen;
    const std::int64_t x0 = std::max<std::int64_t>(truncate(single((rect.x - left_) / cell_width_)), 0);
    const std::int64_t x1 = std::min<std::int64_t>(truncate(single((rect.x - left_ + rect.width) / cell_width_)), x_cells_ - 1);
    const std::int64_t y0 = std::max<std::int64_t>(truncate(single((top_ - (rect.y + rect.height)) / cell_height_)), 0);
    const std::int64_t y1 = std::min<std::int64_t>(truncate(single((top_ - rect.y) / cell_height_)), y_cells_ - 1);
    std::int64_t cells = 0;
    std::int64_t total = 0;
    for (std::int64_t row = y0; row <= y1; ++row) {
        for (std::int64_t column = x0; column <= x1; ++column) {
            ++cells;
            const Real centre_x = single(left_ + single(single(real(column) + real(1) / real(2)) * cell_width_));
            const Real centre_y = single(top_ - single(single(real(row) + real(1) / real(2)) * cell_height_));
            const auto x = numeric::to_fixed(centre_x);
            const auto y = numeric::to_fixed(centre_y);
            // A centre outside the fixed-point range is outside the fog grid: fogged.
            if (x && y && fog->revealed(index, math::Vec3{x.value(), y.value(), math::Fixed{}})) continue;
            total += frame - last_seen[static_cast<std::size_t>(row * x_cells_ + column)];
        }
    }
    if (cells == 0 || total <= 0) return Real{};
    const Real fps = real(static_cast<std::int64_t>(tactical::logical_frames_per_second));
    return single(real(total) / single(real(cells) * fps));
}

std::vector<sim::EntityId> ThreatGrid::objects() const {
    std::vector<sim::EntityId> out;
    out.reserve(tracked_.size());
    for (const auto& [id, tracked] : tracked_) out.push_back(id);
    return out;
}

// PG-05: sum over the cells the rectangle touches of each covering entry's cell threat times
// (1 - attenuator * (1 - health)), for visible live objects other than `excluded` whose company
// type has the category and whose owner is not neutral and is (friendly) or is not allied to
// `player`; times AI_SpaceAreaThreatScaleFactor.
Real ThreatGrid::force(const Host& host, const WorldView& view, const Rect& rect, std::uint64_t category,
    tactical::PlayerId player, bool friendly, Real attenuator, sim::EntityId excluded) const {
    if (!ready()) return Real{};
    const ForceKey key{category, player, friendly, attenuator.repr,
        rect.x.repr, rect.y.repr, rect.width.repr, rect.height.repr, excluded};
    const bool preparation = preparing(host, view);
    const Contributions* prepared = preparation ? contributions(category, attenuator, player) : nullptr;
    if (preparation) {
        if (const auto found = force_queries_.find(key); found != force_queries_.end()) {
            ++query_counts_.scalar_hits;
            return found->second;
        }
    }
    const std::int64_t x0 = std::max<std::int64_t>(truncate(single((rect.x - left_) / cell_width_)), 0);
    const std::int64_t x1 = std::min<std::int64_t>(truncate(single((rect.x - left_ + rect.width) / cell_width_)), x_cells_ - 1);
    const std::int64_t y0 = std::max<std::int64_t>(truncate(single((top_ - (rect.y + rect.height)) / cell_height_)), 0);
    const std::int64_t y1 = std::min<std::int64_t>(truncate(single((top_ - rect.y) / cell_height_)), y_cells_ - 1);
    core::tick_profile::Scope scope(prepared ? "serial.goal.threat-cells" : "serial.goal.threat-cold-cells");
    Real total{};
    for (std::int64_t row = y0; row <= y1; ++row) {
        for (std::int64_t column = x0; column <= x1; ++column) {
            Real cell{};
            const auto cell_index = static_cast<std::size_t>(row * x_cells_ + column);
            const auto& objects = cells_[cell_index];
            for (std::size_t entry = 0; entry < objects.size(); ++entry) {
                const auto& [object, index] = objects[entry];
                const Contribution* contribution{};
                if (prepared) {
                    const auto slot = cell_slots_[cell_index][entry];
                    contribution = &prepared->rows[slot];
                    if (!contribution->unit) continue;
                }
                const ViewUnit* unit = contribution ? contribution->unit : view.find(object);
                if (unit == nullptr || object == excluded) continue;
                if (fogged_for(host, view, *unit, player)) continue;
                if (!prepared) {
                    const AiType* company = company_type(host, *unit);
                    if (company == nullptr || (company->category_bits & category) == 0) continue;
                }
                if (unit->owner == 0 || host.neutral(unit->owner)) continue;
                if (host.allied(unit->owner, player) != friendly) continue;
                if (contribution) {
                    if (index < contribution->count)
                        cell = single(cell + prepared->entries[contribution->offset + index]);
                } else {
                    const auto found = tracked_.find(object);
                    if (found == tracked_.end() || index >= found->second.entries.size()) continue;
                    const Real threat = found->second.entries[index].power;
                    cell = single(cell + single(threat * single(real(1) - attenuator * single(real(1) - unit->health))));
                }
            }
            total = single(total + cell);
        }
    }
    const Real result = single(total * constants_.area_threat_scale);
    if (preparation) force_queries_.emplace(key, result);
    return result;
}

// PG-09: unlike spread threat, concentration counts full company power for
// each entry in its rounded base cell. It has no health/area attenuation and
// does not exclude the queried object. Existing cells supply the broad phase;
// this const path adds no population sweep, cache mutation or saved state.
Real ThreatGrid::concentration(const Host& host, const WorldView& view, const Rect& rect,
    const std::uint64_t category, const tactical::PlayerId player, const bool friendly) const {
    if (!ready()) return Real{};
    const auto x0 = std::max<std::int64_t>(truncate(single((rect.x - left_) / cell_width_)), 0);
    const auto x1 = std::min<std::int64_t>(truncate(single((rect.x - left_ + rect.width) / cell_width_)), x_cells_ - 1);
    const auto y0 = std::max<std::int64_t>(truncate(single((top_ - (rect.y + rect.height)) / cell_height_)), 0);
    const auto y1 = std::min<std::int64_t>(truncate(single((top_ - rect.y) / cell_height_)), y_cells_ - 1);
    Real total{};
    for (auto row = y0; row <= y1; ++row) {
        for (auto column = x0; column <= x1; ++column) {
            const auto cell_index = row * x_cells_ + column;
            Real cell{};
            for (const auto& [object, index] : cells_[static_cast<std::size_t>(cell_index)]) {
                const auto tracked = tracked_.find(object);
                if (tracked == tracked_.end() || index >= tracked->second.entries.size()) continue;
                const auto& entry = tracked->second.entries[index];
                const auto base = truncate(single(single(entry.y / cell_height_) + real(1) / real(2))) * x_cells_
                    + truncate(single(single(entry.x / cell_width_) + real(1) / real(2)));
                // PG-09: compare only; the unclamped flattened value never indexes memory.
                if (base != cell_index) continue;
                const auto* unit = view.find(object);
                if (!unit || unit->owner == 0 || host.neutral(unit->owner)
                    || host.allied(unit->owner, player) != friendly || fogged_for(host, view, *unit, player)) continue;
                const auto* company = company_type(host, *unit);
                if (company && (company->category_bits & category) != 0)
                    cell = single(cell + power_metric(company));
            }
            total = single(total + cell);
        }
    }
    return total;
}

// PG-06.
Real ThreatGrid::total_force(const Host& host, const WorldView& view, std::uint64_t category,
    tactical::PlayerId player, bool friendly, Real attenuator) const {
    const bool preparation = preparing(host, view);
    const TotalKey key{category, player, friendly, attenuator.repr};
    const Contributions* prepared = preparation ? contributions(category, attenuator, player) : nullptr;
    if (preparation) {
        if (const auto found = total_queries_.find(key); found != total_queries_.end()) {
            ++query_counts_.scalar_hits;
            return found->second;
        }
    }
    core::tick_profile::Scope scope(prepared ? "serial.goal.threat-total" : "serial.goal.threat-cold-total");
    if (prepared) {
        Real total{};
        // PG-06: keep player order, ascending entity order and every rounding step.
        for (const detail::ViewPlayer& owner : view.players) {
            if (player != 0 && (host.neutral(player) || host.allied(player, owner.id) != friendly)) continue;
            const auto found = std::lower_bound(contribution_owners_.begin(), contribution_owners_.end(), owner.id);
            const auto owner_index = static_cast<std::size_t>(found - contribution_owners_.begin());
            for (const auto slot : prepared->owners[owner_index]) {
                const auto& contribution = prepared->rows[slot];
                if (contribution.total)
                    total = single(total + *contribution.total);
            }
        }
        total_queries_.emplace(key, total);
        return total;
    }
    Real total{};
    for (const detail::ViewPlayer& owner : view.players) {
        if (player != 0 && (host.neutral(player) || host.allied(player, owner.id) != friendly)) continue;
        for (const auto& [object, tracked] : tracked_) {
            const ViewUnit* unit = view.find(object);
            if (unit == nullptr || unit->owner != owner.id) continue;
            const AiType* company = company_type(host, *unit);
            if (company == nullptr || (company->category_bits & category) == 0) continue;
            total = single(total + single(power_metric(company) * single(real(1) - attenuator * single(real(1) - unit->health))));
        }
    }
    if (preparation) total_queries_.emplace(key, total);
    return total;
}

// PG-07. No object of the category: fully visible (project choice, unverified), never 0/0.
Real ThreatGrid::force_visibility(const Host& host, const WorldView& view, std::uint64_t category,
    tactical::PlayerId player) const {
    std::int64_t seen = 0;
    std::int64_t all = 0;
    for (const auto& [object, tracked] : tracked_) {
        const ViewUnit* unit = view.find(object);
        if (unit == nullptr || unit->owner == player) continue;
        const AiType* company = company_type(host, *unit);
        if (company == nullptr || (company->category_bits & category) == 0) continue;
        ++all;
        if (!fogged_for(host, view, *unit, player)) ++seen;
    }
    if (all == 0) return real(1);
    return single(real(seen) / real(all));
}

// ---- Evaluation -----------------------------------------------------------------------------

namespace {

constexpr std::uint64_t all_categories = ~std::uint64_t{0};

struct Parameters {
    std::uint64_t category{all_categories};
    Real attenuator{};
    std::vector<std::string> types;     // Parameter_Type names, upper case
    std::vector<std::string> factions;  // Parameter_Faction
    std::optional<Real> difficulty;
    std::optional<Real> hard_point;
};

Parameters read_parameters(const std::vector<Binding>& bindings) {
    Parameters out;
    for (const Binding& binding : bindings) {
        const Real* number = std::get_if<Real>(&binding.value);
        const std::string* text = std::get_if<std::string>(&binding.value);
        if (binding.parameter == "PARAMETER_CATEGORY" && number != nullptr) {
            out.category = static_cast<std::uint64_t>(truncate(*number));
        } else if (binding.parameter == "PARAMETER_ATTENUATOR" && number != nullptr) {
            out.attenuator = *number;
        } else if (binding.parameter == "PARAMETER_TYPE" && text != nullptr) {
            out.types.push_back(*text);
        } else if (binding.parameter == "PARAMETER_FACTION" && text != nullptr) {
            out.factions.push_back(*text);
        } else if (binding.parameter == "PARAMETER_DIFFICULTY_LEVEL_TYPE" && number != nullptr) {
            out.difficulty = *number;
        } else if (binding.parameter == "PARAMETER_HARD_POINT_TYPE" && number != nullptr) {
            out.hard_point = *number;
        }
    }
    return out;
}

// PE-07: a normalised perception: 0 stays 0, above the normaliser 1, else the ratio.
Real normalised(Real value, Real normaliser) {
    if (value == Real{}) return Real{};
    if (normaliser < value) return real(1);
    return value / normaliser;
}

} // namespace

void Engine::observe_inputs(const InputObserver observer) noexcept {
    input_observer.store(observer, std::memory_order_relaxed);
}

void Engine::report_inputs() const noexcept {
    if (core::tick_profile::active) {
        if (const auto observer = input_observer.load(std::memory_order_relaxed))
            observer(host_->view ? host_->view->tick : 0, input_counts_);
    }
}

Engine::PadInput Engine::pad_input(const sim::EntityId object, const tactical::PadState& state, const bool prepare_open) const {
    PadInput row;
    row.object = object;
    const auto& world = *host_->ai_world;
    const auto& view = *host_->view;
    row.unit = view.find(object);
    row.children = {view.find(state.constructed), view.find(state.under_construction)};
    if (!row.unit) return row;
    const auto* profile = world.economy().pads.point(row.unit->type);
    for (const auto& player : world.players()) {
        const bool allied = host_->allied(player.player_id, row.unit->owner) && !host_->neutral(row.unit->owner);
        const bool can_build = allied && profile && profile->build_pad && view.tick >= state.cooldown_until;
        std::optional<bool> open;
        if (!can_build) open = false;
        else if (prepare_open && pad_open_pending_.contains(player.player_id))
            open = world.construction_allowed(player.player_id, object);
        row.buyers.push_back({player.player_id, world.economy().menu(row.unit->type, player.faction_id), allied, can_build, open});
    }
    return row;
}

void Engine::prepare_pad_inputs(const sim::PartitionExecutor* executor) {
    pad_inputs_.reset();
    pad_open_pending_ = std::move(pad_open_requested_);
    pad_open_requested_.clear();
    if (!host_->world || !host_->view) return;
    const auto& pads = host_->ai_world->pads();
    const std::vector<std::pair<sim::EntityId, tactical::PadState>> inputs(pads.begin(), pads.end());
    std::vector<PadInput> rows(inputs.size());
    const auto prepare = [&](const std::size_t index) { rows[index] = pad_input(inputs[index].first, inputs[index].second, true); };
    constexpr std::size_t parallel_pad_rows = 256;
    if (executor && inputs.size() * host_->ai_world->players().size() >= parallel_pad_rows)
        prepare_rows(*executor, "ai-pad-perception", inputs.size(), prepare);
    else for (std::size_t index = 0; index < inputs.size(); ++index) prepare(index);
    input_counts_.pad_rows += rows.size();
    ++input_counts_.pad_prepasses;
    if (player_inputs_active_) ++input_counts_.late_prepasses;
    pad_inputs_ = std::move(rows);
}

std::optional<Real> Engine::pad_count(const tactical::PlayerId player,
    const std::vector<std::string>& names, const bool open) const {
    if (player_inputs_active_) note_pad_query(player, open); // staged in a player turn (S-T3b)
    std::vector<tactical::TypeId> types;
    for (const auto& name : names) {
        const auto found = host_->types_by_name.find(name);
        if (found == host_->types_by_name.end()) {
            // WAS-10: an unknown built-count type contributes nothing; the
            // remaining filters still count. Open-pad validation is separate.
            if (open) return std::nullopt;
            journal_plan(PlanEvent{static_cast<std::uint64_t>(frame_),
                player, {}, {}, {}, "error", "TacticalBuiltStructureCount: unknown type " + name});
            continue;
        }
        types.push_back(found->second->type_id);
    }
    if (!host_->ai_world->available() || host_->view == nullptr) return Real{};
    const auto& world = *host_->ai_world;
    const auto& view = *host_->view;
    const auto players = world.players();
    const auto owner = std::find_if(players.begin(), players.end(),
        [player](const auto& entry) { return entry.player_id == player; });
    if (owner == players.end()) return Real{};
    // Immutable pad facts are prepared before players run. Reservations remain
    // live ordered reads: an earlier goal in this barrier can close this pad.
    const auto count_row = [&](const PadInput& row) {
        const auto* pad = row.unit;
        if (!pad) return std::int64_t{};
        const auto buyer = std::find_if(row.buyers.begin(), row.buyers.end(),
            [player](const auto& entry) { return entry.player == player; });
        if (buyer == row.buyers.end() || !buyer->allied) return std::int64_t{};
        const auto* reserved = pad_claim(row.object); // stacked: earlier turns' staged claims count (S-T3b)
        const bool pending_or_live = reserved != nullptr
            && (!reserved->consumed || view.tick <= reserved->issued
                || row.children[1] != nullptr || row.children[0] != nullptr);
        const auto reservation = pending_or_live && host_->allied(player, reserved->player)
            ? reserved->type : tactical::TypeId{};
        std::int64_t count = 0;
        if (open) {
            if (reservation != 0 || !buyer->can_build) return count;
            // Only preceding-tick actual open queries request a broad-phase memo.
            // Cold dynamic players use the same const inline query, never a phase.
            const bool allowed = buyer->open ? *buyer->open : world.construction_allowed(player, row.object);
            if (!allowed) return count;
            if (types.empty() || std::any_of(types.begin(), types.end(), [&](const auto type) {
                return type == pad->type || (buyer->menu && buyer->menu->find(type));
            })) count = 1;
        } else {
            if (names.empty()) {
                if (reservation != 0) count = 1;
                else for (const auto* child : row.children) if (child) ++count;
                return count;
            }
            for (const auto type : types) {
                if (reservation != 0) {
                    const auto* child = world.economy().pads.child(reservation);
                    if (reservation == type || (child && child->constructed == type)) ++count;
                } else for (const auto* child : row.children) if (child && child->type == type) ++count;
            }
        }
        return count;
    };
    std::int64_t total = 0;
    if (pad_inputs_) {
        for (const auto& row : *pad_inputs_) total += count_row(row);
    } else {
        // Cache-free const path outside the engine barrier (including Lua).
        // Queries never start a worker phase.
        for (const auto& [object, state] : world.pads()) total += count_row(pad_input(object, state));
    }
    return real(total);
}

struct TacticalWorld::Evaluator final : LookupResolver {
    const Engine& engine;
    const Context& context;
    AiRandom& random;

    Evaluator(const Engine& owner, const Context& evaluation, AiRandom& draws)
        : engine(owner), context(evaluation), random(draws) {}

    enum class Kind : std::uint8_t { player, unit, region, location, type, game };
    struct Subject {
        Kind kind{Kind::game};
        tactical::PlayerId player{};
        const ViewUnit* unit{};   // unit / location (object-bound)
        const Target* region{};   // region
        tactical::TypeId type{};
    };

    [[nodiscard]] const Host& host() const { return *engine.host_; }
    [[nodiscard]] const WorldView& view() const { return *engine.host_->view; }

    std::optional<Real> resolve(const Lookup& lookup, const std::vector<Binding>& bindings) override {
        const auto value = resolve_value(lookup, bindings);
        if (context.proposal_trace) {
            *context.proposal_trace += " [";
            for (std::size_t index = 0; index < lookup.tokens.size(); ++index) {
                if (index) *context.proposal_trace += '.';
                *context.proposal_trace += lookup.tokens[index];
            }
            for (const auto& binding : bindings) {
                *context.proposal_trace += " " + binding.parameter + "=";
                if (const auto* number = std::get_if<Real>(&binding.value))
                    *context.proposal_trace += numeric::decimal::format_lua_number(number->repr);
                else *context.proposal_trace += std::get<std::string>(binding.value);
            }
            *context.proposal_trace += "=" + (value ? numeric::decimal::format_lua_number(value->repr) : "unavailable") + "]";
        }
        return value;
    }

    std::optional<Real> resolve_value(const Lookup& lookup, const std::vector<Binding>& bindings) {
        if (lookup.tokens.empty()) return std::nullopt;
        const Parameters parameters = read_parameters(bindings);
        const std::string& head = lookup.tokens.front();
        if (head.starts_with("FUNCTION_")) {
            // PE-08: Function_X.Evaluate runs another equation, reseeding the AI random.
            if (lookup.tokens.size() != 2 || lookup.tokens[1] != "EVALUATE") return std::nullopt;
            const Equation* equation = engine.data_.equations.find(head.substr(9));
            if (equation == nullptr) return std::nullopt;
            return engine.run_equation(*equation, context);
        }
        if (head.starts_with("SCRIPT_")) return script(head.substr(7), lookup, bindings);
        Subject subject;
        if (head == "GAME") {
            subject.kind = Kind::game;
        } else if (head == "VARIABLE_SELF") {
            subject = Subject{Kind::player, context.player};
        } else if (head == "VARIABLE_ENEMY") {
            if (!context.enemy) return std::nullopt;
            subject = Subject{Kind::player, *context.enemy};
        } else if (head == "VARIABLE_HUMAN") {
            if (!context.human) return std::nullopt;
            subject = Subject{Kind::player, *context.human};
        } else if (head == "VARIABLE_TARGET") {
            if (context.target == nullptr) return std::nullopt;
            if (context.target->object != 0) {
                subject.kind = Kind::unit;
                subject.unit = view().find(context.target->object);
                if (subject.unit == nullptr) return std::nullopt;
            } else {
                subject.kind = Kind::region;
                subject.region = context.target;
            }
        } else {
            return std::nullopt;
        }
        return walk(subject, lookup.tokens, 1, parameters);
    }

    std::optional<Real> walk(Subject subject, const std::vector<std::string>& tokens, std::size_t index,
        const Parameters& parameters) {
        if (index >= tokens.size()) return std::nullopt;
        const std::string& token = tokens[index];
        const bool last = index + 1 == tokens.size();
        // Sub-evaluators.
        if (!last) {
            if (subject.kind == Kind::unit && token == "LOCATION") {
                subject.kind = Kind::location;
                return walk(subject, tokens, index + 1, parameters);
            }
            if (subject.kind == Kind::unit && token == "TYPE") {
                subject.kind = Kind::type;
                subject.type = subject.unit->type;
                return walk(subject, tokens, index + 1, parameters);
            }
            if (subject.kind == Kind::unit && token == "OWNER") {
                subject = Subject{Kind::player, subject.unit->owner};
                return walk(subject, tokens, index + 1, parameters);
            }
            return std::nullopt;
        }
        switch (subject.kind) {
        case Kind::game: return game(token, parameters);
        case Kind::player: return player(subject.player, token, parameters);
        case Kind::unit: return unit(*subject.unit, token, parameters);
        case Kind::region: return location(subject, token, parameters);
        case Kind::location: return location(subject, token, parameters);
        case Kind::type: return type(subject.type, token, parameters);
        }
        return std::nullopt;
    }

    // PE-20 Game.*.
    std::optional<Real> game(const std::string& token, const Parameters& parameters) {
        if (token == "AGE") return mode_age();
        if (token == "ISCAMPAIGNGAME") return real(host().setup.perception.campaign_game ? 1 : 0);
        if (token == "FORCEUNNORMALIZED") {
            return engine.grid_.total_force(host(), view(), parameters.category, 0, true, parameters.attenuator);
        }
        if (token == "FORCEVISIBILITY") {
            return engine.grid_.force_visibility(host(), view(), parameters.category, context.player);
        }
        if (token == "TIMESINCESTORYPOPUP") return mode_age(); // no story dialog in a tactical battle
        return std::nullopt;
    }

    [[nodiscard]] Real mode_age() const {
        const Real inverse = to_single(real(1) / real(static_cast<std::int64_t>(tactical::logical_frames_per_second)));
        return real(engine.frame_) * inverse;
    }

    // PE-21 player tokens.
    std::optional<Real> player(tactical::PlayerId id, const std::string& token, const Parameters& parameters) {
        const auto force = [&](bool friendly) {
            return engine.grid_.total_force(host(), view(), parameters.category, id, friendly, parameters.attenuator);
        };
        const auto game_force = [&] {
            return engine.grid_.total_force(host(), view(), parameters.category, 0, true, parameters.attenuator);
        };
        if (token == "FRIENDLYFORCEUNNORMALIZED" || token == "FORCEUNNORMALIZED") return force(true);
        if (token == "ENEMYFORCEUNNORMALIZED") return force(false);
        if (token == "FRIENDLYFORCE" || token == "FORCE") return normalised(force(true), game_force());
        if (token == "ENEMYFORCE") return normalised(force(false), game_force());
        if (token == "FORCENBTD") return normalised(force(true), force(true));
        const AiPerception& fixture = host().setup.perception;
        if (token == "ISDEFENDER") return real(fixture.campaign_defender
            ? id == *fixture.campaign_defender : fixture.defender);
        if (token == "CANRETREAT") {
            const auto& setup = host().setup;
            const bool allowed = setup.perception.campaign_game && setup.campaign_can_retreat
                && setup.campaign_retreat_order && host().snapshot && setup.campaign_can_retreat(*host().snapshot, id);
            return real(allowed ? 1 : 0);
        }
        const auto* account = host().economy(id);
        if (token == "BASELEVEL") {
            core::tick_profile::Scope scope("serial.goal.base-level");
            return engine.station_base_level(id);
        }
        if (token == "CREDITSUNNORMALIZED") return fixed(host().credits(id));
        if (token == "UNITSPACEAVAILABLE") {
            return account != nullptr ? real(account->population_cap - std::min(account->population, account->population_cap)) : Real{};
        }
        if (token == "REINFORCEMENTSUNNORMALIZED") {
            Real total{};
            if (account != nullptr) for (const auto type : account->pool) {
                const auto* info = host().type(type);
                if (info != nullptr && (info->category_bits & parameters.category) != 0) total = to_single(total + info->combat_power);
            }
            return total;
        }
        if (token == "TACTICALBUILTSTRUCTURECOUNT") {
            if (id != context.player) return std::nullopt; // WAS-10: self context only
            return engine.pad_count(id, parameters.types, false);
        }
        if (token == "OPENBUILDPADCOUNT") {
            if (id != context.player) return std::nullopt; // WAS-11: self context only
            return engine.pad_count(id, parameters.types, true);
        }
        if (token == "ISFACTION") {
            const auto found = host().players.find(id);
            if (found == host().players.end()) return std::nullopt;
            for (const std::string& faction : parameters.factions) {
                if (faction == found->second->faction) return real(1);
            }
            return real(0);
        }
        if (token == "ISDIFFICULTY") {
            // WTA-47: the evaluated player's level; Easy = 0, Normal = 1, Hard = 2. A player the host does
            // not know is Normal.
            const auto found = host().players.find(id);
            const auto level = found == host().players.end() ? sim::tactical::AiLevel::normal : found->second->difficulty;
            const std::int64_t number = level == sim::tactical::AiLevel::easy ? 0 : level == sim::tactical::AiLevel::hard ? 2 : 1;
            return real(parameters.difficulty && *parameters.difficulty == real(number) ? 1 : 0);
        }
        return std::nullopt;
    }

    // PE-22 tactical unit tokens.
    std::optional<Real> unit(const ViewUnit& self, const std::string& token, const Parameters& parameters) {
        const AiType* company = host().type(self.type);
        if (token == "HEALTH") return self.health;
        if (token == "SHIELD") return self.shield;
        const auto own_force = [&]() -> Real {
            if (company == nullptr || (company->category_bits & parameters.category) == 0) return Real{};
            return company->combat_power * (real(1) - parameters.attenuator * (real(1) - self.health));
        };
        if (token == "FORCEUNNORMALIZED") return own_force();
        if (token == "FORCE") {
            return normalised(own_force(),
                engine.grid_.total_force(host(), view(), parameters.category, 0, true, parameters.attenuator));
        }
        if (token == "FORCENBTD") {
            return normalised(own_force(),
                engine.grid_.total_force(host(), view(), parameters.category, self.owner, true, parameters.attenuator));
        }
        if (token == "DISTANCETONEARESTFRIENDLY") return distance_to_nearest(self, true, parameters);
        if (token == "DISTANCETONEARESTENEMY") return distance_to_nearest(self, false, parameters);
        if (token == "ISCONTESTABLE") return real(company != nullptr && company->capture_point ? 1 : 0);
        if (token == "ISBUILDPAD") {
            const auto* pad = host().world != nullptr ? host().world->economy().pads.point(self.type) : nullptr;
            return real(pad != nullptr && pad->build_pad ? 1 : 0);
        }
        if (token == "HASBUILTOBJECT") {
            if (host().world == nullptr) return Real{};
            const auto pad = host().world->pads().find(self.id);
            return real(pad != host().world->pads().end() && (pad->second.constructed != 0 || pad->second.under_construction != 0) ? 1 : 0);
        }
        if (token == "CONTAINSHERO") return Real{};   // no heroes in the fixture (AI-G03)
        if (token == "AREENGINESONLINE") return real(self.engines_online ? 1 : 0);
        if (token == "HARDPOINTHEALTH") return hard_point_health(self, parameters);
        return std::nullopt;
    }

    // PE-23 Type.*.
    std::optional<Real> type(tactical::TypeId id, const std::string& token, const Parameters& parameters) {
        if (token != "ISTYPE") return std::nullopt;
        const AiType* self = host().type(id);
        if (self == nullptr) return real(0);
        for (const std::string& name : parameters.types) {
            if (name == self->name) return real(1);
        }
        return real(0);
    }

    // PE-24 location tokens: a region target's rectangle, or the square of side twice the
    // object's Targeting_Max_Attack_Distance around it.
    [[nodiscard]] Rect rect_of(const Subject& subject) const {
        if (subject.kind == Kind::region) return subject.region->region;
        const AiType* type = host().type(subject.unit->type);
        const Real reach = type == nullptr ? Real{} : type->max_attack_distance;
        Rect rect;
        rect.width = to_single(reach * real(2));
        rect.height = rect.width;
        rect.x = to_single(fixed(subject.unit->position.x) - rect.width / real(2));
        rect.y = to_single(fixed(subject.unit->position.y) - rect.height / real(2));
        return rect;
    }

    std::optional<Real> location(const Subject& subject, const std::string& token, const Parameters& parameters) {
        const Rect rect = rect_of(subject);
        const sim::EntityId excluded = subject.kind == Kind::location ? subject.unit->id : 0;
        const auto force = [&](bool friendly) {
            return engine.grid_.force(host(), view(), rect, parameters.category, context.player, friendly,
                parameters.attenuator, excluded);
        };
        const auto game_force = [&] {
            return engine.grid_.total_force(host(), view(), parameters.category, 0, true, parameters.attenuator);
        };
        const auto player_force = [&](bool friendly) {
            return engine.grid_.total_force(host(), view(), parameters.category, context.player, friendly, parameters.attenuator);
        };
        if (token == "FRIENDLYFORCEUNNORMALIZED") return force(true);
        if (token == "ENEMYFORCEUNNORMALIZED") return force(false);
        if (token == "FRIENDLYFORCE") return normalised(force(true), game_force());
        if (token == "ENEMYFORCE") return normalised(force(false), game_force());
        if (token == "FRIENDLYFORCENBTD") return normalised(force(true), player_force(true));
        if (token == "ENEMYFORCENBTD") return normalised(force(false), player_force(false));
        const auto concentration = [&](bool friendly) {
            return engine.grid_.concentration(host(), view(), rect, parameters.category, context.player, friendly);
        };
        if (token == "FRIENDLYUNITCONCENTRATIONUNNORMALIZED") return concentration(true);
        if (token == "ENEMYUNITCONCENTRATIONUNNORMALIZED") return concentration(false);
        if (token == "FRIENDLYUNITCONCENTRATION") return normalised(concentration(true), game_force());
        if (token == "ENEMYUNITCONCENTRATION") return normalised(concentration(false), game_force());
        if (token == "FRIENDLYUNITCONCENTRATIONNBTD") return normalised(concentration(true), player_force(true));
        if (token == "ENEMYUNITCONCENTRATIONNBTD") return normalised(concentration(false), player_force(false));
        // PE-25: the mean time since the fogged threat cells of the rectangle were last seen (raw
        // fog, also for an AI player); the plain token is that time over the game age.
        if (token == "TIMELASTSEENUNNORMALIZED") {
            return engine.grid_.time_since_seen(host(), rect, context.player, engine.frame_);
        }
        if (token == "TIMELASTSEEN") {
            const Real seconds = engine.grid_.time_since_seen(host(), rect, context.player, engine.frame_);
            if (engine.frame_ <= 0) return Real{};
            return to_single(to_single(seconds * real(static_cast<std::int64_t>(tactical::logical_frames_per_second)))
                / real(engine.frame_));
        }
        // PE-26: no attacker-entry or defender marker in the fixture.
        if (token == "ISENEMYSTARTLOCATION" || token == "ISFRIENDLYSTARTLOCATION") return Real{};
        // PE-27b: a location measures in XY from its object, or a region's centre.
        if (token == "DISTANCETONEARESTFRIENDLY" || token == "DISTANCETONEARESTENEMY") {
            const bool friendly = token == "DISTANCETONEARESTFRIENDLY";
            if (subject.kind == Kind::location) {
                return nearest(fixed(subject.unit->position.x), fixed(subject.unit->position.y), Real{}, false, subject.unit,
                    friendly, parameters);
            }
            const Target& region = *subject.region;
            return nearest(to_single(region.region.x + region.region.width / real(2)),
                to_single(region.region.y + region.region.height / real(2)), Real{}, false, nullptr, friendly, parameters);
        }
        return std::nullopt;
    }

    // PE-27 DistanceToNearestFriendly / Enemy of an object (3D).
    std::optional<Real> distance_to_nearest(const ViewUnit& self, bool friendly, const Parameters& parameters) {
        return nearest(fixed(self.position.x), fixed(self.position.y), fixed(self.position.z), true, &self, friendly, parameters);
    }

    // The nearest visible object of the players allied (friendly) or not to the context player,
    // of the category and, when given, of the Parameter_Type types; the distance less both soft
    // radii, or 999999984306749440 without one.
    std::optional<Real> nearest(Real sx, Real sy, Real sz, bool three_d, const ViewUnit* self_unit, bool friendly,
        const Parameters& parameters) {
        std::vector<const AiType*> types;
        for (const std::string& name : parameters.types) {
            const auto found = host().types_by_name.find(name);
            types.push_back(found == host().types_by_name.end() ? nullptr : found->second);
        }
        const Real none = Real::from_repr(0x43abc16d60000000ULL); // 999999984306749440 (1e18 in single precision)
        Real best = none;
        const ViewUnit* nearest = nullptr;
        const auto objects = engine.grid_.objects();
        for (const detail::ViewPlayer& owner : view().players) {
            if (host().neutral(owner.id) || host().allied(owner.id, context.player) != friendly) continue;
            for (const sim::EntityId id : objects) {
                const ViewUnit* other = view().find(id);
                if (other == nullptr || other->owner != owner.id || (self_unit != nullptr && other->id == self_unit->id)) continue;
                const AiType* type = host().type(other->type);
                if (type == nullptr || (type->category_bits & parameters.category) == 0) continue;
                if (fogged_for(host(), view(), *other, context.player)) continue;
                if (!types.empty() && std::find(types.begin(), types.end(), type) == types.end()) continue;
                const Real dx = fixed(other->position.x) - sx;
                const Real dy = fixed(other->position.y) - sy;
                const Real dz = three_d ? fixed(other->position.z) - sz : Real{};
                const Real length2 = to_single(dx * dx + dy * dy + dz * dz);
                if (length2 < best) {
                    best = length2;
                    nearest = other;
                }
            }
        }
        if (nearest == nullptr) return best;
        const AiType* self_type = self_unit == nullptr ? nullptr : host().type(self_unit->type);
        const AiType* other_type = host().type(nearest->type);
        Real out = square_root(best);
        if (self_type != nullptr) out = out - self_type->soft_radius;
        if (other_type != nullptr) out = out - other_type->soft_radius;
        return out;
    }

    // PE-28 HardPointHealth: the average health of the object's targetable destroyable
    // hardpoints of the type; no such hardpoint gives 0.
    std::optional<Real> hard_point_health(const ViewUnit& self, const Parameters& parameters) {
        static_cast<void>(parameters);
        static_cast<void>(self);
        return Real{}; // the fixture's hardpoint types are not shield generators (fidelity list)
    }

    // PE-29 evaluator scripts: GetDistanceToNearestSpaceField has no fields to find on the map.
    std::optional<Real> script(const std::string& name, const Lookup&, const std::vector<Binding>&) {
        if (name == "GETDISTANCETONEARESTSPACEFIELD") return Real::from_repr(0x43abc16d60000000ULL);
        return std::nullopt;
    }
};

std::optional<Real> TacticalWorld::perception(const Engine& engine, const Equation& equation, const Context& context, AiRandom& random) const {
    Evaluator evaluator(engine, context, random);
    return run(equation, evaluator, random);
}

std::optional<Real> Engine::run_equation(const Equation& equation, const Context& context) const {
    core::tick_profile::Scope scope("serial.goal.equation");
    // PE-12: every function evaluation reseeds the AI random from the game seed, the player,
    // the function name and the target name.
    AiRandom random;
    {
        core::tick_profile::Scope seed_scope("serial.goal.seed");
        std::uint32_t seed = game_seed_;
        seed += (static_cast<std::uint32_t>(context.player) + 1U) * 0x83U + 0x12345678U;
        seed += crc32(equation.name);
        seed += crc32(target_name(context.target));
        random.set_seed(seed);
    }
    return host_->ai_world->perception(*this, equation, context, random);
}

std::string Engine::target_name(const Target* target) const { return target == nullptr ? std::string() : target->name; }

std::optional<Real> Engine::evaluate(std::string_view equation, tactical::PlayerId player, const Target* target) const {
    const Equation* found = data_.equations.find(equation);
    if (found == nullptr) return std::nullopt;
    const PlayerAi* ai = player_ai(player);
    Context context;
    if (ai != nullptr) {
        context = context_of(*ai, target);
    } else {
        context.player = player;
        context.target = target;
    }
    return run_equation(*found, context);
}

// PE-06: Self is the AI player; Enemy the first enemy player (by ID) that is not neutral;
// Human the first human player.
Context Engine::context_of(const PlayerAi& player, const Target* target) const {
    return host_->ai_world->goal_context(player, target);
}

// ---- Targets (GS-10) ------------------------------------------------------------------------

// Every object whose type has Has_Space_Evaluator, in object order, then a region per
// AI_SpaceEvaluatorRegionSize square over the map, columns outer and rows inner.
void Engine::build_targets(PlayerAi& player) {
    // S9a: the target table is a frozen shared memo, written only by the serial barrier prologue.
    if (staged()) throw std::logic_error("AI targets are frozen during a player turn");
    const auto native_record_write = native_record_scope();
    native_records_changed(player.player, {1});
    mark_native_state_changed();
    for (auto candidate : host_->ai_world->targets(*this, player)) {
        const Target* existing = candidate.object != 0 ? target_of_object(candidate.object) : nullptr;
        if (candidate.object == 0) for (const auto& [id, target] : targets_) {
            static_cast<void>(id);
            if (target.object == 0 && target.name == candidate.name) existing = &target;
        }
        if (existing) { player.targets.push_back(existing->id); continue; }
        // S9-early: a galactic target takes its identity from the player whose list first names it.
        const auto id = allocate_target_id(player);
        if (!id) { record(nullptr, player.player, "failed", "galactic target identity range exhausted"); continue; }
        candidate.id = *id;
        player.targets.push_back(candidate.id);
        native_record_changed(0, 20, candidate.id);
        targets_.emplace(candidate.id, std::move(candidate));
    }
}

const WorldView* Engine::completed_view() const { return host_->view.get(); }

const Target* Engine::target(std::uint64_t id) const {
    const auto found = targets_.find(id);
    return found == targets_.end() ? nullptr : &found->second;
}

const Target* Engine::target_of_object(sim::EntityId object) const {
    for (const auto& [id, target] : targets_) {
        if (target.object == object) return &target;
    }
    return nullptr;
}

const std::vector<std::uint64_t>* Engine::target_list(tactical::PlayerId player) const {
    const PlayerAi* ai = player_ai(player);
    return ai == nullptr ? nullptr : &ai->targets;
}

} // namespace eawr::script::foc::ai

namespace eawr::script::foc::ai {

// GS-10: an object created with Has_Space_Evaluator joins the end of every list; a dead one
// leaves them (its record stays for the plans that name it).
void Engine::update_targets() {
    if (staged()) throw std::logic_error("AI targets are frozen during a player turn");
    const auto native_record_write = native_record_scope();
    mark_native_state_changed();
    std::vector<std::uint64_t> fresh;
    auto candidates = [&] {
        core::tick_profile::Scope scope("serial.ai.target-inputs");
        return host_->ai_world->new_targets(*this);
    }();
    core::tick_profile::Scope commit("serial.ai.target-commit");
    // S9-early: a target that appears later is shared by every list; a galactic engine allocates it from
    // the lowest-ID player's range (players_ is ascending), a tactical one from the engine counter.
    for (auto target : candidates) {
        const auto id = players_.empty() ? std::optional<std::uint64_t>{next_target_id_++} : allocate_target_id(players_.front());
        if (!id) { record(nullptr, players_.front().player, "failed", "galactic target identity range exhausted"); continue; }
        target.id = *id;
        fresh.push_back(target.id);
        native_record_changed(0, 20, target.id);
        targets_.emplace(target.id, std::move(target));
    }
    for (PlayerAi& player : players_) {
        native_records_changed(player.player, {1});
        std::erase_if(player.targets, [&](std::uint64_t id) {
            const Target* target = this->target(id);
            return target == nullptr || !host_->ai_world->target_alive(*target);
        });
        player.targets.insert(player.targets.end(), fresh.begin(), fresh.end());
        if (player.next_target > player.targets.size()) player.next_target = player.targets.size();
    }
}

} // namespace eawr::script::foc::ai
