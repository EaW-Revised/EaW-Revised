#pragma once
#include "eawr/sim/math/fixed.hpp"
#include "eawr/sim/commands.hpp"
#include "eawr/sim/tactical/types.hpp"
#include "eawr/sim/tactical/economy.hpp"
#include "eawr/sim/tactical/durability.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"
#include <cstdint>
#include <optional>
#include <vector>

namespace station_upgrade_test_support {
namespace t = eawr::sim::tactical;
namespace m = eawr::sim::math;
using m::Fixed;
void expect(bool value, const char* message);
Fixed whole(std::int64_t value);
Fixed decimal(const char* value);
t::PlayerCommand buy(std::uint64_t tick, t::PlayerId player, std::uint64_t sequence,
    eawr::sim::EntityId station, t::TypeId type);
t::TacticalSetup setup();
t::BuildOption option(t::TypeId type, t::BuildKind kind, t::BuildQueue queue, std::uint32_t frames);
t::EconomyRules economy();
t::DurabilityTable durability();
std::optional<t::TacticalSession> create(const t::EconomyRules& rules = economy());
std::vector<t::Event> through(t::TacticalSession& session, std::uint64_t tick);
const t::PlayerEconomy& account(const t::TacticalSession& session, t::PlayerId player);
void test_no_upgrade_layout();
void test_replacement_attack();
void test_order_after_reinforcement();
void test_destroyed_station();
void test_replacement_projectiles();
void test_hangar_replacement();
void test_garrison_marked_station_upgrade();
void test_free_garrison_replenishment();
void test_free_garrison_timer_order();
void test_free_garrison_pending_and_generic();
void test_free_garrison_bays_and_claims();
void test_disabled_authored_bay();
void test_free_garrison_replay();
void test_garrison_replay_extension();
void test_staged_source_removal();
void test_upgrade_holder_loss();
void test_respawn_inherits_upgrade();
void test_census_recount();
void test_production_work();
void test_hero_command_ledger();
void test_hero_worker_determinism();
void test_signed_command_reduction();
void test_bound_production_relationships();
void test_live_station_repair();
void test_human_pad_radar_creation();
} // namespace station_upgrade_test_support
