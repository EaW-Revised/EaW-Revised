#pragma once

#include "checkpoint_archive_internal.hpp"
#include "session_impl.hpp"

namespace eawr::sim::tactical::checkpoint_detail {

// Retain the actual publication: live effects are derived earlier in the tick,
// and replaying their service here could change the saved barrier view.
struct PublishedVisibilityRow {
    std::uint64_t visible_to{};
    std::optional<math::Fixed> reveal_range;
    bool in_tractor_beam{};
};

template <> struct Fields<PublishedVisibilityRow> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.visible_to, v.reveal_range, v.in_tractor_beam);
    }
};

struct Access {
    using State = TacticalSession::Impl;
    using CommandSource = State::CommandSource;
    using ConcentrateEffect = State::ConcentrateEffect;
    using PendingBlastDamage = State::PendingBlastDamage;
    using FreeGarrisonState = State::FreeGarrisonState;
    using PathInputs = State::PathInputs;
    using TreeNode = detail::CollectionTree::Node;
    using TreeLink = detail::CollectionTree::Link;
    using FogAnchor = FogCells::Anchor;
    using PublishedVisibility = std::map<EntityId, PublishedVisibilityRow>;
    static PublishedVisibility visibility(const State& state) {
        PublishedVisibility rows;
        for (const auto& instance : state.current_snapshot->instances())
            rows.emplace(instance.entity_id, PublishedVisibilityRow{instance.visible_to, instance.reveal_range, instance.in_tractor_beam});
        return rows;
    }
    static void restore_visibility(State& state, const PublishedVisibility& rows) {
        auto snapshot = std::make_shared<TacticalSnapshot>(*state.current_snapshot);
        if (snapshot->instances_.size() != rows.size()) throw Invalid("published visibility disagrees with live units");
        const auto mask = state.setup.players.size() == 64U ? ~std::uint64_t{0}
            : (std::uint64_t{1} << state.setup.players.size()) - 1U;
        for (auto& instance : snapshot->instances_) {
            const auto found = rows.find(instance.entity_id);
            if (found == rows.end() || (found->second.visible_to & ~mask) != 0U
                || (found->second.reveal_range && found->second.reveal_range->raw() < 0)) throw Invalid("invalid published visibility");
            instance.visible_to = found->second.visible_to;
            instance.reveal_range = found->second.reveal_range;
            instance.in_tractor_beam = found->second.in_tractor_beam;
        }
        state.current_snapshot = std::move(snapshot);
    }

