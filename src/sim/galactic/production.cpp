#include "eawr/sim/galactic/production.hpp"
#include "eawr/sim/galactic/income.hpp"
#include "session_impl.hpp"
#include "encoding.hpp"
#include "native_population_projection.hpp"
#include "native_production_admission.hpp"

#include <algorithm>
#include <cassert>
#include <limits>
#include <map>
#include <set>
#include <string>

namespace eawr::sim::galactic {
namespace {
using math::Fixed;
constexpr auto one = Fixed::from_raw(Fixed::scale);
template <class Rows, class Key> bool ordered(const Rows& rows, Key key) {
    return std::adjacent_find(rows.begin(), rows.end(), [&](const auto& a, const auto& b) { return key(a) >= key(b); }) == rows.end();
}
const ProductionOffer* offer(const PlanetProduction& planet, TypeId type) {
    const auto found = std::lower_bound(planet.offers.begin(), planet.offers.end(), type,
        [](const auto& row, auto id) { return row.type < id; });
    return found != planet.offers.end() && found->type == type ? &*found : nullptr;
}
Fixed remaining(const ProductionEntry& entry, std::uint64_t frame) {
    const auto elapsed = frame > entry.start_frame ? frame - entry.start_frame : 0;
    return Fixed::from_ratio(static_cast<std::int64_t>(elapsed >= entry.duration_frames ? 0 : entry.duration_frames - elapsed), galactic_frames_per_second).value();
}
void start(ProductionEntry& entry, std::uint64_t frame) { session_detail::native_production_start(entry, frame); }
Fixed paid(const ProductionEntry& entry) { return session_detail::native_production_paid(entry); }
bool prerequisites(const PlanetProduction& planet, const ProductionOffer& type, std::span<const Fleet> fleets,
    bool servicing = true) {
    // LSA-09: required levels retain signed parser bits but compare unsigned.
    if (servicing && (type.domain == ProductionDomain::ground ? static_cast<std::uint32_t>(type.required_ground_level) > planet.ground_level
            : static_cast<std::uint32_t>(type.required_space_level) > planet.space_level)) return false;
    for (const auto& group : type.required_structures) {
        if (std::none_of(group.begin(), group.end(), [&](auto wanted) {
            return std::any_of(planet.objects.begin(), planet.objects.end(), [&](const auto& object) { return object.type == wanted && object.kind == ProductionKind::structure; });
        })) return false;
    }
    for (auto wanted : type.required_orbiting_units) {
        // WGEP-21: exact type, no additional owner/ally check.
        if (std::none_of(fleets.begin(), fleets.end(), [&](const auto& fleet) {
            return fleet.orbit == planet.planet && std::any_of(fleet.members.begin(), fleet.members.end(),
                [&](const auto& member) { return member.type == wanted; });
        })) return false;
    }
    return true;
}
bool starbase_blocked(const ProductionSetup& production, const PlanetProduction& planet,
    PlayerId builder, std::span<const Fleet> fleets) {
    if (!production.fleet_blockers) return planet.starbase_blocked;
    // WGEP-24/38: different owner, even an ally; WGM-27 excludes in-transit fleets.
    return std::any_of(production.fleet_blockers->begin(), production.fleet_blockers->end(), [&](const auto& row) {
        if (row.pending_deletion || row.skips_space_combat || !row.cancels_starbase_production) return false;
        const auto fleet = std::lower_bound(fleets.begin(), fleets.end(), row.fleet,
            [](const auto& value, auto id) { return value.id < id; });
        return fleet != fleets.end() && fleet->id == row.fleet && !fleet->motion.transit
            && fleet->orbit == planet.planet && fleet->owner != builder;
    });
}
core::Result<void> invalid(std::string message) {
    return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "production: " + std::move(message)));
}
} // namespace

core::Result<std::size_t> assign_technology(GalacticPlayerEconomy& account,
    ProductionPlayer& player, std::span<const TechnologyType> registry,
    std::int64_t target, bool process_unlocks, const TechnologyAssigned& notify) {
    using Result = core::Result<std::size_t>;
    if (account.player != player.player || account.maximum_tech < 0)
        return Result::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "unbound technology account"));
    account.current_tech = std::clamp(target, std::int64_t{0}, account.maximum_tech);
    if (notify) {
        auto notified = notify(account.player, account.current_tech);
        if (!notified) return Result::failure(notified.error());
    }
    std::size_t added{};
    if (process_unlocks) for (const auto& type : registry) {
        if (!type.initially_locked || !type.slicer_unlockable || type.tech >= account.current_tech
            || !std::binary_search(type.affiliated_players.begin(), type.affiliated_players.end(), player.player)) continue;
        const auto at = std::lower_bound(player.unlocked.begin(), player.unlocked.end(), type.type);
        // Explicit locks remain independent and override subsequent admission.
        if (at == player.unlocked.end() || *at != type.type) {
            player.unlocked.insert(at, type.type); ++added;
        }
    }
    return Result::success(added);
}

core::Result<std::int64_t> GalacticSession::Impl::assign_technology(const TechnologyWrite& write) {
    using Result = core::Result<std::int64_t>;
    if (failed || !economy || !production)
        return Result::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "technology writer requires economy and production owners"));
    auto account = std::find_if(economy->players.begin(), economy->players.end(),
        [&](const auto& row) { return row.player == write.player; });
    auto player = std::find_if(production->players.begin(), production->players.end(),
        [&](const auto& row) { return row.player == write.player; });
    if (account == economy->players.end() || player == production->players.end())
        return Result::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "unbound technology writer"));
    dirty[static_cast<std::size_t>(StateSection::players)] = true;
    dirty[static_cast<std::size_t>(StateSection::production)] = true;
    // WST-41: SET_MAX_TECH_LEVEL moves the maximum and nothing else; SET_TECH_LEVEL raises it first when needed.
    if (write.maximum == TechnologyMaximum::set_only) {
        account->maximum_tech = write.target;
        return Result::success(account->current_tech);
    }
    if (write.maximum == TechnologyMaximum::raise_to_target && write.target > account->maximum_tech)
        account->maximum_tech = write.target;
    auto assigned = galactic::assign_technology(*account, *player, production->technology_types,
        write.target, write.process_unlocks, [&](PlayerId id, std::int64_t tech) {
            return technology_notice_handler ? technology_notice_handler({TechnologyNotice::Kind::assigned, id, tech})
                : core::Result<void>::success();
        });
    if (!assigned) { failed = true; return Result::failure(assigned.error()); }
    if (assigned.value() && write.request_sound && observer && std::binary_search(observer->human_observers.begin(), observer->human_observers.end(), write.player) && technology_notice_handler) {
        auto notified = technology_notice_handler({TechnologyNotice::Kind::new_options, write.player, account->current_tech});
        if (!notified) { failed = true; return Result::failure(notified.error()); }
    }
    return Result::success(account->current_tech);
}
void GalacticSession::set_technology_notice_handler(TechnologyNoticeHandler handler) {
    impl_->technology_notice_handler = std::move(handler);
}
core::Result<std::int64_t> GalacticSession::set_technology(const TechnologyWrite& write) {
    using Result = core::Result<std::int64_t>;
    auto& impl = *impl_;
    if (impl.failed || !impl.economy || !impl.production)
        return Result::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "technology writer requires live owners"));
    const auto account = std::find_if(impl.economy->players.begin(), impl.economy->players.end(),
        [&](const auto& row) { return row.player == write.player; });
    if (account == impl.economy->players.end())
        return Result::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "unbound technology writer"));
    if (write.maximum == TechnologyMaximum::set_only && write.target < account->current_tech)
        return Result::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "a maximum technology level below the current level is refused"));
    std::vector<std::uint8_t> bytes;
    detail::put_u32(bytes, write.player); detail::put_i64(bytes, write.target);
    detail::put_u8(bytes, write.process_unlocks ? 1 : 0); detail::put_u8(bytes, write.request_sound ? 1 : 0);
    detail::put_i64(bytes, account->current_tech); detail::put_i64(bytes, account->maximum_tech);
    // Only story writes carry a tail, so every other recording keeps its bytes.
    if (write.maximum != TechnologyMaximum::keep) detail::put_u8(bytes, static_cast<std::uint8_t>(write.maximum));
    auto recorded = append_native_effect(NativeEffectKind::technology, std::move(bytes));
    if (!recorded) return Result::failure(recorded.error());
    auto result = impl.assign_technology(write);
    if (!result) return result;
    const auto events = impl.snapshot->events();
    impl.snapshot = impl.make_snapshot({events.begin(), events.end()});
    return result;
}
core::Result<GalacticSession::ProductionLockReceipt> GalacticSession::apply_production_lock(const ProductionLockWrite& write) {
    using Result = core::Result<ProductionLockReceipt>;
    auto& impl = *impl_;
    if (impl.failed || !impl.economy || !impl.production)
        return Result::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "production lock writer requires live owners"));
    auto account = std::find_if(impl.economy->players.begin(), impl.economy->players.end(),
        [&](const auto& row) { return row.player == write.player; });
    auto player = std::find_if(impl.production->players.begin(), impl.production->players.end(),
        [&](const auto& row) { return row.player == write.player; });
    if (account == impl.economy->players.end() || player == impl.production->players.end())
        return Result::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "unbound production lock writer"));
    const auto& registry = impl.production->technology_types;
    const auto registered = [&](TypeId type) {
        if (!type) return true;
        const auto at = std::lower_bound(registry.begin(), registry.end(), type, [](const auto& row, TypeId id) { return row.type < id; });
        return at != registry.end() && at->type == type;
    };
    if (!registered(write.unlock) || !registered(write.lock))
        return Result::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "production lock names an unregistered type"));
    std::vector<std::uint8_t> bytes;
    detail::put_u32(bytes, write.player); detail::put_u64(bytes, write.unlock); detail::put_u64(bytes, write.lock);
    detail::put_u8(bytes, write.slicer_check ? 1 : 0); detail::put_i64(bytes, account->current_tech);
    auto recorded = append_native_effect(NativeEffectKind::production_lock, std::move(bytes));
    if (!recorded) return Result::failure(recorded.error());
    ProductionLockReceipt receipt;
    // WST-64: remove the build lock and list the unlock, then list the lock. Both lists stay ascending.
    if (write.unlock) {
        const auto at = std::lower_bound(player->locked.begin(), player->locked.end(), write.unlock);
        if (at != player->locked.end() && *at == write.unlock) player->locked.erase(at);
        const auto listed = std::lower_bound(player->unlocked.begin(), player->unlocked.end(), write.unlock);
        if (listed == player->unlocked.end() || *listed != write.unlock) player->unlocked.insert(listed, write.unlock);
    }
    if (write.lock) {
        const auto at = std::lower_bound(player->locked.begin(), player->locked.end(), write.lock);
        if (at == player->locked.end() || *at != write.lock) player->locked.insert(at, write.lock);
    }
    impl.dirty[static_cast<std::size_t>(StateSection::production)] = true;
    if (write.slicer_check) {
        const bool remains = std::any_of(registry.begin(), registry.end(), [&](const auto& type) {
            return type.initially_locked && type.slicer_unlockable && type.tech <= account->current_tech
                && std::binary_search(type.affiliated_players.begin(), type.affiliated_players.end(), write.player)
                && !std::binary_search(player->unlocked.begin(), player->unlocked.end(), type.type);
        });
        // WGEP-148: at or above the maximum nothing is written.
        if (!remains && account->current_tech < account->maximum_tech) {
            auto raised = impl.assign_technology({write.player, account->current_tech + 1, true, true});
            if (!raised) { impl.failed = true; return Result::failure(raised.error()); }
            receipt.tech_incremented = true;
        }
    }
    const auto events = impl.snapshot->events();
    impl.snapshot = impl.make_snapshot({events.begin(), events.end()});
    return Result::success(receipt);
}
core::Result<std::int64_t> GalacticSession::increment_technology(PlayerId id) {
    using Result = core::Result<std::int64_t>;
    if (impl_->failed || !impl_->economy)
        return Result::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "increment requires a live technology account"));
    const auto account = std::find_if(impl_->economy->players.begin(), impl_->economy->players.end(),
        [&](const auto& row) { return row.player == id; });
    if (account == impl_->economy->players.end())
        return Result::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "unbound technology increment"));
    // WGEP-148: no journal, notification or unlock traversal at/above maximum.
    if (account->current_tech >= account->maximum_tech) return Result::success(account->current_tech);
    return set_technology({id, account->current_tech + 1, true, true});
}

core::Result<std::uint64_t> production_frames(Fixed seconds) {
    // Split before multiplication: the full unsigned source-second range fits a frame count.
    if (seconds.raw() < 0)
        return core::Result<std::uint64_t>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "production duration out of range"));
    return core::Result<std::uint64_t>::success(static_cast<std::uint64_t>(seconds.raw() / Fixed::scale) * galactic_frames_per_second
        + static_cast<std::uint64_t>(seconds.raw() % Fixed::scale) * galactic_frames_per_second / Fixed::scale);
}

math::Fixed production_product(Fixed left, Fixed right) {
    Fixed result;
    if (math::try_multiply(left, right, result)) return result;
    return Fixed::from_raw((left.raw() < 0) != (right.raw() < 0)
        ? -std::numeric_limits<std::int64_t>::max() : std::numeric_limits<std::int64_t>::max());
}

math::Fixed production_price(Fixed price) {
    if (price.raw() > 0 && price.raw() < Fixed::scale) return one;
    const auto whole = price.raw() / Fixed::scale;
    const auto remainder = price.raw() % Fixed::scale;
    const auto rounded = whole + (remainder >= Fixed::scale / 2 ? 1 : remainder <= -Fixed::scale / 2 ? -1 : 0);
    // A rounded endpoint can exceed the fixed range by less than one credit.
    return Fixed::from_raw(std::clamp(rounded,
        std::numeric_limits<std::int64_t>::min() / Fixed::scale,
        std::numeric_limits<std::int64_t>::max() / Fixed::scale) * Fixed::scale);
}

