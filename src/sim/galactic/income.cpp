#include "eawr/sim/galactic/income.hpp"

#include <array>
#include <map>
#include <algorithm>
#include <string>
#include <tuple>
#include "encoding.hpp"
#include "session_impl.hpp"
#include "native_income_projection.hpp"
#include "eawr/sim/tactical/combat.hpp"
#include "eawr/sim/galactic/production.hpp"
#include "eawr/sim/galactic/economy_corruption.hpp"
#include "../replay_internal.hpp"

namespace eawr::sim::galactic {

core::Result<std::vector<IncomeCurvePoint>> parse_income_curve(std::string_view text) {
    std::vector<math::Fixed> values;
    while (!text.empty()) {
        const auto first = text.find_first_not_of(" ,\t\r\n");
        if (first == std::string_view::npos) break;
        text.remove_prefix(first);
        const auto end = text.find_first_of(" ,\t\r\n");
        auto number = math::Fixed::from_decimal(text.substr(0, end));
        if (!number) return core::Result<std::vector<IncomeCurvePoint>>::failure(number.error());
        values.push_back(number.value());
        if (values.size() > galactic_max_objects * 2) return core::Result<std::vector<IncomeCurvePoint>>::failure(
            detail::diagnostic(diagnostic_codes::invalid_setup, "income curve exceeds point budget"));
        if (end == std::string_view::npos) break;
        text.remove_prefix(end);
    }
    if (values.size() % 2) return core::Result<std::vector<IncomeCurvePoint>>::failure(
        detail::diagnostic(diagnostic_codes::invalid_setup, "income curve needs x/y pairs"));
    std::vector<IncomeCurvePoint> points;
    for (std::size_t i = 0; i < values.size(); i += 2) points.push_back({values[i], values[i + 1]});
    std::sort(points.begin(), points.end(), [](const auto& a, const auto& b) { return a.x < b.x; });
    // WGEP-63/U-03: duplicate-x semantics remain unqualified; reject, never guess.
    if (std::adjacent_find(points.begin(), points.end(), [](const auto& a, const auto& b) { return a.x == b.x; }) != points.end())
        return core::Result<std::vector<IncomeCurvePoint>>::failure(
            detail::diagnostic(diagnostic_codes::invalid_setup, "duplicate income curve x is unqualified (WGEP-63/U-03)"));
    return core::Result<std::vector<IncomeCurvePoint>>::success(std::move(points));
}

core::Result<math::Fixed> evaluate_income_curve(std::span<const IncomeCurvePoint> points, math::Fixed x) {
    // WGEP-63: no extrapolation, and insufficient points return zero.
    if (points.size() < 2 || x < points.front().x || x > points.back().x)
        return core::Result<math::Fixed>::success({});
    const auto upper = std::lower_bound(points.begin(), points.end(), x,
        [](const auto& point, auto value) { return point.x < value; });
    if (upper->x == x) return core::Result<math::Fixed>::success(upper->y);
    const auto& lower = *(upper - 1);
    auto offset = math::subtract(x, lower.x); if (!offset) return offset;
    auto width = math::subtract(upper->x, lower.x); if (!width) return width;
    auto fraction = math::divide(offset.value(), width.value()); if (!fraction) return fraction;
    auto rise = math::subtract(upper->y, lower.y); if (!rise) return rise;
    auto scaled = math::multiply(fraction.value(), rise.value()); if (!scaled) return scaled;
    return math::add(lower.y, scaled.value());
}

core::Result<PlanetIncomeContribution> planet_income_contribution(math::Fixed basic,
    math::Fixed black_market_multiplier, math::Fixed bonus, math::Fixed alignment_bonus,
    math::Fixed alignment_penalty, math::Fixed maximum_alignment_bonus,
    math::Fixed maximum_alignment_penalty, math::Fixed political_value, math::Fixed route_factor) {
    PlanetIncomeContribution out;
    const auto fail = [](const auto& result) { return core::Result<PlanetIncomeContribution>::failure(result.error()); };
    auto base = math::multiply(basic, black_market_multiplier); if (!base) return fail(base);
    out.base = base.value(); out.bonus = bonus;
    auto aligned = math::multiply(basic, alignment_bonus); if (!aligned) return fail(aligned);
    aligned = math::multiply(aligned.value(), maximum_alignment_bonus); if (!aligned) return fail(aligned);
    if (aligned.value().raw() > 0) out.alignment = aligned.value();
    else {
        auto penalty = math::multiply(basic, alignment_penalty); if (!penalty) return fail(penalty);
        penalty = math::multiply(penalty.value(), maximum_alignment_penalty); if (!penalty) return fail(penalty);
        if (penalty.value().raw() > 0) {
            auto negative = math::subtract({}, penalty.value()); if (!negative) return fail(negative);
            out.alignment = negative.value();
        }
    }
    // WGEP-08: callers preserve their retained source for neutral/nonpositive input.
    if (political_value.raw() > 0) {
        auto factor = math::subtract(political_value, math::Fixed::from_raw(math::Fixed::scale));
        if (!factor) return fail(factor);
        auto political = math::multiply(basic, factor.value()); if (!political) return fail(political);
        out.political = political.value();
    }
    auto trade = math::multiply(basic, route_factor); if (!trade) return fail(trade);
    out.trade = trade.value(); out.total = out.base;
    for (auto value : {out.alignment, out.political, out.trade, out.bonus}) {
        auto sum = math::add(out.total, value); if (!sum) return fail(sum);
        out.total = sum.value();
    }
    return core::Result<PlanetIncomeContribution>::success(out);
}

core::Result<void> validate_planet_income_rules(const GalacticSetup& setup) {
    if (!setup.fiscal || !setup.fiscal->contributions) return core::Result<void>::success();
    const auto& rules = *setup.fiscal->contributions;
    const auto invalid = [] { return core::Result<void>::failure(detail::diagnostic(
        diagnostic_codes::invalid_setup, "invalid planet income contributions")); };
    const auto bounded = [](math::Fixed value) { return value.raw() >= -(std::int64_t{1} << 40) && value.raw() <= (std::int64_t{1} << 40); };
    if (rules.factions.size() != setup.players.size() || rules.planets.size() != setup.planets.size()
        || rules.routes.size() != setup.routes.size() || rules.political_curve.size() > galactic_max_objects
        || !bounded(rules.maximum_alignment_bonus) || !bounded(rules.maximum_alignment_penalty)
        || rules.maximum_alignment_bonus.raw() < 0 || rules.maximum_alignment_bonus.raw() > math::Fixed::scale
        || rules.maximum_alignment_penalty.raw() < 0 || rules.maximum_alignment_penalty.raw() > math::Fixed::scale) return invalid();
    for (std::size_t i = 0; i < rules.factions.size(); ++i)
        if (rules.factions[i].player != setup.players[i].player_id || rules.factions[i].alignment > IncomeAlignment::dark) return invalid();
    // CGS-16: authored strength is unclamped; checked arithmetic owns overflow.
    for (std::size_t i = 0; i < rules.planets.size(); ++i)
        if (rules.planets[i].planet != setup.planets[i].id || rules.planets[i].alignment > IncomeAlignment::dark) return invalid();
    for (std::size_t i = 0; i < rules.routes.size(); ++i)
        if (rules.routes[i].route != setup.routes[i].id || !bounded(rules.routes[i].factor)) return invalid();
    for (std::size_t i = 0; i < rules.political_curve.size(); ++i)
        if (!bounded(rules.political_curve[i].x) || !bounded(rules.political_curve[i].y)
            || (i && rules.political_curve[i-1].x >= rules.political_curve[i].x)) return invalid();
    return core::Result<void>::success();
}

namespace {
// WGEP-31: the strongest reduction per stacking category, summed in ascending category order.
template<class Categories>
core::Result<math::Fixed> combine_reductions(const Categories& categories, bool price) {
    math::Fixed total;
    for (const auto& [category, reduction] : categories) {
        (void)category;
        auto sum = math::add(total, reduction);
        if (!sum) return sum;
        total = sum.value();
    }
    const auto one = math::Fixed::from_raw(math::Fixed::scale);
    // WGEP-31: time deliberately retains zero/negative multipliers.
    if (price && total >= one) total = math::Fixed::from_decimal("0.999999").value();
    return math::subtract(one, total);
}
} // namespace

core::Result<math::Fixed> production_reduction_multiplier(
    std::span<const ProductionReduction> global, std::span<const ProductionReduction> local,
    bool price) {
    std::map<std::uint32_t, math::Fixed> categories;
    for (auto rows : {global, local}) for (const auto& row : rows) {
        const auto [at, inserted] = categories.emplace(row.category, row.reduction);
        if (!inserted && at->second < row.reduction) at->second = row.reduction;
    }
    return combine_reductions(categories, price);
}

core::Result<math::Fixed> mining_income_bonus(math::Fixed basic, math::Fixed percentage,
    math::Fixed absolute, bool mining_colony, math::Fixed colony_multiplier) {
    auto product = math::multiply(basic, percentage);
    if (!product) return product;
    auto sum = math::add(product.value(), absolute);
    if (!sum || !mining_colony) return sum;
    return math::multiply(sum.value(), colony_multiplier);
}

core::Result<math::Fixed> gambling_income_sample(math::Fixed basic, const GamblingIncomeAbility& ability,
    std::uint32_t win_draw, std::uint32_t factor_draw) {
    if (win_draw > 32767 || factor_draw > 32767) return core::Result<math::Fixed>::failure(
        detail::diagnostic(diagnostic_codes::invalid_setup, "gambling requires two 15-bit samples"));
    const auto unit = math::Fixed::from_ratio(win_draw, 32767).value();
    const auto low = unit < ability.chance ? math::Fixed{} : ability.minimum;
    const auto high = unit < ability.chance ? ability.maximum : math::Fixed{};
    auto width = math::subtract(high, low); if (!width) return width;
    auto scaled = math::multiply(width.value(), math::Fixed::from_ratio(factor_draw, 32767).value());
    if (!scaled) return scaled;
    auto factor = math::add(low, scaled.value()); if (!factor) return factor;
    return math::multiply(basic, factor.value());
}

namespace {
template <class Rows, class Key> bool ordered(const Rows& rows, Key key) {
    return std::adjacent_find(rows.begin(), rows.end(), [&](const auto& a, const auto& b) { return key(a) >= key(b); }) == rows.end();
}
// WGEP-165: authored halt seconds become GC frames at the nearest frame (half up), like the
// fiscal cycle and gambling interval conversions: a Q24 decimal such as 0.2 s is not an
// exact multiple of a frame and must not fall one frame short.
core::Result<std::uint64_t> halt_frames(math::Fixed seconds) {
    constexpr std::int64_t half = std::int64_t{1} << (math::Fixed::fractional_bits - 1);
    if (seconds.raw() < 0 || seconds.raw() > static_cast<std::int64_t>(galactic_max_ticks) * math::Fixed::scale / galactic_frames_per_second)
        return core::Result<std::uint64_t>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "credit-halt duration out of range"));
    return core::Result<std::uint64_t>::success(static_cast<std::uint64_t>(
        (seconds.raw() * galactic_frames_per_second + half) >> math::Fixed::fractional_bits));
}
const EconomicType* profile(const IncomeSetup& setup, TypeId type) {
    const auto at = std::lower_bound(setup.types.begin(), setup.types.end(), type,
        [](const auto& row, auto id) { return row.type < id; });
    return at != setup.types.end() && at->type == type ? &*at : nullptr;
}
const EconomicPlanet* location(const IncomeSetup& setup, PlanetId planet) {
    const auto at = std::lower_bound(setup.planets.begin(), setup.planets.end(), planet,
        [](const auto& row, auto id) { return row.planet < id; });
    return at != setup.planets.end() && at->planet == planet ? &*at : nullptr;
}
} // namespace

