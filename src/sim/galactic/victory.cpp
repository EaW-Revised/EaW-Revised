#include "eawr/sim/galactic/victory.hpp"

#include "encoding.hpp"
#include "session_impl.hpp"
#include "../replay_internal.hpp"

#include <algorithm>
#include <limits>

namespace eawr::sim::galactic {
namespace {
bool same(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    const auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
    for (std::size_t i = 0; i < a.size(); ++i)
        if (lower(static_cast<unsigned char>(a[i])) != lower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}
const VictoryPlayer* player(const VictoryContext& c, PlayerId id) {
    const auto it = std::find_if(c.players.begin(), c.players.end(), [&](const auto& p) { return p.player == id; });
    return it == c.players.end() ? nullptr : &*it;
}
bool allied(const VictoryContext& c, PlayerId a, PlayerId b) {
    const auto it = std::find_if(c.relations.begin(), c.relations.end(), [&](const auto& r) { return r.player == a && r.other == b; });
    return it != c.relations.end() && it->allied;
}
bool matches(const std::vector<std::string>& names, std::string_view name) {
    return names.empty() || std::any_of(names.begin(), names.end(), [&](const auto& n) { return same(n, name); });
}
bool active(const VictoryState& s, VictoryKind k, PlayerId id) {
    return std::any_of(s.installed.begin(), s.installed.end(), [&](const auto& row) { return row.kind == k && row.player == id; });
}
core::Result<bool> yes(bool value) { return core::Result<bool>::success(value); }
core::Result<bool> partitioned_predicate(const GalacticVictoryCondition& row, const VictoryContext& c,
    const PartitionExecutor& executor) {
    if (c.players.size() > galactic_max_players || c.planets.size() > galactic_max_objects || c.leaders.size() > galactic_max_objects)
        return core::Result<bool>::failure(detail::diagnostic(diagnostic_codes::resource_limit, "victory provider limits exceeded"));
    const auto* contender = player(c, row.player);
    if (!contender) return yes(false);
    const auto kind = row.kind;
    if (kind == VictoryKind::credits || kind == VictoryKind::cycles
        || kind == VictoryKind::destroy_superweapon || kind == VictoryKind::superweapon_last_planet
        || kind == VictoryKind::superweapon_kill_leader || kind == VictoryKind::story_forced) return victory_predicate(row, c);
    if ((kind == VictoryKind::all_planets || kind == VictoryKind::no_enemy_planets
        || kind == VictoryKind::control_fraction || kind == VictoryKind::homeworld_and_leader || kind == VictoryKind::kill_leader) && !contender->playable) return yes(false);
    if ((kind == VictoryKind::all_planets || kind == VictoryKind::control_fraction) && c.planets.empty()) return yes(false);
    if (kind == VictoryKind::all_planets) {
        const auto* first = player(c, c.planets.front().current);
        if (!first || first->neutral) return yes(false);
    }
    std::vector<TypeId> named;
    if (kind == VictoryKind::named_planets) for (const auto& name : row.parameters.planets) {
        const auto t = std::find_if(c.types.begin(), c.types.end(), [&](const auto& v) { return same(v.name, name); });
        if (t == c.types.end()) return core::Result<bool>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "victory references an unknown planet type: " + name));
        named.push_back(t->type);
    }
    const auto* enemy = player(c, contender->primary_enemy);
    if (kind == VictoryKind::homeworld_and_leader && (!enemy || !enemy->home || !enemy->leader_company)) return yes(false);
    struct LeaderPart { bool scheduled{}, first_kill{}; std::size_t first{std::numeric_limits<std::size_t>::max()}; };
    struct Partial {
        bool all{true}, home{}, returned{};
        std::size_t home_first{std::numeric_limits<std::size_t>::max()};
        std::uint64_t target{};
        std::array<LeaderPart, galactic_max_players> leaders;
    };
    std::array<Partial, tick_partition_count> parts;
    const auto phase = executor.execute_phase("galactic-victory", tick_partition_count, [&](std::size_t partition) {
        auto& out = parts[partition];
        if (kind != VictoryKind::kill_leader) {
            const auto range = partition_range(partition, c.planets.size());
            for (auto i = range.begin; i < range.end; ++i) {
                const auto& p = c.planets[i];
                if (kind == VictoryKind::all_planets) out.all = out.all && allied(c, row.player, p.current);
                else if (kind == VictoryKind::no_enemy_planets) {
                    const auto* owner = player(c, p.current);
                    if (owner && !owner->neutral && owner->playable && !(contender->ai && owner->ai)
                        && !(c.multiplayer && !owner->human) && matches(row.parameters.factions, owner->faction))
                        out.all = out.all && allied(c, row.player, owner->player);
                } else if (kind == VictoryKind::control_fraction) out.target += p.target == row.player ? 1U : 0U;
                else if (kind == VictoryKind::named_planets) {
                    if (std::find(named.begin(), named.end(), p.type) != named.end()) out.all = out.all && p.owner == row.player;
                } else if (kind == VictoryKind::homeworld_and_leader && p.type == enemy->home && i < out.home_first) {
                    out.home_first = i; out.home = p.target == row.player;
                }
            }
        }
        if (kind == VictoryKind::kill_leader || kind == VictoryKind::homeworld_and_leader) {
            const auto range = partition_range(partition, c.leaders.size());
            for (auto i = range.begin; i < range.end; ++i) {
                const auto& l = c.leaders[i];
                if (kind == VictoryKind::homeworld_and_leader) {
                    out.returned = out.returned || (l.owner == enemy->player && l.company == enemy->leader_company && l.scheduled_return);
                    continue;
                }
                const auto* owner = player(c, l.owner);
                if (!owner || !owner->leader_company || owner->leader_unit.empty() || allied(c, row.player, owner->player)
                    || !matches(row.parameters.leaders, owner->leader_unit)) continue;
                auto& result = out.leaders[static_cast<std::size_t>(owner - c.players.data())];
                if (l.scheduled_return && l.company == owner->leader_company && l.killer == row.player) result.scheduled = true;
                if (!l.scheduled_return && same(l.unit, owner->leader_unit) && i < result.first) {
                    result.first = i;
                    result.first_kill = (l.dead || l.marked_for_death || l.deletion_pending) && l.killer == row.player;
                }
            }
        }
    });
    if (!phase) return core::Result<bool>::failure(phase.error());
    Partial total;
    for (const auto& part : parts) {
        total.all = total.all && part.all;
        if (part.home_first < total.home_first) { total.home_first = part.home_first; total.home = part.home; }
        total.returned = total.returned || part.returned; total.target += part.target;
        for (std::size_t i = 0; i < c.players.size(); ++i) {
            auto& l = total.leaders[i]; l.scheduled = l.scheduled || part.leaders[i].scheduled;
            if (part.leaders[i].first < l.first) { l.first = part.leaders[i].first; l.first_kill = part.leaders[i].first_kill; }
        }
    }
    if (kind == VictoryKind::control_fraction) {
        const auto threshold = row.parameters.control_fraction.raw();
        return yes(threshold <= 0 || (threshold <= math::Fixed::scale
            && static_cast<std::int64_t>(total.target) * math::Fixed::scale >= static_cast<std::int64_t>(c.planets.size()) * threshold));
    }
    if (kind == VictoryKind::homeworld_and_leader) return yes(total.home && total.returned);
    if (kind == VictoryKind::kill_leader) {
        for (std::size_t i = 0; i < c.players.size(); ++i) if (total.leaders[i].scheduled || total.leaders[i].first_kill) return yes(true);
        return yes(false);
    }
    return yes(total.all);
}

}