namespace {
// `multiplier(price)` is the WGEP-54/74 reduction multiplier; called only with income sources.
template<class Multiplier>
core::Result<ProductionOffer> prepare_offer(const ProductionOffer& original, const IncomeSetup* sources,
    const Multiplier& multiplier, PlanetId planet, PlayerId player, const EconomyCorruptionState* corruption) {
    auto adjusted = original;
    Fixed corruption_price = one, corruption_time = one, technology_price = one;
    if (corruption) {
        const auto p = std::lower_bound(corruption->players.begin(), corruption->players.end(), player,
            [](const auto& row, auto id) { return row.player < id; });
        const auto where = std::lower_bound(corruption->planets.begin(), corruption->planets.end(), planet,
            [](const auto& row, auto id) { return row.planet < id; });
        if (p == corruption->players.end() || p->player != player || where == corruption->planets.end() || where->planet != planet)
            return core::Result<ProductionOffer>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "missing corruption production facts"));
        const auto active = where->corruption != corruption_fixture::Type::none;
        if (original.kind == ProductionKind::technology) technology_price = p->tech_purchase_multiplier;
        corruption_price = corruption_fixture::production_factor(p->benefits, active, p->price[0], p->price[1]);
        corruption_time = corruption_fixture::production_factor(p->benefits, active, p->time[0], p->time[1]);
        // Stock basis providers already include their uncorrupted factor. A zero
        // basis cannot be inverted: retain the named raw-production-basis seam.
        const auto price_basis = p->benefits ? one : p->price[0];
        const auto time_basis = p->benefits ? one : p->time[0];
        auto price = corruption_price == price_basis ? core::Result<Fixed>::success(one) : math::divide(corruption_price, price_basis);
        auto time = corruption_time == time_basis ? core::Result<Fixed>::success(one) : math::divide(corruption_time, time_basis);
        if (!price || !time) return core::Result<ProductionOffer>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "raw-production-basis seam: zero uncorrupted factor"));
        corruption_price = price.value(); corruption_time = corruption->tutorial ? one : time.value();
    }
    if (sources) {
        const auto basis = std::lower_bound(sources->offers.begin(), sources->offers.end(), std::pair{planet, original.type},
            [](const auto& row, const auto& key) { return std::pair{row.planet, row.type} < key; });
        // Preserve WGEP-29/30 ordering: reductions precede the single rounding step.
        if (basis == sources->offers.end() || basis->planet != planet || basis->type != original.type)
            return core::Result<ProductionOffer>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "production offer adjustment failed"));
        auto price = multiplier(true);
        auto time = multiplier(false);
        if (!price || !time) return core::Result<ProductionOffer>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "production offer adjustment failed"));
        auto cost = production_product(basis->price, price.value());
        auto seconds = production_product(basis->seconds, time.value());
        cost = production_product(cost, corruption_price);
        cost = production_product(cost, technology_price); // WGEP-29/30: before the one rounding step
        seconds = production_product(seconds, corruption_time);
        // Negative time is retained by the reducer; queue admission diagnoses it.
        adjusted.price = production_price(cost);
        adjusted.seconds = seconds;
    } else if (corruption) {
        auto cost = production_product(original.price, corruption_price);
        cost = production_product(cost, technology_price);
        auto seconds = production_product(original.seconds, corruption_time);
        adjusted.price = production_price(cost);
        adjusted.seconds = seconds;
    }
    return core::Result<ProductionOffer>::success(std::move(adjusted));
}
} // namespace

core::Result<ProductionOffer> prepared_production_offer(const ProductionOffer& original,
    const IncomeSetup* sources, std::span<const Planet> owners, PlanetId planet, PlayerId player,
    const EconomyCorruptionState* corruption) {
    return prepare_offer(original, sources, [&](bool price) {
        return income_production_multiplier(*sources, owners, planet, player, original.type, price);
    }, planet, player, corruption);
}

core::Result<ProductionOffer> prepared_production_offer(const ProductionOffer& original,
    const IncomeSetup* sources, std::span<const ProductionReductionSource> reductions, PlanetId planet, PlayerId player,
    const EconomyCorruptionState* corruption) {
    return prepare_offer(original, sources, [&](bool price) {
        return income_production_multiplier(reductions, planet, player, original.type, price);
    }, planet, player, corruption);
}

ProductionAdmission ground_sale_admission(const PlanetProduction& planet, PlayerId player, EntityId id) {
    const auto object = std::find_if(planet.objects.begin(), planet.objects.end(),
        [id](const auto& row) { return row.id == id; });
    if (object == planet.objects.end()) return ProductionAdmission::stale_entry;
    if (object->owner != player) return ProductionAdmission::ownership;
    if (object->domain != ProductionDomain::ground || object->kind != ProductionKind::structure)
        return ProductionAdmission::wrong_entry;
    const auto* type = offer(planet, object->type);
    if (!type || !type->sellable) return ProductionAdmission::unavailable;
    // GHUD-S84: every queued product in both domains can occupy this facility.
    for (const auto& queue : planet.queues) for (const auto& entry : queue) {
        const auto* product = offer(planet, entry.type);
        if (!product) continue;
        for (const auto& group : product->required_structures)
            if (std::find(group.begin(), group.end(), object->type) != group.end())
                return ProductionAdmission::locked;
    }
    return ProductionAdmission::allowed;
}
math::Fixed ground_sale_refund(const ProductionObject& object, const ProductionOffer& type) {
    // GHUD-S84: free starting objects use half the authored integer price, truncated.
    // A recorded positive cost of at most one credit refunds zero; larger costs round half up.
    if (object.paid_cost.raw() <= 0)
        return Fixed::from_integer(type.authored_price.trunc_to_integer() / 2).value();
    if (object.paid_cost.raw() <= Fixed::scale) return {};
    return Fixed::from_integer((object.paid_cost.raw() / 2 + Fixed::scale / 2) / Fixed::scale).value();
}

ObjectSale object_sale(const ProductionSetup& production, std::span<const CensusType> sale_types,
    std::span<const CensusObject> actors, std::span<const Fleet> fleets, const PoliticalControlSetup* control,
    const MovementSetup* movement, PlayerId player, PlanetId planet_id, EntityId id) {
    const auto planet = std::lower_bound(production.planets.begin(), production.planets.end(), planet_id,
        [](const auto& row, auto key) { return row.planet < key; });
    if (!id || planet == production.planets.end() || planet->planet != planet_id) return {};
    const auto object = std::find_if(planet->objects.begin(), planet->objects.end(),
        [id](const auto& row) { return row.id == id; });
    if (object != planet->objects.end() && object->kind == ProductionKind::structure) {
        const auto why = ground_sale_admission(*planet, player, id);
        const auto* type = offer(*planet, object->type);
        return {why, object->type, type ? ground_sale_refund(*object, *type) : Fixed{}, false};
    }
    const auto actor = std::lower_bound(actors.begin(), actors.end(), id,
        [](const auto& row, auto key) { return row.id < key; });
    const bool have_actor = actor != actors.end() && actor->id == id;
    const auto owner = have_actor ? actor->owner : object != planet->objects.end() ? object->owner : PlayerId{};
    if (!owner || (have_actor && (actor->dead || actor->delete_pending))) return {ProductionAdmission::stale_entry};
    if (owner != player) return {ProductionAdmission::ownership};
    bool present{};
    for (const auto& fleet : fleets) if (std::any_of(fleet.members.begin(), fleet.members.end(),
        [id](const auto& member) { return member.id == id; })) {
        // Command admission uses the same parked identity that the fleet card displays.
        if (fleet.owner != player || fleet.orbit != planet_id || fleet.motion.transit) return {};
        present = true; break;
    }
    if (!present && control) for (const auto& row : control->planets) if (row.planet == planet_id) {
        present = row.surface_accessible && std::any_of(row.landed.begin(), row.landed.end(),
            [=](const auto& unit) { return unit.id == id && unit.owner == player; });
        break;
    }
    if (!present) return {};
    const auto original = have_actor ? actor->original_type : object->type;
    const auto current = have_actor ? actor->current_type : object->type;
    const auto trait = [&](TypeId type) -> const CensusType* {
        const auto at = std::lower_bound(sale_types.begin(), sale_types.end(), type,
            [](const auto& row, auto key) { return row.type < key; });
        return at != sale_types.end() && at->type == type ? &*at : nullptr;
    };
    const auto allowed = [&](TypeId type, bool contents) {
        if (const auto* fact = trait(type))
            return fact->sale_policy == 1 || (fact->sale_policy == 0 && !fact->named_hero && (contents || !fact->superweapon));
        const auto* fallback = offer(*planet, type);
        return fallback && fallback->sellable;
    };
    if (!allowed(current, false)) return {};
    const auto* true_type = trait(original);
    if (true_type ? true_type->sale_policy == 2 : !allowed(original, false)) return {};
    // Transport contents may be ordinary troops. Any default hero or authored Never child refuses the entire sale.
    std::vector<EntityId> pending{id}; std::set<EntityId> visited;
    for (std::size_t index = 0; index < pending.size(); ++index) {
        if (!visited.insert(pending[index]).second) return {}; // malformed/cyclic payload refuses
        if (!movement) continue;
        const auto at = std::lower_bound(movement->contents.begin(), movement->contents.end(), pending[index],
            [](const auto& row, auto key) { return row.container < key; });
        if (at == movement->contents.end() || at->container != pending[index]) continue;
        for (const auto& child : at->children) {
            const auto live = std::lower_bound(actors.begin(), actors.end(), child.id,
                [](const auto& row, auto key) { return row.id < key; });
            if (live == actors.end() || live->id != child.id || live->dead || live->delete_pending
                || !allowed(live->current_type, true)) return {};
            pending.push_back(child.id);
        }
    }
    ProductionObject value{id, original, player, have_actor ? actor->paid_cost : object->paid_cost,
        ProductionDomain::ground, ProductionKind::unit};
    ProductionOffer price;
    if (true_type) price.authored_price = true_type->authored_price;
    else if (const auto* type = offer(*planet, original)) price.authored_price = type->authored_price;
    else return {};
    return {ProductionAdmission::allowed, original, ground_sale_refund(value, price), true};
}

ProductionReservations production_reservations(const ProductionSetup& state, PlayerId player) {
    ProductionReservations result;
    std::map<TypeId, std::uint64_t> counts;
    for (const auto& planet : state.planets) {
        Fixed subtotal;
        for (const auto& queue : planet.queues) for (const auto& entry : queue) if (entry.builder == player) {
            ++counts[entry.type];
            subtotal = math::add(subtotal, offer(planet, entry.type)->population).value();
        }
        // WGEP-50: round once per producer, preserving the enqueue arithmetic.
        result.population = math::add(result.population,
            Fixed::from_integer((subtotal.raw() + Fixed::scale / 2) / Fixed::scale).value()).value();
    }
    result.counts.assign(counts.begin(), counts.end());
    return result;
}

namespace {
bool population_fits(const ProductionAdmissionInputs& inputs, const ProductionOffer& type,
    const ProductionReservations& reservations) {
    const auto& player = inputs.player;
    const auto usage = math::add(player.population_used, reservations.population).value();
    const auto room = std::max(Fixed{}, math::subtract(player.population_capacity, usage).value());
    return inputs.population ? population_admitted(*inputs.population, type.population)
        : !player.uses_population_cap || type.population <= room;
}
} // namespace

bool production_population_fits(const ProductionAdmissionInputs& inputs, const ProductionOffer& type) {
    if (inputs.reservations) return population_fits(inputs, type, *inputs.reservations);
    return population_fits(inputs, type, production_reservations(inputs.production, inputs.player.player));
}

