#include "render_test_support.hpp"

using namespace particle_render_contracts;

int main() {
    test_parser_retains_renderer_fields();
    test_death_burst_ignores_parent_velocity();
    test_parent_link_rejection();
    test_plan_policy();
    test_quad_geometry();
    test_finite_rotation_boundary();
    test_stream_validation();
    test_fixed_seed_streams();
    test_release_and_replacement_lifecycle();
    test_effect_brightness();
    test_emitter_glow_follows_turning_pose();
    test_present_allocates_nothing();
    test_camera_and_attachment_frames();
    test_mesh_registry_boundary();
    test_mesh_root_precedence();
    test_proxy_mesh_binding();
    test_no_detach_golden_unchanged();
    test_detach_release_branch();
    test_detach_drain_branch();
    test_stop_emission_drains_whatever_the_flag();
    test_detach_invalid_and_early_handles();
    test_detach_schedule_determinism();
    test_attachment_generation_seed();
    test_attachment_always_visible_matches_unmanaged();
    test_attachment_hide_drains_exactly_once();
    test_attachment_hide_releases_without_leave_particles();
    test_attachment_reappearance_policies();
    test_attachment_hidden_at_start_spawns_on_first_visible();
    test_attachment_moving_host();
    test_attachment_draining_bound();
    test_attachment_determinism();
    test_attachment_merge_stats();
    test_heat_pixel_change_bound();
    test_batch_matches_serial();
    test_batch_hashes_on_request();
    test_batch_work_counts();
    test_batch_rejects_repeats_and_unknown();
    test_batch_present_allocates_nothing();
    if (failures != 0) { std::cerr << failures << " particle render contract(s) failed\n"; return 1; }
    std::cout << "particle render contracts passed\n";
    return 0;
}
