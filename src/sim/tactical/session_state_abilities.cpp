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

void TacticalSession::Impl::append_canonical_named_locks_state(std::vector<std::uint8_t>& bytes) const {
        if (!named_ability_locks.empty()) {
            bytes.insert(bytes.end(), {'N', 'L', 'C', 'K'});
            sim::detail::append_u32(bytes, 1);
            sim::detail::append_u32(bytes, 0);
            sim::detail::append_u64(bytes, named_ability_locks.size());
            for (const auto& [key, locked] : named_ability_locks) {
                const auto& [owner, type, name] = key;
                sim::detail::append_u32(bytes, owner);
                sim::detail::append_u64(bytes, type);
                sim::detail::append_u64(bytes, name.size());
                bytes.insert(bytes.end(), name.begin(), name.end());
                sim::detail::append_u32(bytes, locked ? 1U : 0U);
            }
        }
}

void TacticalSession::Impl::append_canonical_special_weapons_state(std::vector<std::uint8_t>& bytes) const {
        // WSW-14/19: coordinator-reserved optional block; ordinary sessions retain their bytes.
        if (!special_weapons.empty() || !special_disabled_by_hardpoint.empty()) {
            bytes.insert(bytes.end(), {'S', 'W', 'P', 'N'});
            sim::detail::append_u32(bytes, special_disabled_by_hardpoint.empty() ? 1U : 2U);
            sim::detail::append_u32(bytes, 0);
            sim::detail::append_u64(bytes, special_weapons.size());
            for (const auto& [id, state] : special_weapons) {
                sim::detail::append_u64(bytes, id);
                sim::detail::append_u64(bytes, state.recharge_started);
                sim::detail::append_u64(bytes, state.recharge_until);
                sim::detail::append_u64(bytes, state.next_pulse_frame);
                sim::detail::append_u64(bytes, state.target);
                sim::detail::append_u32(bytes, state.pulses_left);
                sim::detail::append_u32(bytes, state.instances);
            }
            if (!special_disabled_by_hardpoint.empty()) {
                sim::detail::append_u64(bytes, special_disabled_by_hardpoint.size());
                for (const auto player : special_disabled_by_hardpoint) sim::detail::append_u32(bytes, player);
            }
        }
}

void TacticalSession::Impl::append_canonical_abilities_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts, const std::span<const std::vector<std::uint8_t>> encoded) const {
        // Unit abilities (#76), ascending ID, only when a live unit's type has abilities.
        std::uint64_t able = counts ? counts->able : 0;
        if (!counts) for (const auto& unit : units) able += unit.abilities ? 1 : 0;
        if (able != 0) {
            bytes.insert(bytes.end(), ability_tag.begin(), ability_tag.end());
            sim::detail::append_u64(bytes, able);
            if (encoded.empty()) {
                for (const auto& unit : units) {
                    if (!unit.abilities) continue;
                    sim::detail::append_u64(bytes, unit.state.entity_id);
                    append_abilities(bytes, *unit.abilities);
                }
            } else for (const auto& part : encoded) bytes.insert(bytes.end(), part.begin(), part.end());
        }
}

void TacticalSession::Impl::append_canonical_stealth_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts) const {
        // Cloaks (#1887, WST-03), ascending ID, only while one is on: the last detection frame.
        const auto cloaked = (counts ? counts->cloaked : static_cast<std::uint64_t>(std::count_if(units.begin(), units.end(), [](const LiveUnit& unit) {
            return unit.abilities && unit.abilities->stealth_on;
        })));
        if (cloaked != 0) {
            bytes.insert(bytes.end(), stealth_tag.begin(), stealth_tag.end());
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(cloaked));
            for (const auto& unit : units) {
                if (!unit.abilities || !unit.abilities->stealth_on) continue;
                sim::detail::append_u64(bytes, unit.state.entity_id);
                sim::detail::append_u64(bytes, unit.abilities->stealth_detected);
            }
        }
}

void TacticalSession::Impl::append_canonical_stealth_end_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts) const {
        // Cloaks with their own end (#1906, WST-03), ascending ID, only while one is on.
        const auto lengthened = (counts ? counts->lengthened : static_cast<std::uint64_t>(std::count_if(units.begin(), units.end(), [](const LiveUnit& unit) {
            return unit.abilities && unit.abilities->stealth_on && unit.abilities->stealth_end != 0;
        })));
        if (lengthened != 0) {
            bytes.insert(bytes.end(), stealth_end_tag.begin(), stealth_end_tag.end());
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(lengthened));
            for (const auto& unit : units) {
                if (!unit.abilities || !unit.abilities->stealth_on || unit.abilities->stealth_end == 0) continue;
                sim::detail::append_u64(bytes, unit.state.entity_id);
                sim::detail::append_u64(bytes, unit.abilities->stealth_end);
            }
        }
}