core::Result<void> validate_income(const GalacticSetup& setup) {
    if (!setup.income) return core::Result<void>::success();
    const auto invalid = [](const std::string& where = {}) { return core::Result<void>::failure(detail::diagnostic(
        diagnostic_codes::invalid_setup, "invalid galactic economic profiles or source bindings" + (where.empty() ? std::string{} : " (" + where + ")"))); };
    const auto& income = *setup.income;
    if (!setup.fiscal || income.types.size() > galactic_max_objects || income.sources.size() > galactic_max_objects
        || income.planets.size() != setup.planets.size() || !ordered(income.types, [](const auto& row) { return row.type; })
        || !ordered(income.sources, [](const auto& row) { return row.id; })) return invalid();
    for (std::size_t i = 0; i < income.planets.size(); ++i)
        if (income.planets[i].planet != setup.planets[i].id) return invalid();
    for (const auto& type : income.types) {
        if (!type.type || type.reductions.size() > galactic_max_objects) return invalid("type " + std::to_string(type.type));
        if (type.technology_price_reduction) {
            if (!setup.economy_corruption) return invalid("type " + std::to_string(type.type));
            const auto negative = math::subtract({}, *type.technology_price_reduction);
            if (!negative || !technology_purchase_multiplier(std::array{TechnologyPriceModifier{1, negative.value(), {}}})) return invalid("type " + std::to_string(type.type));
        }
        const auto bounded = [](math::Fixed n) { return n.raw() >= -(std::int64_t{1} << 40) && n.raw() <= (std::int64_t{1} << 40); };
        if (type.mining && (!bounded(type.mining->percentage) || !bounded(type.mining->absolute)
            || !bounded(type.mining->colony_multiplier) || type.mining->colony_multiplier.raw() < 0)) return invalid("type " + std::to_string(type.type));
        if (type.gambling && (type.gambling->chance.raw() < 0 || type.gambling->chance.raw() > math::Fixed::scale
            || !bounded(type.gambling->minimum) || !bounded(type.gambling->maximum)
            || type.gambling->minimum.raw() > 0 || type.gambling->maximum.raw() < 0
            || type.gambling->interval_frames > 32767)) return invalid("type " + std::to_string(type.type)); // GFC-09 supported integer span
        // WGEP-163: the authored halt duration must convert to a bounded frame count.
        if (type.credit_halt && (!bounded(type.credit_halt->seconds) || !halt_frames(type.credit_halt->seconds))) return invalid("type " + std::to_string(type.type));
        if (!ordered(type.reductions, [](const auto& row) { return std::tuple{row.type, row.price, row.category, row.galaxy_wide}; })) return invalid("type " + std::to_string(type.type));
        for (const auto& row : type.reductions) if (!row.type || !bounded(row.reduction)) return invalid("type " + std::to_string(type.type));
    }
    std::map<EntityId, std::tuple<TypeId, PlanetId, PlayerId>> live_sources;
    for (const auto& row : setup.fleets) for (const auto& member : row.members)
        live_sources.emplace(member.id, std::tuple{member.type, row.orbit, row.owner});
    if (setup.fleet_control) for (const auto& row : setup.fleet_control->detached_units)
        live_sources.emplace(row.member.id, std::tuple{row.member.type, row.planet, row.owner});
    if (setup.control) for (const auto& planet : setup.control->planets) for (const auto& row : planet.landed)
        live_sources.emplace(row.id, std::tuple{row.type, planet.planet, row.owner});
    if (setup.production) for (const auto& planet : setup.production->planets) for (const auto& row : planet.objects)
        live_sources.emplace(row.id, std::tuple{row.type, planet.planet, row.owner});
    // WGEP-10/73/74: a nested original binds at its live parent's location,
    // with the same owner. Static contents alone cannot admit a fiscal source.
    if (setup.census && setup.movement) {
        std::vector<EntityId> pending;
        for (const auto& [id, row] : live_sources) { (void)row; pending.push_back(id); }
        for (std::size_t i = 0; i < pending.size(); ++i) {
            const auto parent = pending[i];
            const auto contents = std::lower_bound(setup.movement->contents.begin(), setup.movement->contents.end(), parent,
                [](const auto& row, auto id) { return row.container < id; });
            if (contents == setup.movement->contents.end() || contents->container != parent) continue;
            const auto [type, planet, owner] = live_sources.at(parent); (void)type;
            for (const auto& child : contents->children) {
                const auto actor = std::lower_bound(setup.census->objects.begin(), setup.census->objects.end(), child.id,
                    [](const auto& row, auto id) { return row.id < id; });
                if (actor == setup.census->objects.end() || actor->id != child.id || actor->dead || actor->delete_pending
                    || actor->container != parent || actor->planet != planet || actor->owner != owner) continue;
                if (live_sources.emplace(actor->id, std::tuple{actor->original_type, planet, owner}).second)
                    pending.push_back(actor->id);
            }
        }
    }
    for (const auto& source : income.sources) {
        if (profile(income, source.type) && profile(income, source.type)->technology_price_reduction && !source.planet_source)
            return invalid("source " + std::to_string(source.id) + " type " + std::to_string(source.type)); // WGEP-167: a traveling holder has no planetary data to register this effect.
        if (!source.id || !profile(income, source.type) || !location(income, source.planet)
            || (source.planet_source ? source.id != source.planet
                : !live_sources.contains(source.id) || live_sources.at(source.id) != std::tuple{source.type, source.planet, source.owner})
            || (source.owner == 0 ? !source.planet_source : !std::any_of(setup.players.begin(), setup.players.end(), [&](const auto& row) { return row.player_id == source.owner; }))) return invalid("source " + std::to_string(source.id) + " type " + std::to_string(source.type));
    }
    if (income.offers.size() > galactic_max_objects || !ordered(income.offers,
        [](const auto& row) { return std::pair{row.planet, row.type}; })) return invalid("production offer table");
    for (const auto& row : income.offers) {
        if (!location(income, row.planet) || !row.type) return invalid("production offer table");
    }
    if (setup.production) for (const auto& planet : setup.production->planets) for (const auto& offer : planet.offers) {
        const auto at = std::lower_bound(income.offers.begin(), income.offers.end(), std::pair{planet.planet, offer.type},
            [](const auto& row, const auto& key) { return std::pair{row.planet, row.type} < key; });
        if (at == income.offers.end() || at->planet != planet.planet || at->type != offer.type) return invalid("production offer table");
    }
    return core::Result<void>::success();
}