    template <class A, class T> static void tree(A& a, T& v) {
        a(v.nodes_, v.links_, v.moved_, v.last_service_frame_, v.need_rebuild_);
        if constexpr (A::reading) {
            if (v.nodes_.empty() || v.nodes_[0].parent != -1) throw Invalid("collection tree has no root");
            std::set<EntityId> found;
            std::vector<std::uint32_t> depth(v.nodes_.size());
            for (std::size_t i = 0; i < v.nodes_.size(); ++i) {
                const auto& node = v.nodes_[i];
                for (const auto child : node.child) {
                    if (child < -1 || (child >= 0 && (static_cast<std::size_t>(child) <= i
                        || static_cast<std::size_t>(child) >= v.nodes_.size()
                        || v.nodes_[static_cast<std::size_t>(child)].parent != static_cast<std::int32_t>(i))))
                        throw Invalid("collection tree has an invalid child");
                }
                if (i > 0 && (node.parent < 0 || static_cast<std::size_t>(node.parent) >= i))
                    throw Invalid("collection tree has an invalid parent");
                if (i > 0) {
                    const auto& parent = v.nodes_[static_cast<std::size_t>(node.parent)];
                    if (parent.child[0] != static_cast<std::int32_t>(i) && parent.child[1] != static_cast<std::int32_t>(i))
                        throw Invalid("collection tree has an unreachable node");
                    depth[i] = depth[static_cast<std::size_t>(node.parent)] + 1U;
                    if (depth[i] > 17U) throw Invalid("collection tree exceeds CO-09 split depth");
                }
                for (const auto id : node.links) {
                    const auto link = v.links_.find(id);
                    if (id == invalid_entity_id || !found.insert(id).second || link == v.links_.end()
                        || link->second.node != static_cast<std::int32_t>(i)) throw Invalid("collection tree link disagrees with node");
                }
            }
            if (found.size() != v.links_.size()) throw Invalid("collection tree has detached links");
        }
    }
    template <class A, class T> static void trees(A& a, T& v) {
        a(v.trees_, v.owners_);
        if constexpr (A::reading) {
            if (!v.owners_) throw Invalid("missing collection owner index");
            std::map<EntityId, PlayerId> owners;
            for (const auto& [owner, tree] : v.trees_) {
                if (!tree) throw Invalid("missing collection tree");
                for (const auto& [id, link] : tree->links_) {
                    static_cast<void>(link);
                    if (!owners.emplace(id, owner).second) throw Invalid("duplicate collection owner");
                }
            }
            if (owners != *v.owners_) throw Invalid("collection owner index disagrees with trees");
        }
    }
    template <class A, class T> static void fog(A& a, T& v) {
        a(v.anchors_, v.values_, v.holds_, v.full_reveals_);
        if constexpr (A::reading) {
            if (!v.anchors_ || v.values_.size() != v.players_.size() || v.holds_.size() != v.players_.size())
                throw Invalid("fog player count disagrees with content");
            const auto declared = [&](const PlayerId owner) {
                return std::any_of(v.players_.begin(), v.players_.end(), [&](const Player& player) { return player.player_id == owner; });
            };
            for (const auto& [id, anchor] : *v.anchors_) {
                if (id == invalid_entity_id || !declared(anchor.owner) || anchor.radius < 0 || anchor.radius > 65535
                    || anchor.dense_radius < 0 || anchor.dense_radius > 65535 || anchor.column < 0 || anchor.row < 0
                    || static_cast<std::uint32_t>(anchor.column) >= v.rules_.cells_wide
                    || static_cast<std::uint32_t>(anchor.row) >= v.rules_.cells_tall) throw Invalid("invalid fog anchor");
            }
            for (const auto player : v.full_reveals_) if (!declared(player)) throw Invalid("unknown full-reveal player");
            for (std::size_t p = 0; p < v.players_.size(); ++p) {
                if (v.values_[p].size() != v.rules_.cells_tall || v.holds_[p].size() != v.rules_.cells_tall)
                    throw Invalid("fog row count disagrees with content");
                for (std::size_t r = 0; r < v.rules_.cells_tall; ++r) {
                    if (!v.values_[p][r] || !v.holds_[p][r] || v.values_[p][r]->size() != v.rules_.cells_wide
                        || v.holds_[p][r]->size() != v.rules_.cells_wide) throw Invalid("fog column count disagrees with content");
                }
            }
            v.flat_values_.assign(v.players_.size(), {});
        }
    }
    template <class A, class T> static void state(A& a, T& v) {
        a(v.completed_tick, v.next_id, v.rng_state, v.next_projectile,
            v.tracking_anchor, v.last_submitted, v.pads, v.construction, v.ledgers, v.arrivals,
            v.shares, v.earners, v.respawns, v.starbases, v.outcome, v.quits, v.losses,
            v.productions, v.economy_cues, v.squadrons, v.projectiles, v.pending_blast_damage,
            v.crafts, v.minds, v.spawners, v.free_garrisons, v.spins, v.collection,
            v.projectile_collection, v.manual_clocks, v.special_weapons, v.special_disabled_by_hardpoint,
            v.projectile_defence_order, v.ability_spawns, v.buzz_droids, v.reinforcement_searches,
            v.reinforcement_search_results, v.command_ledger.sources, v.command_ledger.recipients,
            v.concentrate_effects, v.concentrate_changed_targets,
            v.concentrate_service_census, v.concentrate_service_last_id,
            v.committed_bonus_profiles, v.named_ability_locks, v.departure_controls, v.basic_ai, v.beacons, v.snapshot_players);
        if (v.setup.campaign) a(v.command_ledger.battlefield_defence, v.command_ledger.reveal_multipliers, v.last_fog_multipliers);
        bool has_fog = v.fog.has_value();
        a(has_fog);
        if (has_fog != v.fog.has_value()) throw Invalid("fog binding disagrees with content");
        if (has_fog) fog(a, *v.fog);
    }
};

template <> struct Fields<Access::TreeNode> {
    template <class A, class T> static void apply(A& a, T& v) { a(v.box, v.links, v.parent, v.child); }
};
template <> struct Fields<Access::TreeLink> {
    template <class A, class T> static void apply(A& a, T& v) { a(v.bounds, v.node); }
};
template <> struct Fields<Access::FogAnchor> {
    template <class A, class T> static void apply(A& a, T& v) { a(v.column, v.row, v.radius, v.x, v.y, v.owner, v.dense_radius); }
};
template <> struct Fields<detail::CullBox> {
    template <class A, class T> static void apply(A& a, T& v) { a(v.min, v.max); }
};
template <> struct Fields<detail::CollectionTree> {
    template <class A, class T> static void apply(A& a, T& v) { Access::tree(a, v); }
};
template <> struct Fields<detail::CollectionTrees> {
    template <class A, class T> static void apply(A& a, T& v) { Access::trees(a, v); }
};
template <> struct Fields<SlicedPathSearch::Checkpoint> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.limits, v.footprint, v.layer, v.statics, v.interval, v.has_layer, v.has_statics,
            v.entity, v.tick, v.position, v.target, v.yaw, v.speed, v.through_hazards,
            v.filter_destination, v.expansions, v.started, v.ended);
    }
};
template <> struct Fields<CommandKey> {
    template <class A, class T> static void apply(A& a, T& v) { a(v.tick, v.player_id, v.sequence); }
};

} // namespace eawr::sim::tactical::checkpoint_detail
