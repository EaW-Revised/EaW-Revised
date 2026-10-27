#include "../support/self_test.hpp"
#include "ability_cases.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace ability_tests {
eawr::testing::Checker local_checker;
int& failures = local_checker.failures;
} // namespace ability_tests

using namespace ability_tests;

int main(int argc, char* argv[]) {
    if (argc == 2 && std::string_view(argv[1]) == "--mod-loadability") {
        test_spawned_hero_abilities();
        test_self_destruct();
        test_hero_beams();
        if (failures != 0) return EXIT_FAILURE;
        std::cout << "mod loadability ability contracts accepted\n";
        return EXIT_SUCCESS;
    }
    if (!eawr::testing::support_contract_self_test()) {
        std::cerr << "FAIL: shared check/require self-test\n";
        return 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--stealth") {
        test_stealth_session();
        test_stealth_duration_modifiers();
        test_squadron_stealth();
        test_squadron_stealth_orders();
        if (failures != 0) return EXIT_FAILURE;
        std::cout << "stealth contracts accepted\n";
        return EXIT_SUCCESS;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--reveal-ping") {
        test_squadron_reveal_ping();
        test_cluster_bomb_and_reveal_ping();
        if (failures != 0) return EXIT_FAILURE;
        std::cout << "reveal ping contracts accepted\n";
        return EXIT_SUCCESS;
    }
    test_station_radar_and_named_locks();
    test_radar_lifecycle_work_budget();
    test_replenished_radar_creation();
    test_hardpoint_dependencies();
    test_space_healing();
    test_passive_damage_aura();
    test_passive_damage_edges();
    test_peacebringer_blast_session();
    test_deployment_abilities();
    test_lure();
    test_self_destruct_projectile_order();
    test_self_destruct();
    test_corrupt_scatter();
    test_corrupt_systems();
    test_full_salvo();
    test_rocket_attack();
    test_stealth_session();
    test_stealth_duration_modifiers();
    test_squadron_stealth();
    test_squadron_stealth_orders();
    test_leech_shields();
    test_redirect_blaster();
    test_targeted_autofire();
    test_laser_defence();
    test_projectile_defence_producers();
    test_projectile_defence_transaction();
    test_impact_shooter_bonus();
    test_fire_range_bonus();
    test_hero_environment_damage_routing();
    test_hero_damage_after_escorts_die();
    test_hero_wingmen();
    test_spawned_hero_abilities();
    test_buzz_team_request();
    test_buzz_droids();
    test_hostile_buzz_droids();
    test_hero_beams();
    test_beam_autofire();
    test_concentrate_autofire();
    test_arriving_beam_autofire();
    test_banked_ability_geometry();
    test_hunt_destinations();
    test_hunt_ship_order();
    test_hunt_session();
    test_names_and_validation();
    test_hero_damage_modes();
    test_nested_special_handlers();
    test_concentrate_fire();
    test_concentrate_damage();
    test_concentrate_effect_total_order();
    test_concentrate_sparse_lifecycle();
    test_timers();
    test_multipliers();
    test_turbo_session();
    test_expiry_snapshot_keeps_timed_duration();
    test_defend_stand_in();
    test_autofire_rules();
    test_rejections();
    test_launched_squadron_sfoils();
    test_created_squadron_autofire();
    test_barrage_proxy_and_override();
    test_cluster_bomb_and_reveal_ping();
    test_squadron_reveal_ping();
    test_mc30_object_script();
    test_mc30_proximity_sample();
    if (failures != 0) {
        std::cerr << failures << " ability contract failure(s)\n";
        return 1;
    }
    std::cout << "ability contracts passed\n";
    return 0;
}