namespace {
enum class ProductionQuery { purchase, eligibility, menu };
ProductionAdmission queried_admission(const ProductionAdmissionInputs& inputs,
    const ProductionOffer* type, ProductionDomain domain, ProductionQuery query) {
    const bool eligibility_only = query != ProductionQuery::purchase;
    const auto& state = inputs.production;
    const auto& planet = inputs.planet;
    const auto& player = inputs.player;
    const auto& account = inputs.account;
    if (inputs.owner != player.player) return ProductionAdmission::ownership;
    if (domain != ProductionDomain::ground && domain != ProductionDomain::space)
        return ProductionAdmission::unavailable;
    const auto& queue = planet.queues[static_cast<std::size_t>(domain)];
    // WGEP-18..27, in the sourced gate order. AI prequeue/budget payment is an explicit seam.
    if (!planet.enabled || !type || type->domain != domain) return ProductionAdmission::unavailable;
    // WGEP-18: affiliation belongs to the queried builder, not the producer's initial owner.
    // The installed registry contains every original type (WGEP-147), including structures.
    // Explicit primitive setups can supply an offer without a registered original.
    const auto registered = std::lower_bound(state.technology_types.begin(), state.technology_types.end(), type->type,
        [](const auto& row, auto id) { return row.type < id; });
    const bool affiliated = registered != state.technology_types.end() && registered->type == type->type
        ? std::binary_search(registered->affiliated_players.begin(), registered->affiliated_players.end(), player.player)
        : type->affiliated;
    if (!affiliated || type->cinematic
        || !type->direct_build_allowed || type->price < one || type->seconds.raw() < 0) return (ProductionAdmission::unavailable);
    if (!eligibility_only && !player.human) return (ProductionAdmission::unsupported_payment);
    if (!eligibility_only && !player.pay_as_you_go && type->price.trunc_to_integer() > account.credits.trunc_to_integer()) return (ProductionAdmission::credits);
    if (type->kind == ProductionKind::technology && (account.current_tech != type->tech || type->target_tech > account.maximum_tech)) return (ProductionAdmission::tech);
    if (std::binary_search(player.locked.begin(), player.locked.end(), type->type)
        || (type->initially_locked && !std::binary_search(player.unlocked.begin(), player.unlocked.end(), type->type)
            && !std::binary_search(inputs.local_unlocks.begin(), inputs.local_unlocks.end(), type->type))) return (ProductionAdmission::locked);
    if (planet.target_owner != player.player || (type->domain == ProductionDomain::space && planet.starbase_owner != 0 && planet.starbase_owner != player.player)) return (ProductionAdmission::ownership);
    if (!prerequisites(planet, *type, inputs.fleets, false)) return (ProductionAdmission::prerequisites);
    if (account.current_tech < type->tech) return (ProductionAdmission::tech);
    if (static_cast<std::uint32_t>(type->required_ground_level) > planet.ground_level
        || static_cast<std::uint32_t>(type->required_space_level) > planet.space_level) return (ProductionAdmission::prerequisites);
    std::uint64_t local_count{}, slot_count{}, queued_count{};
    for (const auto& object : planet.objects) { if (object.type == type->type) ++local_count; if (object.kind == ProductionKind::structure && object.domain == type->domain) ++slot_count; }
    for (const auto& q : planet.queues) for (const auto& entry : q) {
        if (entry.type == type->type) ++local_count;
        const auto* queued_type = offer(planet, entry.type);
        if (queued_type->kind == ProductionKind::structure && queued_type->domain == type->domain) ++slot_count;
        if (type->kind == ProductionKind::starbase && queued_type->kind == ProductionKind::starbase) return (ProductionAdmission::slots);
    }
    if (type->kind == ProductionKind::structure && (slot_count >= static_cast<std::uint64_t>(type->domain == ProductionDomain::ground ? planet.ground_slots : planet.space_slots)
        || (type->domain == ProductionDomain::space && planet.space_level < 1))) return (ProductionAdmission::slots);
    if (type->kind == ProductionKind::starbase && (type->base_level != planet.space_level + 1 || type->base_level > planet.maximum_space_level || starbase_blocked(state, planet, player.player, inputs.fleets))) return (ProductionAdmission::slots);
    if (type->kind == ProductionKind::structure && type->planet_limit >= 0 && local_count >= static_cast<std::uint64_t>(type->planet_limit)) return (ProductionAdmission::planet_limit);
    const auto count = std::lower_bound(player.counts.begin(), player.counts.end(), type->type, [](const auto& row, auto id) { return row.type < id; });
    const auto original_owned = count != player.counts.end() && count->type == type->type ? count->owned : 0;
    const auto owned = original_owned; // census refresh already resolves WGEP-78 composites
    const auto historical = count != player.counts.end() && count->type == type->type ? count->historical : 0;
    const auto computed = inputs.reservations ? ProductionReservations{}
        : production_reservations(state, player.player);
    const auto& reservations = inputs.reservations ? *inputs.reservations : computed;
    const auto queued = std::lower_bound(reservations.counts.begin(), reservations.counts.end(), type->type,
        [](const auto& row, auto id) { return row.first < id; });
    queued_count = queued != reservations.counts.end() && queued->first == type->type ? queued->second : 0;
    if (type->lifetime_limit >= 0 && historical >= static_cast<std::uint64_t>(type->lifetime_limit)) return (ProductionAdmission::lifetime_limit);
    if (type->current_limit >= 0 && owned + queued_count >= static_cast<std::uint64_t>(type->current_limit)) return (ProductionAdmission::current_limit);
    if ((type->domain == ProductionDomain::ground && !planet.surface_accessible && !type->surface_exception)
        || (type->requires_initial_placement && !type->initially_placed)) return (ProductionAdmission::prerequisites);
    if (!eligibility_only && queue.size() >= state.maximum_human_queue) return (ProductionAdmission::queue_full);
    // WGUI-170: capability skips queue/population admission and the project's host resource bounds.
    if (query != ProductionQuery::menu) {
        if (!population_fits(inputs, *type, reservations)) return (ProductionAdmission::population);
        if (inputs.next_id >= std::numeric_limits<EntityId>::max() - 2 || inputs.live_object_count >= galactic_max_objects) return (ProductionAdmission::resource_limit);
    }
    return ProductionAdmission::allowed;
}
} // namespace

ProductionAdmission production_admission(const ProductionAdmissionInputs& inputs,
    const ProductionOffer* type, ProductionDomain domain, bool eligibility_only) {
    return queried_admission(inputs, type, domain,
        eligibility_only ? ProductionQuery::eligibility : ProductionQuery::purchase);
}

core::Result<void> validate_production(const GalacticSetup& setup) {
    if (!setup.production) return core::Result<void>::success();
    const auto& p = *setup.production;
    if (!setup.fiscal || !setup.fleet_control || p.maximum_human_queue == 0 || p.maximum_human_queue >= 63
        || p.players.size() != setup.players.size() || p.planets.size() != setup.planets.size())
        return invalid("requires fiscal/fleet services, bounded queue cap and complete tables");
    if (p.technology_types.size() > galactic_max_objects
        || !ordered(p.technology_types, [](const auto& row) { return row.type; }))
        return invalid("technology registry requires bounded ordered identities");
    std::size_t affiliations{};
    for (const auto& type : p.technology_types) {
        affiliations += type.affiliated_players.size();
        if (!type.type || type.tech < std::numeric_limits<std::int32_t>::min() || type.tech > std::numeric_limits<std::int32_t>::max()
            || affiliations > galactic_max_objects
            || !ordered(type.affiliated_players, [](auto id) { return id; }))
            return invalid("invalid technology registry row");
        for (auto id : type.affiliated_players)
            if (std::none_of(setup.players.begin(), setup.players.end(), [&](const auto& row) { return row.player_id == id; }))
                return invalid("unbound technology affiliation");
    }
    if (p.nested_creation) {
        const auto& nested = *p.nested_creation;
        if (!setup.control || !setup.movement || !setup.census || !nested.fleet_type
            || nested.types.size() > galactic_max_objects || !ordered(nested.types, [](const auto& row) { return row.type; })
            || nested.relations.size() != setup.players.size() * setup.players.size())
            return invalid("nested creation requires bounded ordered types and complete live services");
        const auto find = [&](TypeId id) -> const NestedCreationType* {
            const auto at = std::lower_bound(nested.types.begin(), nested.types.end(), id,
                [](const auto& row, auto key) { return row.type < key; });
            return at != nested.types.end() && at->type == id ? &*at : nullptr;
        };
        if (!find(nested.fleet_type)) return invalid("nested fleet type is unbound");
        std::size_t members{};
        for (const auto& row : nested.types) {
            if (!row.type || static_cast<unsigned>(row.role) > 3) return invalid("invalid nested original type");
            if (row.role == NestedCreationRole::squadron && !find(nested.squadron_type))
                return invalid("current_representation_squadron_container_unbound");
            members += row.members.size();
            if (members > galactic_max_objects) return invalid("nested authored member limit exceeded");
            if ((row.role == NestedCreationRole::company || row.role == NestedCreationRole::squadron) && row.members.empty())
                return invalid("nested template has no authored members");
            if (row.role == NestedCreationRole::company && (!find(row.transport)
                || find(row.transport)->role != NestedCreationRole::transport)) return invalid("nested company transport is unbound");
            for (auto member : row.members) if (!find(member)) return invalid("nested member type is unbound");
            const auto census = std::lower_bound(setup.census->types.begin(), setup.census->types.end(), row.type,
                [](const auto& value, auto id) { return value.type < id; });
            if (census == setup.census->types.end() || census->type != row.type) return invalid("nested original census type is absent");
        }
        std::size_t index{};
        for (const auto& player : setup.players) for (const auto& other : setup.players) {
            const auto& relation = nested.relations[index++];
            if (relation.player != player.player_id || relation.other != other.player_id)
                return invalid("nested enemy relation table is unordered or incomplete");
        }
    }
    if (p.fleet_blockers) {
        if (p.fleet_blockers->size() > galactic_max_objects
            || !ordered(*p.fleet_blockers, [](const auto& row) { return row.fleet; }))
            return invalid("blocker declarations must be bounded and ordered by fleet ID");
        for (const auto& row : *p.fleet_blockers) {
            const auto fleet = std::lower_bound(setup.fleets.begin(), setup.fleets.end(), row.fleet,
                [](const auto& value, auto id) { return value.id < id; });
            if (fleet == setup.fleets.end() || fleet->id != row.fleet)
                return invalid("blocker declaration requires an existing fleet");
        }
    }
    for (std::size_t i = 0; i < p.players.size(); ++i) {
        const auto& row = p.players[i];
        if (row.player != setup.players[i].player_id || row.population_used.raw() < 0
            || row.population_used.raw() > (std::int64_t{1} << 48)
            || !ordered(row.counts, [](const auto& count) { return count.type; })
            || !ordered(row.locked, [](auto id) { return id; }) || !ordered(row.unlocked, [](auto id) { return id; }))
            return invalid("invalid player census/lock table");
        const bool human = std::any_of(setup.seats.begin(), setup.seats.end(), [&](const auto& seat) { return seat.player == row.player && seat.kind == SeatKind::human; });
        if (row.human != human || (human && !row.uses_population_cap) || (!human && !row.pay_as_you_go))
            return invalid("player payment/cap policy disagrees with WGEP-28/50");
        for (const auto& count : row.counts) if (count.type == 0 || count.owned > galactic_max_objects || count.historical > galactic_max_commands)
            return invalid("invalid census count");
    }
    std::size_t total_offers{}, total_objects = setup.planets.size() + setup.routes.size() + setup.fleets.size();
    std::set<EntityId> ids;
    for (const auto& row : setup.planets) ids.insert(row.id);
    for (const auto& row : setup.routes) ids.insert(row.id);
    for (const auto& row : setup.fleets) { ids.insert(row.id); for (const auto& member : row.members) { ids.insert(member.id); ++total_objects; } }
    for (const auto& row : setup.fleet_control->detached_units) { ids.insert(row.member.id); ++total_objects; }
    if (setup.movement) for (const auto& row : setup.movement->contents) for (const auto& child : row.children) {
        ids.insert(child.id); ++total_objects;
    }
    for (std::size_t i = 0; i < p.planets.size(); ++i) {
        const auto& row = p.planets[i];
        if (row.planet != setup.planets[i].id || !row.queues[0].empty() || !row.queues[1].empty()
            || !ordered(row.offers, [](const auto& type) { return type.type; }))
            return invalid("tick-zero planet tables require ordered offers and empty queues");
        const auto player = [&](PlayerId id) { return id == 0 || std::any_of(setup.players.begin(), setup.players.end(), [&](const auto& r) { return r.player_id == id; }); };
        if (!player(row.target_owner) || !player(row.starbase_owner) || !player(row.ground_occupier) || !player(row.orbit_occupier)
            || row.ground_level < 0 || row.ground_level > 5 || row.space_level < 0 || row.space_level > 5
            || row.maximum_space_level < 0 || row.maximum_space_level > 5 || row.ground_slots < 0 || row.space_slots < 0
            || row.landed_transports < 0 || row.maximum_ground_forces < 0) return invalid("invalid planet gates");
        if (!ordered(row.objects, [](const auto& object) { return object.id; })) return invalid("initial results must follow ID order");
        for (const auto& object : row.objects) {
            if (object.id == 0 || object.id == std::numeric_limits<EntityId>::max() || !ids.insert(object.id).second
                || object.type == 0 || object.owner == 0 || !player(object.owner) || object.paid_cost.raw() < 0
                || object.paid_cost.raw() > (std::int64_t{1} << 48) || static_cast<unsigned>(object.domain) > 1
                || static_cast<unsigned>(object.kind) > 1 || (object.domain == ProductionDomain::space && object.kind == ProductionKind::unit))
                return invalid("invalid initial structure/surface identity");
            if (++total_objects > galactic_max_objects) return invalid("too many production objects");
        }
        total_offers += row.offers.size();
        if (total_offers > galactic_max_objects) return invalid("offer table exceeds resource bound");
        for (const auto& type : row.offers) {
            if (type.type == 0 || static_cast<unsigned>(type.domain) > 1 || static_cast<unsigned>(type.kind) > 3
                || type.price.raw() % Fixed::scale != 0
                || type.population.raw() < 0 || type.population.raw() > (std::int64_t{1} << 40)
                || type.tech < std::numeric_limits<std::int32_t>::min() || type.tech > std::numeric_limits<std::int32_t>::max()
                || type.target_tech < 0 || static_cast<std::uint64_t>(type.target_tech) > std::numeric_limits<std::uint32_t>::max()
                || type.required_ground_level < std::numeric_limits<std::int32_t>::min() || type.required_ground_level > std::numeric_limits<std::int32_t>::max()
                || type.required_space_level < std::numeric_limits<std::int32_t>::min() || type.required_space_level > std::numeric_limits<std::int32_t>::max()
                || type.base_level < std::numeric_limits<std::int32_t>::min() || type.base_level > std::numeric_limits<std::int32_t>::max()
                || (type.kind == ProductionKind::starbase && type.domain != ProductionDomain::space))
                return invalid("invalid offer price/time/type inputs");
            for (const auto& group : type.required_structures) if (group.empty() || std::any_of(group.begin(), group.end(), [](auto id) { return id == 0; })) return invalid("empty prerequisite group");
            if (std::any_of(type.required_orbiting_units.begin(), type.required_orbiting_units.end(), [](auto id) { return id == 0; })) return invalid("invalid orbit prerequisite identity");
            // WGEP-18/27/41: atomic orbital results require leaf traits;
            // a sourced nested template supplies its own commissioned children.
            if (type.direct_build_allowed && type.domain == ProductionDomain::space && type.kind == ProductionKind::unit) {
                const auto found = std::find_if(setup.fleet_control->types.begin(), setup.fleet_control->types.end(), [&](const auto& trait) { return trait.type == type.type; });
                const bool nested = p.nested_creation && std::any_of(p.nested_creation->types.begin(), p.nested_creation->types.end(),
                    [&](const auto& row) { return row.type == type.type
                        && (row.role == NestedCreationRole::company || row.role == NestedCreationRole::squadron); });
                if (found == setup.fleet_control->types.end() || (found->requires_live_children && !nested))
                    return invalid("orbital result requires supported original fleet traits (type " + std::to_string(type.type) + ")");
            }
        }
    }
    return core::Result<void>::success();
}

