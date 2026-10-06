#include "eawr/core/load_profile.hpp"
#include "eawr/sim/tactical/session.hpp"

#include "eawr/sim/tactical/formation.hpp"
#include "eawr/sim/tactical/pathfind.hpp"

#include "../math/wide.hpp"
#include "../replay_internal.hpp"
#include "combat_internal.hpp"
#include "blast_internal.hpp"
#include "fighters_internal.hpp"
#include "motion_internal.hpp"
#include "orders_internal.hpp"
#include "staging.hpp"
#include "session_impl.hpp"
#include "tactical_internal.hpp"

#include "../../../third_party/entt/single_include/entt/entt.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "session_encoding.hpp"

namespace eawr::sim::tactical::session_detail {

void append_outcome(std::vector<std::uint8_t>& bytes, const BattleOutcome& outcome) {
    bytes.insert(bytes.end(), victory_tag.begin(), victory_tag.end());
    sim::detail::append_u32(bytes, victory_block_version);
    sim::detail::append_u32(bytes, static_cast<std::uint32_t>(outcome.condition));
    sim::detail::append_u32(bytes, outcome.winner);
    sim::detail::append_u32(bytes, outcome.winner_team);
    sim::detail::append_u64(bytes, outcome.decided_tick);
    sim::detail::append_u64(bytes, outcome.deciding_unit);
    sim::detail::append_u64(bytes, outcome.end_tick);
}

void append_fixed(std::vector<std::uint8_t>& bytes, const math::Fixed value) {
    sim::detail::append_i64(bytes, value.raw());
}

// Canonical ledger record (#530): player, credits, both queues and the pool.
void append_ledger(std::vector<std::uint8_t>& bytes, const PlayerEconomy& ledger) {
    sim::detail::append_u32(bytes, ledger.player);
    sim::detail::append_u32(bytes, 0);
    append_fixed(bytes, ledger.credits);
    for (const auto& queue : ledger.queues) {
        sim::detail::append_u64(bytes, queue.size());
        for (const auto& entry : queue) {
            sim::detail::append_u64(bytes, entry.type);
            sim::detail::append_u64(bytes, entry.station);
            append_fixed(bytes, entry.paid);
            sim::detail::append_u32(bytes, entry.frames);
            sim::detail::append_u32(bytes, 0);
            sim::detail::append_u64(bytes, entry.complete_frame);
        }
    }
    sim::detail::append_u64(bytes, ledger.pool.size());
    for (const auto type : ledger.pool) {
        sim::detail::append_u64(bytes, type);
    }
    sim::detail::append_u64(bytes, ledger.completed.size());
    for (const auto& build : ledger.completed) {
        sim::detail::append_u64(bytes, build.type);
        sim::detail::append_u64(bytes, build.station);
    }
}

void append_vec3(std::vector<std::uint8_t>& bytes, const math::Vec3& value) {
    append_fixed(bytes, value.x);
    append_fixed(bytes, value.y);
    append_fixed(bytes, value.z);
}

// Canonical craft record (#75): ID, roll, pitch, yaw, velocity and the flip flag, then (#457) a
// mask of the dogfight state it carries: bit 0 a chase (FD-05, FD-06), bit 1 an avoidance
// relaxation (FD-10). A craft without either hashes as before #457.
void append_craft(std::vector<std::uint8_t>& bytes, const EntityId id, const CraftState& craft) {
    sim::detail::append_u64(bytes, id);
    append_fixed(bytes, craft.roll);
    append_fixed(bytes, craft.pitch);
    append_fixed(bytes, craft.yaw);
    append_vec3(bytes, craft.velocity);
    sim::detail::append_u32(bytes, craft.flipping ? 1U : 0U);
    const bool chasing = craft.chase != invalid_entity_id || craft.chase_until != 0;
    sim::detail::append_u32(bytes, (chasing ? 1U : 0U) | (craft.relaxation != 0 ? 2U : 0U));
    if (chasing) {
        sim::detail::append_u64(bytes, craft.chase);
        sim::detail::append_u64(bytes, craft.chase_until);
    }
    if (craft.relaxation != 0) sim::detail::append_u32(bytes, craft.relaxation);
}

// Canonical spinning craft record (#447).
void append_spin(std::vector<std::uint8_t>& bytes, const DeathSpin& spin) {
    sim::detail::append_u64(bytes, spin.unit);
    sim::detail::append_u64(bytes, spin.type);
    sim::detail::append_u32(bytes, spin.owner);
    sim::detail::append_u32(bytes, spin.path ? 1U : 0U);
    append_vec3(bytes, spin.position);
    append_fixed(bytes, spin.roll);
    append_fixed(bytes, spin.pitch);
    append_fixed(bytes, spin.yaw);
    append_vec3(bytes, spin.velocity);
    append_fixed(bytes, spin.spin_roll);
    for (const auto& point : spin.points) {
        append_vec3(bytes, point);
    }
    for (const auto length : spin.lengths) {
        append_fixed(bytes, length);
    }
    append_fixed(bytes, spin.travelled);
}

// Canonical squadron order record (#75).
void append_squadron_state(std::vector<std::uint8_t>& bytes, const SquadronState& state) {
    sim::detail::append_u64(bytes, state.container);
    sim::detail::append_u64(bytes, state.squadron_type);
    sim::detail::append_u64(bytes, state.spawner);
    sim::detail::append_u32(bytes, state.entry);
    sim::detail::append_u32(bytes, static_cast<std::uint32_t>(state.mode));
    sim::detail::append_u64(bytes, state.escorted);
    append_vec3(bytes, state.anchor);
    sim::detail::append_u64(bytes, state.target);
    sim::detail::append_u64(bytes, state.next_scan_frame);
    // Bit 0 the FA-07 approach; #457: bit 1 a recorded combat cell, bit 2 joined (WU-25).
    sim::detail::append_u32(bytes, (state.approach ? 1U : 0U) | (state.cell ? 2U : 0U) | (state.joined ? 4U : 0U));
    if (state.cell) {
        sim::detail::append_u32(bytes, static_cast<std::uint32_t>(state.cell->x));
        sim::detail::append_u32(bytes, static_cast<std::uint32_t>(state.cell->y));
    }
    sim::detail::append_u64(bytes, state.roster.size());
    for (const auto member : state.roster) {
        sim::detail::append_u64(bytes, member);
    }
    // FO-01 (#424): only a squadron on a player move carries its origin, so a session without
    // one hashes as before.
    if (state.mode == SquadronMode::move) {
        append_vec3(bytes, state.move_origin);
    }
    // FO-05, FO-06 (#452): only a squadron on a player attack-move or guard carries its diversion.
    if (state.diversion != SquadronDiversion::idle) {
        sim::detail::append_u32(bytes, static_cast<std::uint32_t>(state.diversion));
    }
    // #687 (FM-23): only a squadron holding an idle cell carries it, so a session where none does
    // hashes as before.
    if (state.idle_cell) {
        sim::detail::append_u32(bytes, 0x1d1eU);
        sim::detail::append_u32(bytes, static_cast<std::uint32_t>(state.idle_cell->x));
        sim::detail::append_u32(bytes, static_cast<std::uint32_t>(state.idle_cell->y));
    }
    // FO-10 (#599): only a squadron on a group move carries its lane.
    if (state.lane) {
        sim::detail::append_u64(bytes, state.lane->formation);
        append_vec3(bytes, state.lane->origin);
        append_vec3(bytes, state.lane->direction);
        append_fixed(bytes, state.lane->ahead);
        append_fixed(bytes, state.lane->aside);
    }
    // #531: only a squadron on a hardpoint attack order carries the hardpoint (index plus one).
    if (state.target_hardpoint != attack_hull) {
        sim::detail::append_u32(bytes, state.target_hardpoint + 1U);
    }
    if (state.formation) {
        sim::detail::append_u32(bytes, 0xf04dU);
        append_vec3(bytes, state.formation->base_position);
        sim::detail::append_u64(bytes, state.formation->base_target);
        sim::detail::append_u32(bytes, (state.formation->complete ? 1U : 0U)
            | (state.formation->has_reached_done ? 2U : 0U) | (state.formation->attack_override ? 4U : 0U));
    }
}

// Canonical hangar record (#75).
void append_spawner(std::vector<std::uint8_t>& bytes, const EntityId id, const SpawnerState& state) {
    sim::detail::append_u64(bytes, id);
    sim::detail::append_u64(bytes, state.next_service_frame);
    sim::detail::append_u64(bytes, state.next_spawn_frame);
    sim::detail::append_u32(bytes, state.ready ? 1U : 0U);
    sim::detail::append_u32(bytes, static_cast<std::uint32_t>(state.entries.size()));
    for (const auto& entry : state.entries) {
        sim::detail::append_i64(bytes, entry.alive);
        sim::detail::append_i64(bytes, entry.remaining);
    }
}

// Canonical motion record (docs/replay-format.md): kind, zero, start tick, start position, start
// yaw, start speed and target; zero apart from the kind while at rest.
void append_motion(std::vector<std::uint8_t>& bytes, const MotionState& motion) {
    sim::detail::append_u32(bytes, static_cast<std::uint32_t>(motion.kind));
    sim::detail::append_u32(bytes, 0);
    sim::detail::append_u64(bytes, motion.start_tick);
    append_fixed(bytes, motion.start_position.x);
    append_fixed(bytes, motion.start_position.y);
    append_fixed(bytes, motion.start_position.z);
    append_fixed(bytes, motion.start_yaw);
    append_fixed(bytes, motion.start_speed);
    append_fixed(bytes, motion.target.x);
    append_fixed(bytes, motion.target.y);
    append_fixed(bytes, motion.target.z);
}

// With the path finder (#71) a plan's nodes depend on the other ships' predictions, so the
// record carries them: node count, then per node frame, position, yaw and speed.
void append_nodes(std::vector<std::uint8_t>& bytes, const MotionState& motion) {
    sim::detail::append_u64(bytes, motion.nodes.size());
    for (const auto& node : motion.nodes) {
        append_fixed(bytes, node.frame);
        append_fixed(bytes, node.position.x);
        append_fixed(bytes, node.position.y);
        append_fixed(bytes, node.position.z);
        append_fixed(bytes, node.yaw);
        append_fixed(bytes, node.speed);
    }
}

} // namespace eawr::sim::tactical::session_detail

