#include "eawr/sim/galactic/economy_corruption.hpp"
#include "session_impl.hpp"
#include "encoding.hpp"
#include "../replay_internal.hpp"

#include <algorithm>
#include <limits>

namespace eawr::sim::galactic {
namespace {
using math::Fixed;
namespace cf = corruption_fixture;
template<class T> core::Result<T> fail(std::string message) {
    return core::Result<T>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "corruption economy: " + message));
}
template<class Rows, class Key> bool ordered(const Rows& rows, Key key) {
    return std::adjacent_find(rows.begin(), rows.end(), [&](const auto& a, const auto& b) { return key(a) >= key(b); }) == rows.end();
}
template<class Rows, class Id, class Key> auto find(Rows& rows, Id id, Key key) {
    const auto at = std::lower_bound(rows.begin(), rows.end(), id, [&](const auto& row, auto value) { return key(row) < value; });
    return at != rows.end() && key(*at) == id ? &*at : nullptr;
}
bool bounded(Fixed n) { return n.raw() >= -(std::int64_t{1} << 48) && n.raw() <= (std::int64_t{1} << 48); }
} // namespace

core::Result<Fixed> technology_purchase_multiplier(std::span<const TechnologyPriceModifier> modifiers) {
    if (modifiers.size() > galactic_max_objects || !ordered(modifiers, [](const auto& m) { return m.source; }))
        return fail<Fixed>("invalid technology-price ledger order or size");
    Fixed total = cf::one;
    std::int64_t magnitude{};
    for (const auto& modifier : modifiers) {
        if (!modifier.source || !bounded(modifier.additive)
            || (modifier.first_expired_frame && *modifier.first_expired_frame > galactic_max_ticks))
            return fail<Fixed>("invalid technology-price modifier");
        const auto raw = modifier.additive.raw();
        // Every subset remains safe after expiry, independently of cancellation.
        magnitude += raw < 0 ? -raw : raw;
        if (magnitude > (std::int64_t{1} << 48) - Fixed::scale) return fail<Fixed>("technology-price ledger exceeds arithmetic bounds");
        auto next = math::add(total, modifier.additive);
        if (!next) return core::Result<Fixed>::failure(next.error());
        total = next.value();
    }
    return core::Result<Fixed>::success(total > Fixed{} ? total : Fixed::from_decimal("0.0001").value());
}

core::Result<void> GalacticSession::Impl::refresh_planet_technology_price(PlanetId planet, bool ownership, bool remove) {
    if (!income) return core::Result<void>::success();
    const auto& inputs = income->inputs;
    const auto* source = find(inputs.sources, planet, [](const auto& row) { return row.id; });
    if (!source || !source->planet_source || source->planet != planet) return core::Result<void>::success();
    const auto* type = find(inputs.types, source->type, [](const auto& row) { return row.type; });
    if (!type || !type->technology_price_reduction) return core::Result<void>::success();
    if (!economy_corruption) return fail<void>("planet technology ability requires a player ledger");
    const auto* location = find(inputs.planets, planet, [](const auto& row) { return row.planet; });
    const auto* economic = find(economy_corruption->planets, planet, [](const auto& row) { return row.planet; });
    const auto* surface = control ? find(control->planets, planet, [](const auto& row) { return row.planet; }) : nullptr;
    const auto owner = registry.get<session_detail::PlanetData>(planets.at(planet)).owner;
    const bool destroyed = !location || location->destroyed || !economic || economic->destroyed || (surface && surface->destroyed);
    // WGEP-167/169: ownership delivery calls registration directly, without the
    // ordinary application gate for intimidation. A corruption change uses that gate.
    const bool enabled = !remove && source->enabled && !destroyed && (ownership
        || (!location->intimidation && economic->corruption != corruption_fixture::Type::intimidation));
    const auto additive = math::subtract({}, *type->technology_price_reduction);
    if (!additive) return core::Result<void>::failure(additive.error());
    // One planet callback commits only its own source, preserving manual writers
    // and other planets. Stage all affected player rows before changing any ledger.
    auto players = economy_corruption->players;
    for (auto& player : players) {
        auto& rows = player.technology_price_modifiers;
        const auto at = std::lower_bound(rows.begin(), rows.end(), source->id,
            [](const auto& row, auto id) { return row.source < id; });
        const bool present = at != rows.end() && at->source == source->id;
        if (enabled && player.player == owner) {
            const TechnologyPriceModifier modifier{source->id, additive.value(), {}};
            if (present) *at = modifier; else rows.insert(at, modifier);
        } else if (present) rows.erase(at);
        const auto multiplier = technology_purchase_multiplier(rows);
        if (!multiplier) return core::Result<void>::failure(multiplier.error());
        player.tech_purchase_multiplier = multiplier.value();
    }
    if (players != economy_corruption->players) {
        economy_corruption->players = std::move(players);
        dirty[static_cast<std::size_t>(StateSection::economy_corruption)] = true;
    }
    return core::Result<void>::success();
}