VictoryState install_victory(const VictoryAuthoring& a, const VictoryContext& c, PlayerId starting) {
    VictoryState s; s.context = c;
    for (const auto& p : c.players) {
        if (p.neutral) continue;
        const bool nonstarting = !c.multiplayer && p.player != starting;
        const auto& list = nonstarting ? a.ai : a.human;
        const auto& params = nonstarting ? a.ai_parameters : a.human_parameters;
        // WGS-15..17/78: append authored entries and defaults, retaining duplicates.
        for (auto k : list) s.installed.push_back({k, p.player, 30, params, false, false});
        if (nonstarting && !list.empty()) s.installed.push_back({VictoryKind::no_enemy_planets, p.player, 30, {}, false, false});
        s.installed.push_back({VictoryKind::all_planets, p.player, 30, {}, false, false});
        s.installed.push_back({VictoryKind::superweapon_last_planet, p.player, 30, {}, false, false});
        if (nonstarting || c.multiplayer) s.installed.push_back({VictoryKind::no_enemy_planets, p.player, 30, {}, false, false});
    }
    return s;
}

core::Result<bool> victory_predicate(const GalacticVictoryCondition& row, const VictoryContext& c) {
    const auto* contender = player(c, row.player);
    if (!contender) return yes(false);
    switch (row.kind) {
    case VictoryKind::story_forced: return yes(true); // WGS-173: no conquest predicate.
    case VictoryKind::all_planets: // WGS-19: allied, not exact ownership.
        if (!contender->playable || c.planets.empty()) return yes(false);
        if (const auto* first = player(c, c.planets.front().current); !first || first->neutral) return yes(false);
        return yes(std::all_of(c.planets.begin(), c.planets.end(), [&](const auto& p) { return allied(c, row.player, p.current); }));
    case VictoryKind::no_enemy_planets: // WGS-20: current allegiance, playable enemy filter.
        if (!contender->playable) return yes(false);
        for (const auto& planet : c.planets) {
            const auto* owner = player(c, planet.current);
            if (!owner || owner->neutral || !owner->playable || (contender->ai && owner->ai)
                || (c.multiplayer && !owner->human) || !matches(row.parameters.factions, owner->faction)) continue;
            if (!allied(c, row.player, owner->player)) return yes(false);
        }
        return yes(true);
    case VictoryKind::credits: // WGS-21: truncate current balance towards zero.
        return yes(contender->playable && contender->credits.raw() / math::Fixed::scale >= row.parameters.credits);
    case VictoryKind::control_fraction: { // WGS-22: target exact owner; Q24 threshold comparison.
        if (!contender->playable || c.planets.empty()) return yes(false);
        const auto count = std::count_if(c.planets.begin(), c.planets.end(), [&](const auto& p) { return p.target == row.player; });
        if (row.parameters.control_fraction.raw() <= 0) return yes(true);
        if (row.parameters.control_fraction.raw() > math::Fixed::scale) return yes(false);
        return yes(static_cast<std::int64_t>(count) * math::Fixed::scale
            >= static_cast<std::int64_t>(c.planets.size()) * row.parameters.control_fraction.raw());
    }
    case VictoryKind::homeworld_and_leader: { // WGS-23: scheduled return, no kill attribution.
        const auto* enemy = player(c, contender->primary_enemy);
        if (!contender->playable || !enemy || !enemy->home || !enemy->leader_company) return yes(false);
        const auto home = std::find_if(c.planets.begin(), c.planets.end(), [&](const auto& p) { return p.type == enemy->home; });
        if (home == c.planets.end() || home->target != row.player) return yes(false);
        return yes(std::any_of(c.leaders.begin(), c.leaders.end(), [&](const auto& l) { return l.owner == enemy->player && l.company == enemy->leader_company && l.scheduled_return; }));
    }
    case VictoryKind::kill_leader: // WGS-24: first matching active object, exact final blow.
        if (!contender->playable) return yes(false);
        for (const auto& enemy : c.players) {
            if (!enemy.leader_company || enemy.leader_unit.empty() || allied(c, row.player, enemy.player)
                || !matches(row.parameters.leaders, enemy.leader_unit)) continue;
            for (const auto& l : c.leaders)
                if (l.owner == enemy.player && l.company == enemy.leader_company && l.scheduled_return && l.killer == row.player) return yes(true);
            const auto found = std::find_if(c.leaders.begin(), c.leaders.end(), [&](const auto& l) { return !l.scheduled_return && l.owner == enemy.player && same(l.unit, enemy.leader_unit); });
            if (found != c.leaders.end() && (found->dead || found->marked_for_death || found->deletion_pending) && found->killer == row.player) return yes(true);
        }
        return yes(false);
    case VictoryKind::named_planets: // WGS-25: empty list/missing instances remain true.
        for (const auto& name : row.parameters.planets) {
            const auto t = std::find_if(c.types.begin(), c.types.end(), [&](const auto& v) { return same(name, v.name); });
            if (t == c.types.end()) return core::Result<bool>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "victory references an unknown planet type: " + name));
            for (const auto& p : c.planets) if (p.type == t->type && p.owner != row.player) return yes(false);
        }
        return yes(true);
    case VictoryKind::cycles: // WGS-26: strictly greater; a negative authored limit is already exceeded.
        return yes(row.parameters.cycles < 0 || c.fiscal_cycle > static_cast<std::uint64_t>(row.parameters.cycles));
    case VictoryKind::destroy_superweapon:
    case VictoryKind::superweapon_last_planet:
    case VictoryKind::superweapon_kill_leader: return yes(false); // WGS-27: event producers only.
    }
    return yes(false);
}