namespace eawr::sim::tactical {

TacticalSnapshot::TacticalSnapshot(
    const std::uint64_t completed_tick,
    std::vector<SnapshotPlayer> players,
    std::vector<TacticalInstance> instances,
    std::vector<Event> events,
    std::vector<CombatEvent> combat_events,
    std::vector<Projectile> projectiles,
    std::optional<BattleOutcome> outcome,
    std::vector<SpinningCraft> spinning,
    std::vector<SquadronTarget> squadron_targets,
    std::vector<Squadron> squadrons,
    std::vector<EconomyView> economy,
    std::vector<PadView> pads,
    std::vector<PlayerQuit> quits,
    std::shared_ptr<const std::vector<BattleLoss>> losses,
    std::shared_ptr<const std::vector<BattleProduction>> productions,
    std::vector<std::pair<PlayerId, ManualPlayerClock>> manual_clocks,
    std::shared_ptr<const std::vector<BattleEconomyCue>> economy_cues,
    std::vector<AbilitySpawnState> ability_spawns)
    : completed_tick_(completed_tick), players_(std::move(players)), instances_(std::move(instances)),
      events_(std::move(events)), combat_events_(std::move(combat_events)), projectiles_(std::move(projectiles)),
      outcome_(outcome), spinning_(std::move(spinning)), squadron_targets_(std::move(squadron_targets)),
      squadrons_(std::move(squadrons)), economy_(std::move(economy)), pads_(std::move(pads)), quits_(std::move(quits)),
      losses_(std::move(losses)), productions_(std::move(productions)), manual_clocks_(std::move(manual_clocks)),
      economy_cues_(std::move(economy_cues)) { ability_spawns_ = std::move(ability_spawns); }

std::uint64_t TacticalSnapshot::completed_tick() const noexcept { return completed_tick_; }
std::span<const SnapshotPlayer> TacticalSnapshot::players() const noexcept { return players_; }

std::span<const TacticalInstance> TacticalSnapshot::instances() const noexcept { return instances_; }
std::span<const Event> TacticalSnapshot::events() const noexcept { return events_; }
std::span<const CombatEvent> TacticalSnapshot::combat_events() const noexcept { return combat_events_; }
std::span<const Projectile> TacticalSnapshot::projectiles() const noexcept { return projectiles_; }
std::span<const AbilitySpawnState> TacticalSnapshot::ability_spawns() const noexcept { return ability_spawns_; }
const std::optional<BattleOutcome>& TacticalSnapshot::outcome() const noexcept { return outcome_; }
std::span<const SpinningCraft> TacticalSnapshot::spinning() const noexcept { return spinning_; }
std::span<const SquadronTarget> TacticalSnapshot::squadron_targets() const noexcept { return squadron_targets_; }
std::span<const Squadron> TacticalSnapshot::squadrons() const noexcept { return squadrons_; }
std::span<const EconomyView> TacticalSnapshot::economy() const noexcept { return economy_; }
std::span<const PadView> TacticalSnapshot::pads() const noexcept { return pads_; }
std::span<const PlayerQuit> TacticalSnapshot::quits() const noexcept { return quits_; }
std::span<const BattleLoss> TacticalSnapshot::losses() const noexcept {
    return losses_ ? std::span<const BattleLoss>(*losses_) : std::span<const BattleLoss>{};
}
std::span<const BattleProduction> TacticalSnapshot::productions() const noexcept {
    return productions_ ? std::span<const BattleProduction>(*productions_) : std::span<const BattleProduction>{};
}
std::span<const std::pair<PlayerId, ManualPlayerClock>> TacticalSnapshot::manual_clocks() const noexcept { return manual_clocks_; }

std::span<const BattleEconomyCue> TacticalSnapshot::economy_cues() const noexcept {
    return economy_cues_ ? std::span<const BattleEconomyCue>(*economy_cues_) : std::span<const BattleEconomyCue>{};
}

std::vector<EntityId> TacticalSnapshot::visible_entities(const PlayerId player) const {
    std::vector<EntityId> result;
    const auto found = std::find_if(players_.begin(), players_.end(),
        [&](const SnapshotPlayer& entry) { return entry.player_id == player; });
    if (found == players_.end()) {
        return result;
    }
    const auto bit = std::uint64_t{1} << static_cast<unsigned>(found - players_.begin());
    for (const auto& instance : instances_) {
        if ((instance.visible_to & bit) != 0U) {
            result.push_back(instance.entity_id);
        }
    }
    return result;
}

std::vector<std::uint8_t> TacticalSnapshot::canonical_bytes() const {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(44 + players_.size() * detail::snapshot_player_record_size
        + instances_.size() * detail::instance_record_size + events_.size() * detail::event_record_size);
    bytes.insert(bytes.end(), snapshot_magic.begin(), snapshot_magic.end());
    sim::detail::append_u32(bytes, tactical_snapshot_encoding_version);
    sim::detail::append_u64(bytes, completed_tick_);
    sim::detail::append_u64(bytes, players_.size());
    for (const auto& player : players_) {
        sim::detail::append_u32(bytes, player.player_id);
        sim::detail::append_u32(bytes, player.team_id);
    }
    sim::detail::append_u64(bytes, instances_.size());
    for (const auto& instance : instances_) {
        sim::detail::append_u64(bytes, instance.entity_id);
        sim::detail::append_u64(bytes, instance.type_id);
        sim::detail::append_u32(bytes, instance.owner);
        sim::detail::append_u32(bytes, instance.team);
        for (const auto& row : instance.fixed_transform.rows) {
            for (const auto value : row) {
                sim::detail::append_i64(bytes, value.raw());
            }
        }
        sim::detail::append_u64(bytes, instance.visible_to);
        sim::detail::append_u32(bytes, instance.reveal_range ? 1U : 0U);
        sim::detail::append_u32(bytes, 0);
        sim::detail::append_i64(bytes, instance.reveal_range ? instance.reveal_range->raw() : 0);
        const auto& durability = instance.durability;
        sim::detail::append_u32(bytes, durability ? 1U : 0U);
        sim::detail::append_u32(bytes, durability ? static_cast<std::uint32_t>(durability->hardpoints.size()) : 0U);
        if (!durability) {
            continue;
        }
        append_fixed(bytes, durability->hull);
        append_fixed(bytes, durability->max_hull);
        append_fixed(bytes, durability->max_speed_factor);
        append_fixed(bytes, durability->max_speed.value_or(math::Fixed{}));
        sim::detail::append_u32(bytes, (durability->engines_online ? 1U : 0U) | (durability->shields_online ? 2U : 0U)
                | (durability->launch_ready ? 4U : 0U) | (durability->max_speed ? 8U : 0U)
                | (durability->shields ? 16U : 0U));
        // A shielded instance of a session with damage rules (#74) appends its shield.
        if (durability->shields) {
            append_fixed(bytes, *durability->shields);
            append_fixed(bytes, durability->max_shields.value_or(math::Fixed{}));
        }
        sim::detail::append_u32(bytes, 0);
        for (const auto& hardpoint : durability->hardpoints) {
            append_fixed(bytes, hardpoint.health);
            bytes.push_back(static_cast<std::uint8_t>(hardpoint.role));
            bytes.push_back(static_cast<std::uint8_t>(hardpoint.state));
            bytes.push_back(hardpoint.enabled ? 1U : 0U);
            bytes.push_back(0);
            sim::detail::append_u32(bytes, 0);
        }
    }
    sim::detail::append_u64(bytes, events_.size());
    for (const auto& event : events_) {
        detail::append_event(bytes, event);
    }
    // A snapshot with combat events (#73) appends them, and one with projectiles in flight (#80)
    // the (possibly empty) combat events and then the projectiles; any other encodes exactly as
    // before.
    // WAD-07: expiry poses are presentation events; they add no canonical bytes.
    const auto canonical_combat_events = std::count_if(combat_events_.begin(), combat_events_.end(),
        [](const CombatEvent& event) { return event.kind != CombatEventKind::projectile_expired; });
    if (canonical_combat_events != 0 || !projectiles_.empty()) {
        sim::detail::append_u64(bytes, static_cast<std::uint64_t>(canonical_combat_events));
        for (const auto& event : combat_events_) {
            if (event.kind == CombatEventKind::projectile_expired) continue;
            detail::append_combat_event(bytes, event);
        }
    }
    if (!projectiles_.empty()) {
        sim::detail::append_u64(bytes, projectiles_.size());
        for (const auto& projectile : projectiles_) {
            detail::append_projectile(bytes, projectile);
        }
    }
    // A decided battle (#77) appends its outcome; an undecided one encodes exactly as before.
    if (outcome_) {
        append_outcome(bytes, *outcome_);
    }
    // Instances with abilities (#76), ascending ID, as a tagged block only when any has them.
    std::uint64_t able = 0;
    for (const auto& instance : instances_) able += instance.abilities.empty() ? 0 : 1;
    if (able != 0) {
        bytes.insert(bytes.end(), ability_tag.begin(), ability_tag.end());
        sim::detail::append_u64(bytes, able);
        for (const auto& instance : instances_) {
            if (instance.abilities.empty()) continue;
            sim::detail::append_u64(bytes, instance.entity_id);
            sim::detail::append_u32(bytes, static_cast<std::uint32_t>(instance.abilities.size()));
            for (const auto& ability : instance.abilities) {
                bytes.push_back(static_cast<std::uint8_t>(ability.kind));
                bytes.push_back(static_cast<std::uint8_t>((ability.active ? 1U : 0U) | (ability.ready ? 2U : 0U)
                    | (ability.autofire ? 4U : 0U) | (ability.supports_autofire ? 8U : 0U)));
                sim::detail::append_u16(bytes, 0);
                sim::detail::append_u32(bytes, ability.remaining_frames);
                sim::detail::append_u32(bytes, ability.total_frames);
            }
        }
    }
    // Ion-stunned instances (#561), ascending ID, as a tagged block only when any is stunned.
    std::uint64_t stunned = 0;
    for (const auto& instance : instances_) stunned += instance.ion_stun_frames != 0 ? 1 : 0;
    if (stunned != 0) {
        bytes.insert(bytes.end(), ion_stun_tag.begin(), ion_stun_tag.end());
        sim::detail::append_u64(bytes, stunned);
        for (const auto& instance : instances_) {
            if (instance.ion_stun_frames == 0) continue;
            sim::detail::append_u64(bytes, instance.entity_id);
            sim::detail::append_u32(bytes, instance.ion_stun_frames);
        }
    }
    const auto contacts = std::count_if(instances_.begin(), instances_.end(), [](const TacticalInstance& instance) {
        return instance.in_asteroid_field;
    });
    if (contacts != 0) {
        bytes.insert(bytes.end(), asteroid_tag.begin(), asteroid_tag.end());
        sim::detail::append_u32(bytes, 1);
        sim::detail::append_u32(bytes, 0);
        sim::detail::append_u64(bytes, static_cast<std::uint64_t>(contacts));
        for (const auto& instance : instances_) {
            if (instance.in_asteroid_field) sim::detail::append_u64(bytes, instance.entity_id);
        }
    }
    const auto nebulas = std::count_if(instances_.begin(), instances_.end(), [](const TacticalInstance& instance) { return instance.in_nebula; });
    if (nebulas != 0) {
        bytes.insert(bytes.end(), nebula_tag.begin(), nebula_tag.end());
        sim::detail::append_u64(bytes, static_cast<std::uint64_t>(nebulas));
        for (const auto& instance : instances_) {
            if (instance.in_nebula) sim::detail::append_u64(bytes, instance.entity_id);
        }
    }
    const auto storms = std::count_if(instances_.begin(), instances_.end(), [](const TacticalInstance& instance) { return instance.in_ion_storm; });
    if (storms != 0) {
        bytes.insert(bytes.end(), storm_tag.begin(), storm_tag.end());
        sim::detail::append_u64(bytes, static_cast<std::uint64_t>(storms));
        for (const auto& instance : instances_) if (instance.in_ion_storm) sim::detail::append_u64(bytes, instance.entity_id);
    }
    // Craft spinning away (#447) follow as a tagged block, only when there are any.
    if (!spinning_.empty()) {
        bytes.insert(bytes.end(), spin_tag.begin(), spin_tag.end());
        sim::detail::append_u64(bytes, spinning_.size());
        for (const auto& spin : spinning_) {
            sim::detail::append_u64(bytes, spin.entity_id);
            sim::detail::append_u64(bytes, spin.type_id);
            sim::detail::append_u32(bytes, spin.owner);
            sim::detail::append_u32(bytes, 0);
            for (const auto& row : spin.fixed_transform.rows) {
                for (const auto value : row) {
                    sim::detail::append_i64(bytes, value.raw());
                }
            }
            append_fixed(bytes, spin.roll);
            append_fixed(bytes, spin.pitch);
            append_fixed(bytes, spin.yaw);
            sim::detail::append_u64(bytes, spin.visible_to);
        }
    }
    if (!quits_.empty()) {
        // coordinator-reserved QUIT: absent status keeps all existing snapshot pins.
        constexpr std::array<std::uint8_t, 4> tag{'Q', 'U', 'I', 'T'};
        bytes.insert(bytes.end(), tag.begin(), tag.end());
        sim::detail::append_u64(bytes, quits_.size());
        for (const auto& quit : quits_) {
            sim::detail::append_u32(bytes, quit.player);
            sim::detail::append_u64(bytes, quit.tick);
        }
    }
    std::uint64_t manual_count = 0;
    for (const auto& instance : instances_) manual_count += instance.manual_weapons.size();
    if (manual_count != 0 || !manual_clocks_.empty()) {
        bytes.insert(bytes.end(), manual_tag.begin(), manual_tag.end());
        sim::detail::append_u64(bytes, manual_count);
        for (const auto& instance : instances_) {
            for (const auto& manual : instance.manual_weapons) {
                sim::detail::append_u64(bytes, instance.entity_id);
                sim::detail::append_u32(bytes, manual.hardpoint);
                sim::detail::append_u32(bytes, manual.requesting_player);
                sim::detail::append_u64(bytes, manual.target);
                sim::detail::append_u64(bytes, manual.assigned_frame);
                append_fixed(bytes, manual.yaw);
                append_fixed(bytes, manual.pitch);
            }
        }
        sim::detail::append_u64(bytes, manual_clocks_.size());
        for (const auto& [player, clock] : manual_clocks_) {
            sim::detail::append_u32(bytes, player);
            sim::detail::append_u32(bytes, clock.cooldown_frames);
            sim::detail::append_u64(bytes, clock.last_fired_frame);
        }
    }
    return bytes;
}

std::string TacticalSnapshot::sha256() const {
    return sim::sha256_hex(canonical_bytes());
}


} // namespace eawr::sim::tactical