namespace {
const CensusType* census_type(const PopulationCensus& census, TypeId id) {
    const auto at = std::lower_bound(census.types.begin(), census.types.end(), id,
        [](const auto& row, auto value) { return row.type < value; });
    return at != census.types.end() && at->type == id ? &*at : nullptr;
}
struct CensusContext { PlanetId planet{}; bool suppressed{}, valid{true}; };
CensusContext census_context(const PopulationCensus& census, std::span<const CensusObject> objects,
    const CensusObject& object) {
    CensusContext result{object.planet};
    auto parent_id = object.container;
    for (std::size_t depth = 0; parent_id; ++depth) {
        if (depth >= objects.size()) { result.valid = false; break; }
        const auto parent = std::lower_bound(objects.begin(), objects.end(), parent_id,
            [](const auto& row, auto id) { return row.id < id; });
        if (parent == objects.end() || parent->id != parent_id) { result.valid = false; break; }
        if (depth == 0) {
            const auto* type = census_type(census, parent->original_type);
            result.suppressed = type && type->population.raw() > 0;
        }
        // WGEP-68: enclosing-planet traversal is independent of the immediate
        // original-type suppression. Follow containers through moved wrappers.
        if (parent->planet) result.planet = parent->planet;
        parent_id = parent->container;
    }
    return result;
}
Fixed rounded_population(Fixed value) {
    return Fixed::from_integer((value.raw() + Fixed::scale / 2) / Fixed::scale).value();
}
std::uint64_t census_retained_reinforcements(PlayerId, TypeId) {
    // WGEP-78: this input is the type-only skirmish purchase reinforcement list.
    // Campaign retained/reserve forces keep their galactic owned identities and
    // already count in the canonical gather; they are not additional purchases.
    // GC has no type-only purchase pool, so this mode contributes no entries.
    return 0;
}
std::optional<std::vector<TypeId>> census_member_order(const PopulationCensus& census) {
    // Leaf-first DAG order avoids recursion and repeated expansion of shared members.
    std::map<TypeId, std::size_t> pending;
    std::map<TypeId, std::vector<TypeId>> parents;
    std::set<TypeId> ready;
    std::size_t links{};
    for (const auto& type : census.types) {
        if (!ordered(type.members, [](auto id) { return id; })) return std::nullopt;
        links += type.members.size();
        if (links > galactic_max_objects) return std::nullopt;
        pending[type.type] = type.members.size();
        if (type.members.empty()) ready.insert(type.type);
        for (auto member : type.members) {
            if (!census_type(census, member)) return std::nullopt;
            parents[member].push_back(type.type);
        }
    }
    std::vector<TypeId> result;
    while (!ready.empty()) {
        const auto id = *ready.begin(); ready.erase(ready.begin()); result.push_back(id);
        for (auto parent : parents[id]) if (--pending[parent] == 0) ready.insert(parent);
    }
    if (result.size() != census.types.size()) return std::nullopt;
    return result;
}
} // namespace

core::Result<void> validate_census(const GalacticSetup& setup) {
    if (!setup.census) return core::Result<void>::success();
    const auto& census = *setup.census;
    if (!setup.production || !setup.fleet_control || census.players.size() != setup.players.size()
        || census.types.size() > galactic_max_objects || census.objects.size() > galactic_max_objects
        || !ordered(census.types, [](const auto& row) { return row.type; })
        || !ordered(census.objects, [](const auto& row) { return row.id; })) return invalid("invalid census tables");
    for (const auto& row : census.types) if (row.type == 0 || row.population.raw() < 0
        || row.population.raw() > (std::int64_t{1} << 40)
        || row.additional_capacity < std::numeric_limits<std::int32_t>::min()
        || row.additional_capacity > std::numeric_limits<std::int32_t>::max() || row.sale_policy > 2)
        return invalid("census type outside arithmetic bounds");
    if (!census_member_order(census)) return invalid("census members require a bounded ordered acyclic type graph");
    for (const auto& type : census.types) {
        if (type.company != (type.transport != 0) || (type.company
            && (!census_type(census, type.transport) || type.transport == type.type
                || !std::binary_search(type.members.begin(), type.members.end(), type.transport))))
            return invalid("invalid exact company transport binding");
        if (type.company && setup.movement) {
            const auto transport = std::lower_bound(setup.movement->types.begin(), setup.movement->types.end(), type.transport,
                [](const auto& value, auto id) { return value.type < id; });
            if (transport == setup.movement->types.end() || transport->type != type.transport || !transport->transport)
                return invalid("company binding requires the live transport movement role");
        }
    }
    const auto planet_exists = [&](PlanetId id) {
        return std::any_of(setup.planets.begin(), setup.planets.end(), [&](const auto& row) { return row.id == id; });
    };
    for (std::size_t i = 0; i < census.players.size(); ++i) {
        const auto& row = census.players[i];
        if (row.player != setup.players[i].player_id || row.base_capacity < 0 || row.base_capacity > (std::int64_t{1} << 24)
            || !ordered(row.unusable_planets, [](auto id) { return id; })) return invalid("invalid census player");
        for (auto id : row.unusable_planets) if (!planet_exists(id)) return invalid("unknown AI-unusable planet");
    }
    std::set<EntityId> identities;
    std::map<EntityId, TypeId> original_types;
    for (const auto& row : setup.planets) identities.insert(row.id);
    for (const auto& row : setup.fleets) {
        identities.insert(row.id);
        for (const auto& member : row.members) { identities.insert(member.id); original_types[member.id] = member.type; }
    }
    for (const auto& row : setup.fleet_control->detached_units) { identities.insert(row.member.id); original_types[row.member.id] = row.member.type; }
    if (setup.control) for (const auto& planet : setup.control->planets) for (const auto& force : planet.landed) {
        identities.insert(force.id); original_types[force.id] = force.type;
    }
    if (setup.movement) for (const auto& row : setup.movement->contents) for (const auto& child : row.children) {
        identities.insert(child.id); original_types[child.id] = child.type;
    }
    for (const auto& planet : setup.production->planets) {
        if (planet.starbase_type && !census_type(census, planet.starbase_type)) return invalid("missing station census type");
        for (const auto& type : planet.offers) {
            const auto* trait = census_type(census, type.type);
            if (!trait || trait->population != type.population) return invalid("offer population differs from original census type");
        }
        for (const auto& row : planet.objects) { identities.insert(row.id); original_types[row.id] = row.type; }
    }
    for (const auto& [id, type] : original_types) {
        (void)id;
        if (!census_type(census, type)) return invalid("missing owned census type");
    }
    for (const auto& row : census.objects) {
        if (row.id == 0 || row.id == std::numeric_limits<EntityId>::max() || !census_type(census, row.original_type)
            || (row.current_type && !census_type(census, row.current_type))
            || row.paid_cost.raw() < 0 || row.paid_cost.raw() > (std::int64_t{1} << 48)
            || row.generated_source == row.id
            || row.combat_role > ActorCombatRole::squadron
            || row.abilities.evade_chance.raw() < 0 || row.abilities.evade_chance.raw() > Fixed::scale
            || (!row.abilities.present && (row.abilities.combat_skip || row.abilities.self_target || row.abilities.evade_chance.raw()))
            || (row.planet && !planet_exists(row.planet)) || row.container == row.id
            || (row.owner != 0 && std::none_of(setup.players.begin(), setup.players.end(), [&](const auto& player) { return player.player_id == row.owner; })))
            return invalid("invalid census owned-list row");
        if (original_types.contains(row.id) && original_types.at(row.id) != row.original_type) {
            // WGS-80 / WGEP-78: a company retains its original dummy type on the
            // exact authored live transport. No other original override is admitted.
            const auto* original = census_type(census, row.original_type);
            const auto actual = original_types.at(row.id);
            if (!original->company || original->transport != actual || !setup.movement)
                return invalid("census override changes original identity");
            const auto transport = std::lower_bound(setup.movement->types.begin(), setup.movement->types.end(), actual,
                [](const auto& value, auto id) { return value.type < id; });
            const auto contents = std::lower_bound(setup.movement->contents.begin(), setup.movement->contents.end(), row.id,
                [](const auto& value, auto id) { return value.container < id; });
            if (transport == setup.movement->types.end() || transport->type != actual || !transport->transport
                || contents == setup.movement->contents.end() || contents->container != row.id || contents->children.empty())
                return invalid("company original identity requires explicit live transport contents");
            for (const auto& child : contents->children)
                if (!std::binary_search(original->members.begin(), original->members.end(), child.type))
                    return invalid("company transport contains an unauthored original member");
        }
        if (std::any_of(setup.routes.begin(), setup.routes.end(), [&](const auto& route) { return route.id == row.id; })) return invalid("census ID aliases a route");
        identities.insert(row.id);
    }
    if (identities.size() + setup.routes.size() > galactic_max_objects) return invalid("census exceeds object limit");
    for (const auto& row : census.objects) if (row.container && !identities.contains(row.container)) return invalid("missing immediate census container");
    return core::Result<void>::success();
}

bool population_admitted(const PopulationTotal& total, Fixed population) {
    // WGEP-69: a nonhuman with cap disabled bypasses even a reported zero capacity.
    if (!total.uses_cap) return true;
    const auto usage = math::add(total.used, total.reserved);
    if (!usage) return false;
    const auto room = math::subtract(total.capacity, usage.value());
    return room && population <= std::max(Fixed{}, room.value());
}

core::Result<std::vector<PopulationTotal>> population_totals(const PopulationCensus& census,
    std::span<const CensusObject> objects, const ProductionSetup& production, const PartitionExecutor& executor) {
    using Totals = std::vector<PopulationTotal>;
    const auto fail = [&] { return core::Result<Totals>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "invalid population census contribution")); };
    if (census.players.size() != production.players.size() || objects.size() > galactic_max_objects
        || !ordered(objects, [](const auto& row) { return row.id; })) return fail();
    struct Contribution { PlayerId player{}; Fixed usage, capacity; bool valid{true}; };
    std::vector<Contribution> contributions(objects.size());
    auto phase = executor.execute_phase("galactic-population", tick_partition_count, [&](std::size_t partition) {
        const auto range = partition_range(partition, objects.size());
        for (auto i = range.begin; i < range.end; ++i) {
            const auto& object = objects[i]; auto& out = contributions[i]; out.player = object.owner;
            if (object.owner == 0) continue;
            const auto player = std::lower_bound(census.players.begin(), census.players.end(), object.owner,
                [](const auto& row, auto id) { return row.player < id; });
            if (player == census.players.end() || player->player != object.owner) { out.valid = false; continue; }
            const auto index = static_cast<std::size_t>(player - census.players.begin());
            const auto context = census_context(census, objects, object);
            if (!context.valid) { out.valid = false; continue; }
            if (!production.players[index].human && (!player->ai_uses_cap
                || std::binary_search(player->unusable_planets.begin(), player->unusable_planets.end(), context.planet))) continue;
            const auto* type = census_type(census, object.original_type);
            if (!type) { if (object.original_type != 0) out.valid = false; continue; }
            // WGEP-68: owned-list usage has no dead/delete guard. Suppression
            // reads only the immediate container's ORIGINAL type, even if dead.
            if (!context.suppressed) out.usage = type->population;
            if (!object.dead && !object.delete_pending) out.capacity = Fixed::from_integer(type->additional_capacity).value();
        }
    });
    if (!phase) return core::Result<Totals>::failure(phase.error());
    Totals totals;
    for (std::size_t i = 0; i < census.players.size(); ++i) {
        const auto& player = census.players[i];
        totals.push_back({player.player, Fixed::from_integer(player.base_capacity).value(), {}, {}, production.players[i].human || player.ai_uses_cap});
    }
    // Ordered reduction is independent of executor completion order.
    for (const auto& row : contributions) {
        if (!row.valid) return fail();
        if (row.player == 0) continue;
        auto& total = *std::lower_bound(totals.begin(), totals.end(), row.player, [](const auto& item, auto id) { return item.player < id; });
        if (!math::try_add(total.used, row.usage, total.used) || !math::try_add(total.capacity, row.capacity, total.capacity)) return fail();
    }
    struct Producer { PlayerId owner{}; Fixed reserved, station_capacity; bool valid{true}; };
    std::vector<Producer> producers(production.planets.size());
    phase = executor.execute_phase("galactic-population-queues", tick_partition_count, [&](std::size_t partition) {
        const auto range = partition_range(partition, producers.size());
        for (auto i = range.begin; i < range.end; ++i) {
            const auto& planet = production.planets[i]; auto& out = producers[i];
            const auto owned = std::lower_bound(objects.begin(), objects.end(), planet.planet, [](const auto& row, auto id) { return row.id < id; });
            if (owned == objects.end() || owned->id != planet.planet) { out.valid = false; continue; }
            out.owner = owned->owner;
            if (!out.owner) continue;
            const auto player = std::lower_bound(census.players.begin(), census.players.end(), out.owner, [](const auto& row, auto id) { return row.player < id; });
            if (player == census.players.end() || player->player != out.owner) { out.valid = false; continue; }
            const auto index = static_cast<std::size_t>(player - census.players.begin());
            const auto context = census_context(census, objects, *owned);
            if (!context.valid) { out.valid = false; continue; }
            if (!production.players[index].human && (!player->ai_uses_cap
                || std::binary_search(player->unusable_planets.begin(), player->unusable_planets.end(), context.planet))) continue;
            // WGEP-68: both ACTUAL queues, with no AI prequeue addition;
            // immediate positive-population parents suppress production too.
            if (!context.suppressed) for (const auto& queue : planet.queues) for (const auto& entry : queue) {
                const auto* type = census_type(census, entry.type);
                if (!type || !math::try_add(out.reserved, type->population, out.reserved)) out.valid = false;
            }
            if (out.reserved.raw() > std::numeric_limits<std::int64_t>::max() - Fixed::scale / 2) out.valid = false;
            if (out.valid) out.reserved = rounded_population(out.reserved);
            if (!owned->dead && !owned->delete_pending && planet.starbase_type) {
                const auto* type = census_type(census, planet.starbase_type);
                if (!type) out.valid = false;
                else out.station_capacity = Fixed::from_integer(type->additional_capacity).value();
            }
        }
    });
    if (!phase) return core::Result<Totals>::failure(phase.error());
    for (const auto& row : producers) {
        if (!row.valid) return fail();
        if (!row.owner) continue;
        auto& total = *std::lower_bound(totals.begin(), totals.end(), row.owner, [](const auto& item, auto id) { return item.player < id; });
        if (!math::try_add(total.reserved, row.reserved, total.reserved) || !math::try_add(total.capacity, row.station_capacity, total.capacity)) return fail();
    }
    for (auto& row : totals) {
        row.used = rounded_population(row.used);
        if (!row.uses_cap) row.capacity = row.used = row.reserved = Fixed{}; // WGEP-69 getters; admission bypasses
    }
    return core::Result<Totals>::success(std::move(totals));
}