bool register_victory(VictoryState& s, VictoryKind kind, PlayerId id, std::uint32_t delay, std::uint64_t frame) {
    if (!id || !player(s.context, id) || s.pending) return false; // WGS-30: first accepted pending copy wins.
    const auto it = std::find_if(s.installed.begin(), s.installed.end(), [&](const auto& r) { return r.kind == kind && r.player == id; });
    if (it == s.installed.end()) return false;
    s.pending = *it; s.pending->delay = delay; s.pending->occurred = true;
    s.remaining = delay; static_cast<void>(frame); // WGS-31: registration preserves the previous service stamp.
    return true;
}

core::Result<bool> test_victory(VictoryState& s, VictoryTest request, const VictoryContext& c,
    const PartitionExecutor& executor, std::uint64_t frame) {
    // WGS-18: wait admission precedes the pending-match test.
    if (s.waits) {
        if (s.queued.size() >= galactic_max_objects) return core::Result<bool>::failure(detail::diagnostic(diagnostic_codes::resource_limit, "victory wait queue limit"));
        s.queued.push_back(request); return yes(false);
    }
    if (s.pending) return yes(s.pending->kind == request.kind && s.pending->player == request.player);
    const auto* p = player(c, request.player);
    if (!p || c.tutorial_delayed || (c.multiplayer && !p->human)) return yes(false);
    std::vector<std::size_t> selected;
    for (std::size_t i = 0; i < s.installed.size(); ++i)
        if (s.installed[i].player == request.player && s.installed[i].kind == request.kind) selected.push_back(i);
    // ADR-009: partition planet/leader work from copied inputs; commit in installed order.
    for (const auto index : selected) {
        auto& row = s.installed[index];
        auto staged = row.occurred ? yes(true) : partitioned_predicate(row, c, executor);
        if (!staged) return staged;
        if (!staged.value()) continue;
        row.occurred = true;
        s.pending = row; s.remaining = row.delay; static_cast<void>(frame);
        return yes(true);
    }
    return yes(false);
}
core::Result<bool> release_victory_wait(VictoryState& s, bool evaluate, const VictoryContext& c,
    const PartitionExecutor& executor, std::uint64_t frame) {
    if (s.waits) --s.waits;
    if (s.waits || !evaluate) return yes(false);
    auto tests = std::move(s.queued); s.queued.clear(); // WGS-32: ordered replay, then clear.
    for (const auto& test : tests) {
        auto result = test_victory(s, test, c, executor, frame);
        if (!result || result.value()) return result;
    }
    return yes(false);
}
void service_victory(VictoryState& s, std::uint64_t frame, bool retry_active) {
    if (frame < s.last_service) return;
    const auto elapsed = frame - s.last_service;
    s.last_service = frame;
    if (!s.pending || s.remaining <= 0) return;
    s.remaining = elapsed >= static_cast<std::uint64_t>(s.remaining) ? 0 : s.remaining - static_cast<std::int64_t>(elapsed);
    if (s.remaining == 0 && s.use_retry) {
        if (!retry_active && !s.retry_waiting) s.retry_waiting = true; // presentation opens once
        s.remaining = 1; // WGS-31: retry activation itself holds, including an open dialog.
    }
}
bool pending_victory(const VictoryState& s, GalacticVictoryCondition& output) {
    output = {}; // WGS-32: even a failed read clears the output.
    if (s.waits || !s.pending || s.remaining > 0) return false;
    output = *s.pending; return true;
}
void victory_superweapon_return(VictoryState& s, const VictorySuperweaponReturn& r,
    const VictoryContext& c, std::uint64_t frame) {
    if (!r.weapon_owner || (r.weapon_owner != r.winner && r.weapon_owner != r.loser)) return;
    const bool killed = r.weapon_owner == r.loser && r.weapon_alive && !r.weapon_deletion_pending && r.killer_survivor;
    if (killed) { static_cast<void>(register_victory(s, VictoryKind::destroy_superweapon, r.winner, 5, frame)); return; }
    if (!r.weapon_alive || r.weapon_deletion_pending) return;
    const auto opposing = r.weapon_owner == r.winner ? r.loser : r.winner;
    const auto* enemy = player(c, opposing);
    if (!enemy || !enemy->playable || !active(s, VictoryKind::superweapon_last_planet, r.weapon_owner)) return;
    if (std::none_of(c.planets.begin(), c.planets.end(), [&](const auto& p) { return p.current == opposing && !p.destroyed; }))
        static_cast<void>(register_victory(s, VictoryKind::superweapon_last_planet, r.weapon_owner, 30, frame));
}

std::optional<CampaignResult> campaign_result(const VictoryState& s, PlayerId local,
    bool dialog_active, bool movie_playing) {
    if (!s.context.multiplayer && (dialog_active || movie_playing)) return std::nullopt;
    GalacticVictoryCondition condition;
    if (!local || !pending_victory(s, condition)) return std::nullopt;
    return CampaignResult{condition, condition.player == local,
        s.context.multiplayer ? CampaignResultRoute::multiplayer_quit : CampaignResultRoute::score};
}