void GalacticSession::Impl::rebuild_technology_price_expiry() {
    technology_price_expiry.clear();
    if (economy_corruption) for (const auto& player : economy_corruption->players)
        for (const auto& modifier : player.technology_price_modifiers) if (modifier.first_expired_frame)
            technology_price_expiry.emplace(*modifier.first_expired_frame, player.player, modifier.source);
}
void GalacticSession::Impl::expire_technology_prices() {
    if (!economy_corruption) { technology_price_expiry.clear(); return; }
    // Ordered sparse expiry commits: idle frames neither scan players nor allocate.
    while (!technology_price_expiry.empty() && std::get<0>(*technology_price_expiry.begin()) <= clock.galactic_frame) {
        const auto [frame, player_id, source] = *technology_price_expiry.begin();
        technology_price_expiry.erase(technology_price_expiry.begin());
        auto* player = find(economy_corruption->players, player_id, [](const auto& p) { return p.player; });
        if (!player) continue; // A stale derived entry cannot dereference an absent owner.
        auto& rows = player->technology_price_modifiers;
        const auto at = std::lower_bound(rows.begin(), rows.end(), source, [](const auto& m, auto id) { return m.source < id; });
        if (at == rows.end() || at->source != source || at->first_expired_frame != frame) continue;
        rows.erase(at);
        player->tech_purchase_multiplier = technology_purchase_multiplier(rows).value();
        dirty[static_cast<std::size_t>(StateSection::economy_corruption)] = true;
    }
}
core::Result<Fixed> GalacticSession::set_technology_price_modifier(const TechnologyPriceWrite& write) {
    auto& impl = *impl_;
    if (impl.failed || !impl.economy_corruption || !write.source || (!write.additive && write.first_expired_frame)
        || (write.first_expired_frame && *write.first_expired_frame <= impl.clock.galactic_frame))
        return fail<Fixed>("technology-price write requires live owner and a future expiry");
    auto* player = find(impl.economy_corruption->players, write.player, [](const auto& p) { return p.player; });
    if (!player) return fail<Fixed>("unbound technology-price player");
    auto rows = player->technology_price_modifiers;
    auto at = std::lower_bound(rows.begin(), rows.end(), write.source, [](const auto& m, auto id) { return m.source < id; });
    const auto previous = at != rows.end() && at->source == write.source ? std::optional(*at) : std::nullopt;
    if (write.additive) {
        const TechnologyPriceModifier value{write.source, *write.additive, write.first_expired_frame};
        if (previous) *at = value; else rows.insert(at, value);
    } else if (previous) rows.erase(at);
    auto multiplier = technology_purchase_multiplier(rows);
    if (!multiplier) return multiplier;
    if (rows == player->technology_price_modifiers) return multiplier;
    std::vector<std::uint8_t> bytes;
    detail::put_u32(bytes, write.player); detail::put_u64(bytes, write.source);
    const auto encode = [&](std::optional<Fixed> additive, std::optional<std::uint64_t> expiry) {
        detail::put_u8(bytes, additive.has_value());
        if (additive) detail::put_i64(bytes, additive->raw());
        detail::put_u8(bytes, expiry.has_value());
        if (expiry) detail::put_u64(bytes, *expiry);
    };
    encode(write.additive, write.first_expired_frame);
    encode(previous ? std::optional(previous->additive) : std::nullopt, previous ? previous->first_expired_frame : std::nullopt);
    detail::put_i64(bytes, player->tech_purchase_multiplier.raw());
    auto recorded = append_native_effect(NativeEffectKind::technology_price, std::move(bytes));
    if (!recorded) return core::Result<Fixed>::failure(recorded.error());
    if (previous && previous->first_expired_frame)
        impl.technology_price_expiry.erase({*previous->first_expired_frame, write.player, write.source});
    if (write.first_expired_frame) impl.technology_price_expiry.emplace(*write.first_expired_frame, write.player, write.source);
    player->technology_price_modifiers = std::move(rows); player->tech_purchase_multiplier = multiplier.value();
    impl.dirty[static_cast<std::size_t>(StateSection::economy_corruption)] = true;
    const auto events = impl.snapshot->events();
    impl.snapshot = impl.make_snapshot({events.begin(), events.end()});
    return multiplier;
}
core::Result<void> GalacticSession::Impl::replay_technology_price(GalacticSession& world, const NativeEffect& effect) {
    sim::detail::Reader in(effect.payload); TechnologyPriceWrite write;
    std::optional<Fixed> old_additive; std::optional<std::uint64_t> old_expiry; std::int64_t old_multiplier{};
    const auto read = [&](std::optional<Fixed>& additive, std::optional<std::uint64_t>& expiry) {
        std::uint8_t flag{}; std::int64_t raw{}; std::uint64_t frame{};
        if (!in.read_u8(flag) || flag > 1) return false;
        if (flag) { if (!in.read_i64(raw)) return false; additive = Fixed::from_raw(raw); }
        if (!in.read_u8(flag) || flag > 1) return false;
        if (flag) { if (!additive || !in.read_u64(frame)) return false; expiry = frame; }
        return true;
    };
    if (!in.read_u32(write.player) || !in.read_u64(write.source) || !read(write.additive, write.first_expired_frame)
        || !read(old_additive, old_expiry) || !in.read_i64(old_multiplier) || in.remaining() || !economy_corruption)
        return fail<void>("invalid technology-price journal");
    const auto* player = find(economy_corruption->players, write.player, [](const auto& p) { return p.player; });
    if (!player || player->tech_purchase_multiplier.raw() != old_multiplier)
        return fail<void>("technology-price reconstruction differs from retained player");
    const auto* previous = find(player->technology_price_modifiers, write.source, [](const auto& m) { return m.source; });
    if ((previous != nullptr) != old_additive.has_value()
        || (previous && (previous->additive != *old_additive || previous->first_expired_frame != old_expiry)))
        return fail<void>("technology-price reconstruction differs from retained source");
    const auto written = world.set_technology_price_modifier(write);
    return written ? core::Result<void>::success() : core::Result<void>::failure(written.error());
}