core::Result<math::Fixed> planet_income_bonus(const IncomeSetup& income, PlanetId planet,
    PlayerId current_owner, math::Fixed basic) {
    math::Fixed total;
    const auto* where = location(income, planet);
    if (!where) return core::Result<math::Fixed>::success(total);
    for (const auto& source : income.sources) {
        if (!source.enabled || source.planet != planet || (!source.planet_source && source.owner != current_owner)) continue;
        const auto* type = profile(income, source.type);
        if (!type || !type->mining) continue;
        const auto& mine = *type->mining;
        auto bonus = mining_income_bonus(basic, mine.percentage, mine.absolute, where->mining_colony, mine.colony_multiplier);
        if (!bonus) return bonus;
        auto sum = math::add(total, bonus.value());
        if (!sum) return sum;
        total = sum.value();
    }
    return core::Result<math::Fixed>::success(total);
}

std::vector<ProductionReductionSource> production_reduction_sources(const IncomeSetup& income, std::span<const Planet> planets) {
    std::vector<ProductionReductionSource> out;
    for (const auto& source : income.sources) {
        if (!source.enabled) continue;
        const auto* where = location(income, source.planet);
        const auto* source_type = profile(income, source.type);
        if (!where || where->destroyed || !source_type || source_type->reductions.empty()) continue;
        const auto at = std::lower_bound(planets.begin(), planets.end(), source.planet,
            [](const auto& row, auto id) { return row.id < id; });
        if (at == planets.end() || at->id != source.planet) continue;
        // WGEP-54/74: local planet contributions survive capture; global ones rebind.
        if (!source.planet_source && source.owner != at->owner) continue;
        for (const auto& reduction : source_type->reductions)
            out.push_back({reduction.type, reduction.price, source.id, source.planet, at->owner, source.planet_source,
                where->intimidation, reduction.galaxy_wide, reduction.category, reduction.reduction});
    }
    // Stable: each (type, price) range keeps the income source and authored order.
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return std::pair{a.type, a.price} < std::pair{b.type, b.price}; });
    return out;
}

