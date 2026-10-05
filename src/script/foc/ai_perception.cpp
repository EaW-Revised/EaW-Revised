// FoC tactical AI perception (#449, docs/behaviour/foc-tactical-ai.md "Perception", rules PG-xx
// and PE-xx): the threat grid, the goal targets and the evaluation of perceptual equations.

#include "ai_engine.hpp"

#include <algorithm>
#include <stdexcept>

namespace eawr::script::foc::ai {
namespace {

Real single(Real value) { return to_single(value); }
Real fixed(math::Fixed value) { return numeric::from_fixed(value); }

std::int64_t ceiling(Real value) {
    const std::int64_t whole = truncate(value);
    return real(whole) < value ? whole + 1 : whole;
}

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

} // namespace

// ---- Threat grid ----------------------------------------------------------------------------

void ThreatGrid::clear_queries() noexcept {
    total_queries_.clear();
    force_queries_.clear();
}

ThreatGrid::Preparation::Preparation(ThreatGrid& grid, const sim::PartitionExecutor* executor,
    const Host& host, const WorldView& view) : grid_(grid) {
    grid_.clear_queries();
    grid_.executor_ = executor;
    grid_.preparation_host_ = &host;
    grid_.preparation_view_ = &view;
}

ThreatGrid::Preparation::~Preparation() {
    grid_.executor_ = nullptr;
    grid_.preparation_host_ = nullptr;
    grid_.preparation_view_ = nullptr;
    grid_.clear_queries();
}

// PG-01: the grid spans the map with a cell per AI_FogCellsPerThreatCell fog cells (rounded up);
// the smallest zone radius is half the cell diagonal.
void ThreatGrid::partition(const AiBounds& bounds, std::int32_t x_cells, std::int32_t y_cells, const Constants& constants) {
    clear_queries();
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
}

// PG-02: an object's entries are its targetable weapon hardpoints, or the object itself when it
// has none. Each entry's zone is a square of side 2r around it, r the weapon's range (the
// object's Targeting_Max_Attack_Distance), grown by speed times the look-ahead time, at least
// half a cell diagonal and at most AI_SpaceThreatRangeCap. The zone spreads the entry's combat
// power evenly: a cell holds power * cell area / (4 r^2).
void ThreatGrid::add(const ViewUnit& unit, const Host& host, Tracked& tracked) {
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
    for (auto iterator = tracked_.begin(); iterator != tracked_.end();) {
        if (view.find(iterator->first) == nullptr) {
            remove(iterator->first, iterator->second);
            iterator = tracked_.erase(iterator);
        } else {
            ++iterator;
        }
    }
    for (const ViewUnit& unit : view.units) {
        Tracked& tracked = tracked_[unit.id];
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
    if (preparation) {
        if (const auto found = force_queries_.find(key); found != force_queries_.end()) return found->second;
    }
    const std::int64_t x0 = std::max<std::int64_t>(truncate(single((rect.x - left_) / cell_width_)), 0);
    const std::int64_t x1 = std::min<std::int64_t>(truncate(single((rect.x - left_ + rect.width) / cell_width_)), x_cells_ - 1);
    const std::int64_t y0 = std::max<std::int64_t>(truncate(single((top_ - (rect.y + rect.height)) / cell_height_)), 0);
    const std::int64_t y1 = std::min<std::int64_t>(truncate(single((top_ - rect.y) / cell_height_)), y_cells_ - 1);
    if (preparation) {
        const auto columns = x1 < x0 ? std::size_t{} : static_cast<std::size_t>(x1 - x0 + 1);
        const auto rows = y1 < y0 ? std::size_t{} : static_cast<std::size_t>(y1 - y0 + 1);
        std::vector<Real> cells(rows * columns);
        prepare_rows(*executor_, "ai-threat-cells", cells.size(), [&](const std::size_t offset) {
            const auto row = y0 + static_cast<std::int64_t>(offset / columns);
            const auto column = x0 + static_cast<std::int64_t>(offset % columns);
            Real cell{};
            // PG-05: keep covering entries in insertion order, including each rounding step.
            for (const auto& [object, index] : cells_[static_cast<std::size_t>(row * x_cells_ + column)]) {
                const ViewUnit* unit = view.find(object);
                if (unit == nullptr || object == excluded) continue;
                if (fogged_for(host, view, *unit, player)) continue;
                const AiType* company = company_type(host, *unit);
                if (company == nullptr || (company->category_bits & category) == 0) continue;
                if (unit->owner == 0 || host.neutral(unit->owner)) continue;
                if (host.allied(unit->owner, player) != friendly) continue;
                const auto found = tracked_.find(object);
                if (found == tracked_.end() || index >= found->second.entries.size()) continue;
                const Real threat = found->second.entries[index].power;
                cell = single(cell + single(threat * single(real(1) - attenuator * single(real(1) - unit->health))));
            }
            cells[offset] = cell;
        });
        Real total{};
        // Fold the prepared cell scalars in the original row/column order, not partition order.
        for (const Real cell : cells) total = single(total + cell);
        const Real result = single(total * constants_.area_threat_scale);
        force_queries_.emplace(key, result);
        return result;
    }
    Real total{};
    for (std::int64_t row = y0; row <= y1; ++row) {
        for (std::int64_t column = x0; column <= x1; ++column) {
            Real cell{};
            for (const auto& [object, index] : cells_[static_cast<std::size_t>(row * x_cells_ + column)]) {
                const ViewUnit* unit = view.find(object);
                if (unit == nullptr || object == excluded) continue;
                if (fogged_for(host, view, *unit, player)) continue;
                const AiType* company = company_type(host, *unit);
                if (company == nullptr || (company->category_bits & category) == 0) continue;
                if (unit->owner == 0 || host.neutral(unit->owner)) continue;
                if (host.allied(unit->owner, player) != friendly) continue;
                const auto found = tracked_.find(object);
                if (found == tracked_.end() || index >= found->second.entries.size()) continue;
                const Real threat = found->second.entries[index].power;
                cell = single(cell + single(threat * single(real(1) - attenuator * single(real(1) - unit->health))));
            }
            total = single(total + cell);
        }
    }
    return single(total * constants_.area_threat_scale);
}

// PG-06.
Real ThreatGrid::total_force(const Host& host, const WorldView& view, std::uint64_t category,
    tactical::PlayerId player, bool friendly, Real attenuator) const {
    if (preparing(host, view)) {
        const TotalKey key{category, player, friendly, attenuator.repr};
        if (const auto found = total_queries_.find(key); found != total_queries_.end()) return found->second;
        std::vector<std::optional<Real>> contributions(view.units.size());
        prepare_rows(*executor_, "ai-threat-total", view.units.size(), [&](const std::size_t index) {
            const ViewUnit& unit = view.units[index];
            if (!tracked_.contains(unit.id)) return;
            const AiType* company = company_type(host, unit);
            if (company == nullptr || (company->category_bits & category) == 0) return;
            contributions[index] = single(power_metric(company) * single(real(1) - attenuator * single(real(1) - unit.health)));
        });
        Real total{};
        // PG-06: the copied view and tracked map both have ascending IDs. Keep the player-first
        // fold and its rounding; an entity-only fold or a sum per partition changes the result.
        for (const detail::ViewPlayer& owner : view.players) {
            if (player != 0 && (host.neutral(player) || host.allied(player, owner.id) != friendly)) continue;
            for (std::size_t index = 0; index < view.units.size(); ++index) {
                if (view.units[index].owner == owner.id && contributions[index]) {
                    total = single(total + *contributions[index]);
                }
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
    return total;
}

// PG-07.
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

struct Engine::Evaluator final : LookupResolver {
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
        if (token == "ISDEFENDER") return real(fixture.defender ? 1 : 0);
        if (token == "CANRETREAT") return real(0);            // SK-43: retreat is not allowed
        const auto* account = host().economy(id);
        if (token == "BASELEVEL") {
            if (account == nullptr || host().world == nullptr) return real(fixture.base_level);
            std::uint32_t level = 0;
            for (const auto& type : host().setup.content.types) {
                if (!type.star_base) continue;
                // SAE-02: the shared team station supplies every teammate's base level.
                for (const auto& owner : host().world->players()) {
                    if (host().allied(id, owner.player_id)
                        && host().world->production_counts(owner.player_id, type.type_id).owned_player != 0) {
                        level = std::max(level, type.base_level);
                        break;
                    }
                }
            }
            return real(level);
        }
        if (token == "CREDITSUNNORMALIZED") return account != nullptr ? fixed(account->credits) : Real{};
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
            std::int64_t total = 0;
            if (host().world != nullptr) for (const auto& name : parameters.types) {
                const auto type = host().types_by_name.find(name);
                if (type != host().types_by_name.end()) total += static_cast<std::int64_t>(host().world->production_counts(id, type->second->type_id).owned_player);
            }
            return real(total);
        }
        if (token == "OPENBUILDPADCOUNT") {
            std::int64_t total = 0;
            if (host().world != nullptr) for (const auto& [object, state] : host().world->pads()) {
                const auto* pad = view().find(object);
                const auto* type = pad != nullptr ? host().type(pad->type) : nullptr;
                if (pad == nullptr || type == nullptr || pad->owner != id || state.under_construction != 0 || state.constructed != 0) continue;
                const auto* profile = host().world->economy().pads.point(pad->type);
                if (profile == nullptr || !profile->build_pad) continue;
                if (parameters.types.empty() || std::any_of(parameters.types.begin(), parameters.types.end(),
                    [&](const std::string& name) { return name == type->name; })) ++total;
            }
            return real(total);
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
            // Normal difficulty (AI-G03); Easy = 0, Normal = 1, Hard = 2.
            return real(parameters.difficulty && *parameters.difficulty == real(1) ? 1 : 0);
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
        // PE-25: the AI sees every cell (AI fog off), so no cell has gone unseen.
        if (token == "TIMELASTSEEN" || token == "TIMELASTSEENUNNORMALIZED") return Real{};
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

std::optional<Real> Engine::run_equation(const Equation& equation, const Context& context) const {
    // PE-12: every function evaluation reseeds the AI random from the game seed, the player,
    // the function name and the target name.
    AiRandom random;
    std::uint32_t seed = game_seed_;
    seed += (static_cast<std::uint32_t>(context.player) + 1U) * 0x83U + 0x12345678U;
    seed += crc32(equation.name);
    seed += crc32(target_name(context.target));
    random.set_seed(seed);
    Evaluator evaluator(*this, context, random);
    return run(equation, evaluator, random);
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
    Context context;
    context.player = player.player;
    context.target = target;
    for (const AiPlayer& other : host_->setup.players) {
        if (!context.enemy && !other.neutral && !host_->allied(other.player, player.player)) context.enemy = other.player;
        if (!context.human && !other.ai && !other.neutral) context.human = other.player;
    }
    return context;
}

// ---- Targets (GS-10) ------------------------------------------------------------------------

// Every object whose type has Has_Space_Evaluator, in object order, then a region per
// AI_SpaceEvaluatorRegionSize square over the map, columns outer and rows inner.
void Engine::build_targets(PlayerAi& player) {
    for (const ViewUnit& unit : host_->view->units) {
        const AiType* type = host_->type(unit.type);
        if (type == nullptr || !type->space_evaluator) continue;
        if (const Target* existing = target_of_object(unit.id)) {
            player.targets.push_back(existing->id);
            continue;
        }
        Target target;
        target.id = next_target_id_++;
        target.object = unit.id;
        target.name = "OBJECT_" + std::to_string(unit.id);
        player.targets.push_back(target.id);
        targets_.emplace(target.id, std::move(target));
    }
    if (!host_->setup.bounds) return;
    const AiBounds& bounds = *host_->setup.bounds;
    const Real size = data_.constants.region_size;
    const std::int64_t columns = ceiling(to_single((bounds.right - bounds.left) / size));
    const std::int64_t rows = ceiling(to_single((bounds.top - bounds.bottom) / size));
    for (std::int64_t column = 0; column < columns; ++column) {
        for (std::int64_t row = 0; row < rows; ++row) {
            const std::string name = "CELL_" + std::to_string(column) + "_" + std::to_string(row);
            const Target* existing = nullptr;
            for (const auto& [id, target] : targets_) {
                if (target.object == 0 && target.name == name) existing = &target;
            }
            if (existing != nullptr) {
                player.targets.push_back(existing->id);
                continue;
            }
            Target target;
            target.id = next_target_id_++;
            target.cell_x = static_cast<std::int32_t>(column);
            target.cell_y = static_cast<std::int32_t>(row);
            target.region.x = to_single(bounds.left + real(column) * size);
            target.region.y = to_single(bounds.bottom + real(row) * size);
            target.region.width = clamp_to(size, Real{}, to_single(bounds.right - target.region.x));
            target.region.height = clamp_to(size, Real{}, to_single(bounds.top - target.region.y));
            target.x = to_single(target.region.x + size / real(2));
            target.y = to_single(target.region.y + size / real(2));
            target.name = name;
            player.targets.push_back(target.id);
            targets_.emplace(target.id, std::move(target));
        }
    }
}

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

const Target* Engine::region_at(Real x, Real y) const {
    for (const auto& [id, target] : targets_) {
        if (target.object != 0) continue;
        const Rect& rect = target.region;
        if (!(x < rect.x) && x < rect.x + rect.width && !(y < rect.y) && y < rect.y + rect.height) return &target;
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
    const WorldView& view = *host_->view;
    std::vector<std::uint64_t> fresh;
    for (const ViewUnit& unit : view.units) {
        const AiType* type = host_->type(unit.type);
        if (type == nullptr || !type->space_evaluator || target_of_object(unit.id) != nullptr) continue;
        Target target;
        target.id = next_target_id_++;
        target.object = unit.id;
        target.name = "OBJECT_" + std::to_string(unit.id);
        fresh.push_back(target.id);
        targets_.emplace(target.id, std::move(target));
    }
    for (PlayerAi& player : players_) {
        std::erase_if(player.targets, [&](std::uint64_t id) {
            const Target* target = this->target(id);
            return target == nullptr || (target->object != 0 && view.find(target->object) == nullptr);
        });
        player.targets.insert(player.targets.end(), fresh.begin(), fresh.end());
        if (player.next_target > player.targets.size()) player.next_target = player.targets.size();
    }
}

} // namespace eawr::script::foc::ai