core::Result<void> validate_economy_corruption(const GalacticSetup& setup) {
    if (!setup.economy_corruption) return core::Result<void>::success();
    const auto& s = *setup.economy_corruption;
    if (!setup.fiscal || !setup.player_economy || s.players.size() != setup.players.size()
        || s.planets.size() != setup.planets.size() || s.items.size() > galactic_max_objects
        || s.buyers.size() > galactic_max_objects || s.menus.size() > s.buyers.size()
        || !ordered(s.buyers, [](const auto& r) { return r.buyer; })
        || !ordered(s.menus, [](const auto& r) { return r.buyer; })
        || !bounded(s.factor_min) || !bounded(s.factor_max) || s.factor_min < Fixed{} || s.factor_min > s.factor_max)
        return fail<void>("invalid tables or factor range");
    for (auto n : s.income_factors) if (!bounded(n)) return fail<void>("income factor outside arithmetic bounds");
    std::set<std::uint32_t> items;
    for (const auto& item : s.items) {
        if (!item.id || !items.insert(item.id).second || !bounded(item.base_cost) || !bounded(item.tutorial_base_cost)
            || !bounded(item.uncommon_multiplier) || item.uncommon_multiplier < Fixed{}) return fail<void>("invalid market item");
    }
    for (std::size_t i = 0; i < s.players.size(); ++i) {
        const auto& p = s.players[i];
        if (p.player != setup.players[i].player_id || !p.faction || !bounded(p.tech_purchase_multiplier)
            || p.tech_purchase_multiplier < Fixed{} || p.displayed_tech_adjustment < std::numeric_limits<int>::min()
            || p.displayed_tech_adjustment > std::numeric_limits<int>::max()) return fail<void>("invalid player");
        for (auto item : p.unlocks) if (!items.contains(item)) return fail<void>("unknown unlock");
        const auto multiplier = technology_purchase_multiplier(p.technology_price_modifiers);
        if (!multiplier || multiplier.value() != p.tech_purchase_multiplier)
            return fail<void>("technology-price cache differs from modifier ledger");
    }
    for (std::size_t i = 0; i < s.planets.size(); ++i) {
        const auto& p = s.planets[i];
        if (p.planet != setup.planets[i].id || p.corruption > cf::Type::bonded_citizens
            || !bounded(p.factor) || p.factor < Fixed{} || !bounded(p.piracy) || !bounded(p.market_modifier)
            || p.siphon.fraction < Fixed{} || p.siphon.fraction > cf::one) return fail<void>("invalid planet");
        // An absent recipient is explicitly permitted by WGEP-12.
    }
    for (const auto& b : s.buyers) {
        if (!b.buyer || !find(s.players, b.owner, [](const auto& p) { return p.player; })
            || !bounded(b.price_modifier) || b.price_modifier < Fixed{}
            || b.absence_min.size() != b.absence_max.size() || b.absence_min.size() > 5)
            return fail<void>("invalid buyer");
        for (std::size_t i = 0; i < b.absence_min.size(); ++i)
            if (!bounded(b.absence_min[i]) || !bounded(b.absence_max[i]) || b.absence_min[i] < Fixed{}
                || b.absence_min[i] > b.absence_max[i]) return fail<void>("invalid absence range");
    }
    for (const auto& m : s.menus) {
        if (!find(s.buyers, m.buyer, [](const auto& b) { return b.buyer; })
            || !find(s.planets, m.planet, [](const auto& p) { return p.planet; })
            || m.offers.size() > s.items.size() || !bounded(m.absence) || m.absence < Fixed{}) return fail<void>("invalid menu");
        std::set<std::uint32_t> seen;
        for (const auto& o : m.offers) if (!items.contains(o.item) || !seen.insert(o.item).second
            || !bounded(o.price) || o.price < Fixed{}) return fail<void>("invalid prepared offer");
    }
    return core::Result<void>::success();
}

