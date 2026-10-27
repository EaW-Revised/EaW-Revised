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

void TacticalSession::Impl::append_canonical_economy_state(std::vector<std::uint8_t>& bytes) const {
        // Skirmish purchasing (#530): the ledgers with economy rules, arrivals and population
        // shares while any; a session without an economy hashes as before.
        if (!economy.empty()) {
            bytes.insert(bytes.end(), economy_tag.begin(), economy_tag.end());
            sim::detail::append_u64(bytes, ledgers.size());
            for (const auto& ledger : ledgers) {
                append_ledger(bytes, ledger);
            }
        }
}

void TacticalSession::Impl::append_canonical_arrivals_state(std::vector<std::uint8_t>& bytes) const {
        if (!arrivals.empty()) {
            bytes.insert(bytes.end(), arrival_tag.begin(), arrival_tag.end());
            sim::detail::append_u64(bytes, arrivals.size());
            for (const auto& [id, arrival] : arrivals) {
                sim::detail::append_u64(bytes, id);
                sim::detail::append_u32(bytes, arrival.frame);
                sim::detail::append_u32(bytes, 0);
                append_vec3(bytes, arrival.exit);
                append_vec3(bytes, arrival.direction);
            }
        }
}

void TacticalSession::Impl::append_canonical_pads_state(std::vector<std::uint8_t>& bytes) const {
        if (!pads.empty() || !construction.empty() || !respawns.empty()) {
            constexpr std::array<std::uint8_t, 4> tag{'P', 'A', 'D', 'S'};
            bytes.insert(bytes.end(), tag.begin(), tag.end());
            sim::detail::append_u32(bytes, 2);
            sim::detail::append_u32(bytes, 0);
            sim::detail::append_u64(bytes, pads.size());
            for (const auto& [id, state] : pads) {
                sim::detail::append_u64(bytes, id);
                sim::detail::append_u32(bytes, state.target);
                sim::detail::append_u32(bytes, state.contents_locked ? 1U : 0U);
                sim::detail::append_i64(bytes, state.progress.raw());
                sim::detail::append_u64(bytes, state.under_construction);
                sim::detail::append_u64(bytes, state.constructed);
                sim::detail::append_u64(bytes, state.cooldown_until);
                sim::detail::append_u64(bytes, state.cooldown_start);
            }
            sim::detail::append_u64(bytes, construction.size());
            for (const auto& [id, state] : construction) {
                sim::detail::append_u64(bytes, id);
                sim::detail::append_u64(bytes, state.parent);
                sim::detail::append_u32(bytes, state.builder);
                sim::detail::append_u32(bytes, 0);
                sim::detail::append_u64(bytes, state.finish_frame);
                sim::detail::append_i64(bytes, state.increment.raw());
                sim::detail::append_u64(bytes, state.start_frame);
            }
            sim::detail::append_u64(bytes, respawns.size());
            for (const auto& [due, batch] : respawns) {
                sim::detail::append_u64(bytes, due);
                sim::detail::append_u64(bytes, batch.objects.size());
                for (const auto& request : batch.objects) {
                    sim::detail::append_u64(bytes, request.type);
                    sim::detail::append_u32(bytes, request.owner);
                    sim::detail::append_u32(bytes, 0);
                    append_vec3(bytes, request.position);
                    for (const auto component : {request.rotation.x, request.rotation.y, request.rotation.z, request.rotation.w}) {
                        append_fixed(bytes, component);
                    }
                }
            }
        }
}

void TacticalSession::Impl::append_canonical_population_state(std::vector<std::uint8_t>& bytes) const {
        if (!shares.empty()) {
            bytes.insert(bytes.end(), population_tag.begin(), population_tag.end());
            sim::detail::append_u64(bytes, shares.size());
            for (const auto& [id, share] : shares) {
                sim::detail::append_u64(bytes, id);
                sim::detail::append_u32(bytes, share.owner);
                sim::detail::append_u32(bytes, 0);
                sim::detail::append_i64(bytes, share.share);
            }
        }
}

void TacticalSession::Impl::append_canonical_arrival_defence_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts) const {
        // WR-41: AVUL v1, coordinator-reserved. Only active independent defense timers;
        // the initial records and sessions without arrivals retain their existing encoding.
        const auto vulnerable = (counts ? counts->vulnerable : static_cast<std::uint64_t>(std::count_if(units.begin(), units.end(), [](const LiveUnit& unit) {
            return unit.arrival_vulnerable_until.has_value();
        })));
        if (vulnerable != 0) {
            constexpr std::array<std::uint8_t, 4> tag{'A', 'V', 'U', 'L'};
            bytes.insert(bytes.end(), tag.begin(), tag.end());
            sim::detail::append_u32(bytes, 1);
            sim::detail::append_u32(bytes, 0);
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(vulnerable));
            for (const auto& unit : units) {
                if (!unit.arrival_vulnerable_until) continue;
                sim::detail::append_u64(bytes, unit.state.entity_id);
                sim::detail::append_u64(bytes, *unit.arrival_vulnerable_until);
            }
        }
}

