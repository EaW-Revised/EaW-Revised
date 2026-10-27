#pragma once

// Implementation header included only by live_session_report.cpp. It keeps the
// opt-in inventory out of the ordinary frame and component implementations.
#include "live_session_view.hpp"
#include "space_populate.hpp"
#include "battle_effects.hpp"
#include "battle_audio.hpp"
#include "live_fog_view.hpp"
#include "memory_attribution.hpp"
#include "eawr/diagnostics/owner_census.hpp"
#include <optional>
#include <godot_cpp/classes/audio_stream_wav.hpp>
#include <godot_cpp/classes/audio_stream_mp3.hpp>
#include <chrono>
#include <set>
#include <sstream>

namespace eawr::presentation::godot_backend {
void LiveSessionView::capture_memory_capacity(const BattleScene& population) {
    // The flag guard precedes every query/traversal; normal frames do no sampling.
    if (!memory_capacity_probe_ || !memory_capacity_report_.empty() || presented_tick_ < 1200.0) return;
    memory_capacity_report_ = memory_capacity_snapshot(population,
        "after_live_pose_before_outer_effects_hud_audio", presented_tick_,
        battle_frame_.latest ? battle_frame_.latest->completed_tick() : 0, false);
}

void LiveSessionView::capture_memory_capacity_600(const BattleScene& population) {
    if (!memory_capacity_probe_ || !memory_capacity_600_report_.empty() || presented_tick_ < 600.0) return;
    memory_capacity_600_report_ = memory_capacity_snapshot(population,
        "after_live_pose_before_outer_effects_hud_audio", presented_tick_,
        battle_frame_.latest ? battle_frame_.latest->completed_tick() : 0, true);
}

void LiveSessionView::capture_memory_capacity_post_load(const BattleScene& population) {
    // Standalone start already holds admission paused; never pause or relabel a borrowed feed.
    if (!memory_capacity_probe_ || !memory_capacity_post_load_report_.empty() || options_.borrowed_feed || !session_) return;
    if (!session_->paused() || session_->completed_tick() != 0) return;
    memory_capacity_post_load_report_ = memory_capacity_snapshot(population,
        "after_session_start_paused_before_first_step", 0.0, 0, true);
}

std::string LiveSessionView::memory_capacity_snapshot(const BattleScene& population,
    const char* boundary, const double presented_tick, const std::uint64_t snapshot_tick,
    const bool include_owner_census) const {
    const auto began = std::chrono::steady_clock::now();
    std::optional<diagnostics::OwnerCensus> census;
    if (include_owner_census) census.emplace();
    diagnostics::MemoryCapacity result;
    if (tables_) {
        result.vector("unit_tables", "unit_structures", tables_->units);
        std::uint64_t meshes = 0, triangles = 0, mesh_count = 0, triangle_count = 0;
        std::set<const void*> collision_storage;
        for (const auto& unit : tables_->units) {
            meshes += diagnostics::MemoryCapacity::bytes(unit.collision_meshes);
            mesh_count += unit.collision_meshes.size();
            for (const auto& mesh : unit.collision_meshes) {
                // Copies of a table share immutable indexed model-space geometry.
                // Keep the logical count, but list each retained payload only once.
                if (!mesh.triangles.empty() && collision_storage.insert(mesh.triangles.vertices().data()).second)
                    triangles += mesh.triangles.capacity_bytes();
                triangle_count += mesh.triangles.size();
                result.collision_buffer(mesh.triangles.vertices().data(), mesh.triangles.capacity_bytes());
            }
        }
        result.add("unit_tables", "collision_mesh_structures", meshes, mesh_count);
        result.add("unit_tables", "collision_triangles", triangles, triangle_count);
    }
    if (content_) diagnostics::combat_capacity(result, "viewer_content_combat", content_->combat);
    result.vector("live_view", "placed_ships", placed_ships_);
    result.vector("live_view", "session_records", session_records_);
    result.vector("live_view", "unfogged_map_props", unfogged_map_props_);
    result.vector("live_view", "draw_visibility", draw_visibility_);
    result.vector("live_view", "launch_slots", launch_slots_);
    result.vector("live_view", "placement_clones", placement_clones_);
    result.vector("live_view", "visible", visible_);
    result.vector("live_view", "poses", poses_);
    result.vector("live_view", "live_poses", live_poses_);
    result.vector("live_view", "clip_poses", clip_poses_);
    result.vector("live_view", "fog_immediate", fog_immediate_);
    result.vector("live_view", "fog_observations", fog_observations_);
    result.vector("live_view", "fog_ghost_instances", fog_ghost_instances_);
    result.vector("live_view", "launch_clones", launch_clones_);
    result.vector("live_view", "active_clones", active_clones_);
    result.vector("live_view", "retiring_clones", retiring_clones_);
    std::uint64_t nested_poses = 0;
    for (const auto& pose : live_poses_) nested_poses += diagnostics::MemoryCapacity::bytes(pose.hardpoints)
        + diagnostics::MemoryCapacity::bytes(pose.hardpoint_turrets);
    result.add("live_view", "nested_live_pose_vectors", nested_poses);
    if (battle_frame_.fog) {
        result.vector("live_fog_latest", "row_handles", battle_frame_.fog->values);
        // Deduplicate shared rows in this one snapshot; history ownership is excluded.
        std::set<const void*> seen;
        std::uint64_t bytes = 0;
        for (const auto& row : battle_frame_.fog->values) {
            if (row && seen.insert(row.get()).second) bytes += diagnostics::MemoryCapacity::bytes(*row);
        }
        result.add("live_fog_latest", "unique_rows_overlap_sim_history", bytes, seen.size());
    }
    result.append(population.memory_capacity());
    if (projectile_models_) result.append(projectile_models_->memory_capacity());
    if (session_) result.append(session_->memory_capacity());
    if (memory_capacity_probe_) result.append(memory_capacity_probe_());
    if (census) {
        result.collision_census(*census);
        population.owner_census(*census);
        if (owner_census_probe_) owner_census_probe_(*census);
        else {
            census->models_incomplete();
            for (unsigned group = diagnostics::OwnerCensus::picking; group < diagnostics::OwnerCensus::group_count; ++group)
                census->incomplete(static_cast<diagnostics::OwnerCensus::Group>(group));
        }
    }
    std::ostringstream output;
    output << "{\"boundary\":\"" << boundary << "\",\"presented_tick\":" << presented_tick
           << ",\"snapshot_tick\":" << snapshot_tick
           << ",\"unix_us\":" << std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count()
           << ",\"query_ms\":" << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count()
           << ",\"inventory\":";
    result.write(output);
    if (census) { output << ",\"owner_census\":"; census->write(output); }
    output << '}';
    return output.str();
}

diagnostics::MemoryCapacity SpacePopulation::memory_capacity() const {
    diagnostics::MemoryCapacity result;
    result.vector("population", "decisions", decisions_);
    result.vector("population", "pieces", pieces_);
    result.vector("population", "instances", instances_);
    result.vector("population", "piece_visibility", piece_visibility_);
    result.vector("population", "live_alternates", live_alternates_);
    result.vector("population", "live_alternate_counts", live_alternate_counts_);
    result.vector("population", "uploaded", uploaded_);
    result.vector("population", "effect_clock_assets", effect_clock_assets_);
    result.vector("population", "clips", clips_);
    result.vector("population", "idle_placements", idle_placements_);
    result.vector("population", "animated_instances", animated_instances_);
    result.vector("population", "live_decisions", live_decisions_);
    result.vector("population", "live_models", live_models_);
    result.vector("population", "live_shown", live_shown_);
    result.vector("population", "live_death_clone", live_death_clone_);
    result.vector("population", "live_defend", live_defend_);
    result.vector("population", "live_stealth", live_stealth_);
    result.vector("population", "live_blast_charging", live_blast_charging_);
    result.vector("population", "live_blast_charged", live_blast_charged_);
    result.vector("population", "live_bounds", live_bounds_);
    result.vector("population", "live_transforms", live_transforms_);
    result.vector("population", "live_clips", live_clips_);
    result.vector("population", "live_alternate_clips", live_alternate_clips_);
    result.vector("population", "live_clip_instances", live_clip_instances_);
    result.vector("population", "prepared_clips", prepared_clips_);
    result.vector("population", "live_retired", live_retired_);
    result.vector("population", "live_identity", live_identity_);
    result.vector("population", "options_placed_ships", options_.placed_ships);
    result.vector("population", "options_session_records", options_.session_records);
    std::uint64_t bones = 0, geometry = 0, parked = 0, fog = 0, asset_lists = 0;
    for (const auto& row : contact_bones_) bones += diagnostics::MemoryCapacity::bytes(row);
    for (const auto& entry : held_clips_) bones += diagnostics::MemoryCapacity::bytes(entry.second.bones);
    for (const auto& row : hardpoint_states_) bones += diagnostics::MemoryCapacity::bytes(row);
    // Main's prepared clips hold borrowed spans into cached geometry. Count only
    // the retained span table, not the pointed-to payload already owned elsewhere.
    for (const auto& clip : prepared_clips_) geometry += diagnostics::MemoryCapacity::bytes(clip.geometry);
    for (const auto& entry : parked_ships_) {
        parked += diagnostics::MemoryCapacity::bytes(entry.second.pieces)
            + diagnostics::MemoryCapacity::bytes(entry.second.clips)
            + diagnostics::MemoryCapacity::bytes(entry.second.animated);
    }
    for (const auto& entry : fog_models_) fog += diagnostics::MemoryCapacity::bytes(entry.second);
    for (const auto& row : hull_surface_assets_) asset_lists += diagnostics::MemoryCapacity::bytes(row);
    result.add("population", "listed_nested_bones_hardpoints", bones);
    result.add("population", "prepared_clip_geometry_views", geometry);
    result.add("population", "parked_ship_vectors", parked);
    result.add("population", "fog_model_vectors", fog);
    result.add("population", "hull_surface_asset_lists", asset_lists);
    return result;
}

diagnostics::MemoryCapacity BattleEffects::memory_capacity() const {
    diagnostics::MemoryCapacity result;
    result.vector("effects", "live_effect_structures", effects_);
    result.vector("effects", "modelled_now", modelled_now_);
    result.vector("effects", "contact_samples", contact_samples_);
    std::uint64_t pools = 0;
    for (const auto& entry : model_pools_) pools += diagnostics::MemoryCapacity::bytes(entry.second.ships);
    result.add("effects", "model_pool_ship_indices", pools, model_pools_.size());
    result.vector("effects", "kite_vertices", kites_.stream.vertices);
    result.vector("effects", "kite_indices", kites_.stream.indices);
    result.vector("effects", "beam_vertices", beams_.stream.vertices);
    result.vector("effects", "beam_indices", beams_.stream.indices);
    return result;
}

diagnostics::MemoryCapacity BattleAudio::memory_capacity() const {
    diagnostics::MemoryCapacity result;
    std::set<const void*> seen;
    std::uint64_t wav_bytes = 0, mp3_bytes = 0, unknown = 0;
    const auto visit = [&](const godot::Ref<godot::AudioStream>& stream) {
        if (stream.is_null() || !seen.insert(stream.ptr()).second) return;
        // PackedByteArray getters share COW backing; size() never detaches or
        // decodes. Unique stream identity prevents repeated cache references.
        if (const auto* wav = godot::Object::cast_to<godot::AudioStreamWAV>(stream.ptr())) {
            wav_bytes += static_cast<std::uint64_t>(wav->get_data().size());
        } else if (const auto* mp3 = godot::Object::cast_to<godot::AudioStreamMP3>(stream.ptr())) {
            mp3_bytes += static_cast<std::uint64_t>(mp3->get_data().size());
        } else ++unknown;
    };
    for (const auto& entry : samples_) visit(entry.second.stream);
    for (const auto& entry : music_streams_) visit(entry.second);
    for (const auto& entry : speech_streams_) visit(entry.second);
    result.vector("audio", "load_diagnostics", audio_load_rows_);
    result.add("audio_godot_overlap", "unique_wav_data_size", wav_bytes, seen.size());
    result.add("audio_godot_overlap", "unique_mp3_data_size", mp3_bytes);
    result.add("audio_godot_overlap", "unknown_stream_types", 0, unknown);
    return result;
}

diagnostics::MemoryCapacity LiveFogView::memory_capacity() const {
    diagnostics::MemoryCapacity result;
    if (field_) result.append(field_->memory_capacity());
    return result;
}

} // namespace eawr::presentation::godot_backend