core::Result<Fixed> economy_draw(EconomyCorruptionState& s, std::uint64_t seed) {
    if (s.random_draws == std::numeric_limits<std::uint64_t>::max()) return fail<Fixed>("random stream exhausted");
    // Coordinator-approved dedicated counter stream; shared-sync equivalence is
    // an explicit fidelity seam. Integer mixing is platform independent.
    auto n = seed ^ 0x45434f4e4f4d5901ULL;
    n += (++s.random_draws) * 0x9e3779b97f4a7c15ULL;
    n = (n ^ (n >> 30)) * 0xbf58476d1ce4e5b9ULL;
    n = (n ^ (n >> 27)) * 0x94d049bb133111ebULL;
    n ^= n >> 31;
    return core::Result<Fixed>::success(Fixed::from_raw(static_cast<std::int64_t>(n >> 40)));
}

core::Result<std::vector<Fixed>> corruption_payments(EconomyCorruptionState& s,
    std::span<const Planet> owners, std::span<const FiscalPlayer> fiscal,
    std::span<const Fixed> pre_siphon, std::uint64_t frame, const PartitionExecutor& executor, bool collect) {
    if (owners.size() != s.planets.size() || pre_siphon.size() != owners.size() || fiscal.size() != s.players.size())
        return fail<std::vector<Fixed>>("payment input dimensions");
    std::vector<cf::IncomePlayer> players;
    for (std::size_t i = 0; i < s.players.size(); ++i) players.push_back({s.players[i].player, s.players[i].benefits, fiscal[i].earns_planet_income});
    std::vector<cf::IncomeResult> rows(owners.size());
    std::vector<std::optional<core::Diagnostic>> errors(owners.size());
    auto phase = executor.execute_phase("galactic-corruption-income", tick_partition_count, [&](std::size_t partition) {
        for (std::size_t i = partition; i < rows.size(); i += tick_partition_count) {
            const auto& p = s.planets[i];
            auto value = cf::income({p.corruption, owners[i].owner, pre_siphon[i], p.piracy,
                p.siphon, frame, s.income_factors}, players, collect);
            if (!value) errors[i] = value.error(); else rows[i] = std::move(value).value();
        }
    });
    if (!phase) return core::Result<std::vector<Fixed>>::failure(phase.error());
    std::vector<Fixed> totals(players.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (errors[i]) return core::Result<std::vector<Fixed>>::failure(*errors[i]);
        for (std::size_t j = 0; j < players.size(); ++j) {
            auto payment = rows[i].payments[j];
            auto sum = math::add(totals[j], payment);
            if (!sum) return core::Result<std::vector<Fixed>>::failure(sum.error());
            totals[j] = sum.value();
        }
    }
    if (collect) for (std::size_t i = 0; i < rows.size(); ++i) {
        s.planets[i].piracy = rows[i].remaining_piracy;
        s.planets[i].siphon.active = rows[i].siphon_active;
    }
    return core::Result<std::vector<Fixed>>::success(std::move(totals));
}

