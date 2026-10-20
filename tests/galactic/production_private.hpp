#pragma once

#include "eawr/sim/galactic/commands.hpp"
#include <filesystem>

namespace eawr::sim::galactic { class GalacticSession; struct GalacticReplay; }

namespace galactic_production_test {
namespace galactic = eawr::sim::galactic;
using eawr::sim::math::Fixed;
extern std::filesystem::path fixture_root;

Fixed f(std::int64_t value);
galactic::GalacticSetup setup();
galactic::GameCommand buy(std::uint64_t tick, std::uint64_t sequence, galactic::PlanetId planet = 1,
    galactic::TypeId type = 1000, galactic::ProductionDomain domain = galactic::ProductionDomain::ground);
galactic::GameCommand cancel(std::uint64_t tick, std::uint64_t sequence, std::uint32_t index,
    galactic::TypeId type = 1000, galactic::ProductionAction action = galactic::ProductionAction::cancel_index);
void submit(galactic::GalacticSession& game, galactic::GameCommand command);
void admission();
void offer_projection();
void orbit_completion_shares_slot_card();
void technology_price_production();
void cancel_identity();
void timing_and_cancel();
void inactive_work_budget();
void refund_recipient();
void finish_task_behind_front();
void queue_policies();
void base_and_tech_completion();
galactic::GalacticSetup blocker_setup();
void live_starbase_blockers();
void ground_structure_sale();
void content_and_placement();
void shared_technology_writer();
void story_production_writers();
void production_corpus_bytes();
void blocker_parity_checkpoint();
galactic::GalacticReplay corpus();
galactic::GalacticSetup census_setup();
void native_action_production_barrier();
void unit_sale();
void census_admission();
void census_campaign_retained_counts();
void company_original_binding();
void census_container_and_capacity();
void census_rounding_and_data();
void census_stock_composite_boundary();
void census_composite_counts();
void census_corpus_checkpoint();
void parity_checkpoint_case(bool seat_policy, bool census = false);
void parity_checkpoint();
void write_corpus(const std::filesystem::path& directory);
} // namespace galactic_production_test