core::Result<math::Fixed> income_production_multiplier(std::span<const ProductionReductionSource> sources,
    PlanetId planet, PlayerId player, TypeId type, bool price) {
    const auto key = std::pair{type, price};
    const auto first = std::lower_bound(sources.begin(), sources.end(), key,
        [](const auto& row, const auto& wanted) { return std::pair{row.type, row.price} < wanted; });
    const auto last = std::upper_bound(first, sources.end(), key,
        [](const auto& wanted, const auto& row) { return wanted < std::pair{row.type, row.price}; });
    const auto applies = [&](const ProductionReductionSource& source) {
        if (source.planet_source && source.galaxy_wide) return !source.intimidation && source.planet_owner == player;
        return source.planet == planet;
    };
    // The same per-category maximum as production_reduction_multiplier, in an inline buffer kept
    // in ascending category order; more categories than it holds take the general path.
    std::array<std::pair<std::uint32_t, math::Fixed>, 8> categories{};
    std::size_t count{};
    for (auto source = first; source != last; ++source) {
        if (!applies(*source)) continue;
        auto at = std::lower_bound(categories.begin(), categories.begin() + static_cast<std::ptrdiff_t>(count), source->category,
            [](const auto& row, auto category) { return row.first < category; });
        if (at != categories.begin() + static_cast<std::ptrdiff_t>(count) && at->first == source->category) {
            if (at->second < source->reduction) at->second = source->reduction;
            continue;
        }
        if (count == categories.size()) {
            std::vector<ProductionReduction> rows;
            for (auto row = first; row != last; ++row)
                if (applies(*row)) rows.push_back({row->id, row->category, row->reduction});
            return production_reduction_multiplier(rows, {}, price);
        }
        std::move_backward(at, categories.begin() + static_cast<std::ptrdiff_t>(count), categories.begin() + static_cast<std::ptrdiff_t>(count) + 1);
        *at = {source->category, source->reduction};
        ++count;
    }
    return combine_reductions(std::span(categories).first(count), price);
}

core::Result<math::Fixed> income_production_multiplier(const IncomeSetup& income,
    std::span<const Planet> planets, PlanetId planet, PlayerId player, TypeId type, bool price) {
    return income_production_multiplier(production_reduction_sources(income, planets), planet, player, type, price);
}

void session_detail::project_native_income(IncomeSetup& inputs, const IncomeSetup& schema,
    const PoliticalControlSetup* control, std::span<const Planet> planets, std::span<const Fleet> fleets,
    std::span<const DetachedUnit> detached_units, const ProductionSetup* production,
    std::span<const CensusObject> actors, std::vector<NativeIncomeSourceCandidate>& income_source_candidates) {
    for (std::size_t i = 0; i < inputs.planets.size(); ++i)
        inputs.planets[i].destroyed = schema.planets[i].destroyed;
    if (control) for (std::size_t i = 0; i < inputs.planets.size(); ++i)
        inputs.planets[i].destroyed = inputs.planets[i].destroyed || control->planets[i].destroyed;
    income_source_candidates.clear(); // retain the gather buffer across misses
    const auto add = [&](EntityId id, TypeId type, PlanetId planet, PlayerId owner, bool fallback = false) {
        if (profile(inputs, type)) income_source_candidates.push_back({{id, type, planet, owner, false, true}, fallback});
    };
    // Immutable identity gather; arithmetic runs in partitioned staging below.
    for (const auto& fleet : fleets) for (const auto& member : fleet.members)
        add(member.id, member.type, fleet.motion.transit ? 0 : fleet.orbit, fleet.owner);
    for (const auto& row : detached_units) add(row.member.id, row.member.type, row.planet, row.owner);
    if (control) for (const auto& planet : control->planets) for (const auto& row : planet.landed)
        add(row.id, row.type, planet.planet, row.owner);
    if (production) for (const auto& planet : production->planets) for (const auto& row : planet.objects)
        add(row.id, row.type, planet.planet, row.owner, true);
    // WGEP-10/73/74: canonical actor writes recursively relocate carried heroes,
    // set their location to zero in transit, and remove them on losses. Reading
    // retained actors also covers detached/landed payloads without rewalking the
    // static contents graph or reviving a child after its destruction.
    for (const auto& actor : actors)
        if (actor.id != actor.planet && !actor.dead && !actor.delete_pending)
            add(actor.id, actor.original_type, actor.planet, actor.owner);
    // Match the former map's precedence exactly: last primary row, otherwise first
    // eligible production fallback. Stable sorting preserves the canonical gather order.
    std::stable_sort(income_source_candidates.begin(), income_source_candidates.end(),
        [](const auto& a, const auto& b) { return a.source.id < b.source.id; });
    inputs.sources.clear();
    for (std::size_t begin = 0; begin < income_source_candidates.size();) {
        auto end = begin + 1;
        while (end < income_source_candidates.size()
            && income_source_candidates[end].source.id == income_source_candidates[begin].source.id) ++end;
        auto selected = begin;
        for (auto i = begin; i < end; ++i)
            if (!income_source_candidates[i].fallback) selected = i;
        inputs.sources.push_back(income_source_candidates[selected].source);
        begin = end;
    }
    for (const auto& declared : schema.sources) {
        const auto at = std::lower_bound(inputs.sources.begin(), inputs.sources.end(), declared.id,
            [](const auto& row, auto id) { return row.id < id; });
        if (declared.planet_source) {
            auto source = declared;
            source.owner = std::lower_bound(planets.begin(), planets.end(), source.planet, [](const auto& row, auto id) { return row.id < id; })->owner;
            if (at != inputs.sources.end() && at->id == source.id) *at = source;
            else inputs.sources.insert(at, source);
        } else if (at != inputs.sources.end() && at->id == declared.id) at->enabled = at->enabled && declared.enabled;
    }
}

const IncomeSetup& GalacticSession::Impl::live_income_inputs() const {
    if (gathered_income_inputs && income_gather_generation == income_input_generation)
        return *gathered_income_inputs;
    if (!gathered_income_inputs) gathered_income_inputs = *setup.income;
    auto& inputs = *gathered_income_inputs;
    std::vector<DetachedUnit> detached;
    detached.reserve(detached_units.size());
    for (const auto& [id, row] : detached_units) { (void)id; detached.push_back(row); }
    session_detail::project_native_income(inputs, *setup.income, control ? &*control : nullptr,
        sorted_planets(), sorted_fleets(), detached, production ? &*production : nullptr, actor_inputs(), income_source_candidates);
    income_sources_gathered += income_source_candidates.size();
    income_gather_generation = income_input_generation;
    return inputs;
}

