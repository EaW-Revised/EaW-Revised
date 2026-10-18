#include "session_impl.hpp"
#include "session_canonical.hpp"
#include "session_encoding.hpp"
#include "../replay_internal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace eawr::sim::tactical {

void TacticalSession::Impl::append_canonical_manual_weapons_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts) const {
        std::uint64_t manual_weapons = counts ? counts->manual_weapons : 0;
        if (!counts) for (const auto& unit : units) {
            if (!unit.combat) continue;
            for (const auto& weapon : unit.combat->weapons) manual_weapons += weapon.manual ? 1 : 0;
        }
        // WAD-39/40: MANN exists only with manual weapons/clocks; all old sessions keep their bytes.
        if (manual_weapons != 0 || !manual_clocks.empty()) {
            bytes.insert(bytes.end(), {'M', 'A', 'N', 'N'});
            sim::detail::append_u64(bytes, manual_weapons);
            for (const auto& unit : units) {
                if (!unit.combat) continue;
                for (std::uint32_t index = 0; index < unit.combat->weapons.size(); ++index) {
                    const auto& weapon = unit.combat->weapons[index];
                    if (!weapon.manual) continue;
                    const auto& state = *weapon.manual;
                    sim::detail::append_u64(bytes, unit.state.entity_id);
                    sim::detail::append_u32(bytes, index);
                    sim::detail::append_u32(bytes, state.requesting_player);
                    sim::detail::append_u64(bytes, state.target);
                    sim::detail::append_u64(bytes, state.assigned_frame);
                    for (const auto angle : {state.yaw, state.pitch, state.desired_yaw, state.desired_pitch}) append_fixed(bytes, angle);
                }
            }
            sim::detail::append_u64(bytes, manual_clocks.size());
            for (const auto& [player, clock] : manual_clocks) {
                sim::detail::append_u32(bytes, player);
                sim::detail::append_u32(bytes, clock.cooldown_frames);
                sim::detail::append_u64(bytes, clock.last_fired_frame);
            }
        }
}

void TacticalSession::Impl::append_canonical_turrets_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts) const {
        std::uint64_t turret_count = counts ? counts->turret_count : 0;
        if (!counts) for (const auto& unit : units) {
            if (!unit.combat) continue;
            for (const auto& weapon : unit.combat->weapons) turret_count += weapon.turret ? 1 : 0;
        }
        if (turret_count != 0) {
            bytes.insert(bytes.end(), {'O', 'T', 'U', 'R'});
            sim::detail::append_u64(bytes, turret_count);
            for (const auto& unit : units) {
                if (!unit.combat) continue;
                for (std::uint32_t index = 0; index < unit.combat->weapons.size(); ++index) {
                    const auto& turret = unit.combat->weapons[index].turret;
                    if (!turret) continue;
                    sim::detail::append_u64(bytes, unit.state.entity_id);
                    sim::detail::append_u32(bytes, index);
                    sim::detail::append_u64(bytes, turret->target);
                    for (const auto angle : {turret->yaw, turret->pitch, turret->desired_yaw, turret->desired_pitch})
                        append_fixed(bytes, angle);
                    bytes.push_back(turret->ready ? 1 : 0);
                    bytes.push_back(turret->rest_started_frame ? 1 : 0);
                    sim::detail::append_u64(bytes, turret->rest_started_frame.value_or(0));
                    bytes.push_back(turret->returning_to_rest ? 1 : 0);
                }
            }
        }
}

void TacticalSession::Impl::append_canonical_projectiles_state(std::vector<std::uint8_t>& bytes, const std::span<const std::vector<std::uint8_t>> encoded) const {
        // Projectiles in flight (#74), with damage rules only.
        if (durability.damage) {
            bytes.insert(bytes.end(), projectile_tag.begin(), projectile_tag.end());
            sim::detail::append_u64(bytes, next_projectile);
            sim::detail::append_u64(bytes, projectiles.size());
            if (encoded.empty()) {
                for (const auto& projectile : projectiles) detail::append_projectile(bytes, projectile);
            } else for (const auto& part : encoded) bytes.insert(bytes.end(), part.begin(), part.end());
        }
}

void TacticalSession::Impl::append_canonical_blast_damage_state(std::vector<std::uint8_t>& bytes) const {
        // Waiting group members (#344), sliced searches (PC-08), and failed searches (FC-68), each
        if (!pending_blast_damage.empty()) {
            constexpr std::array<std::uint8_t, 4> blast_tag{'D', 'D', 'M', 'G'};
            bytes.insert(bytes.end(), blast_tag.begin(), blast_tag.end());
            sim::detail::append_u32(bytes, 1);
            sim::detail::append_u32(bytes, 0);
            sim::detail::append_u64(bytes, pending_blast_damage.size());
            std::vector<std::size_t> order(pending_blast_damage.size());
            for (std::size_t index = 0; index < order.size(); ++index) order[index] = index;
            std::stable_sort(order.begin(), order.end(), [&](const auto left, const auto right) {
                return pending_blast_damage[left].due < pending_blast_damage[right].due;
            });
            for (const auto index : order) {
                const auto& queued = pending_blast_damage[index];
                sim::detail::append_u64(bytes, queued.due);
                sim::detail::append_u64(bytes, queued.recipient);
                append_fixed(bytes, queued.amount);
                sim::detail::append_u32(bytes, queued.owner);
                sim::detail::append_u32(bytes, queued.damage_type);
                sim::detail::append_u32(bytes, queued.internal_damage_misc ? 1U : 0U);
                sim::detail::append_u32(bytes, static_cast<std::uint32_t>(queued.contacted_mesh.size()));
                bytes.insert(bytes.end(), queued.contacted_mesh.begin(), queued.contacted_mesh.end());
            }
        }
}

