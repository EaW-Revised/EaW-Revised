#include "eawr/sim/tactical/session.hpp"
#include "eawr/core/sha256.hpp"

#include "session_impl.hpp"
#include "tactical_internal.hpp"

#include <algorithm>
#include <set>

namespace eawr::sim::tactical {
core::Result<void> TacticalSession::configure_retreat(RetreatConfig config) {
    if (impl_->setup.domain != BattleDomain::space)
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup,
            "hyperspace retreat requires the space battle domain"));
    if (impl_->object_turn_running)
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_command,
            "configure_retreat during an object-turn provider call"));
    if (impl_->completed_tick || impl_->retreat)
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup,
            "retreat content must be installed once before stepping"));
    if (const auto valid = validate_retreat_rules(config.rules); !valid) return valid;
    std::set<EntityId> ids;
    for (const auto& actor : config.actors)
        if (!actor.id || !ids.insert(actor.id).second || !impl_->teams.contains(actor.owner))
            return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup,
                "retreat actor needs unique identity and known owner"));
    for (const auto& source : config.prevention) {
        const auto found = impl_->handles.find(source.unit);
        PlayerId owner{};
        std::size_t slots{};
        if (found != impl_->handles.end()) {
            const auto live = impl_->live_at(source.unit, found->second);
            owner = live.state.owner;
            if (live.abilities) slots = live.abilities->slots.size();
        } else {
            // GC reserves retain their authored identity before deployment. The
            // live gate below ignores them until their actor actually exists.
            const auto* reserve = impl_->setup.campaign ? [&]() -> const CampaignReserve* {
                for (const auto& row : impl_->setup.campaign->reserves)
                    if (row.id == source.unit) return &row;
                return nullptr;
            }() : nullptr;
            if (!config.rules.campaign || !reserve)
                return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup,
                    "retreat prevention source is absent"));
            owner = reserve->owner;
            const auto* deployment = impl_->economy.hero(reserve->type);
            if (const auto* profile = impl_->abilities.find(deployment ? deployment->deployed : reserve->type))
                slots = profile->abilities.size();
        }
        if (source.ability_slot >= slots)
            return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup,
                "retreat prevention needs an authored ability slot"));
        for (const auto enemy : source.enemies)
            if (!impl_->teams.contains(enemy) || impl_->teams.at(enemy) == impl_->teams.at(owner))
                return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup,
                    "retreat prevention recipient must be an enemy"));
    }
    resolve_cinematic_seats(impl_->setup, impl_->economy.players);
    if (impl_->setup.campaign) {
        TacticalReplay recording; recording.setup = impl_->setup;
        const auto encoded = write_replay(recording);
        if (!encoded) return core::Result<void>::failure(encoded.error());
        impl_->campaign_binding = core::sha256(encoded.value());
    }
    impl_->retreat = std::move(config);
    impl_->publish_staged(impl_->sorted_live(), {}, {});
    return core::Result<void>::success();
}
RetreatGate TacticalSession::Impl::retreat_gate(const PlayerId player) const {
    RetreatGate gate; gate.battle_decided = outcome.has_value();
    if (!retreat) return gate;
    for (const auto& source : retreat->prevention) {
        if (std::find(source.enemies.begin(), source.enemies.end(), player) == source.enemies.end()) continue;
        const auto at = handles.find(source.unit);
        if (at == handles.end()) continue;
        const auto live = live_at(source.unit, at->second);
        gate.prevention = gate.prevention || (live.abilities && source.ability_slot < live.abilities->slots.size()
            && live.abilities->slots[source.ability_slot].active);
    }
    for (const auto id : retreat->superweapons) {
        const auto at = handles.find(id);
        if (at != handles.end() && live_at(id, at->second).state.owner == player) gate.superweapon = true;
    }
    return gate;
}
std::vector<RetreatActor> TacticalSession::Impl::retreat_actors(const UnitStage* staged) const {
    if (!retreat) return {};
    auto actors = retreat->actors;
    // A request/expiry boundary only: preserve the source roster traversal for WGT-36.
    // Ordinary per-unit work stays in the world's existing worker phases.
    for (auto& actor : actors) {
        std::optional<LiveUnit> current;
        if (staged) {
            const auto at = staged->find(actor.id);
            if (at != staged->end()) current = at->second;
        } else {
            const auto at = handles.find(actor.id);
            if (at != handles.end()) current = live_at(actor.id, at->second);
        }
        actor.live = current.has_value();
        if (!actor.live) continue;
        const auto& live = *current; actor.owner = live.state.owner;
        const auto named_carried = [&](auto&& self, const auto& children) -> bool {
            for (const auto& child : children)
                if (child.named_hero || self(self, child.members)) return true;
            return false;
        };
        actor.carries_named_hero |= named_carried(named_carried, live.state.contained);
        actor.locomotion = actor.locomotion && (live.motion.has_value() || minds.contains(actor.id));
        if (live.durability) {
            const auto* profile = health_profile(live);
            if (profile) {
                actor.engines_present = std::any_of(profile->hardpoints.begin(), profile->hardpoints.end(),
                    [](const auto& hp) { return hp.role == HardpointRole::engine; });
                actor.engines_standing = !actor.engines_present;
                for (std::size_t i = 0; i < profile->hardpoints.size(); ++i)
                    if (profile->hardpoints[i].role == HardpointRole::engine && !hardpoint_destroyed(*profile, *live.durability, i))
                        actor.engines_standing = true;
                actor.engines_disabled = live.durability->engines_disabled_until
                    && completed_tick < *live.durability->engines_disabled_until;
            }
        }
    }
    return actors;
}
bool TacticalSession::Impl::retreat_holds(const EntityId id) const {
    return retreat_state.phase != RetreatPhase::idle
        && std::find(retreat_state.actors.begin(), retreat_state.actors.end(), id) != retreat_state.actors.end();
}
bool TacticalSession::Impl::retreat_escape(const EntityId id) const {
    return retreat_state.phase >= RetreatPhase::escaping && retreat_holds(id);
}
std::vector<RetreatView> TacticalSession::retreat_views() const {
    return impl_->retreat_views();
}
std::vector<RetreatView> TacticalSession::Impl::retreat_views() const {
    std::vector<RetreatView> out;
    if (!retreat) return out;
    for (const auto& player : retreat->rules.players)
        out.push_back({retreat_state, !retreat->flight_qualified ? RetreatRefusal::wrong_mode
            : retreat_eligibility(retreat->rules, retreat_state,
            player.player, completed_tick, retreat_gate(player.player)), player.player,
            false, // WAR-41/45 native interleaved allocation owner is not installed.
            retreat->rules.lockout_frames});
    return out;
}
const RetreatState& TacticalSession::retreat_state() const noexcept { return impl_->retreat_state; }
} // namespace eawr::sim::tactical