core::Result<void> GalacticSession::Impl::service_income(const PartitionExecutor& executor,
    std::uint64_t tick, std::vector<GalacticEvent>& events, bool sample) {
    if (!income) return core::Result<void>::success();
    auto staged = *income;
    staged.inputs = live_income_inputs();
    const auto owners = sorted_planets();
    std::vector<std::optional<GamblingIncomeState>> rows(staged.inputs.sources.size());
    std::vector<std::optional<GalacticEvent>> notices(rows.size());
    std::vector<std::optional<core::Diagnostic>> errors(rows.size());
    std::vector<std::uint32_t> draws(rows.size());
    constexpr std::uint32_t deadline_slot = 0xfffe000cU; // coordinator-reserved, GFC-09
    constexpr std::uint32_t sample_slot = 0xfffe000dU; // coordinator-reserved, WGEP-76
    const auto phase = executor.execute_phase("galactic-income-sources", tick_partition_count, [&](std::size_t partition) {
        for (std::size_t i = partition; i < rows.size(); i += tick_partition_count) {
            const auto& source = staged.inputs.sources[i]; const auto* type = profile(staged.inputs, source.type);
            if (!type || !type->gambling) continue;
            const auto& ability = *type->gambling;
            const auto* where = location(staged.inputs, source.planet);
            const auto owner = std::lower_bound(owners.begin(), owners.end(), source.planet,
                [](const auto& value, auto id) { return value.id < id; });
            const bool valid = source.enabled && where && owner != owners.end()
                && owner->id == source.planet; // WGEP-76 has no source-owner comparison
            GamblingIncomeState row{source.id, source.planet, std::numeric_limits<std::uint32_t>::max(), {}};
            const auto old = std::lower_bound(staged.gambling.begin(), staged.gambling.end(), row.source,
                [](const auto& value, auto id) { return value.source < id; });
            if (old != staged.gambling.end() && old->source == row.source) row = *old;
            // WGEP-77: an ownership callback removes a nonplanet source's old
            // contribution; the next due WGEP-76 service may install a new one.
            if (old != staged.gambling.end() && old->source == row.source && !source.planet_source
                && owner != owners.end() && owner->id == source.planet && row.planet == source.planet
                && row.planet_owner != owner->owner && (row.planet_owner == 0 || row.planet_owner == source.owner)) row.income = {};
            if (!valid || row.planet != source.planet) { row.income = {}; row.planet = source.planet; }
            row.planet_owner = owner != owners.end() && owner->id == source.planet ? owner->owner : 0;
            if (!source.enabled) {
                row.scheduled = false; row.deadline = std::numeric_limits<std::uint32_t>::max();
            }
            if (source.enabled && ability.interval_frames && !row.scheduled) {
                // GFC-09: inclusive first phase; activation itself does not sample.
                tactical::CombatRandom random(setup.seed, clock.galactic_frame, row.source, deadline_slot);
                row.deadline = clock.galactic_frame + random.uniform(0, ability.interval_frames);
                draws[i] += random.draws(); row.scheduled = true;
            }
            if (sample && row.scheduled && clock.galactic_frame >= row.deadline) {
                if (valid) {
                    tactical::CombatRandom random(setup.seed, clock.galactic_frame, row.source, sample_slot);
                    const auto win = random.uniform(0, 32767), factor = random.uniform(0, 32767);
                    draws[i] += random.draws();
                    const auto index = static_cast<std::size_t>(owner - owners.begin());
                    auto value = gambling_income_sample(planet_base_credits(index), ability, win, factor);
                    if (!value) { errors[i] = value.error(); continue; }
                    row.income = value.value();
                    notices[i] = GalacticEvent{tick, GalacticEventKind::gambling_income_sampled, {0}, row.source, 0,
                        static_cast<std::uint64_t>(row.income.raw()), GalacticRejectReason::none};
                }
                // GFC-09: missing target still resets once; no catch-up loop.
                row.deadline = clock.galactic_frame + ability.interval_frames;
            }
            rows[i] = row;
        }
    });
    if (!phase) return core::Result<void>::failure(phase.error());
    for (const auto& error : errors) if (error) return core::Result<void>::failure(*error);
    staged.gambling.clear();
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i]) staged.gambling.push_back(*rows[i]);
        staged.random_draws += draws[i];
    }
    if (staged != *income) { *income = std::move(staged); dirty[15] = true; }
    for (const auto& notice : notices) if (notice) events.push_back(*notice);
    return core::Result<void>::success();
}

core::Result<std::vector<math::Fixed>> session_detail::project_native_income_bonuses(
    const IncomeSetup* source_inputs, std::span<const GamblingIncomeState> gambling, std::span<const Planet> current,
    std::span<const math::Fixed> base_credits, const PartitionExecutor& executor) {
    std::vector<math::Fixed> totals(current.size());
    if (!source_inputs) return core::Result<std::vector<math::Fixed>>::success(std::move(totals));
    const auto& inputs = *source_inputs;
    std::vector<math::Fixed> bonuses(inputs.sources.size());
    std::vector<std::optional<core::Diagnostic>> errors(inputs.sources.size());
    const auto phase = executor.execute_phase("galactic-income-bonuses", tick_partition_count, [&](std::size_t partition) {
        for (std::size_t i = partition; i < inputs.sources.size(); i += tick_partition_count) {
            const auto& source = inputs.sources[i];
            if (!source.enabled || !source.planet) continue;
            const auto planet = std::lower_bound(current.begin(), current.end(), source.planet,
                [](const auto& row, auto id) { return row.id < id; });
            if (planet == current.end() || planet->id != source.planet || (!source.planet_source && planet->owner != source.owner)) continue;
            const auto* where = location(inputs, source.planet); const auto* type = profile(inputs, source.type);
            if (!where || !type || !type->mining) continue;
            const auto index = static_cast<std::size_t>(planet - current.begin());
            const auto& mine = *type->mining;
            auto value = mining_income_bonus(base_credits[index],
                mine.percentage, mine.absolute, where->mining_colony, mine.colony_multiplier);
            if (!value) errors[i] = value.error(); else bonuses[i] = value.value();
        }
    });
    if (!phase) return core::Result<std::vector<math::Fixed>>::failure(phase.error());
    for (std::size_t i = 0; i < bonuses.size(); ++i) {
        if (errors[i]) return core::Result<std::vector<math::Fixed>>::failure(*errors[i]);
        const auto planet = std::lower_bound(current.begin(), current.end(), inputs.sources[i].planet,
            [](const auto& row, auto id) { return row.id < id; });
        if (planet == current.end() || planet->id != inputs.sources[i].planet) continue;
        const auto index = static_cast<std::size_t>(planet - current.begin());
        auto sum = math::add(totals[index], bonuses[i]);
        if (!sum) return core::Result<std::vector<math::Fixed>>::failure(sum.error());
        totals[index] = sum.value();
    }
    for (const auto& row : gambling) if (row.planet && row.income.raw()) {
        const auto source = std::lower_bound(inputs.sources.begin(), inputs.sources.end(), row.source,
            [](const auto& value, auto id) { return value.id < id; });
        if (source == inputs.sources.end() || source->id != row.source || !source->enabled || source->planet != row.planet) continue;
        const auto planet = std::lower_bound(current.begin(), current.end(), row.planet,
            [](const auto& value, auto id) { return value.id < id; });
        if (planet == current.end() || planet->id != row.planet) continue;
        const auto* where = location(inputs, row.planet);
        if (!where) continue;
        const auto index = static_cast<std::size_t>(planet - current.begin());
        auto sum = math::add(totals[index], row.income);
        if (!sum) return core::Result<std::vector<math::Fixed>>::failure(sum.error());
        totals[index] = sum.value();
    }
    return core::Result<std::vector<math::Fixed>>::success(std::move(totals));
}