void TacticalSession::Impl::append_canonical_collection_state(std::vector<std::uint8_t>& bytes, const std::span<const std::vector<std::uint8_t>> encoded) const {
        if (!collection.empty()) {
            bytes.insert(bytes.end(), collection_tag.begin(), collection_tag.end());
            if (encoded.empty()) collection.append_state(bytes);
            else {
                sim::detail::append_u64(bytes, collection.state_trees().size());
                for (const auto& part : encoded) bytes.insert(bytes.end(), part.begin(), part.end());
            }
        }
}

void TacticalSession::Impl::append_canonical_projectile_collection_state(std::vector<std::uint8_t>& bytes, const std::span<const std::vector<std::uint8_t>> encoded) const {
        if (projectile_collection.has_history()) {
            bytes.insert(bytes.end(), projectile_collection_tag.begin(), projectile_collection_tag.end());
            if (encoded.empty()) projectile_collection.append_state(bytes);
            else {
                sim::detail::append_u64(bytes, projectile_collection.state_trees().size());
                for (const auto& part : encoded) bytes.insert(bytes.end(), part.begin(), part.end());
            }
        }
}

void TacticalSession::Impl::append_canonical_ion_stuns_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts) const {
        // Ion stuns (#561), ascending ID, only while a live unit has one.
        std::uint64_t stunned = counts ? counts->stunned : 0;
        if (!counts) for (const auto& unit : units) stunned += unit.ion_stun ? 1 : 0;
        if (stunned != 0) {
            bytes.insert(bytes.end(), ion_stun_tag.begin(), ion_stun_tag.end());
            sim::detail::append_u64(bytes, stunned);
            for (const auto& unit : units) {
                if (!unit.ion_stun) continue;
                sim::detail::append_u64(bytes, unit.state.entity_id);
                append_ion_stun(bytes, *unit.ion_stun);
            }
        }
}

void TacticalSession::Impl::append_canonical_engine_disable_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts) const {
        const auto disabled_engines = (counts ? counts->disabled_engines : static_cast<std::uint64_t>(std::count_if(units.begin(), units.end(), [](const LiveUnit& unit) {
            return unit.durability && unit.durability->engines_disabled_until;
        })));
        if (disabled_engines != 0) {
            bytes.insert(bytes.end(), engine_disable_tag.begin(), engine_disable_tag.end());
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(disabled_engines));
            for (const auto& unit : units) {
                if (!unit.durability || !unit.durability->engines_disabled_until) continue;
                sim::detail::append_u64(bytes, unit.state.entity_id);
                sim::detail::append_u64(bytes, *unit.durability->engines_disabled_until);
            }
        }
}

void TacticalSession::Impl::append_canonical_damage_tracking_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts) const {
        // Damage tracking (WCC-82), ascending ID, only while a live unit's window or rate is not zero.
        const auto tracking = [](const LiveUnit& unit) {
            return unit.durability && (unit.durability->window_damage.raw() != 0 || unit.durability->damage_rate.raw() != 0);
        };
        if (const auto tracked = (counts ? counts->tracked : static_cast<std::uint64_t>(std::count_if(units.begin(), units.end(), tracking))); tracked != 0) {
            bytes.insert(bytes.end(), damage_tracking_tag.begin(), damage_tracking_tag.end());
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(tracked));
            for (const auto& unit : units) {
                if (!tracking(unit)) continue;
                sim::detail::append_u64(bytes, unit.state.entity_id);
                sim::detail::append_i64(bytes, unit.durability->window_damage.raw());
                sim::detail::append_i64(bytes, unit.durability->damage_rate.raw());
            }
        }
}

void TacticalSession::Impl::append_canonical_command_bonuses_state(std::vector<std::uint8_t>& bytes) const {
        if (!command_ledger.sources.empty()) {
            // Coordinator-reserved HBON; station-only sessions keep their exact encoding.
            constexpr std::array<std::uint8_t, 4> tag{'H', 'B', 'O', 'N'};
            bytes.insert(bytes.end(), tag.begin(), tag.end());
            sim::detail::append_u64(bytes, command_ledger.sources.size());
            for (const auto& source : command_ledger.sources) {
                sim::detail::append_u64(bytes, source.id);
                sim::detail::append_u64(bytes, source.host);
                const auto& profile = economy.command_bonuses[source.profile];
                sim::detail::append_u64(bytes, profile.type);
                sim::detail::append_u32(bytes, profile.slot);
                sim::detail::append_u64(bytes, source.targets.size());
                for (const auto target : source.targets) sim::detail::append_u64(bytes, target);
            }
        }
}

void TacticalSession::Impl::append_canonical_projectile_defence_state(std::vector<std::uint8_t>& bytes) const {
        if (!projectile_defence_order.empty()) {
            // Coordinator-reserved PDEF: source IDs in registration order, never sorted.
            constexpr std::array<std::uint8_t, 4> tag{'P', 'D', 'E', 'F'};
            bytes.insert(bytes.end(), tag.begin(), tag.end());
            sim::detail::append_u64(bytes, projectile_defence_order.size());
            for (const auto id : projectile_defence_order) sim::detail::append_u64(bytes, id);
        }
}

} // namespace eawr::sim::tactical