void TacticalSession::Impl::append_canonical_upgrades_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts) const {
        // WPR-02/22/51/52: UPGD v1, or v2 with nonzero WFR-01 range. Omitted for empty state.
        const auto modified = [](const LiveUnit& unit) {
            return unit.upgrade_bonuses != CombatBonuses{} || (unit.durability
                && (std::any_of(unit.durability->disabled.begin(), unit.durability->disabled.end(), [](const bool value) { return value; })
                    || std::any_of(unit.durability->repairing_players.begin(), unit.durability->repairing_players.end(),
                        [](const auto& players) { return !players.empty(); })));
        };
        const auto account_state = [&](const PlayerEconomy& account) {
            const auto* initial = economy.player(account.player);
            return (initial != nullptr && account.tech_level != initial->start_tech) || !account.completed.empty()
                || std::any_of(account.lifetime.begin(), account.lifetime.end(), [&](const auto& built) {
                    return economy.upgrade(built.first) != nullptr && built.second != 0;
                });
        };
        const auto modified_count = (counts ? counts->modified_count : static_cast<std::uint64_t>(std::count_if(units.begin(), units.end(), modified)));
        const auto account_count = std::count_if(ledgers.begin(), ledgers.end(), account_state);
        if (modified_count != 0 || account_count != 0) {
            constexpr std::array<std::uint8_t, 4> tag{'U', 'P', 'G', 'D'};
            bytes.insert(bytes.end(), tag.begin(), tag.end());
            const bool fire_range = std::any_of(units.begin(), units.end(), [](const auto& unit) {
                return unit.upgrade_bonuses[6].raw() != 0;
            });
            sim::detail::append_u32(bytes, fire_range ? 2U : 1U); sim::detail::append_u32(bytes, 0);
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(account_count));
            for (const auto& account : ledgers) {
                if (!account_state(account)) continue;
                sim::detail::append_u32(bytes, account.player); sim::detail::append_u32(bytes, account.tech_level);
                sim::detail::append_u64(bytes, account.lifetime.size());
                for (const auto& [type, count] : account.lifetime) {
                    sim::detail::append_u64(bytes, type); sim::detail::append_u64(bytes, count);
                }
                sim::detail::append_u64(bytes, account.completed.size());
                for (const auto& held : account.completed) {
                    sim::detail::append_u64(bytes, held.type); sim::detail::append_u64(bytes, held.station);
                    sim::detail::append_u64(bytes, held.object);
                }
            }
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(modified_count));
            for (const auto& unit : units) {
                if (!modified(unit)) continue;
                sim::detail::append_u64(bytes, unit.state.entity_id);
                // UPGD v2 includes all seven slots; zero-range sessions keep the six-slot v1 bytes.
                for (std::size_t slot = 0; slot < 6; ++slot) append_fixed(bytes, unit.upgrade_bonuses[slot]);
                if (fire_range) append_fixed(bytes, unit.upgrade_bonuses[6]);
                sim::detail::append_u64(bytes, unit.durability ? unit.durability->hardpoints.size() : 0);
                if (!unit.durability) continue;
                for (std::size_t slot = 0; slot < unit.durability->hardpoints.size(); ++slot) {
                    sim::detail::append_u32(bytes, hardpoint_disabled(*unit.durability, slot) ? 1U : 0U);
                    const auto* repairing = slot < unit.durability->repairing_players.size()
                        ? &unit.durability->repairing_players[slot] : nullptr;
                    sim::detail::append_u32(bytes, repairing != nullptr ? static_cast<std::uint32_t>(repairing->size()) : 0U);
                    if (repairing != nullptr) for (const auto player : *repairing) sim::detail::append_u32(bytes, player);
                }
            }
        }
}

void TacticalSession::Impl::append_canonical_level_up_state(std::vector<std::uint8_t>& bytes) const {
        // WSL-31: LSVC v1, coordinator-reserved; only pending held level-up deadlines.
        std::uint64_t level_up_services = 0;
        for (const auto& account : ledgers) for (const auto& held : account.completed)
            level_up_services += held.level_up_service_frame.has_value() ? 1U : 0U;
        if (level_up_services != 0) {
            constexpr std::array<std::uint8_t, 4> tag{'L', 'S', 'V', 'C'};
            bytes.insert(bytes.end(), tag.begin(), tag.end());
            sim::detail::append_u32(bytes, 1); sim::detail::append_u32(bytes, 0);
            sim::detail::append_u64(bytes, level_up_services);
            for (const auto& account : ledgers) for (const auto& held : account.completed) {
                if (!held.level_up_service_frame) continue;
                sim::detail::append_u32(bytes, account.player); sim::detail::append_u32(bytes, 0);
                sim::detail::append_u64(bytes, held.object);
                sim::detail::append_u64(bytes, *held.level_up_service_frame);
            }
        }
}