std::shared_ptr<const session_detail::NativePopulationSchema> session_detail::native_population_schema(const GalacticSetup& setup) {
    if (!setup.census) return {};
    auto schema = std::make_shared<NativePopulationSchema>();
    schema->census = *setup.census;
    auto& backed = schema->backed;
    auto& initial_containers = schema->initial_containers;
    for (const auto& row : setup.planets) backed.insert(row.id);
    for (const auto& row : setup.fleets) {
        backed.insert(row.id);
        for (const auto& member : row.members) { backed.insert(member.id); initial_containers[member.id] = row.id; }
    }
    for (const auto& row : setup.fleet_control->detached_units) backed.insert(row.member.id);
    if (setup.control) for (const auto& planet : setup.control->planets) for (const auto& force : planet.landed) backed.insert(force.id);
    if (setup.movement) for (const auto& row : setup.movement->contents) for (const auto& child : row.children) {
        backed.insert(child.id); initial_containers[child.id] = row.container;
    }
    for (const auto& planet : setup.production->planets) for (const auto& row : planet.objects) backed.insert(row.id);
    return schema;
}
core::Result<session_detail::NativePopulationProjection> session_detail::project_native_population(
    const NativePopulationSchema& schema, std::span<const Planet> planets, std::span<const Fleet> fleets,
    std::span<const DetachedUnit> detached_units, const PoliticalControlSetup* control,
    const MovementSetup* movement_inputs, std::span<const CensusObject> production_originals,
    const ProductionSetup& production, const PartitionExecutor& executor) {
    using Result = core::Result<NativePopulationProjection>;
    // Canonical gather only; per-object arithmetic is in population_totals.
    std::map<EntityId, CensusObject> owned;
    const auto gather = [&](EntityId id, TypeId type, PlayerId owner, PlanetId planet, EntityId container = 0) {
        auto [at, inserted] = owned.try_emplace(id, CensusObject{id, type, owner, planet, container, false, false, 0, 0, ActorCombatRole::unavailable, {}, {}});
        if (!inserted) { at->second.owner = owner; at->second.planet = planet; }
    };
    for (const auto& row : planets) gather(row.id, 0, row.owner, row.id);
    for (const auto& row : fleets) {
        gather(row.id, 0, row.owner, row.orbit);
        for (const auto& member : row.members) gather(member.id, member.type, row.owner, row.orbit, row.id);
    }
    for (const auto& row : detached_units) gather(row.member.id, row.member.type, row.owner, row.planet);
    // WGM-49..53 / WGEP-68/78: landing changes the provider, not owned identity.
    if (control) for (const auto& planet : control->planets) for (const auto& force : planet.landed)
        gather(force.id, force.type, force.owner, planet.planet, planet.planet);
    // Movement owns immutable child identities; resolve their current enclosing
    // parent from the canonical gather, including nested containers.
    if (movement_inputs) {
        const auto gather_children = [&](auto&& self, EntityId parent) -> void {
            const auto at = std::lower_bound(movement_inputs->contents.begin(), movement_inputs->contents.end(), parent,
                [](const auto& row, auto id) { return row.container < id; });
            if (at == movement_inputs->contents.end() || at->container != parent) return;
            const auto enclosing = owned.at(parent);
            for (const auto& child : at->children) {
                gather(child.id, child.type, enclosing.owner, enclosing.planet, parent);
                self(self, child.id);
            }
        };
        for (const auto& fleet : fleets) for (const auto& member : fleet.members) gather_children(gather_children, member.id);
        for (const auto& row : detached_units) gather_children(gather_children, row.member.id);
        if (control) for (const auto& planet : control->planets) for (const auto& force : planet.landed)
            gather_children(gather_children, force.id);
    }
    for (const auto& planet : production.planets) for (const auto& row : planet.objects)
        // Orbital results also live in fleets: count their stable identity once.
        if (!owned.contains(row.id)) gather(row.id, row.type, row.owner, planet.planet);
    const auto& backed = schema.backed;
    const auto& initial_containers = schema.initial_containers;
    for (const auto& row : schema.census.objects) {
        const auto at = owned.find(row.id);
        if (at != owned.end()) {
            auto metadata = row; metadata.owner = at->second.owner; metadata.planet = at->second.planet;
            // Membership commands change the enclosing fleet for an ordinary member.
            if (metadata.container == 0 || (initial_containers.contains(row.id)
                && metadata.container == initial_containers.at(row.id))) metadata.container = at->second.container;
            at->second = metadata;
        } else if (!backed.contains(row.id)) owned.emplace(row.id, row);
    }
    // Commissioned originals override runtime carrier types for IDs in the owned
    // gather (not a dead/delete predicate). Membership/location come from that gather.
    for (const auto& row : production_originals) if (auto at = owned.find(row.id); at != owned.end()) {
        at->second.original_type = row.original_type;
    }
    std::vector<CensusObject> objects;
    for (const auto& [id, row] : owned) { (void)id; objects.push_back(row); }
    auto totals = population_totals(schema.census, objects, production, executor);
    if (!totals) return Result::failure(totals.error());
    std::map<PlayerId, std::map<TypeId, std::uint64_t>> owned_counts;
    // WGEP-78: ordered exact-original owned count has no dead/delete guard.
    // It shares the canonical gather, not a new service traversal.
    for (const auto& row : objects) if (row.owner && row.original_type) ++owned_counts[row.owner][row.original_type];
    for (const auto& planet : production.planets)
        if (planet.starbase_owner && planet.starbase_type) ++owned_counts[planet.starbase_owner][planet.starbase_type];
    const auto member_order = census_member_order(schema.census);
    if (!member_order) return Result::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "production: invalid live census member graph"));
    const auto& direct_counts = owned_counts;
    std::vector<std::map<TypeId, std::uint64_t>> resolved(production.players.size());
    auto phase = executor.execute_phase("galactic-census-owned-types", tick_partition_count, [&](std::size_t partition) {
        const auto range = partition_range(partition, resolved.size());
        for (auto i = range.begin; i < range.end; ++i) {
            const auto player = production.players[i].player;
            if (const auto at = direct_counts.find(player); at != direct_counts.end()) resolved[i] = at->second;
            auto& counts = resolved[i];
            // WGEP-78: direct original count PLUS recursive member MAX, owner only.
            // Every DAG node is evaluated once; shared descendants are not re-expanded.
            for (auto id : *member_order) {
                std::uint64_t maximum{};
                for (auto member : census_type(schema.census, id)->members) {
                    const auto at = counts.find(member);
                    if (at != counts.end()) maximum = std::max(maximum, at->second);
                }
                const auto retained = census_retained_reinforcements(player, id);
                if (maximum || retained) counts[id] += maximum + retained;
            }
        }
    });
    if (!phase) return Result::failure(phase.error());
    return Result::success({std::move(totals).value(), std::move(owned_counts), std::move(resolved), objects.size()});
}
bool session_detail::apply_native_population(std::vector<ProductionPlayer>& players, std::span<const PopulationTotal> totals,
    std::vector<std::map<TypeId, std::uint64_t>>& resolved) {
    bool changed{};
    for (std::size_t i = 0; i < totals.size(); ++i) {
        auto& player = players[i]; const auto& total = totals[i];
        if (player.population_used != total.used || player.population_capacity != total.capacity || player.uses_population_cap != total.uses_cap) changed = true;
        player.population_used = total.used; player.population_capacity = total.capacity; player.uses_population_cap = total.uses_cap;
        // WGEP-25/78: resolved live owned identities replace the supplied seam;
        // historical completions remain a separate monotonic ledger.
        auto counts = std::move(resolved[i]);
        auto previous = player.counts;
        for (auto& count : player.counts) { count.owned = counts[count.type]; counts.erase(count.type); }
        for (const auto& [type, count] : counts) player.counts.push_back({type, count, 0});
        std::sort(player.counts.begin(), player.counts.end(), [](const auto& a, const auto& b) { return a.type < b.type; });
        if (player.counts != previous) changed = true;
    }
    return changed;
}
core::Result<void> GalacticSession::Impl::refresh_population(const PartitionExecutor& executor) {
    if (!setup.census || !population_stale) return core::Result<void>::success();
    std::vector<DetachedUnit> detached;
    detached.reserve(detached_units.size());
    for (const auto& [id, row] : detached_units) { (void)id; detached.push_back(row); }
    auto projected = session_detail::project_native_population(*native_population_schema, sorted_planets(), sorted_fleets(), detached,
        control ? &*control : nullptr, movement_inputs.get(), production_originals, *production, executor);
    if (!projected) return core::Result<void>::failure(projected.error());
    auto result = std::move(projected).value();
    population_objects += result.objects; population_producers += production->planets.size();
    population_stale = false;
    if (population != result.totals) { population = std::move(result.totals); dirty[11] = true; }
    original_owned_counts = std::move(result.original_counts);
    auto resolved = std::move(result.resolved_counts);
    if (session_detail::apply_native_population(production->players, population, resolved)) dirty[8] = true;
    return core::Result<void>::success();
}

GalacticSession::Impl::ProductionOfferInputs GalacticSession::Impl::production_offer_inputs(
    std::span<const ObservedPlanet> observations, std::span<const Fleet> fleet_inputs,
    std::span<const Planet> owners, const PlayerId local) const {
    ProductionOfferInputs result;
    if (!production || !economy || !observer) return result;
    const auto player = std::lower_bound(production->players.begin(), production->players.end(), local,
        [](const auto& row, auto id) { return row.player < id; });
    const auto account = std::lower_bound(economy->players.begin(), economy->players.end(), local,
        [](const auto& row, auto id) { return row.player < id; });
    if (player == production->players.end() || player->player != local || !player->human
        || account == economy->players.end() || account->player != local) return result;
    // WGUI-91: a raw production payload never supplies local-control knowledge.
    // Index once, then query only the local player's admitted producer rows.
    for (const auto& row : observations)
        if (row.player == local && row.discovered && row.knowledge.ownership
            && row.known_allegiance == std::optional(local)) result.visible.push_back(row.planet);
    std::sort(result.visible.begin(), result.visible.end());
    result.visible.erase(std::unique(result.visible.begin(), result.visible.end()), result.visible.end());
    for (const auto& fleet : fleet_inputs) {
        if (!std::binary_search(result.visible.begin(), result.visible.end(), fleet.orbit)) continue;
        result.fleets.insert(result.fleets.end(), {fleet.id, fleet.owner, fleet.orbit, fleet.motion.transit ? 1U : 0U, fleet.members.size()});
        for (const auto& member : fleet.members) result.fleets.push_back(member.type);
    }
    if (income) result.reductions = production_reduction_sources(live_income_inputs(), owners);
    auto& bytes = result.admission;
    detail::put_u32(bytes, local);
    detail::put_u64(bytes, next_id);
    detail::put_u64(bytes, live_object_count);
    detail::put_i64(bytes, account->credits.raw());
    detail::put_i64(bytes, account->current_tech);
    detail::put_i64(bytes, account->maximum_tech);
    detail::encode_production_menu_inputs(bytes, *production, local, result.visible);
    // Reservations include this player's queues on every producer, including hidden ones.
    const auto reservations = production_reservations(*production, local);
    detail::put_i64(bytes, reservations.population.raw());
    detail::put_u64(bytes, reservations.counts.size());
    for (const auto& [type, count] : reservations.counts) {
        detail::put_u64(bytes, type); detail::put_u64(bytes, count);
    }
    detail::put_u8(bytes, setup.census ? 1 : 0);
    if (setup.census) {
        const auto total = std::find_if(population.begin(), population.end(),
            [local](const auto& row) { return row.player == local; });
        const auto found = total != population.end();
        detail::put_u8(bytes, found ? 1 : 0);
        if (found) detail::encode_population(bytes, std::span<const PopulationTotal>(&*total, 1));
    }
    for (auto planet : result.visible) {
        const auto owner = std::lower_bound(owners.begin(), owners.end(), planet,
            [](const auto& row, auto id) { return row.id < id; });
        detail::put_u32(bytes, owner != owners.end() && owner->id == planet ? owner->owner : 0);
        const auto unlocks = corruption_local_unlocks(planet, local);
        detail::put_u64(bytes, unlocks.size());
        for (auto type : unlocks) detail::put_u64(bytes, type);
    }
    detail::put_u8(bytes, economy_corruption ? 1 : 0);
    if (economy_corruption) {
        detail::put_u8(bytes, economy_corruption->tutorial ? 1 : 0);
        for (const auto& row : economy_corruption->players) if (row.player == local) {
            detail::put_u8(bytes, row.benefits ? 1 : 0);
            for (auto value : row.price) detail::put_i64(bytes, value.raw());
            for (auto value : row.time) detail::put_i64(bytes, value.raw());
            detail::put_i64(bytes, row.tech_purchase_multiplier.raw());
        }
        for (const auto& row : economy_corruption->planets)
            if (std::binary_search(result.visible.begin(), result.visible.end(), row.planet)) {
                detail::put_u64(bytes, row.planet);
                detail::put_u8(bytes, row.corruption != corruption_fixture::Type::none ? 1 : 0);
            }
    }
    return result;
}