bool victory_leader_planet_destruction(VictoryState& s, PlayerId actor, PlayerId controller,
    std::span<const std::string> landed, bool tactical, std::uint64_t frame) {
    const auto* owner = player(s.context, controller);
    if (!owner || owner->planet_leader_unit.empty() || !active(s, VictoryKind::superweapon_kill_leader, actor)) return false;
    if (std::none_of(landed.begin(), landed.end(), [&](const auto& unit) { return same(unit, owner->planet_leader_unit); })) return false;
    if (tactical) {
        const auto condition = std::find_if(s.installed.begin(), s.installed.end(), [&](const auto& r) {
            return r.kind == VictoryKind::superweapon_kill_leader && r.player == actor;
        });
        if (!matches(condition->parameters.leaders, owner->planet_leader_unit)) return false;
    }
    return register_victory(s, VictoryKind::superweapon_kill_leader, actor, 30, frame);
}

core::Result<void> validate_victory(const VictoryState& s) {
    const auto bad = [] { return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "invalid victory monitor state")); };
    if (s.installed.size() > galactic_max_objects || s.queued.size() > galactic_max_objects || s.remaining < 0
        || (!s.pending && (s.remaining || s.retry_waiting || s.result_dispatched))) return bad();
    const auto& c = s.context;
    if (c.players.size() > galactic_max_players || c.planets.size() > galactic_max_objects
        || c.types.size() > galactic_max_objects || c.leaders.size() > galactic_max_objects
        || c.relations.size() > galactic_max_players * galactic_max_players || c.superweapon_killers.size() > galactic_max_objects) return bad();
    std::size_t budget = 0;
    const auto text_ok = [&](const std::string& value) { budget += value.size(); return value.size() <= 65536 && budget <= 16U * 1024U * 1024U; };
    for (const auto& p : c.players) if (!p.player || !text_ok(p.faction) || !text_ok(p.leader_unit) || !text_ok(p.planet_leader_unit)) return bad();
    for (const auto& t : c.types) if (!t.type || !text_ok(t.name)) return bad();
    for (const auto& l : c.leaders) if (!l.owner || !text_ok(l.unit)) return bad();
    const auto valid = [&](const GalacticVictoryCondition& r) {
        const auto kind = static_cast<unsigned>(r.kind);
        const auto& p = r.parameters;
        if (!r.player || kind < 1 || kind > static_cast<unsigned>(VictoryKind::story_forced)) return false;
        for (const auto* names : {&p.factions, &p.leaders, &p.planets}) {
            if (names->size() > galactic_max_objects) return false;
            for (const auto& n : *names) if (!text_ok(n)) return false;
        }
        return true;
    };
    for (const auto& r : s.installed) if (!valid(r) || !player(c, r.player)) return bad();
    if (s.pending && (!valid(*s.pending) || !active(s, s.pending->kind, s.pending->player))) return bad();
    for (const auto& q : s.queued) if (!q.player || static_cast<unsigned>(q.kind) < 1
        || static_cast<unsigned>(q.kind) > static_cast<unsigned>(VictoryKind::story_forced)) return bad();
    return core::Result<void>::success();
}

void GalacticSession::Impl::service_victory_tail(std::uint64_t tick, std::vector<GalacticEvent>& events) {
    if (!victory) return;
    const auto previous_frame = victory->last_service;
    const auto previous_remaining = victory->remaining;
    const auto previous_retry = victory->retry_waiting;
    const auto previous_dispatch = victory->result_dispatched;
    service_victory(*victory, clock.galactic_frame, false);
    GalacticVictoryCondition result;
    if (!victory->result_dispatched && pending_victory(*victory, result)) {
        victory->result_dispatched = true;
        events.push_back({tick, GalacticEventKind::victory_matured, {}, result.player, 0,
            static_cast<std::uint64_t>(result.kind), GalacticRejectReason::none});
    }
    if (previous_frame != victory->last_service || previous_remaining != victory->remaining
        || previous_retry != victory->retry_waiting || previous_dispatch != victory->result_dispatched) dirty[16] = true;
}
void GalacticSession::defer_victory_tail(bool deferred) { impl_->victory_tail_deferred = deferred; }
void GalacticSession::finish_victory_frame() {
    auto events = std::vector<GalacticEvent>(impl_->snapshot->events().begin(), impl_->snapshot->events().end());
    impl_->service_victory_tail(impl_->completed_tick ? impl_->completed_tick - 1 : 0, events);
    impl_->snapshot = impl_->make_snapshot(std::move(events));
}

core::Result<bool> GalacticSession::apply_story_victory(PlayerId winner, bool suppress,
    const PartitionExecutor& executor) {
    auto& impl = *impl_;
    if (impl.failed || !impl.victory || !player(impl.victory->context, winner))
        return core::Result<bool>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "story victory has no owning monitor/player"));
    if (impl.victory->installed.size() >= galactic_max_objects)
        return core::Result<bool>::failure(detail::diagnostic(diagnostic_codes::resource_limit, "story victory condition limit"));
    auto staged = *impl.victory;
    // WGS-172/173: remove all waits without evaluating or clearing queued tests.
    staged.waits = 0;
    staged.installed.push_back({VictoryKind::story_forced, winner,
        (suppress ? 1U : 7U) * galactic_frames_per_second, {}, false, suppress});
    auto tested = test_victory(staged, {VictoryKind::story_forced, winner}, impl.victory_context(), executor, impl.clock.galactic_frame);
    if (!tested) return tested;
    const bool registered = !impl.victory->pending && staged.pending.has_value();
    impl.victory = std::move(staged); impl.dirty[16] = true;
    auto events = impl.snapshot->events();
    std::vector<GalacticEvent> published(events.begin(), events.end());
    if (registered) published.push_back({impl.completed_tick, GalacticEventKind::victory_pending, {}, winner, 0,
        static_cast<std::uint64_t>(VictoryKind::story_forced), GalacticRejectReason::none});
    impl.snapshot = impl.make_snapshot(std::move(published));
    return yes(registered);
}