void TacticalSession::Impl::append_canonical_self_destruct_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts) const {
        // Krayt object scripts (WSD-09), ascending ID, once a live unit's script has run.
        const auto scripted = (counts ? counts->scripted : static_cast<std::uint64_t>(std::count_if(units.begin(), units.end(), [](const LiveUnit& unit) {
            return unit.abilities && unit.abilities->self_destruct_script;
        })));
        if (scripted != 0) {
            bytes.insert(bytes.end(), self_destruct_script_tag.begin(), self_destruct_script_tag.end());
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(scripted));
            for (const auto& unit : units) {
                if (!unit.abilities || !unit.abilities->self_destruct_script) continue;
                const auto& script = *unit.abilities->self_destruct_script;
                sim::detail::append_u64(bytes, unit.state.entity_id);
                sim::detail::append_i64(bytes, script.last_service);
                sim::detail::append_u32(bytes, script.nearby);
                sim::detail::append_i64(bytes, script.threat.raw());
                sim::detail::append_u32(bytes, script.armed ? 1U : 0U);
            }
        }
}

void TacticalSession::Impl::append_canonical_mc30_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts) const {
        // FC-57/58: only initialized MC30 scripts add this optional state block.
        const auto mc30_scripts = (counts ? counts->mc30_scripts : static_cast<std::uint64_t>(std::count_if(units.begin(), units.end(), [](const LiveUnit& unit) {
            return unit.abilities && unit.abilities->mc30_script;
        })));
        if (mc30_scripts != 0) {
            bytes.insert(bytes.end(), mc30_script_tag.begin(), mc30_script_tag.end());
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(mc30_scripts));
            for (const auto& unit : units) {
                if (!unit.abilities || !unit.abilities->mc30_script) continue;
                const auto& script = *unit.abilities->mc30_script;
                sim::detail::append_u64(bytes, unit.state.entity_id);
                sim::detail::append_i64(bytes, script.last_service);
                sim::detail::append_u32(bytes, script.nearby);
                sim::detail::append_i64(bytes, script.threat.raw());
                sim::detail::append_u32(bytes, script.human ? 1U : 0U);
                sim::detail::append_u32(bytes, script.armed ? 1U : 0U);
            }
        }
}

void TacticalSession::Impl::append_canonical_special_abilities_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts) const {
        const auto special_owners = (counts ? counts->special_owners : static_cast<std::uint64_t>(std::count_if(units.begin(), units.end(), [](const LiveUnit& unit) {
            return unit.abilities && !unit.abilities->special.slots.empty();
        })));
        if (special_owners != 0) {
            // Coordinator-reserved SPAB, after HERO and before SPIN, in stable owner order.
            constexpr std::array<std::uint8_t, 4> tag{'S', 'P', 'A', 'B'};
            bytes.insert(bytes.end(), tag.begin(), tag.end());
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(special_owners));
            for (const auto& unit : units) {
                if (!unit.abilities || unit.abilities->special.slots.empty()) continue;
                sim::detail::append_u64(bytes, unit.state.entity_id);
                append_special_abilities(bytes, unit.abilities->special);
            }
        }
}

void TacticalSession::Impl::append_canonical_concentrate_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts) const {
        const auto concentrate_owners = (counts ? counts->concentrate_owners : static_cast<std::uint64_t>(std::count_if(units.begin(), units.end(), [](const LiveUnit& unit) {
            return unit.abilities && !unit.abilities->concentrate_recruits.empty();
        })));
        if (concentrate_owners != 0) {
            constexpr std::array<std::uint8_t, 4> tag{'C', 'F', 'I', 'R'}; // coordinator-reserved
            bytes.insert(bytes.end(), tag.begin(), tag.end());
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(concentrate_owners));
            for (const auto& unit : units) {
                if (!unit.abilities || unit.abilities->concentrate_recruits.empty()) continue;
                sim::detail::append_u64(bytes, unit.state.entity_id);
                sim::detail::append_u64(bytes, unit.abilities->concentrate_recruits.size());
                for (const auto id : unit.abilities->concentrate_recruits) sim::detail::append_u64(bytes, id);
            }
        }
}

void TacticalSession::Impl::append_canonical_ability_spawns_state(std::vector<std::uint8_t>& bytes) const {
        if (!ability_spawns.empty()) {
            constexpr std::array<std::uint8_t, 4> tag{'H', 'A', 'B', 'L'}; // coordinator-reserved
            bytes.insert(bytes.end(), tag.begin(), tag.end());
            sim::detail::append_u64(bytes, ability_spawns.size());
            for (const auto& spawn : ability_spawns) {
                sim::detail::append_u64(bytes, spawn.id);
                sim::detail::append_u64(bytes, spawn.type);
                sim::detail::append_u32(bytes, static_cast<std::uint32_t>(spawn.kind));
                sim::detail::append_u32(bytes, spawn.owner);
                sim::detail::append_u64(bytes, spawn.source);
                append_vec3(bytes, spawn.position);
                append_fixed(bytes, spawn.rotation.x);
                append_fixed(bytes, spawn.rotation.y);
                append_fixed(bytes, spawn.rotation.z);
                append_fixed(bytes, spawn.rotation.w);
                sim::detail::append_u64(bytes, spawn.due);
                sim::detail::append_u32(bytes, spawn.detonated ? 1 : 0);
                sim::detail::append_u64(bytes, spawn.recipients.size());
                for (const auto& recipient : spawn.recipients) {
                    sim::detail::append_u64(bytes, recipient.target);
                    sim::detail::append_u64(bytes, recipient.expires);
                }
            }
        }
}