std::vector<ObservedProductionOffer> GalacticSession::Impl::observed_production_offers(
    const ProductionOfferInputs& inputs, std::span<const Fleet> fleet_inputs, const PartitionExecutor& executor, const PlayerId local) const {
    std::vector<ObservedProductionOffer> result;
    if (!production || !economy || !observer) return result;
    const auto player = std::lower_bound(production->players.begin(), production->players.end(), local,
        [](const auto& row, auto id) { return row.player < id; });
    const auto account = std::lower_bound(economy->players.begin(), economy->players.end(), local,
        [](const auto& row, auto id) { return row.player < id; });
    if (player == production->players.end() || player->player != local || !player->human
        || account == economy->players.end() || account->player != local) return result;
    const auto reservations = production_reservations(*production, local);
    const auto owners = sorted_planets();
    const IncomeSetup* sources = income ? &live_income_inputs() : nullptr;
    // Gathered once for every visible offer below, not per offer (#3003).
    const auto& reductions = inputs.reductions;
    const auto total = std::find_if(population.begin(), population.end(),
        [local](const auto& row) { return row.player == local; });
    // Each admitted producer's rows depend only on copied inputs, so producers are evaluated in partitions with
    // disjoint outputs and joined in producer order (ADR-009): the rows are equal for any worker count.
    struct Producer { const PlanetProduction* planet; const Planet* owner; };
    std::vector<Producer> admitted;
    for (const auto& planet : production->planets) {
        if (!std::binary_search(inputs.visible.begin(), inputs.visible.end(), planet.planet)) continue;
        const auto owner = std::lower_bound(owners.begin(), owners.end(), planet.planet,
            [](const auto& row, auto id) { return row.id < id; });
        if (owner == owners.end() || owner->id != planet.planet) continue;
        if (setup.census && total == population.end()) continue;
        admitted.push_back({&planet, &*owner});
    }
    std::vector<std::vector<ObservedProductionOffer>> staged(admitted.size());
    const auto evaluate = [&](const std::size_t index) {
        const auto& planet = *admitted[index].planet;
        const auto* owner = admitted[index].owner;
        auto& rows = staged[index];
        const auto unlocks = corruption_local_unlocks(planet.planet, local);
        for (const auto& original : planet.offers) {
            ObservedProductionOffer row;
            row.player = local; row.planet = planet.planet; row.type = original.type;
            row.domain = original.domain; row.kind = original.kind;
            row.base_price = original.authored_price;
            const auto& queue = planet.queues[static_cast<std::size_t>(original.domain)];
            row.already_in_production = std::any_of(queue.begin(), queue.end(),
                [&](const auto& entry) { return entry.type == original.type; });
            auto adjusted = prepared_production_offer(original, sources, reductions, planet.planet, local,
                economy_corruption ? &*economy_corruption : nullptr);
            if (!adjusted) row.admission = ProductionAdmission::resource_limit;
            else {
                row.price = adjusted.value().price; row.seconds = adjusted.value().seconds;
                const ProductionAdmissionInputs admission_inputs{*production, planet, *player, *account, fleet_inputs,
                    owner->owner, setup.census ? &*total : nullptr, next_id, live_object_count, &reservations, unlocks};
                row.admission = production_admission(admission_inputs, &adjusted.value(), original.domain);
                row.current = queried_admission(admission_inputs, &adjusted.value(), original.domain,
                    ProductionQuery::menu) == ProductionAdmission::allowed;
                row.population_fits = production_population_fits(admission_inputs, adjusted.value());
            }
            rows.push_back(std::move(row));
        }
    };
    const auto phase = executor.execute_phase("galactic-production-offers", tick_partition_count, [&](const std::size_t partition) {
        const auto range = partition_range(partition, admitted.size());
        for (auto index = range.begin; index < range.end; ++index) evaluate(index);
    });
    if (!phase) {
        // A failed phase never publishes partial rows: recompute every producer in order on this thread.
        for (std::size_t index = 0; index < admitted.size(); ++index) { staged[index].clear(); evaluate(index); }
    }
    for (auto& rows : staged) result.insert(result.end(), std::make_move_iterator(rows.begin()), std::make_move_iterator(rows.end()));
    return result;
}

void GalacticSession::Impl::execute_object_sale(const GameCommand& command, std::uint64_t tick, std::vector<GalacticEvent>& events) {
    const auto& order = std::get<SellObject>(command.payload);
    GalacticEvent event{tick, GalacticEventKind::command_executed, command.key.seat,
        order.object, command.key.sequence, 0, GalacticRejectReason::none};
    const auto reject = [&](ProductionAdmission why) {
        event.kind = GalacticEventKind::command_rejected; event.reason = GalacticRejectReason::production_admission;
        event.value = static_cast<std::uint64_t>(why); events.push_back(event);
    };
    const auto* issuer = seat(command.key.seat);
    if (!issuer || !issuer->active) {
        event.kind = GalacticEventKind::command_rejected; event.reason = GalacticRejectReason::inactive_seat;
        events.push_back(event); return;
    }
    if (!production || !economy || !setup.fiscal) return reject(ProductionAdmission::unavailable);
    const auto planet = std::lower_bound(production->planets.begin(), production->planets.end(), order.planet,
        [](const auto& row, auto id) { return row.planet < id; });
    if (planet == production->planets.end() || planet->planet != order.planet) return reject(ProductionAdmission::unavailable);
    const auto live_actors = actor_inputs();
    const auto sale = object_sale(*production, setup.census ? std::span<const CensusType>(setup.census->types) : std::span<const CensusType>{},
        live_actors, sorted_fleets(), control ? &*control : nullptr, movement_inputs.get(), issuer->player, order.planet, order.object);
    const auto why = sale.admission;
    if (why != ProductionAdmission::allowed) return reject(why);
    const auto object = std::find_if(planet->objects.begin(), planet->objects.end(),
        [&](const auto& row) { return row.id == order.object; });
    const auto player = std::lower_bound(production->players.begin(), production->players.end(), issuer->player,
        [](const auto& row, auto id) { return row.player < id; });
    if (player == production->players.end() || player->player != issuer->player) return reject(ProductionAdmission::unavailable);
    const auto index = static_cast<std::size_t>(player - production->players.begin());
    const auto* type = offer(*planet, sale.type);
    auto& account = economy->players[index];
    const auto refund = sale.refund;
    const auto balance = adjusted_credits(account.credits, refund,
        setup.fiscal->players[index].positive_credit_multiplier, account.credit_cap);
    if (!balance) return reject(ProductionAdmission::resource_limit);
    event.value = sale.type;
    if (sale.unit) {
        // One identity's payload tree is removed in this ordered command commit, never a tick-wide scan.
        std::set<EntityId> removed{order.object};
        if (movement_inputs) {
            std::vector<EntityId> sale_descendants{order.object};
            for (std::size_t i = 0; i < sale_descendants.size(); ++i) {
                const auto at = std::lower_bound(movement_inputs->contents.begin(), movement_inputs->contents.end(), sale_descendants[i],
                    [](const auto& row, auto key) { return row.container < key; });
                if (at == movement_inputs->contents.end() || at->container != sale_descendants[i]) continue;
                for (const auto& child : at->children) if (removed.insert(child.id).second) sale_descendants.push_back(child.id);
            }
        }
        for (const auto& fleet : sorted_fleets()) {
            if (std::none_of(fleet.members.begin(), fleet.members.end(), [&](const auto& row) { return row.id == order.object; })) continue;
            auto& live = registry.get<session_detail::FleetData>(fleets.at(fleet.id));
            std::erase_if(live.members, [&](const auto& row) { return row.id == order.object; });
            if (live.members.empty()) {
                lose_actor(fleet.id); registry.destroy(fleets.at(fleet.id)); fleets.erase(fleet.id); --live_object_count;
                if (production->fleet_blockers) std::erase_if(*production->fleet_blockers,
                    [&](const auto& row) { return row.fleet == fleet.id; });
                events.push_back({tick, GalacticEventKind::fleet_destroyed, command.key.seat, fleet.id,
                    command.key.sequence, 0, GalacticRejectReason::none});
            }
            break;
        }
        if (control) for (auto& row : control->planets) {
            std::erase_if(row.landed, [&](const auto& unit) { return removed.contains(unit.id); });
            const auto producer = std::lower_bound(production->planets.begin(), production->planets.end(), row.planet,
                [](const auto& value, auto key) { return value.planet < key; });
            if (producer != production->planets.end() && producer->planet == row.planet)
                producer->landed_transports = static_cast<std::int64_t>(row.landed.size());
        }
        lose_actor(order.object);
        if (movement_inputs) {
            auto movement = std::make_shared<MovementSetup>(*movement_inputs);
            std::erase_if(movement->contents, [&](const auto& row) { return removed.contains(row.container); });
            movement_inputs = std::move(movement);
        }
        for (auto& row : production->planets)
            std::erase_if(row.objects, [&](const auto& value) { return removed.contains(value.id); });
        std::erase_if(production_originals, [&](const auto& row) { return removed.contains(row.id); });
        published_production_originals.reset();
        const auto count = std::lower_bound(player->counts.begin(), player->counts.end(), sale.type,
            [](const auto& row, auto key) { return row.type < key; });
        if (count != player->counts.end() && count->type == sale.type && count->owned) --count->owned;
        if (type) player->population_used = std::max(Fixed{}, math::subtract(player->population_used, type->population).value());
        live_object_count -= removed.size();
        dirty[static_cast<std::size_t>(StateSection::fleets)] = true;
        dirty[static_cast<std::size_t>(StateSection::movement)] = true;
        dirty[static_cast<std::size_t>(StateSection::control)] = true;
        dirty[static_cast<std::size_t>(StateSection::fleet_control)] = true;
    } else {
        lose_actor(object->id);
        const auto count = std::lower_bound(player->counts.begin(), player->counts.end(), object->type,
            [](const auto& row, auto id) { return row.type < id; });
        if (count != player->counts.end() && count->type == object->type && count->owned) --count->owned;
        player->population_used = std::max(Fixed{}, math::subtract(player->population_used, type->population).value());
        if (std::erase_if(production_originals, [&](const auto& row) { return row.id == object->id; }))
            published_production_originals.reset();
        const bool special = object->kind == ProductionKind::structure && object->domain == ProductionDomain::ground;
        planet->objects.erase(object); --live_object_count;
        // WGM-188: removing a ground special refreshes the ground base level.
        if (special) refresh_ground_base_level(static_cast<std::size_t>(planet - production->planets.begin()));
    }
    account.credits = balance.value();
    invalidate_income_inputs();
    population_stale = true; dirty[8] = true; dirty[1] = true; dirty[0] = true;
    invalidate_observer_projection();
    events.push_back(event);
    // GHUD-S84: a zero refund does not call the positive-credit writer or its credit-goal callback.
    if (refund.raw() > 0) {
        if (auto result = test_credit_condition(issuer->player, tick, events); !result) failed = true;
    }
}