void TacticalSession::Impl::append_canonical_income_state(std::vector<std::uint8_t>& bytes) const {
        // WBP-44..46: IMOD v1 is omitted when no modifier object has state.
        // Active ownership remains in UPGD; reverse termination effects live only here.
        std::uint64_t modifier_objects = 0;
        for (const auto& account : ledgers) {
            for (const auto& held : account.completed) modifier_objects += !held.income_modifiers.empty() ? 1U : 0U;
            for (const auto& held : account.income_residuals) modifier_objects += !held.income_modifiers.empty() ? 1U : 0U;
        }
        if (modifier_objects != 0) {
            constexpr std::array<std::uint8_t, 4> tag{'I', 'M', 'O', 'D'};
            bytes.insert(bytes.end(), tag.begin(), tag.end());
            sim::detail::append_u32(bytes, 1); sim::detail::append_u32(bytes, 0);
            sim::detail::append_u64(bytes, modifier_objects);
            const auto append = [&](const PlayerId owner, const CompletedBuild& held, const bool residual) {
                if (held.income_modifiers.empty()) return;
                sim::detail::append_u32(bytes, owner); sim::detail::append_u32(bytes, residual ? 1U : 0U);
                sim::detail::append_u64(bytes, held.type); sim::detail::append_u64(bytes, held.station);
                sim::detail::append_u64(bytes, held.object); sim::detail::append_u64(bytes, held.income_modifiers.size());
                for (const auto& state : held.income_modifiers) {
                    sim::detail::append_u32(bytes, state.initialized ? 1U : 0U); sim::detail::append_u32(bytes, 0);
                    sim::detail::append_u64(bytes, state.next_scan_frame); sim::detail::append_u64(bytes, state.attached.size());
                    for (const auto stream : state.attached) sim::detail::append_u64(bytes, stream);
                }
            };
            for (const auto& account : ledgers) {
                for (const auto& held : account.completed) append(account.player, held, false);
                for (const auto& held : account.income_residuals) append(account.player, held, true);
            }
        }
}

void TacticalSession::Impl::append_canonical_pool_tokens_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts) const {
        // coordinator-reserved PTOK: counters remain authoritative after the last pool entry leaves.
        const auto token_accounts = std::count_if(ledgers.begin(), ledgers.end(), [](const auto& ledger) {
            return ledger.next_pool_token != 1;
        });
        const auto token_units = (counts ? counts->token_units : static_cast<std::uint64_t>(std::count_if(units.begin(), units.end(), [](const auto& unit) {
            return unit.state.purchase_token != 0;
        })));
        if (token_accounts != 0 || token_units != 0) {
            constexpr std::array<std::uint8_t, 4> tag{'P', 'T', 'O', 'K'};
            bytes.insert(bytes.end(), tag.begin(), tag.end());
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(token_accounts));
            for (const auto& ledger : ledgers) {
                if (ledger.next_pool_token == 1) continue;
                sim::detail::append_u32(bytes, ledger.player);
                sim::detail::append_u64(bytes, ledger.next_pool_token);
                sim::detail::append_u64(bytes, ledger.pool_tokens.size());
                for (const auto token : ledger.pool_tokens) sim::detail::append_u64(bytes, token);
            }
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(token_units));
            for (const auto& unit : units) {
                if (unit.state.purchase_token == 0) continue;
                sim::detail::append_u64(bytes, unit.state.entity_id);
                sim::detail::append_u64(bytes, unit.state.purchase_token);
            }
        }
}

void TacticalSession::Impl::append_canonical_queue_identities_state(std::vector<std::uint8_t>& bytes) const {
        if (setup.queue_identities && !economy.empty()) {
            // coordinator-reserved QIDS: retain the counter even after every queued entry leaves.
            constexpr std::array<std::uint8_t, 4> tag{'Q', 'I', 'D', 'S'};
            bytes.insert(bytes.end(), tag.begin(), tag.end());
            sim::detail::append_u32(bytes, 1);
            sim::detail::append_u32(bytes, 0);
            sim::detail::append_u64(bytes, ledgers.size());
            for (const auto& ledger : ledgers) {
                sim::detail::append_u32(bytes, ledger.player);
                sim::detail::append_u32(bytes, 0);
                sim::detail::append_u64(bytes, ledger.next_queue_entry_id);
                for (const auto& queue : ledger.queues) {
                    sim::detail::append_u64(bytes, queue.size());
                    for (const auto& entry : queue) sim::detail::append_u64(bytes, entry.entry_id);
                }
            }
        }
}

void TacticalSession::Impl::append_canonical_campaign_state(std::vector<std::uint8_t>& bytes) const {
        if (setup.campaign) {
            bytes.insert(bytes.end(), {'B','O','U','N','T','Y',1});
            for (const auto& ledger : ledgers) sim::detail::append_i64(bytes, ledger.collected_bounty.raw());
            constexpr std::array<std::uint8_t, 4> tag{'C', 'A', 'M', 'C'};
            bytes.insert(bytes.end(), tag.begin(), tag.end());
            sim::detail::append_u32(bytes, 1);
            bytes.insert(bytes.end(), campaign_binding.begin(), campaign_binding.end());
            sim::detail::append_u64(bytes, ledgers.size());
            for (const auto& ledger : ledgers) {
                sim::detail::append_u32(bytes, ledger.player);
                sim::detail::append_i64(bytes, ledger.campaign_extra_releases.raw());
            }
        }
}

} // namespace eawr::sim::tactical