core::Result<bool> GalacticSession::apply_story_victory_wait(bool enable, const PartitionExecutor& executor) {
    auto& impl = *impl_;
    if (impl.failed || !impl.victory)
        return core::Result<bool>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "story victory wait has no owning monitor"));
    if (enable && !impl.victory->waits) return yes(false); // WGS-174: remove only a positive wait.
    if (!enable && impl.victory->waits == std::numeric_limits<std::uint32_t>::max())
        return core::Result<bool>::failure(detail::diagnostic(diagnostic_codes::resource_limit, "story victory wait overflow"));
    auto staged = *impl.victory;
    if (!enable) ++staged.waits;
    else {
        auto tested = release_victory_wait(staged, true, impl.victory_context(), executor, impl.clock.galactic_frame);
        if (!tested) return tested;
    }
    const bool registered = !impl.victory->pending && staged.pending.has_value();
    impl.victory = std::move(staged); impl.dirty[16] = true;
    const auto old = impl.snapshot->events();
    std::vector<GalacticEvent> published(old.begin(), old.end());
    if (registered) published.push_back({impl.completed_tick, GalacticEventKind::victory_pending, {}, impl.victory->pending->player, 0,
        static_cast<std::uint64_t>(impl.victory->pending->kind), GalacticRejectReason::none});
    impl.snapshot = impl.make_snapshot(std::move(published));
    return yes(registered);
}

core::Result<void> GalacticSession::restore_victory_state(const VictoryState& saved) {
    auto& impl = *impl_;
    if (auto valid = validate_victory(saved); !valid) return valid;
    if (!impl.victory || saved.context.players.size() != impl.setup.players.size()
        || saved.context.planets.size() != impl.setup.planets.size())
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "saved victory monitor owner mismatch"));
    for (std::size_t i = 0; i < saved.context.players.size(); ++i)
        if (saved.context.players[i].player != impl.setup.players[i].player_id)
            return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "saved victory player mismatch"));
    for (std::size_t i = 0; i < saved.context.planets.size(); ++i)
        if (saved.context.planets[i].planet != impl.setup.planets[i].id)
            return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "saved victory planet mismatch"));
    impl.victory = saved; impl.dirty[16] = true;
    const auto old = impl.snapshot->events();
    impl.snapshot = impl.make_snapshot({old.begin(), old.end()});
    return core::Result<void>::success();
}

void GalacticSession::Impl::update_victory_actor(const CensusObject& actor) {
    if (!victory) return;
    auto& rows = victory->context.leaders;
    auto existing = std::find_if(rows.begin(), rows.end(), [&](const auto& row) {
        return !row.scheduled_return && row.object == actor.id;
    });
    const auto* owner = player(victory->context, actor.owner);
    const bool matches_leader = owner && owner->leader_company && owner->leader_unit_type
        && !owner->leader_unit.empty() && actor.current_type == owner->leader_unit_type;
    if (!matches_leader) {
        if (existing != rows.end()) { rows.erase(existing); dirty[16] = true; }
        return;
    }
    VictoryLeader current{actor.owner, owner->leader_company, owner->leader_unit,
        false, actor.dead, false, actor.delete_pending, 0, actor.id};
    if (existing != rows.end()) {
        if (existing->owner == current.owner && existing->company == current.company) {
            current.killer = existing->killer;
            current.marked_for_death = existing->marked_for_death;
        }
        if (*existing != current) { *existing = std::move(current); dirty[16] = true; }
    } else {
        // Active census order is ascending persistent identity. Only sparse
        // births/current-type changes touch this projection, never a frame scan.
        const auto position = std::find_if(rows.begin(), rows.end(), [&](const auto& row) {
            return !row.scheduled_return && row.object > actor.id;
        });
        rows.insert(position, std::move(current)); dirty[16] = true;
    }
}
void GalacticSession::Impl::remove_victory_actor(EntityId object) {
    if (!victory) return;
    auto& rows = victory->context.leaders;
    if (std::erase_if(rows, [&](const auto& row) { return !row.scheduled_return && row.object == object; })) dirty[16] = true;
}
void GalacticSession::sync_victory_hero_returns(std::span<const HeroReturnRecord> records) {
    auto& impl = *impl_;
    if (!impl.victory) return;
    std::vector<VictoryLeader> rows;
    for (const auto& record : records) {
        const auto* owner = player(impl.victory->context, record.owner);
        if (!owner || !owner->leader_company || record.type != owner->leader_company) continue;
        rows.push_back({record.owner, record.type, owner->leader_unit, true, false, false, false, record.killer, record.defeated});
    }
    // Zero object identities are explicit fixture/provider inputs, not inferred
    // return records. Runtime lifecycle rows always have the defeated identity.
    for (const auto& row : impl.victory->context.leaders)
        if (!row.scheduled_return || !row.object) rows.push_back(row);
    if (rows == impl.victory->context.leaders) return;
    impl.victory->context.leaders = std::move(rows); impl.dirty[16] = true;
    const auto old = impl.snapshot->events();
    impl.snapshot = impl.make_snapshot({old.begin(), old.end()});
}
core::Result<void> GalacticSession::apply_hero_defeat_victory(const HeroDefeat& defeat,
    std::span<const HeroReturnRecord> records, const PartitionExecutor& executor) {
    auto& impl = *impl_;
    if (!impl.victory) return core::Result<void>::success();
    const auto* victim = player(impl.victory->context, defeat.owner);
    if (!victim || !defeat.id || !defeat.type)
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "leader defeat has no qualified identity"));
    // WGCH-76: this is an explicit attributed defeat, even when respawn refuses.
    // Bind the exact company and its actual active unit/container ancestry.
    const auto belongs = [&](EntityId unit) {
        for (std::size_t depth = 0; unit && depth <= impl.actor_records.size(); ++depth) {
            if (unit == defeat.id) return true;
            const auto at = impl.actor_records.find(unit);
            if (at == impl.actor_records.end()) break;
            unit = at->second.container;
        }
        return false;
    };
    if (defeat.type == victim->leader_company || defeat.respawn_type == victim->leader_company)
        for (auto& row : impl.victory->context.leaders)
            if (!row.scheduled_return && row.owner == defeat.owner && belongs(row.object)) {
                row.dead = true; row.marked_for_death = true; row.deletion_pending = true;
                row.killer = defeat.killer ? defeat.killer : defeat.parent_killer; impl.dirty[16] = true;
            }
    sync_victory_hero_returns(records);
    const auto context = impl.victory_context();
    const auto queued_before = impl.victory->queued.size();
    const auto old = impl.snapshot->events();
    std::vector<GalacticEvent> events(old.begin(), old.end());
    bool registered = false;
    // Bounded player dispatch; WGS-24's entity predicate retains partitioned work.
    for (const auto& contender : context.players) {
        if (impl.victory->pending && !impl.victory->waits) break;
        if (allied(context, contender.player, defeat.owner)) continue;
        auto tested = test_victory(*impl.victory, {VictoryKind::kill_leader, contender.player},
            context, executor, impl.clock.galactic_frame);
        if (!tested) return core::Result<void>::failure(tested.error());
        if (tested.value()) {
            events.push_back({impl.completed_tick, GalacticEventKind::victory_pending, {}, contender.player, 0,
                static_cast<std::uint64_t>(VictoryKind::kill_leader), GalacticRejectReason::none});
            registered = true; break;
        }
    }
    if (registered || impl.victory->queued.size() != queued_before) impl.dirty[16] = true;
    impl.snapshot = impl.make_snapshot(std::move(events));
    return core::Result<void>::success();
}