void GalacticSession::Impl::execute_production(const GameCommand& command, std::uint64_t tick, std::vector<GalacticEvent>& events) {
    const auto& order = std::get<ProductionCommand>(command.payload);
    GalacticEvent event{tick, GalacticEventKind::command_executed, command.key.seat, order.planet, command.key.sequence, order.type, GalacticRejectReason::none};
    const auto reject = [&](ProductionAdmission why) { event.kind = GalacticEventKind::command_rejected; event.reason = GalacticRejectReason::production_admission; event.value = static_cast<std::uint64_t>(why); events.push_back(event); };
    const auto* issuer = seat(command.key.seat);
    if (!issuer || !issuer->active) { event.kind = GalacticEventKind::command_rejected; event.reason = GalacticRejectReason::inactive_seat; events.push_back(event); return; }
    if (!production || !planets.contains(order.planet)) return reject(ProductionAdmission::unavailable);
    auto& state = *production;
    auto& planet = *std::lower_bound(state.planets.begin(), state.planets.end(), order.planet, [](const auto& row, auto id) { return row.planet < id; });
    const auto producer_index = static_cast<std::size_t>(&planet - state.planets.data());
    if (registry.get<session_detail::PlanetData>(planets.at(order.planet)).owner != issuer->player) return reject(ProductionAdmission::ownership);
    const auto player_index = static_cast<std::size_t>(std::lower_bound(state.players.begin(), state.players.end(), issuer->player, [](const auto& row, auto id) { return row.player < id; }) - state.players.begin());
    auto& player = state.players[player_index]; auto& account = economy->players[player_index];
    auto& queue = planet.queues[static_cast<std::size_t>(order.domain)];
    const auto* type = offer(planet, order.type);
    std::optional<ProductionOffer> adjusted;
    if (type && order.action == ProductionAction::buy) {
        const auto sources = income ? std::optional(live_income_inputs()) : std::nullopt;
        auto prepared = prepared_production_offer(*type, sources ? &*sources : nullptr,
            sorted_planets(), order.planet, issuer->player, economy_corruption ? &*economy_corruption : nullptr);
        if (!prepared) return reject(ProductionAdmission::resource_limit);
        adjusted = std::move(prepared).value();
        type = &*adjusted;
    }
    const auto changed = [&] {
        population_stale = true;
        dirty[8] = true; dirty[1] = true; dirty[0] = true;
        if (planet.queues[0].empty() && planet.queues[1].empty()) active_producers.erase(producer_index);
        else active_producers.insert(producer_index);
    };
    const auto notify = [&](GalacticEventKind kind, const ProductionEntry& entry) {
        auto e = event; e.kind = kind; e.value = entry.type; e.sequence = entry.build_id; events.push_back(e);
        if (kind == GalacticEventKind::production_cancelled && native_production_outcome_handler)
            native_production_outcome_handler({entry.builder, entry.build_id, 0, false});
    };
    const auto refund = [&](const ProductionEntry& entry) {
        // WGEP-64: builder equality is a debug assertion, not a release gate.
        // The explicit caller supplies the refund recipient (the issuing player).
        assert(entry.builder == issuer->player);
        const auto& factor = setup.fiscal->players[player_index];
        auto balance = adjusted_credits(account.credits, paid(entry), factor.positive_credit_multiplier, account.credit_cap);
        if (!balance) return false;
        account.credits = balance.value();
        // WGEP-16/46/64: refund admission uses the committed recipient balance.
        if (auto v = test_credit_condition(issuer->player, tick, events); !v) { failed = true; return false; }
        return true;
    };
    if (order.action != ProductionAction::buy) {
        std::size_t index = order.index;
        if (order.action == ProductionAction::stop) {
            events.push_back(event);
            const auto cancelled = queue;
            queue.clear(); changed();
            for (const auto& entry : cancelled) notify(GalacticEventKind::production_cancelled, entry);
            return; // WGEP-56: bulk stop has no direct refund
        }
        if (!type || type->domain != order.domain) return reject(ProductionAdmission::wrong_entry);
        if (order.action == ProductionAction::cancel_type) {
            index = queue.size();
            while (index > 0 && queue[index - 1].type != order.type) --index;
            if (index == 0) return reject(ProductionAdmission::wrong_entry);
            --index;
        }
        if (index >= queue.size() || queue[index].type != order.type) return reject(ProductionAdmission::wrong_entry);
        if (order.target_build_id && queue[index].build_id != *order.target_build_id)
            return reject(ProductionAdmission::stale_entry);
        if (!refund(queue[index])) return reject(ProductionAdmission::resource_limit);
        const auto cancelled = queue[index];
        events.push_back(event);
        queue.erase(queue.begin() + static_cast<std::ptrdiff_t>(index));
        if (index == 0 && !queue.empty()) start(queue.front(), clock.galactic_frame);
        changed(); notify(GalacticEventKind::production_cancelled, cancelled); return;
    }
    const auto why = production_admission({state, planet, player, account, sorted_fleets(),
        registry.get<session_detail::PlanetData>(planets.at(order.planet)).owner,
        setup.census ? &population[player_index] : nullptr, next_id, live_object_count, nullptr, corruption_local_unlocks(order.planet, issuer->player)}, type, order.domain);
    if (why != ProductionAdmission::allowed) return reject(why);
    ProductionEntry entry{type->type, issuer->player, next_id++, type->price, type->seconds, type->seconds, clock.galactic_frame, production_frames(type->seconds).value(), player.pay_as_you_go};
    if (!entry.pay_as_you_go) {
        account.credits = adjusted_credits(account.credits, math::negate(entry.price).value(), one, account.credit_cap).value();
        // WGEP-16: debit admission reads the post-purchase account, not its prior balance.
        if (auto v = test_credit_condition(issuer->player, tick, events); !v) { failed = true; return; }
    }
    events.push_back(event); queue.push_back(entry); notify(GalacticEventKind::production_started, entry); changed();
}

core::Result<NestedCreationResult> place_nested_force(const NestedCreationSetup& inputs, TypeId type_id,
    PlayerId owner, PlanetId location, EntityId first_free, std::span<const Fleet> fleets,
    const MovementSetup& movement, const PoliticalControlSetup& control,
    const std::function<core::Result<bool>(const Fleet&)>& combat_skip) {
    const auto fail = [](const char* message) {
        return core::Result<NestedCreationResult>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, message));
    };
    const auto type = [&](TypeId id) -> const NestedCreationType* {
        const auto at = std::lower_bound(inputs.types.begin(), inputs.types.end(), id,
            [](const auto& row, auto key) { return row.type < key; });
        return at != inputs.types.end() && at->type == id ? &*at : nullptr;
    };
    const auto* source = type(type_id);
    const auto planet = std::find_if(control.planets.begin(), control.planets.end(),
        [&](const auto& row) { return row.planet == location; });
    if (!source || !type(inputs.fleet_type) || !owner || !first_free || planet == control.planets.end()
        || source->members.empty() || (source->role != NestedCreationRole::company && source->role != NestedCreationRole::squadron))
        return fail("nested creation requires original bindings and live planet presence");
    const bool company = source->role == NestedCreationRole::company;
    if (company && !type(source->transport)) return fail("nested company transport is unbound");
    for (auto member : source->members) if (!type(member)) return fail("nested creation member is unbound");
    bool hostile = false;
    if (company) for (const auto& fleet : fleets) if (fleet.orbit == location) {
        const auto relation = std::find_if(inputs.relations.begin(), inputs.relations.end(),
            [&](const auto& row) { return row.player == owner && row.other == fleet.owner; });
        if (relation == inputs.relations.end()) return fail("nested company enemy relation is absent");
        if (!relation->enemy) continue;
        if (combat_skip) {
            auto skip = combat_skip(fleet);
            if (!skip) return core::Result<NestedCreationResult>::failure(skip.error());
            if (!skip.value()) { hostile = true; break; }
            continue;
        }
        std::vector<FleetMember> pending(fleet.members.rbegin(), fleet.members.rend());
        std::set<EntityId> visited; bool any = false, all = true;
        while (!pending.empty()) {
            const auto member = pending.back(); pending.pop_back();
            if (!visited.insert(member.id).second) return fail("nested orbital membership is cyclic or repeated");
            const auto* original = type(member.type);
            if (!original) return fail("nested orbital original type is unbound");
            const auto children = std::lower_bound(movement.contents.begin(), movement.contents.end(), member.id,
                [](const auto& row, auto id) { return row.container < id; });
            const bool has_children = children != movement.contents.end() && children->container == member.id;
            if (original->role == NestedCreationRole::transport && (!has_children || children->children.empty())) {
                all = false; any = true; break; // WGS-92: an empty transport never skips.
            }
            if (has_children) pending.insert(pending.end(), children->children.rbegin(), children->children.rend());
            else { any = true; all &= original->skips_combat; }
        }
        if (!(any && all)) { hostile = true; break; }
    }
    NestedCreationResult out; out.next_id = first_free;
    const auto allocate = [&]() -> EntityId {
        if (out.next_id == std::numeric_limits<EntityId>::max()) return 0;
        return out.next_id++;
    };
    const auto metadata = [&](FleetMember member, TypeId original, EntityId parent, EntityId generated_source = 0) {
        out.originals.push_back({member.id, original, owner, location, parent, false, false, member.type, generated_source, ActorCombatRole::unavailable, {}, {}});
    };
    EntityId outer = company ? 0 : allocate();
    const FleetMember container{allocate(), company ? source->transport : source->type};
    if (!company && !type(inputs.squadron_type)) return fail("current_representation_squadron_container_unbound");
    MovementContents contents; contents.container = container.id;
    bool accumulated_stealth = false, seek_guard = false;
    bool land = company && planet->surface_accessible && planet->landed.size() < control.max_ground_forces;
    for (auto id : source->members) {
        const auto* live = type(id); const FleetMember member{allocate(), id};
        contents.children.push_back(member); metadata(member, id, container.id);
        if (!accumulated_stealth && live->skips_combat) { accumulated_stealth = true; seek_guard = true; }
        if (live->always_spawn_in_orbit && (accumulated_stealth || !hostile)) land = false;
    }
    if (land) {
        out.landed.push_back({container.id, container.type, owner}); metadata(container, source->type, location);
    } else {
        std::uint8_t slot = 0;
        if (company && seek_guard) for (const std::uint8_t candidate : {std::uint8_t{1}, std::uint8_t{2}})
            if (std::none_of(fleets.begin(), fleets.end(), [&](const auto& row) {
                return row.orbit == location && row.orbit_slot == candidate;
            })) { slot = candidate; break; }
        if (company) outer = allocate();
        out.fleets.push_back({outer, owner, location, {container}, slot, {}});
        metadata({outer, inputs.fleet_type}, inputs.fleet_type, location); metadata(container, source->type, outer);
    }
    out.contents.push_back(std::move(contents));
    // CRP-01: the strategic movement table retains its represented template;
    // the actor record retains the literal current squadron container identity.
    if (!company) for (auto& row : out.originals) if (row.id == container.id) row.current_type = inputs.squadron_type;
    for (auto& actor : out.originals) {
        const auto* current = type(actor.current_type);
        if (!current) return fail("actor_constructor_facts_unavailable");
        actor.combat_role = current->combat_role; actor.abilities = current->abilities;
        actor.garrison = false; // GAG-01: ordinary construction, not tactical garrison spawning.
    }
    if (std::any_of(out.originals.begin(), out.originals.end(), [](const auto& row) { return !row.id; }))
        return fail("nested allocation frontier exhausted");
    std::sort(out.originals.begin(), out.originals.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    return core::Result<NestedCreationResult>::success(std::move(out));
}

core::Result<session_detail::NativeProductionFinish> session_detail::prepare_native_production_finish(
    const ProductionSetup& production, std::span<const Planet> planets, EntityId build, bool salvage) {
    using Result = core::Result<NativeProductionFinish>;
    for (std::size_t index = 0; index < production.planets.size(); ++index) {
        const auto& planet = production.planets[index];
        for (std::size_t domain = 0; domain < planet.queues.size(); ++domain) {
            const auto& queue = planet.queues[domain];
            const auto at = std::find_if(queue.begin(), queue.end(), [&](const auto& row) { return row.build_id == build; });
            if (at == queue.end()) continue;
            const auto owner = std::lower_bound(planets.begin(), planets.end(), planet.planet, [](const auto& row, auto id) { return row.id < id; })->owner;
            const auto payer = std::find_if(production.players.begin(), production.players.end(), [&](const auto& row) { return row.player == owner; });
            if (payer == production.players.end()) return Result::failure(detail::diagnostic(diagnostic_codes::invalid_setup,
                "production: finished task requires the current planetary owner"));
            if (salvage || !payer->pay_as_you_go) return Result::success({});
            assert(at->builder == owner);
            return Result::success({true, index, domain, static_cast<std::size_t>(at - queue.begin()), *at, owner});
        }
    }
    return Result::success({});
}
core::Result<void> GalacticSession::finish_native_production_task(EntityId build, bool salvage, Fixed multiplier) {
    auto& impl = *impl_;
    if (impl.failed || !impl.production || !impl.economy) return invalid("finished task requires its live producer");
    if (auto* shadow = native_shadow()) return stage_native_production_finish(*shadow, build, salvage, multiplier);
    auto checked = session_detail::prepare_native_production_finish(*impl.production, impl.sorted_planets(), build, salvage);
    if (!checked) return core::Result<void>::failure(checked.error());
    const auto& prepared = checked.value();
    if (!prepared.cancel) return core::Result<void>::success();
    auto& planet = impl.production->planets[prepared.planet_index]; auto& queue = planet.queues[prepared.domain_index];
    auto refund = add_credits(prepared.owner, paid(prepared.entry), multiplier);
    if (!refund) return core::Result<void>::failure(refund.error());
    queue.erase(queue.begin() + static_cast<std::ptrdiff_t>(prepared.entry_index));
    if (prepared.entry_index == 0 && !queue.empty()) start(queue.front(), impl.clock.galactic_frame);
    if (planet.queues[0].empty() && planet.queues[1].empty()) impl.active_producers.erase(prepared.planet_index);
    impl.population_stale = true; impl.dirty[8] = impl.dirty[1] = impl.dirty[0] = true;
    const auto old = impl.snapshot->events(); std::vector<GalacticEvent> events(old.begin(), old.end());
    events.push_back({impl.completed_tick, GalacticEventKind::production_cancelled, {}, planet.planet,
        prepared.entry.build_id, prepared.entry.type, GalacticRejectReason::none});
    if (impl.native_production_outcome_handler)
        impl.native_production_outcome_handler({prepared.entry.builder, prepared.entry.build_id, 0, false});
    impl.snapshot = impl.publish_native_snapshot(std::move(events));
    return core::Result<void>::success();
}