core::Result<std::vector<math::Fixed>> GalacticSession::Impl::income_bonuses(const PartitionExecutor& executor) const {
    if (!income) return core::Result<std::vector<math::Fixed>>::success(std::vector<math::Fixed>(setup.planets.size()));
    const auto& inputs = live_income_inputs();
    const auto current = sorted_planets();
    std::vector<math::Fixed> base_credits; base_credits.reserve(current.size());
    for (std::size_t i = 0; i < current.size(); ++i) base_credits.push_back(planet_base_credits(i));
    return session_detail::project_native_income_bonuses(&inputs, income->gambling, current, base_credits, executor);
}

core::Result<std::vector<PlanetIncomeContribution>> session_detail::project_native_income_contributions(
    const FiscalSetup& fiscal, std::span<const Planet> current, std::span<const math::Fixed> base_credits,
    const PoliticalControlSetup* control, const EconomyCorruptionState* economy_corruption, const IncomeState* income,
    std::span<const math::Fixed> bonuses, const NativeIncomeEdges& income_edges, const PartitionExecutor& executor) {
    const auto* rules = fiscal.contributions ? &*fiscal.contributions : nullptr;
    std::vector<PlanetIncomeContribution> out(current.size());
    std::vector<std::optional<core::Diagnostic>> errors(current.size());
    // WGEP-05: the same unexpired-blocker predicate serves actual collection and the pure forecast.
    std::vector<std::uint8_t> blocked(current.size());
    if (income) for (const auto& row : income->blockers) {
        const auto at = std::lower_bound(current.begin(), current.end(), row.planet,
            [](const auto& p, auto id) { return p.id < id; });
        if (at != current.end() && at->id == row.planet) blocked[static_cast<std::size_t>(at - current.begin())] = 1;
    }
    const auto phase = executor.execute_phase("galactic-planet-income", tick_partition_count, [&](std::size_t partition) {
        for (std::size_t i = partition; i < current.size(); i += tick_partition_count) {
            // WGEP-05/75: zero before any source refresh or siphon subtraction; the
            // retained political/source state is not touched (collection skips `blocked`).
            if (blocked[i]) { out[i].blocked = true; continue; }
            math::Fixed like, opposing, route_factor, political_value;
            bool replace_political = false;
            if (rules && control) {
                const auto owner = std::lower_bound(control->players.begin(), control->players.end(), current[i].owner,
                    [](const auto& row, auto id) { return row.player < id; });
                if (owner != control->players.end() && owner->player == current[i].owner && !owner->neutral) {
                    auto curve = evaluate_income_curve(rules->political_curve, control->state[i].current);
                    if (!curve) { errors[i] = curve.error(); continue; }
                    political_value = curve.value(); replace_political = political_value.raw() > 0;
                }
            }
            if (rules) {
                const auto faction = std::lower_bound(rules->factions.begin(), rules->factions.end(), current[i].owner,
                    [](const auto& f, auto id) { return f.player < id; });
                // WGEP-75: nonneutral faction vs neutral planet is opposing too.
                if (faction != rules->factions.end() && faction->player == current[i].owner
                    && faction->alignment != IncomeAlignment::neutral) {
                    if (faction->alignment == rules->planets[i].alignment) like = rules->planets[i].strength;
                    else opposing = rules->planets[i].strength;
                }
                for (const auto& [edge, other_index] : std::as_const(income_edges)[i]) {
                    const auto* other = &current[other_index];
                    bool allied = current[i].owner != 0 && current[i].owner == other->owner;
                    if (!allied && control) {
                        const auto relation = std::lower_bound(control->relations.begin(), control->relations.end(),
                            std::pair{current[i].owner, other->owner}, [](const auto& r, const auto& key) { return std::pair{r.player, r.other} < key; });
                        allied = relation != control->relations.end() && relation->player == current[i].owner
                            && relation->other == other->owner && relation->allied;
                    }
                    if (!allied || rules->routes[edge].factor.raw() <= 0) continue;
                    auto sum = math::add(route_factor, rules->routes[edge].factor);
                    if (!sum) { errors[i] = sum.error(); break; }
                    route_factor = sum.value();
                }
            }
            if (errors[i]) continue;
            // WGEP-06/14: last post-payment roll affects this cycle.
            auto value = planet_income_contribution(base_credits[i], // WGCH-143: ruined planets read their authored destroyed value
                economy_corruption ? economy_corruption->planets[i].factor : math::Fixed::from_raw(math::Fixed::scale), bonuses[i], like, opposing,
                rules ? rules->maximum_alignment_bonus : math::Fixed{},
                rules ? rules->maximum_alignment_penalty : math::Fixed{}, political_value, route_factor);
            if (!value) { errors[i] = value.error(); continue; }
            // WGEP-08/75: zero calculated contribution removes the source;
            // neutral/nonpositive curve returns retain it. Preview never writes it.
            if (rules && control && !replace_political) {
                value.value().political = control->state[i].political_income;
                auto total = math::add(value.value().total, value.value().political);
                if (!total) { errors[i] = total.error(); continue; }
                value.value().total = total.value();
            }
            out[i] = value.value();
        }
    });
    if (!phase) return core::Result<std::vector<PlanetIncomeContribution>>::failure(phase.error());
    for (const auto& error : errors) if (error) return core::Result<std::vector<PlanetIncomeContribution>>::failure(*error);
    return core::Result<std::vector<PlanetIncomeContribution>>::success(std::move(out));
}

