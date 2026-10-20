#pragma once

#include "../support/check.hpp"

namespace ability_tests {

extern eawr::testing::Checker local_checker;
extern int& failures;

void test_names_and_validation();
void test_timers();
void test_multipliers();
void test_deployment_abilities();
void test_turbo_session();
void test_expiry_snapshot_keeps_timed_duration();
void test_defend_stand_in();
void test_autofire_rules();
void test_rejections();
void test_launched_squadron_sfoils();
void test_created_squadron_autofire();
void test_hero_damage_modes();
void test_impact_shooter_bonus();
void test_concentrate_damage();
void test_concentrate_fire();
void test_concentrate_effect_total_order();
void test_concentrate_sparse_lifecycle();
void test_nested_special_handlers();
void test_barrage_proxy_and_override();
void test_squadron_reveal_ping();
void test_cluster_bomb_and_reveal_ping();
void test_mc30_object_script();
void test_mc30_proximity_sample();
void test_spawned_hero_abilities();
void test_buzz_team_request();
void test_buzz_droids();
void test_hostile_buzz_droids();
void test_banked_ability_geometry();
void test_hero_beams();
void test_beam_autofire();
void test_concentrate_autofire();
void test_arriving_beam_autofire();
void test_hero_wingmen();
void test_hero_environment_damage_routing();
void test_hero_damage_after_escorts_die();
void test_hunt_destinations();
void test_hunt_ship_order();
void test_hunt_session();
void test_projectile_defence_producers();
void test_projectile_defence_transaction();
void test_leech_shields();
void test_targeted_autofire();
void test_full_salvo();
void test_squadron_stealth();
void test_squadron_stealth_orders();
void test_stealth_session();
void test_stealth_duration_modifiers();
void test_laser_defence();
void test_self_destruct_projectile_order();
void test_self_destruct();
void test_redirect_blaster();
void test_corrupt_scatter();
void test_corrupt_systems();
void test_lure();
void test_space_healing();
void test_peacebringer_blast_session();
void test_replenished_radar_creation();
void test_radar_lifecycle_work_budget();
void test_station_radar_and_named_locks();
void test_hardpoint_dependencies();

} // namespace ability_tests