void TacticalSession::Impl::append_canonical_buzz_requests_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts) const {
        const auto buzz_requests = (counts ? counts->buzz_requests : static_cast<std::uint64_t>(std::count_if(units.begin(), units.end(), [](const LiveUnit& unit) {
            return unit.abilities && unit.abilities->pending_buzz_position;
        })));
        if (buzz_requests != 0) {
            constexpr std::array<std::uint8_t, 4> tag{'B', 'Z', 'Q', 'R'}; // coordinator-reserved
            bytes.insert(bytes.end(), tag.begin(), tag.end());
            sim::detail::append_u32(bytes, 1);
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(buzz_requests));
            for (const auto& unit : units) if (unit.abilities && unit.abilities->pending_buzz_position) {
                sim::detail::append_u64(bytes, unit.state.entity_id);
                append_vec3(bytes, *unit.abilities->pending_buzz_position);
            }
        }
}

void TacticalSession::Impl::append_canonical_buzz_droids_state(std::vector<std::uint8_t>& bytes) const {
        if (!buzz_droids.empty()) {
            constexpr std::array<std::uint8_t, 4> tag{'B', 'U', 'Z', 'Z'}; // coordinator-reserved
            bytes.insert(bytes.end(), tag.begin(), tag.end());
            sim::detail::append_u64(bytes, buzz_droids.size());
            for (const auto& droid : buzz_droids) {
                sim::detail::append_u64(bytes, droid.droid);
                sim::detail::append_u64(bytes, droid.source_type);
                append_vec3(bytes, droid.start);
                sim::detail::append_u64(bytes, droid.active_from);
            }
        }
}

void TacticalSession::Impl::append_canonical_corruption_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts) const {
        const auto corrupt_count = (counts ? counts->corrupt_count : static_cast<std::uint64_t>(std::count_if(units.begin(), units.end(), [](const LiveUnit& unit) { return unit.corrupt.has_value(); })));
        std::uint64_t corrupt_casts = counts ? counts->corrupt_casts : 0;
        if (!counts) for (const auto& unit : units) if (unit.abilities)
            for (const auto& slot : unit.abilities->slots) corrupt_casts += slot.position ? 1U : 0U;
        if (corrupt_count != 0 || corrupt_casts != 0) {
            bytes.insert(bytes.end(), corrupt_tag.begin(), corrupt_tag.end());
            sim::detail::append_u32(bytes, 1);
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(corrupt_count));
            for (const auto& unit : units) if (unit.corrupt) {
                sim::detail::append_u64(bytes, unit.state.entity_id);
                sim::detail::append_u64(bytes, unit.corrupt->source_type);
                sim::detail::append_u64(bytes, unit.corrupt->started_tick);
                sim::detail::append_u64(bytes, unit.corrupt->expires_tick);
                append_fixed(bytes, unit.corrupt->drain_per_frame);
                append_fixed(bytes, unit.corrupt->inaccuracy);
            }
            sim::detail::append_u64(bytes, corrupt_casts);
            for (const auto& unit : units) if (unit.abilities)
                for (std::size_t slot = 0; slot < unit.abilities->slots.size(); ++slot) if (unit.abilities->slots[slot].position) {
                    sim::detail::append_u64(bytes, unit.state.entity_id);
                    sim::detail::append_u32(bytes, static_cast<std::uint32_t>(slot));
                    append_vec3(bytes, *unit.abilities->slots[slot].position);
                }
        }
}

void TacticalSession::Impl::append_canonical_charged_blast_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts) const {
        const auto blast_count = (counts ? counts->blast_count : static_cast<std::uint64_t>(std::count_if(units.begin(), units.end(), [](const auto& unit) {
            return unit.abilities && unit.abilities->charged_blast;
        })));
        if (blast_count != 0) {
            bytes.insert(bytes.end(), charged_blast_tag.begin(), charged_blast_tag.end());
            sim::detail::append_u32(bytes, 1);
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(blast_count));
            for (const auto& unit : units) if (unit.abilities && unit.abilities->charged_blast) {
                const auto& blast = *unit.abilities->charged_blast;
                sim::detail::append_u64(bytes, unit.state.entity_id);
                sim::detail::append_u32(bytes, blast.charge_remaining);
                sim::detail::append_u32(bytes, blast.shots_remaining);
                sim::detail::append_u32(bytes, blast.charged_presentation ? 1U : 0U);
            }
        }
}

} // namespace eawr::sim::tactical