core::Result<std::vector<PlanetIncomeContribution>> GalacticSession::Impl::income_contributions(const PartitionExecutor& executor) const {
    auto bonuses = income_bonuses(executor);
    if (!bonuses) return core::Result<std::vector<PlanetIncomeContribution>>::failure(bonuses.error());
    const auto current = sorted_planets();
    const auto& fiscal = *setup.fiscal;
    const auto* rules = fiscal.contributions ? &*fiscal.contributions : nullptr;
    // The route graph is immutable: index endpoints once, not on every fiscal query.
    if (rules && income_edges.size() != current.size()) {
        income_edges.resize(current.size());
        for (std::size_t i = 0; i < setup.routes.size(); ++i) {
            const auto from = std::lower_bound(current.begin(), current.end(), setup.routes[i].from,
                [](const auto& p, auto id) { return p.id < id; });
            const auto to = std::lower_bound(current.begin(), current.end(), setup.routes[i].to,
                [](const auto& p, auto id) { return p.id < id; });
            const auto a = static_cast<std::size_t>(from - current.begin());
            const auto b = static_cast<std::size_t>(to - current.begin());
            income_edges[a].push_back({i, b}); income_edges[b].push_back({i, a});
        }
    }
    std::vector<math::Fixed> base_credits; base_credits.reserve(current.size());
    for (std::size_t i = 0; i < current.size(); ++i) base_credits.push_back(planet_base_credits(i));
    return session_detail::project_native_income_contributions(fiscal, current, base_credits,
        control ? &*control : nullptr, economy_corruption ? &*economy_corruption : nullptr,
        income ? &*income : nullptr, bonuses.value(), income_edges, executor);
}

core::Result<std::span<const PlanetIncomeContribution>> GalacticSession::Impl::forecast_contributions(
    const PartitionExecutor& executor, const std::vector<Planet>& current) const {
    // Value keys cover every live consumer input; no clock/account/observer dependency.
    // The cache is derived, omitted from hashes/checkpoints and rebuilt after load.
    if (income) (void)live_income_inputs();
    const auto input_generation = income ? income_gather_generation : 0;
    const std::vector<ControlRelation> empty_relations;
    const std::vector<GamblingIncomeState> empty_gambling;
    const auto& relations = control ? control->relations : empty_relations;
    const auto& gambling = income ? income->gambling : empty_gambling;
    if (!income_forecast_cache || income_forecast_cache->planets != current
        || income_forecast_cache->relations != relations || income_forecast_cache->input_generation != input_generation
        || income_forecast_cache->control_generation != income_control_generation
        || income_forecast_cache->gambling != gambling) {
        auto values = income_contributions(executor);
        if (!values) return core::Result<std::span<const PlanetIncomeContribution>>::failure(values.error());
        income_forecast_cache = IncomeForecastCache{current, relations, input_generation, income_control_generation, gambling, std::move(values.value())};
        ++income_forecast_rebuilds;
    }
    return core::Result<std::span<const PlanetIncomeContribution>>::success(income_forecast_cache->values);
}

// WGEP-05/163..165: source-scoped Boolean income blockers. The ledger holds only
// unexpired rows (ordered expiry commits at the GC frame advance), so the OR of
// "unexpired modifiers" is "the planet has a row". Nothing else writes a row.
bool GalacticSession::Impl::income_blocked(PlanetId planet) const {
    if (!income) return false;
    const auto& rows = income->blockers;
    const auto at = std::lower_bound(rows.begin(), rows.end(), planet,
        [](const auto& row, auto id) { return row.planet < id; });
    return at != rows.end() && at->planet == planet;
}

void GalacticSession::Impl::apply_income_blocker(PlanetId planet, EntityId source,
    const std::optional<IncomeBlocker>& row) {
    auto& rows = income->blockers;
    auto at = std::lower_bound(rows.begin(), rows.end(), std::pair{planet, source},
        [](const auto& value, const auto& key) { return std::pair{value.planet, value.source} < key; });
    const bool present = at != rows.end() && at->planet == planet && at->source == source;
    if (row) { if (present) *at = *row; else rows.insert(at, *row); }
    else if (present) rows.erase(at);
    income_blocker_next_expiry = std::numeric_limits<std::uint64_t>::max();
    for (const auto& value : rows) income_blocker_next_expiry = std::min(income_blocker_next_expiry, value.first_expired_frame);
    dirty[static_cast<std::size_t>(StateSection::income)] = true;
    // Derived gathers, modifier breakdowns and the forecast cache all key on this generation.
    invalidate_income_inputs();
}

void GalacticSession::Impl::expire_income_blockers() {
    if (!income || income->blockers.empty() || clock.galactic_frame < income_blocker_next_expiry) return;
    auto& rows = income->blockers;
    rows.erase(std::remove_if(rows.begin(), rows.end(),
        [&](const auto& row) { return row.first_expired_frame <= clock.galactic_frame; }), rows.end());
    income_blocker_next_expiry = std::numeric_limits<std::uint64_t>::max();
    for (const auto& value : rows) income_blocker_next_expiry = std::min(income_blocker_next_expiry, value.first_expired_frame);
    dirty[static_cast<std::size_t>(StateSection::income)] = true;
    invalidate_income_inputs();
}

namespace {
core::Result<void> blocker_failure(std::string_view message) {
    return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_command,
        std::string("income blocker: ") + std::string(message)));
}
constexpr std::uint8_t blocker_set = 1, blocker_remove = 2;
// Operation, planet, source, then the row being written (set) or removed (remove).
std::vector<std::uint8_t> blocker_journal(std::uint8_t operation, const IncomeBlocker& row) {
    std::vector<std::uint8_t> bytes;
    detail::put_u8(bytes, operation); detail::put_u64(bytes, row.planet); detail::put_u64(bytes, row.source);
    detail::put_u64(bytes, row.first_frame); detail::put_u64(bytes, row.first_expired_frame);
    return bytes;
}
} // namespace