core::Result<void> reroll_planet_income(EconomyCorruptionState& s, std::uint64_t seed) {
    auto staged = s;
    // Ordered RNG allocation/commit, not per-object simulation. WGEP-14 requires
    // the post-payment pass; no random draws for unaffected or destroyed planets.
    for (auto& p : staged.planets) {
        if (!p.influenced) continue;
        Fixed draw;
        if (!p.destroyed) { auto value = economy_draw(staged, seed); if (!value) return core::Result<void>::failure(value.error()); draw = value.value(); }
        auto factor = cf::next_income_factor(p.influenced, p.destroyed, p.factor, staged.factor_min, staged.factor_max, draw);
        if (!factor) return core::Result<void>::failure(factor.error());
        p.factor = factor.value();
    }
    s = std::move(staged);
    return core::Result<void>::success();
}

void GalacticSession::Impl::execute_black_market(const GameCommand& command,
    std::uint64_t tick, std::vector<GalacticEvent>& events) {
    const auto* prepare = std::get_if<PrepareBlackMarket>(&command.payload);
    const auto* buy = std::get_if<BuyBlackMarket>(&command.payload);
    const auto identity = prepare ? prepare->buyer : buy->buyer;
    GalacticEvent event{tick, GalacticEventKind::command_executed, command.key.seat, identity, command.key.sequence};
    const auto reject = [&](GalacticRejectReason reason) { event.kind = GalacticEventKind::command_rejected; event.reason = reason; events.push_back(event); };
    const auto* issuer = seat(command.key.seat);
    if (!issuer || !issuer->active) return reject(GalacticRejectReason::inactive_seat);
    if (!economy_corruption || !economy) return reject(GalacticRejectReason::unknown_subject);
    auto staged = *economy_corruption;
    auto* actor = find(staged.buyers, identity, [](const auto& b) { return b.buyer; });
    if (!actor || !actor->present) return reject(GalacticRejectReason::unknown_subject);
    if (actor->owner != issuer->player) return reject(GalacticRejectReason::not_authorized);
    auto* player = find(staged.players, actor->owner, [](const auto& p) { return p.player; });
    auto* account = find(economy->players, actor->owner, [](const auto& p) { return p.player; });
    if (!player || !account) return reject(GalacticRejectReason::unknown_subject);
    if (prepare) {
        auto* planet = find(staged.planets, prepare->planet, [](const auto& p) { return p.planet; });
        const auto current = sorted_planets();
        const auto* owner = find(current, prepare->planet, [](const auto& p) { return p.id; });
        if (!planet || !owner) return reject(GalacticRejectReason::unknown_subject);
        const auto* faction = find(staged.players, owner->owner, [](const auto& p) { return p.player; });
        cf::MarketContext context;
        context.opposing_planet = actor->opposing_planet; // owning ability's sourced admission
        context.playable_faction = faction && faction->playable;
        context.black_market_corruption = planet->corruption == cf::Type::black_market;
        context.owner_present = faction != nullptr; context.owner_faction = faction ? faction->faction : 0;
        context.tutorial = staged.tutorial; context.multiplayer = staged.multiplayer;
        context.human_buyer = player->human; context.buyer = player->player;
        context.planet_modifier = planet->market_modifier; context.price_modifier = actor->price_modifier;
        context.tech_purchase_multiplier = player->tech_purchase_multiplier;
        std::vector<cf::MarketPlayer> players;
        int buyer_tech{};
        for (std::size_t i = 0; i < staged.players.size(); ++i) {
            const auto& p = staged.players[i];
            // WGEP-99: GC always adds the faction adjustment, no clamp or AI/tutorial branch.
            const auto displayed = economy->players[i].current_tech + p.displayed_tech_adjustment;
            if (displayed < std::numeric_limits<int>::min() || displayed > std::numeric_limits<int>::max())
                return reject(GalacticRejectReason::resource_limit);
            players.push_back({p.player, p.faction, static_cast<int>(displayed)});
            if (p.player == player->player) buyer_tech = static_cast<int>(displayed);
        }
        auto offers = cf::offers(context, staged.items, players, player->unlocks);
        if (!offers || offers.value().empty()) return reject(GalacticRejectReason::unknown_value);
        Fixed draw;
        // WGEP-81: displayed buyer tech indexes absence, not selected item tech.
        if (buyer_tech >= 1 && buyer_tech <= 5
            && static_cast<std::size_t>(buyer_tech) <= actor->absence_min.size()) {
            auto sampled = economy_draw(staged, setup.seed);
            if (!sampled) { failed = true; return; } draw = sampled.value();
        }
        auto absence = cf::absence(buyer_tech, actor->absence_min, actor->absence_max, draw);
        if (!absence) { failed = true; return; }
        auto* menu = find(staged.menus, identity, [](const auto& m) { return m.buyer; });
        BlackMarketMenu value{identity, prepare->planet, std::move(offers).value(), absence.value(), true};
        if (menu) *menu = std::move(value);
        else staged.menus.insert(std::lower_bound(staged.menus.begin(), staged.menus.end(), identity,
            [](const auto& m, auto id) { return m.buyer < id; }), std::move(value));
    } else {
        auto* menu = find(staged.menus, identity, [](const auto& m) { return m.buyer; });
        if (!menu || buy->index >= menu->offers.size()) return reject(GalacticRejectReason::unknown_value);
        // WGEP-80: execution does not recheck owned/menu activity or affordability
        // rounded by the UI. Resolve the current buyer owner before exact debit.
        if (!find(staged.planets, menu->planet, [](const auto& p) { return p.planet; }))
            return reject(GalacticRejectReason::unknown_subject);
        const auto& offer = menu->offers[buy->index];
        cf::Player ledger; ledger.id = player->player; ledger.credits = account->credits; ledger.black_market_unlocks = player->unlocks;
        auto purchased = cf::purchase(ledger, offer, actor->present, true);
        if (!purchased || !purchased.value()) return reject(GalacticRejectReason::unknown_value);
        const auto delta = math::subtract(ledger.credits, account->credits);
        if (!delta) { failed = true; return; }
        // WGA-85/170: market debit uses the same retained funding notification.
        if (credit_notification_handler) {
            auto notified = credit_notification_handler({CreditNotification::Kind::windfall,
                player->player, ledger.credits, delta.value(), {}});
            if (!notified) { failed = true; return; }
        }
        account->credits = ledger.credits; player->unlocks = std::move(ledger.black_market_unlocks);
        menu->active = false;
        actor->prepared_absence = menu->absence;
        actor->despawn_requested = actor->causes_despawn;
        // The owning hero lifecycle consumes this request, not an invented timer.
        dirty[static_cast<std::size_t>(StateSection::players)] = true;
        event.value = offer.item;
        event.kind = GalacticEventKind::black_market_purchased; // WGEP-55 local feedback handoff
    }
    economy_corruption = std::move(staged);
    dirty[static_cast<std::size_t>(StateSection::economy_corruption)] = true;
    events.push_back(event);
}
} // namespace eawr::sim::galactic