core::Result<void> GalacticSession::Impl::service_production(const PartitionExecutor& executor, std::uint64_t tick,
    std::vector<GalacticEvent>& events, bool pending_conflict) {
    production_producers = 0; production_entries = 0;
    if (!production || active_producers.empty()) return core::Result<void>::success();
    const std::vector<std::size_t> producers(active_producers.begin(), active_producers.end());
    using Queues = std::array<std::vector<ProductionEntry>, 2>;
    std::vector<Queues> staged(producers.size());
    const auto fleet_inputs = sorted_fleets();
    struct Notices { std::vector<ProductionEntry> cancelled; std::uint64_t visited{}; bool blocked{}; };
    std::vector<Notices> notices(producers.size());
    // WGEP-35: per-planet prerequisite sweep uses copied inputs and disjoint outputs.
    const auto phase = executor.execute_phase("galactic-production", tick_partition_count, [&](std::size_t partition) {
        const auto range = partition_range(partition, staged.size());
        for (auto i = range.begin; i < range.end; ++i) {
            const auto& planet = production->planets[producers[i]];
            staged[i] = planet.queues;
            for (auto& queue : staged[i]) for (std::size_t n = queue.size(); n > 0; --n) {
                ++notices[i].visited;
                const auto index = n - 1; const auto* type = offer(planet, queue[index].type);
                if (!prerequisites(planet, *type, fleet_inputs)) {
                    notices[i].cancelled.push_back(queue[index]); queue.erase(queue.begin() + static_cast<std::ptrdiff_t>(index));
                    if (index == 0 && !queue.empty()) start(queue.front(), clock.galactic_frame);
                }
            }
            // GFD-09: revalidation precedes front countdown/completion,
            // after prerequisite removal has selected the actual space front.
            if (!pending_conflict && !staged[i][1].empty()
                && offer(planet, staged[i][1].front().type)->kind == ProductionKind::starbase)
                notices[i].blocked = starbase_blocked(*production, planet, staged[i][1].front().builder, fleet_inputs);
        }
    });
    if (!phase) return core::Result<void>::failure(phase.error());
    production_producers = producers.size();
    // Ordered commit is required: two planets share one account and population census.
    // Only the two fronts per planet are committed here; per-entry revalidation is partitioned.
    for (std::size_t i = 0; i < producers.size(); ++i) {
        auto& planet = production->planets[producers[i]];
        production_entries += notices[i].visited;
        planet.queues = std::move(staged[i]);
        const auto notify = [&](GalacticEventKind kind, const ProductionEntry& entry) {
            events.push_back({tick, kind, {}, planet.planet, entry.build_id, entry.type, GalacticRejectReason::none});
            if (kind == GalacticEventKind::production_cancelled && native_production_outcome_handler)
                native_production_outcome_handler({entry.builder, entry.build_id, 0, false});
        };
        for (const auto& entry : notices[i].cancelled) notify(GalacticEventKind::production_cancelled, entry);
        for (std::size_t domain = 0; domain < 2; ++domain) {
            auto& queue = planet.queues[domain]; if (queue.empty()) continue;
            auto& front = queue.front(); const auto* type = offer(planet, front.type);
            if ((domain == 1 && type->kind != ProductionKind::starbase && planet.starbase_owner != front.builder)
                || (type->kind == ProductionKind::starbase && !pending_conflict && notices[i].blocked)) {
                const auto cancelled = front; queue.erase(queue.begin());
                if (!queue.empty()) start(queue.front(), clock.galactic_frame);
                dirty[8] = true; dirty[0] = true; population_stale = true;
                notify(GalacticEventKind::production_cancelled, cancelled);
                break; // WGEP-37/38: return from this planet's service
            }
            auto& player = *std::find_if(production->players.begin(), production->players.end(), [&](const auto& row) { return row.player == front.builder; });
            auto& account = *std::find_if(economy->players.begin(), economy->players.end(), [&](const auto& row) { return row.player == front.builder; });
            const auto current = remaining(front, clock.galactic_frame);
            if (current.raw() == 0) {
                if (domain == 0 && planet.landed_transports >= planet.maximum_ground_forces && planet.orbit_occupier != 0 && planet.orbit_occupier != planet.ground_occupier) continue;
                const auto finished = front; queue.erase(queue.begin());
                notify(GalacticEventKind::production_completed, finished);
                // WGP-99: scoring counts the completion for the planet's current owner.
                record_build(registry.get<session_detail::PlanetData>(planets.at(planet.planet)).owner, planet.planet, finished.type);
                EntityId completed_object{};
                auto count = std::lower_bound(player.counts.begin(), player.counts.end(), type->type, [](const auto& row, auto id) { return row.type < id; });
                if (count == player.counts.end() || count->type != type->type) count = player.counts.insert(count, {type->type, 0, 0});
                if (type->kind == ProductionKind::technology) {
                    auto assigned = assign_technology({finished.builder, type->target_tech, true, true});
                    if (!assigned) return core::Result<void>::failure(assigned.error());
                    ++count->historical; // WGEP-137: setter/story/unlocks precede finished count.
                } else if (type->kind == ProductionKind::starbase) {
                    ++count->historical;
                    if (planet.starbase_type != 0) {
                        auto previous = std::find_if(player.counts.begin(), player.counts.end(), [&](const auto& row) { return row.type == planet.starbase_type; });
                        if (previous != player.counts.end() && previous->owned > 0) --previous->owned;
                    }
                    planet.space_level = type->base_level; planet.starbase_owner = finished.builder; planet.starbase_type = type->type; ++count->owned;
                    if (control) {
                        // WGEP-40 / WGM-66..68: committed station presence feeds
                        // the next admitted political-control service.
                        control->planets[producers[i]].starbase = {
                            finished.builder, static_cast<std::uint32_t>(type->base_level)};
                        dirty[static_cast<std::size_t>(StateSection::control)] = true;
                    }
                    if (type->base_level == 1) {
                        // WGEP-41: clearing level-one orbital facilities also releases
                        // their live identities and the supplied owned-count seam.
                        for (const auto& object : planet.objects) if (object.domain == ProductionDomain::space && object.kind == ProductionKind::structure) {
                            --live_object_count;
                            lose_actor(object.id);
                            auto& owner = *std::find_if(production->players.begin(), production->players.end(), [&](const auto& row) { return row.player == object.owner; });
                            auto existing = std::find_if(owner.counts.begin(), owner.counts.end(), [&](const auto& row) { return row.type == object.type; });
                            if (existing != owner.counts.end() && existing->owned > 0) --existing->owned;
                            if (const auto* removed = offer(planet, object.type)) owner.population_used = std::max(Fixed{}, math::subtract(owner.population_used, removed->population).value());
                        }
                        std::erase_if(planet.objects, [](const auto& object) { return object.domain == ProductionDomain::space && object.kind == ProductionKind::structure; });
                    }
                } else if (production->nested_creation && std::any_of(production->nested_creation->types.begin(),
                    production->nested_creation->types.end(), [&](const auto& row) {
                        return row.type == type->type && (row.role == NestedCreationRole::company || row.role == NestedCreationRole::squadron);
                    })) {
                    ++count->historical;
                    if (!control || !setup.movement || !setup.census) return invalid("nested completion requires presence and census services");
                    auto placed = place_nested_force(*production->nested_creation, type->type, finished.builder,
                        planet.planet, next_id, sorted_fleets(), *movement_inputs, *control,
                        [&](const Fleet& fleet) { return fleet_combat_skip(fleet, actor_inputs(), movement_inputs.get()); });
                    if (!placed) return core::Result<void>::failure(placed.error());
                    auto& result = placed.value();
                    const auto actor = std::find_if(result.originals.begin(), result.originals.end(),
                        [&](const auto& row) { return row.original_type == type->type; });
                    if (actor != result.originals.end()) completed_object = actor->id;
                    if (actor != result.originals.end()) actor->paid_cost = paid(finished);
                    const auto needed = result.next_id - next_id;
                    if (needed > galactic_max_objects || live_object_count > galactic_max_objects - needed)
                        return invalid("nested result object budget exhausted");
                    next_id = result.next_id; live_object_count += static_cast<std::size_t>(needed);
                    for (const auto& fleet : result.fleets) insert(fleet);
                    auto& presence = control->planets[producers[i]];
                    presence.landed.insert(presence.landed.end(), result.landed.begin(), result.landed.end());
                    planet.landed_transports = static_cast<std::int64_t>(presence.landed.size());
                    auto movement = std::make_shared<MovementSetup>(*movement_inputs);
                    movement->contents.insert(movement->contents.end(), result.contents.begin(), result.contents.end());
                    std::sort(movement->contents.begin(), movement->contents.end(),
                        [](const auto& a, const auto& b) { return a.container < b.container; });
                    movement_inputs = std::move(movement);
                    production_originals.insert(production_originals.end(), result.originals.begin(), result.originals.end());
                    for (const auto& row : result.originals) if (!row.generated_source) record_actor(row, NativeActorChange::Kind::created);
                    for (const auto& row : result.originals) if (row.generated_source) record_actor(row, NativeActorChange::Kind::created);
                    publish_surface_arrivals(planet.planet, result.landed);
                    published_production_originals.reset();
                    if (production->fleet_blockers) for (const auto& fleet : result.fleets) {
                        // WGS-92: every live leaf must skip; a guard slot alone is not a predicate.
                        bool skips = true;
                        for (const auto& contents : result.contents) for (const auto& member : contents.children) {
                            const auto live = std::lower_bound(production->nested_creation->types.begin(), production->nested_creation->types.end(), member.type,
                                [](const auto& row, auto id) { return row.type < id; });
                            skips &= live->skips_combat;
                        }
                        production->fleet_blockers->push_back({fleet.id, false, skips, true});
                    }
                    invalidate_observer_projection();
                    dirty[static_cast<std::size_t>(StateSection::fleets)] = true;
                    dirty[static_cast<std::size_t>(StateSection::fleet_control)] = true;
                    dirty[static_cast<std::size_t>(StateSection::movement)] = true;
                    dirty[static_cast<std::size_t>(StateSection::control)] = true;
                    dirty[static_cast<std::size_t>(StateSection::census)] = true;
                } else {
                    ++count->historical;
                    const auto needed = domain == 1 && type->kind == ProductionKind::unit ? std::size_t{2} : std::size_t{1};
                    if (live_object_count > galactic_max_objects - needed || next_id >= std::numeric_limits<EntityId>::max() - needed) return invalid("result allocator exhausted");
                    const auto id = next_id++; ++count->owned;
                    completed_object = id;
                    record_actor({id, type->type, finished.builder, planet.planet,
                        domain == 1 && type->kind == ProductionKind::unit ? EntityId{} : planet.planet,
                        false, false, type->type, 0, ActorCombatRole::unavailable, {}, {}, paid(finished)}, NativeActorChange::Kind::created);
                    player.population_used = math::add(player.population_used, type->population).value();
                    if (domain == 1 && type->kind == ProductionKind::unit) {
                        detached_units.emplace(id, DetachedUnit{{id, type->type}, finished.builder, planet.planet}); ++live_object_count;
                        const auto issuer = std::find_if(seats.begin(), seats.end(), [&](const auto& row) { return row.player == finished.builder; });
                        if (issuer == seats.end()) return invalid("orbital placement needs a live controller");
                        // WGEP-40: authoritative completion uses the fleet commit primitives;
                        // UI admission (pause/speech/controller activity) does not gate creation.
                        execute_fleet({{tick, issuer->seat_id, finished.build_id}, CreateFleet{planet.planet, {id}, 0}}, tick, events, false, true);
                        planet.objects.push_back({id, type->type, finished.builder, paid(finished), type->domain, type->kind});
                    } else {
                        planet.objects.push_back({id, type->type, finished.builder, paid(finished), type->domain, type->kind}); ++live_object_count;
                        // WGS-93: a completed ground special raises the planet's ground base level on the spot.
                        if (type->kind == ProductionKind::structure && type->domain == ProductionDomain::ground) refresh_ground_base_level(producers[i]);
                    }
                }
                invalidate_income_inputs(); // an economic object may have entered the live roster
                if (native_production_outcome_handler)
                    native_production_outcome_handler({finished.builder, finished.build_id, completed_object, true});
                if (!queue.empty()) start(queue.front(), clock.galactic_frame);
            } else {
                if (front.pay_as_you_go && front.full_seconds.raw() > 0) {
                    const auto elapsed = std::max(Fixed{}, math::subtract(front.last_remaining, current).value());
                    const auto due = math::multiply(front.price, math::divide(elapsed, front.full_seconds).value()).value();
                    PlayerId payer = front.builder; auto charge = due; bool accepted = account.credits >= due;
                    if (!player.human) {
                        if (!native_production_handler) return invalid("native AI billing requires the reconstructed ledger handler");
                        const auto owner = registry.get<session_detail::PlanetData>(planets.at(planet.planet)).owner;
                        auto payment = native_production_handler({NativeProductionNotice::Kind::billed, front.builder,
                            front.build_id, due, front.price, current, owner});
                        if (!payment) return core::Result<void>::failure(payment.error());
                        accepted = payment.value().accepted; payer = payment.value().payer; charge = payment.value().amount;
                    }
                    if (!accepted) {
                        front.start_frame = clock.galactic_frame;
                        front.duration_frames = production_frames(front.last_remaining).value();
                        continue;
                    }
                    auto recipient = std::find_if(economy->players.begin(), economy->players.end(), [&](const auto& row) { return row.player == payer; });
                    if (recipient == economy->players.end()) return invalid("native bill names an absent account");
                    recipient->credits = adjusted_credits(recipient->credits, math::negate(charge).value(), one, recipient->credit_cap).value();
                    // WGEP-65/WGA-157: account clamps independently; ledger spends the approved amount.
                    if (auto v = test_credit_condition(payer, tick, events); !v) return v;
                }
                front.last_remaining = current;
            }
        }
        if (planet.queues[0].empty() && planet.queues[1].empty()) active_producers.erase(producers[i]);
    }
    population_stale = true;
    dirty[8] = true; dirty[1] = true; dirty[0] = true;
    return core::Result<void>::success();
}
} // namespace eawr::sim::galactic