core::Result<GalacticSession::IncomeBlockerReceipt> GalacticSession::halt_planet_credits(
    EntityId actor, PlanetId planet) {
    using Receipt = core::Result<IncomeBlockerReceipt>;
    auto& impl = *impl_;
    const auto refuse = [](std::string_view message) {
        return Receipt::failure(detail::diagnostic(diagnostic_codes::invalid_command,
            std::string("income blocker: ") + std::string(message)));
    };
    if (impl.failed || !impl.income) return refuse("requires a live income section");
    if (!actor || !planet || !impl.planets.contains(planet)) return refuse("unknown actor or planet");
    // The actor is a live object of the same ordered gather the other income abilities use
    // (fleet members, detached units, landed forces, production objects); it acts on the
    // planet it is located on. Admission, price and actor consumption belong to the host.
    const auto& inputs = impl.live_income_inputs();
    const auto source = std::lower_bound(inputs.sources.begin(), inputs.sources.end(), actor,
        [](const auto& row, auto id) { return row.id < id; });
    if (source == inputs.sources.end() || source->id != actor || source->planet_source)
        return refuse("the sabotage actor is not a live object");
    if (source->planet != planet) return refuse("the sabotage actor is not on the target planet");
    // The authored profile is data: any registered type with a halt ability may write.
    const auto* type = profile(inputs, source->type);
    if (!type || !type->credit_halt) return refuse("actor type has no authored credit-halt ability");
    auto frames = halt_frames(type->credit_halt->seconds);
    if (!frames) return Receipt::failure(frames.error());
    // A zero-length authored halt is never true for any frame: nothing registers.
    if (!frames.value()) return Receipt::success({false, 0});
    const auto first = impl.clock.galactic_frame;
    if (frames.value() > galactic_max_ticks || first > galactic_max_ticks - frames.value())
        return refuse("authored halt exceeds the galactic tick bound");
    const auto& rows = impl.income->blockers;
    const auto live = std::lower_bound(rows.begin(), rows.end(), std::pair{planet, actor},
        [](const auto& value, const auto& key) { return std::pair{value.planet, value.source} < key; });
    if (live != rows.end() && live->planet == planet && live->source == actor)
        return Receipt::success({false, live->first_expired_frame}); // never duplicate or extend
    const IncomeBlocker row{planet, actor, first, first + frames.value()};
    if (auto recorded = append_native_effect(NativeEffectKind::income_block, blocker_journal(blocker_set, row)); !recorded)
        return Receipt::failure(recorded.error());
    impl.apply_income_blocker(planet, actor, row);
    const auto events = impl.snapshot->events();
    impl.snapshot = impl.make_snapshot({events.begin(), events.end()});
    return Receipt::success({true, row.first_expired_frame});
}

core::Result<bool> GalacticSession::remove_income_blocker(PlanetId planet, EntityId source) {
    auto& impl = *impl_;
    if (impl.failed || !impl.income || !planet || !source)
        return core::Result<bool>::failure(detail::diagnostic(diagnostic_codes::invalid_command,
            "income blocker: removal requires a live income section and identities"));
    const auto& rows = impl.income->blockers;
    const auto live = std::lower_bound(rows.begin(), rows.end(), std::pair{planet, source},
        [](const auto& value, const auto& key) { return std::pair{value.planet, value.source} < key; });
    if (live == rows.end() || live->planet != planet || live->source != source)
        return core::Result<bool>::success(false); // no duplicate journal for an absent source
    const IncomeBlocker removed = *live;
    if (auto recorded = append_native_effect(NativeEffectKind::income_block, blocker_journal(blocker_remove, removed)); !recorded)
        return core::Result<bool>::failure(recorded.error());
    impl.apply_income_blocker(planet, source, std::nullopt);
    const auto events = impl.snapshot->events();
    impl.snapshot = impl.make_snapshot({events.begin(), events.end()});
    return core::Result<bool>::success(true);
}

core::Result<void> GalacticSession::Impl::replay_income_blocker(GalacticSession&, const NativeEffect& effect) {
    sim::detail::Reader in(effect.payload);
    std::uint8_t operation{}; IncomeBlocker row;
    if (!in.read_u8(operation) || (operation != blocker_set && operation != blocker_remove)
        || !in.read_u64(row.planet) || !in.read_u64(row.source) || !in.read_u64(row.first_frame)
        || !in.read_u64(row.first_expired_frame) || in.remaining() || !income || !row.planet || !row.source
        || row.first_expired_frame <= row.first_frame || row.first_expired_frame > galactic_max_ticks
        || !planets.contains(row.planet))
        return blocker_failure("invalid journal");
    const auto& rows = income->blockers;
    const auto at = std::lower_bound(rows.begin(), rows.end(), std::pair{row.planet, row.source},
        [](const auto& value, const auto& key) { return std::pair{value.planet, value.source} < key; });
    const bool present = at != rows.end() && at->planet == row.planet && at->source == row.source;
    // Reconstruction never duplicates or extends: the recorded absolute frames are
    // applied as written, a set needs an absent source that has not yet expired, and a
    // removal must name the exact retained row.
    if (operation == blocker_set) {
        if (present || row.first_frame > clock.galactic_frame || row.first_expired_frame <= clock.galactic_frame)
            return blocker_failure("reconstruction differs from the retained ledger");
        apply_income_blocker(row.planet, row.source, row);
    } else {
        if (!present || *at != row) return blocker_failure("reconstruction differs from the retained ledger");
        apply_income_blocker(row.planet, row.source, std::nullopt);
    }
    const auto events = snapshot->events();
    snapshot = make_snapshot({events.begin(), events.end()});
    return core::Result<void>::success();
}

void GalacticSession::Impl::publish_income_modifiers(EconomyForecast& value,
    const std::vector<Planet>& current_planets) const {
    if (!income) return;
    const auto& inputs = live_income_inputs();
    if (income_modifier_generation != income_gather_generation) {
        income_modifiers.resize(value.planets.size());
        for (auto& row : income_modifiers) {
            if (!row) row.emplace(); else row->clear();
        }
        for (const auto& source : inputs.sources) {
            if (!source.enabled) continue;
            const auto at = std::lower_bound(value.planets.begin(), value.planets.end(), source.planet,
                [](const auto& row, auto id) { return row.planet < id; });
            const auto owner = std::lower_bound(current_planets.begin(), current_planets.end(), source.planet,
                [](const auto& row, auto id) { return row.id < id; });
            const auto* type = profile(inputs, source.type);
            const auto* where = location(inputs, source.planet);
            // WGEP-05: an income-blocked planet's actual income is zero, so no source
            // modifier is part of its published breakdown.
            if (at == value.planets.end() || at->planet != source.planet || income_blocked(source.planet)
                || owner == current_planets.end() || owner->id != source.planet
                || (!source.planet_source && source.owner != owner->owner)
                || !type || !type->mining || !where) continue;
            const auto& mine = *type->mining;
            auto amount = mining_income_bonus(at->basic, mine.percentage, mine.absolute,
                where->mining_colony, mine.colony_multiplier);
            auto& modifiers = income_modifiers[static_cast<std::size_t>(at - value.planets.begin())];
            if (amount && modifiers) modifiers->push_back({source.type, amount.value()});
            else modifiers.reset();
        }
        income_modifier_generation = income_gather_generation;
        ++income_modifier_rebuilds;
    }
    // Snapshot ownership requires a copy; the retained gather and breakdown stay reusable.
    for (std::size_t i = 0; i < value.planets.size(); ++i) value.planets[i].modifiers = income_modifiers[i];
}

} // namespace eawr::sim::galactic