core::Result<bool> GalacticSession::apply_planet_destruction_victory(PlayerId actor,
    PlayerId controller, std::span<const TypeId> landed) {
    return impl_->planet_destruction_victory(actor, controller, landed);
}
core::Result<bool> GalacticSession::Impl::planet_destruction_victory(PlayerId actor,
    PlayerId controller, std::span<const TypeId> landed) {
    auto& impl = *this;
    if (impl.failed) return core::Result<bool>::failure(detail::diagnostic(
        diagnostic_codes::invalid_setup, "planet destruction victory has no live owner"));
    if (!impl.victory) return yes(false);
    const auto* owner = player(impl.victory->context, controller);
    if (!owner || !owner->planet_leader_type || !active(*impl.victory, VictoryKind::superweapon_kill_leader, actor)
        || std::find(landed.begin(), landed.end(), owner->planet_leader_type) == landed.end()) return yes(false);
    // WGS-79/WGCH-158: strategic admission ignores the tactical name filter.
    // Registration itself is direct: waits still postpone the mature reader.
    if (!register_victory(*impl.victory, VictoryKind::superweapon_kill_leader, actor,
        galactic_frames_per_second, impl.clock.galactic_frame)) return yes(false);
    impl.dirty[16] = true;
    const auto old = impl.snapshot->events();
    std::vector<GalacticEvent> events(old.begin(), old.end());
    events.push_back({impl.completed_tick, GalacticEventKind::victory_pending, {}, actor, 0,
        static_cast<std::uint64_t>(VictoryKind::superweapon_kill_leader), GalacticRejectReason::none});
    impl.snapshot = impl.make_snapshot(std::move(events));
    return yes(true);
}

core::Result<void> GalacticSession::apply_superweapon_victory_return(PlayerId owner, PlayerId winner,
    PlayerId loser, bool killed) {
    auto& impl = *impl_;
    if (!impl.victory) return core::Result<void>::success();
    auto before = *impl.victory;
    const auto c = impl.victory_context();
    victory_superweapon_return(*impl.victory, {owner, winner, loser, true, false, killed}, c, impl.clock.galactic_frame);
    if (*impl.victory != before) {
        impl.dirty[16] = true;
        auto events = std::vector<GalacticEvent>(impl.snapshot->events().begin(), impl.snapshot->events().end());
        if (!before.pending && impl.victory->pending) events.push_back({impl.completed_tick, GalacticEventKind::victory_pending, {},
            impl.victory->pending->player, 0, static_cast<std::uint64_t>(impl.victory->pending->kind), GalacticRejectReason::none});
        impl.snapshot = impl.make_snapshot(std::move(events));
    }
    return core::Result<void>::success();
}

