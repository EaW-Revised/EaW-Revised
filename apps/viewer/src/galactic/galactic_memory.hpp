#pragma once

#include "../memory_attribution.hpp"
#include "gc_setup_memory.hpp"
#include "eawr/diagnostics/gc_memory.hpp"
#include "eawr/diagnostics/gc_memory_boundary.hpp"
#include "eawr/diagnostics/owner_census.hpp"
#include "eawr/scene/scene.hpp"
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <sstream>

namespace eawr::presentation::godot_backend {
struct GalacticMemorySnapshot final {
    std::string payload;
    MemoryAttribution attribution;
};

template<class State>
void enable_galactic_memory_census(State& state) {
    if (diagnostics::gc_memory_boundary_enabled(MemoryAttribution::enabled(), state.interactive)) {
        diagnostics::GcMemory::census_enabled.store(true, std::memory_order_relaxed);
        if (state.filesystem) scene::VfsAssetCache::enable_owner_census(*state.filesystem, true);
    }
}

// Called before owner transfer, or only after the final driven target has
// completed. Interactive sessions never read mutable native AI containers.
template<class State>
void capture_galactic_memory(State& state, const char* phase, bool prepared) {
    if (state.options.lan_start
        || state.options.synthetic_battle || state.options.all_ai_capture || !state.options.space_cycle_proof.empty()) return;
    diagnostics::with_gc_memory_boundary(MemoryAttribution::enabled(), state.interactive, phase, [&] {
    if (prepared) {
        if (state.campaign_root) state.memory_reader = state.campaign_root->native_ai_goal_reader();
    } else if (!state.game || !state.game->wait_for(state.options.simulation_ticks, std::chrono::milliseconds(0))) {
        return;
    }
    GalacticMemorySnapshot snapshot;
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX process{};
    process.cb = sizeof(process);
    const bool process_available = GetProcessMemoryInfo(GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&process), sizeof(process)) != 0;
#endif
    snapshot.attribution.capture();
    diagnostics::GcMemory memory;
    memory.row("viewer.scene.models_metadata", state.models);
    memory.row("viewer.scene.instances", state.instances);
    memory.row("viewer.scene.production_art", state.production_art);
    memory.row("viewer.scene.setup", state.galaxy_setup);
    if (state.published_snapshot) {
        const auto& published = *state.published_snapshot;
        memory.row("sim.publication.listed_columns", published.players(), published.planets(), published.routes(),
            published.fleets(), published.production_offers(), published.production(), published.player_economy(), published.fleet_control());
    }
    if (state.audio_catalog) memory.row("viewer.audio.catalog", state.audio_catalog);
    if (state.audio) state.audio->memory_inventory(memory);
    diagnostics::OwnerCensus owners;
    diagnostics::OwnerCensus catalog_gc, catalog_setup, catalog_union;
    // The weak observer takes a transient strong reference for this stopped walk;
    // it is released on return and is never retained between boundaries.
    const auto setup = gc_memory_detail::setup_content.lock();
    if (state.audio_catalog) {
        state.audio_catalog->owner_census(catalog_gc);
        state.audio_catalog->owner_census(catalog_union);
    }
    if (setup && setup->catalog) {
        setup->catalog->owner_census(catalog_setup);
        setup->catalog->owner_census(catalog_union);
    }
    if (state.filesystem) scene::VfsAssetCache(*state.filesystem).owner_census(owners);
    if (setup && setup->filesystem) scene::VfsAssetCache(*setup->filesystem).owner_census(owners);
    if (state.renderer) state.renderer->owner_census(owners);
    if (state.capture_renderer) state.capture_renderer->owner_census(owners);
    for (const auto& layer : state.scenes)
        if (layer.renderer) layer.renderer->owner_census(owners);
    for (const auto& model : state.models) {
        if (model.authored_idle_probe) model.authored_idle_probe->owner_census(owners);
        for (const auto& surface : model.surfaces)
            if (surface.idle) surface.idle->owner_census(owners);
    }
    std::ostringstream output;
    output << "{\"phase\":\"" << phase << "\",\"tick\":" << (state.game ? state.game->completed_tick() : 0)
           << ",\"process\":";
#if defined(_WIN32)
    if (process_available) {
        output << "{\"private_bytes\":" << process.PrivateUsage << ",\"working_set_bytes\":" << process.WorkingSetSize
               << ",\"peak_working_set_bytes\":" << process.PeakWorkingSetSize
               << ",\"peak_pagefile_bytes\":" << process.PeakPagefileUsage << '}';
    } else output << "null";
#else
    output << "null";
#endif
    output << ",\"listed_viewer\":";
    memory.write(output);
    output << ",\"listed_services\":";
    const auto native = state.memory_reader ? state.memory_reader() : std::vector<std::string>{};
    output << (native.empty() ? "null" : native.front());
    output << ",\"cpu_owners_overlap_listed_services\":";
    owners.write(output);
    output << ",\"setup_content_alive\":" << (setup ? "true" : "false")
           << ",\"same_catalog_definition_backing\":" << (setup && setup->catalog && state.audio_catalog
               && &setup->catalog->definitions() == &state.audio_catalog->definitions() ? "true" : "false")
           << ",\"same_vfs\":" << (setup && setup->filesystem == state.filesystem ? "true" : "false")
           << ",\"catalog_gc_overlap_services\":";
    catalog_gc.write(output);
    output << ",\"catalog_setup_overlap_gc\":";
    catalog_setup.write(output);
    output << ",\"catalog_union_deduplicated\":";
    catalog_union.write(output);
    const auto cache = [&](const std::shared_ptr<const vfs::Vfs>& filesystem) {
        if (!filesystem) { output << "null"; return; }
        const auto stats = scene::VfsAssetCache(*filesystem).model_stats();
        output << "{\"cache_retained_bytes\":" << stats.cache_retained_bytes
               << ",\"cache_budget_bytes\":" << stats.cache_budget_bytes << ",\"entries\":" << stats.entries
               << ",\"capacity\":";
        assets::write_payload_capacity(output, stats.capacity);
        output << '}';
    };
    output << ",\"gc_vfs_cache\":";
    cache(state.filesystem);
    output << ",\"setup_vfs_cache_overlap_gc\":";
    cache(setup ? setup->filesystem : std::shared_ptr<const vfs::Vfs>{});
    output << ",\"renderer_overlap_cpu_owners\":";
    if (state.renderer) assets::write_retained_payload(output, state.renderer->recipe_memory());
    else output << "null";
    snapshot.payload = output.str();
    // Keep each stopped boundary durable before admitting another sim tick.
    // A floor abort or failed capture must not lose the earlier owner census.
    // Separate files preserve post_load even if a later write is interrupted.
    if (!diagnostics::write_gc_memory_boundary(state.options.report_path, phase, state.options.campaign,
        state.profile, snapshot.payload, [&](std::ostream& boundary) { snapshot.attribution.write(boundary); }))
        godot::UtilityFunctions::printerr("EAWR GC memory boundary write failed");
    state.memory_snapshots.push_back(std::move(snapshot));
    });
}
} // namespace eawr::presentation::godot_backend
