#include "render_test_support.hpp"

using namespace particle_render_contracts;

int main() {
    test_authored_effect_admission();
    test_authored_effect_capacity_and_reset();
    test_parser_retains_renderer_fields();
    test_death_burst_ignores_parent_velocity();
    test_legacy_moving_kite();
    test_parent_link_rejection();
    test_plan_policy();
    test_quad_geometry();
    test_finite_rotation_boundary();
    test_billboard_radian_rotation();
    test_stream_validation();
    test_fixed_seed_streams();
    test_particle_detail_gates();
    test_legacy_sort_triangles_and_atlas();
    test_cpu_steady_state_allocates_nothing();
    test_particle_detail_batch();
    test_release_and_replacement_lifecycle();
    test_effect_brightness();
    test_corruption_registry_settings();
    test_corruption_actual_reservation_budget();
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
    test_default_worker_count_table();
    test_lane_guard_counts_and_never_aborts();
    test_culling_latch_matches_live();
    test_frame_number_latch_matches_live();
    test_frame_work_merge_adds_once();
#ifdef EAWR_LANE_CONTRACTS
    test_particle_lane_contracts();
#endif
    test_catchup_defer_predicate();
    test_batch_matches_serial();
    test_attachment_batch_matches_serial();
    test_attachment_batch_requires_stats();
    test_batch_hashes_on_request();
    test_batch_work_counts();
    test_clean_present_matches_rebuild();
    test_unmoved_frame_keeps_streams();
    test_clean_present_resubmits_after_incomplete_update();
    test_clean_present_resubmits_only_deferred_instance();
    test_clean_present_matches_rebuild_for_every_input();
    test_clean_present_resubmits_deferred_moved_instance();
    test_ordered_run_commits_in_order();
    test_pipelined_commit_matches_serial_batches();
    test_pipelined_commit_off_when_backend_forbids_overlap();
    test_async_advance_matches_advance_all();
    test_async_advance_falls_back_and_joins();
    test_lazy_packing_matches_eager_commit();
    test_lazy_packing_completes_after_failures();
    test_lazy_packing_keeps_merged_groups_current();
    test_lazy_packing_keeps_merged_groups_current_after_failures();
    test_pipelined_failure_keeps_a_prefix_and_recovers();
    test_catchup_screen_space_decision();
    test_offscreen_updates_and_bounds();
    test_empty_stream_residency();
    test_batch_rejects_repeats_and_unknown();
    test_batch_present_allocates_nothing();
    if (failures != 0) { std::cerr << failures << " particle render contract(s) failed\n"; return 1; }
    std::cout << "particle render contracts passed\n";
    return 0;
}