VictoryContext GalacticSession::Impl::victory_context() const {
    // State 16 retains planet type/destroyed metadata and supplied leader records.
    // Predicates read those records; live ownership replaces their initial owners below.
    auto c = victory->context;
    c.fiscal_cycle = clock.fiscal_cycle;
    for (auto& p : c.players) {
        if (economy) for (const auto& e : economy->players) if (e.player == p.player) p.credits = e.credits;
        if (seat_control) for (const auto& owner : seat_control->control.controllers) if (owner.player == p.player) {
            p.ai = owner.kind == FactionControl::ai;
            p.human = owner.kind == FactionControl::human;
        }
    }
    for (auto& p : c.planets) {
        if (const auto it = planets.find(p.planet); it != planets.end()) p.owner = registry.get<session_detail::PlanetData>(it->second).owner;
        p.current = p.owner; p.target = p.owner;
        if (control) {
            for (const auto& row : control->state) if (row.planet == p.planet) { p.current = row.current_player; p.target = row.target_player; }
            for (const auto& row : control->planets) if (row.planet == p.planet) p.destroyed = row.destroyed;
        }
    }
    if (control) c.relations = control->relations;
    // WGS-23/24: leaders are maintained by sparse actor and explicit hero-return
    // lifecycle writes. No disappearance inference or per-frame object gather.
    return c;
}
core::Result<void> GalacticSession::Impl::test_credit_condition(PlayerId id,
    std::uint64_t tick, std::vector<GalacticEvent>& events) {
    if (!victory || (victory->pending && !victory->waits)) return core::Result<void>::success();
    // WGEP-16: scalar account admission, synchronously after its committed write.
    // Credits never enter a partition phase; reuse WGS-18/30 admission unchanged.
    // This input is credits-only: fiscal_cycle, relations, planets and leaders are
    // deliberately omitted. Other condition kinds require victory_context().
    VictoryContext c;
    c.multiplayer = victory->context.multiplayer;
    c.tutorial_delayed = victory->context.tutorial_delayed;
    c.players = victory->context.players;
    for (auto& p : c.players) {
        if (economy) for (const auto& e : economy->players) if (e.player == p.player) p.credits = e.credits;
        if (seat_control) for (const auto& owner : seat_control->control.controllers) if (owner.player == p.player) {
            p.ai = owner.kind == FactionControl::ai;
            p.human = owner.kind == FactionControl::human;
        }
    }
    const auto queued = victory->queued.size();
    const InlineExecutor scalar;
    auto accepted = test_victory(*victory, {VictoryKind::credits, id}, c, scalar, clock.galactic_frame);
    if (!accepted) return core::Result<void>::failure(accepted.error());
    if (accepted.value()) events.push_back({tick, GalacticEventKind::victory_pending, {}, id, 0,
        static_cast<std::uint64_t>(VictoryKind::credits), GalacticRejectReason::none});
    if (accepted.value() || victory->queued.size() != queued) dirty[16] = true;
    return core::Result<void>::success();
}
core::Result<void> GalacticSession::Impl::test_conditions(const PartitionExecutor& executor,
    std::span<const VictoryKind> kinds, std::uint64_t tick, std::vector<GalacticEvent>& events) {
    // WGS-18: a wait queues admission even when a pending copy already exists.
    if (!victory || (victory->pending && !victory->waits)) return core::Result<void>::success();
    const auto c = victory_context();
    const auto queued_before = victory->queued.size();
    // WGM-191/WGS-171: player storage order, then the producer's requested kinds.
    // Bounded dispatch only; entity predicates remain partitioned. A wait queues
    // each request once, independently of duplicate or absent installed rows.
    bool won = false;
    for (const auto& contender : c.players) {
        for (const auto kind : kinds) {
            auto accepted = test_victory(*victory, {kind, contender.player}, c, executor, clock.galactic_frame);
            if (!accepted) return core::Result<void>::failure(accepted.error());
            if (accepted.value()) {
                events.push_back({tick, GalacticEventKind::victory_pending, {}, contender.player, 0,
                    static_cast<std::uint64_t>(kind), GalacticRejectReason::none});
                won = true;
                break;
            }
        }
        if (won) break;
    }
    if (won || queued_before != victory->queued.size()) dirty[16] = true;
    return core::Result<void>::success();
}

namespace detail {
void encode_victory(std::vector<std::uint8_t>& bytes, const VictoryState& s) {
    put_u32(bytes, victory_state_encoding_version);
    const auto names = [&](const auto& list) {
        put_u32(bytes, static_cast<std::uint32_t>(list.size()));
        for (const auto& n : list) { put_u32(bytes, static_cast<std::uint32_t>(n.size())); put_bytes(bytes, {reinterpret_cast<const std::uint8_t*>(n.data()), n.size()}); }
    };
    const auto row = [&](const GalacticVictoryCondition& r) {
        put_u8(bytes, static_cast<std::uint8_t>(r.kind)); put_u32(bytes, r.player); put_u32(bytes, r.delay);
        put_i64(bytes, r.parameters.credits); put_i64(bytes, r.parameters.cycles); put_i64(bytes, r.parameters.control_fraction.raw());
        names(r.parameters.factions); names(r.parameters.leaders); names(r.parameters.planets);
        put_u8(bytes, r.occurred); put_u8(bytes, r.suppress_text);
    };
    const auto text = [&](const std::string& n) { names(std::vector<std::string>{n}); };
    const auto& c = s.context;
    put_u8(bytes, c.multiplayer); put_u8(bytes, c.tutorial_delayed); put_u64(bytes, c.fiscal_cycle);
    put_u32(bytes, static_cast<std::uint32_t>(c.players.size()));
    for (const auto& p : c.players) {
        put_u32(bytes, p.player); text(p.faction); put_u8(bytes, p.neutral); put_u8(bytes, p.playable);
        put_u8(bytes, p.human); put_u8(bytes, p.ai); put_i64(bytes, p.credits.raw());
        put_u32(bytes, p.primary_enemy); put_u64(bytes, p.home); put_u64(bytes, p.leader_company); text(p.leader_unit); text(p.planet_leader_unit);
        put_u64(bytes, p.leader_unit_type); put_u64(bytes, p.planet_leader_type);
    }
    put_u32(bytes, static_cast<std::uint32_t>(c.planets.size()));
    for (const auto& p : c.planets) {
        put_u64(bytes, p.planet); put_u64(bytes, p.type); put_u32(bytes, p.current); put_u32(bytes, p.target); put_u32(bytes, p.owner); put_u8(bytes, p.destroyed);
    }
    put_u32(bytes, static_cast<std::uint32_t>(c.relations.size()));
    for (const auto& r : c.relations) { put_u32(bytes, r.player); put_u32(bytes, r.other); put_u8(bytes, r.allied); }
    put_u32(bytes, static_cast<std::uint32_t>(c.types.size()));
    for (const auto& t : c.types) { text(t.name); put_u64(bytes, t.type); }
    put_u32(bytes, static_cast<std::uint32_t>(c.superweapon_killers.size()));
    for (const auto t : c.superweapon_killers) put_u64(bytes, t);
    put_u32(bytes, static_cast<std::uint32_t>(c.leaders.size()));
    for (const auto& l : c.leaders) {
        put_u32(bytes, l.owner); put_u64(bytes, l.company); text(l.unit);
        put_u8(bytes, l.scheduled_return); put_u8(bytes, l.dead); put_u8(bytes, l.marked_for_death); put_u8(bytes, l.deletion_pending); put_u32(bytes, l.killer);
        put_u64(bytes, l.object);
    }
    put_u32(bytes, static_cast<std::uint32_t>(s.installed.size()));
    for (const auto& r : s.installed) row(r);
    put_u8(bytes, s.pending.has_value()); if (s.pending) row(*s.pending);
    put_i64(bytes, s.remaining); put_u64(bytes, s.last_service); put_u32(bytes, s.waits);
    put_u8(bytes, s.use_retry); put_u8(bytes, s.retry_waiting); put_u8(bytes, s.result_dispatched);
    put_u32(bytes, static_cast<std::uint32_t>(s.queued.size()));
    for (const auto& q : s.queued) { put_u8(bytes, static_cast<std::uint8_t>(q.kind)); put_u32(bytes, q.player); }
}
std::optional<VictoryState> decode_victory(std::span<const std::uint8_t> bytes) {
    sim::detail::Reader in(bytes); VictoryState s;
    std::uint32_t version{};
    if (!in.read_u32(version) || version != victory_state_encoding_version) return std::nullopt;
    const auto count = [&](std::uint32_t& n, std::size_t minimum) { return in.read_u32(n) && n <= galactic_max_objects && n <= in.remaining() / minimum; };
    const auto boolean = [&](bool& v) { std::uint8_t b{}; if (!in.read_u8(b) || b > 1) return false; v = b != 0; return true; };
    const auto names = [&](auto& list) {
        std::uint32_t n{}; if (!count(n, 4)) return false;
        for (std::uint32_t i = 0; i < n; ++i) {
            std::uint32_t size{}; if (!in.read_u32(size) || size > 65536 || size > in.remaining()) return false;
            std::vector<std::uint8_t> text(size); if (!in.read_bytes(text)) return false;
            list.emplace_back(text.begin(), text.end());
        }
        return true;
    };
    const auto row = [&](GalacticVictoryCondition& r) {
        std::uint8_t k{}; std::int64_t fraction{};
        if (!in.read_u8(k) || k < 1 || k > static_cast<unsigned>(VictoryKind::story_forced)
            || !in.read_u32(r.player) || !in.read_u32(r.delay)
            || !in.read_i64(r.parameters.credits) || !in.read_i64(r.parameters.cycles) || !in.read_i64(fraction)) return false;
        r.kind = static_cast<VictoryKind>(k); r.parameters.control_fraction = math::Fixed::from_raw(fraction);
        return names(r.parameters.factions) && names(r.parameters.leaders) && names(r.parameters.planets) && boolean(r.occurred) && boolean(r.suppress_text);
    };
    const auto text = [&](std::string& out) { std::vector<std::string> list; if (!names(list) || list.size() != 1) return false; out = std::move(list.front()); return true; };
    auto& c = s.context;
    std::uint32_t n{};
    if (!boolean(c.multiplayer) || !boolean(c.tutorial_delayed) || !in.read_u64(c.fiscal_cycle) || !count(n, 48)) return std::nullopt;
    for (std::uint32_t i = 0; i < n; ++i) {
        VictoryPlayer p; std::int64_t credits{};
        if (!in.read_u32(p.player) || !text(p.faction) || !boolean(p.neutral) || !boolean(p.playable)
            || !boolean(p.human) || !boolean(p.ai) || !in.read_i64(credits) || !in.read_u32(p.primary_enemy)
            || !in.read_u64(p.home) || !in.read_u64(p.leader_company) || !text(p.leader_unit) || !text(p.planet_leader_unit)
            || !in.read_u64(p.leader_unit_type) || !in.read_u64(p.planet_leader_type)) return std::nullopt;
        p.credits = math::Fixed::from_raw(credits); c.players.push_back(std::move(p));
    }
    if (!count(n, 29)) return std::nullopt;
    for (std::uint32_t i = 0; i < n; ++i) {
        VictoryPlanet p;
        if (!in.read_u64(p.planet) || !in.read_u64(p.type) || !in.read_u32(p.current) || !in.read_u32(p.target)
            || !in.read_u32(p.owner) || !boolean(p.destroyed)) return std::nullopt;
        c.planets.push_back(p);
    }
    if (!count(n, 9)) return std::nullopt;
    for (std::uint32_t i = 0; i < n; ++i) {
        ControlRelation r; if (!in.read_u32(r.player) || !in.read_u32(r.other) || !boolean(r.allied)) return std::nullopt;
        c.relations.push_back(r);
    }
    if (!count(n, 16)) return std::nullopt;
    for (std::uint32_t i = 0; i < n; ++i) {
        VictoryType t; if (!text(t.name) || !in.read_u64(t.type)) return std::nullopt; c.types.push_back(std::move(t));
    }
    if (!count(n, 8)) return std::nullopt;
    for (std::uint32_t i = 0; i < n; ++i) { TypeId t{}; if (!in.read_u64(t)) return std::nullopt; c.superweapon_killers.push_back(t); }
    if (!count(n, 28)) return std::nullopt;
    for (std::uint32_t i = 0; i < n; ++i) {
        VictoryLeader l;
        if (!in.read_u32(l.owner) || !in.read_u64(l.company) || !text(l.unit) || !boolean(l.scheduled_return)
            || !boolean(l.dead) || !boolean(l.marked_for_death) || !boolean(l.deletion_pending) || !in.read_u32(l.killer)
            || !in.read_u64(l.object)) return std::nullopt;
        c.leaders.push_back(std::move(l));
    }
    if (!count(n, 47)) return std::nullopt;
    for (std::uint32_t i = 0; i < n; ++i) { GalacticVictoryCondition r; if (!row(r)) return std::nullopt; s.installed.push_back(std::move(r)); }
    bool pending{}; if (!boolean(pending)) return std::nullopt;
    if (pending) { GalacticVictoryCondition r; if (!row(r)) return std::nullopt; s.pending = std::move(r); }
    if (!in.read_i64(s.remaining) || !in.read_u64(s.last_service) || !in.read_u32(s.waits)
        || !boolean(s.use_retry) || !boolean(s.retry_waiting) || !boolean(s.result_dispatched) || !count(n, 5)) return std::nullopt;
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint8_t k{}; VictoryTest q; if (!in.read_u8(k) || !in.read_u32(q.player)) return std::nullopt;
        q.kind = static_cast<VictoryKind>(k); s.queued.push_back(q);
    }
    if (in.remaining() || !validate_victory(s)) return std::nullopt;
    return s;
}
} // namespace detail
} // namespace eawr::sim::galactic
