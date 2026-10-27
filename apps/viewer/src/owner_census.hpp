#pragma once

// Implementation header, included only by live_session_report.cpp.
#include "eawr/diagnostics/owner_census.hpp"
#include "space_populate.hpp"
#include "unit_emitters.hpp"

namespace eawr::presentation::godot_backend {

void SpacePopulation::owner_census(diagnostics::OwnerCensus& output) const {
    using Census = diagnostics::OwnerCensus;
    for (const auto& player : clips_) {
        if (!output.step(Census::animation)) break;
        if (player) player->owner_census(output);
    }
    for (const auto& binding : turret_bindings_) {
        if (!output.step(Census::animation)) break;
        if (binding && binding->player) binding->player->owner_census(output);
    }
    for (const auto& bindings : hardpoint_turret_bindings_) {
        if (!output.step(Census::animation)) break;
        for (const auto& [hardpoint, binding] : bindings) {
            if (!output.step(Census::animation)) break;
            static_cast<void>(hardpoint);
            if (binding.player) binding.player->owner_census(output);
        }
    }
    for (const auto& binding : parent_hardpoint_turret_bindings_) {
        if (!output.step(Census::animation)) break;
        if (binding && binding->player) binding->player->owner_census(output);
    }
    for (const auto& clip : prepared_clips_) {
        if (!output.step(Census::animation)) break;
        if (clip.player) clip.player->owner_census(output);
    }
}

void UnitEmitters::owner_census(diagnostics::OwnerCensus& output) const {
    using Census = diagnostics::OwnerCensus;
    output.emitter_raw_wrapper(&cache_);
    // These are copies. Neither ModelFrames, MeshBinding nor the geometry-stripped emitter models own or borrow
    // a decoded Model's backing (#4645): this cache's raw wrapper is never populated, so no model carries the
    // raw_cache role for UnitEmitters any more. The wrapper label stays for a raw model a future caller adds.
    for (const auto& entry : emitter_models_) {
        if (!output.step(Census::emitter_metadata_copies)) break;
        output.string(Census::emitter_metadata_copies, entry.first);
        if (!entry.second) continue;
        const assets::Model& model = *entry.second;
        output.range(Census::emitter_metadata_copies, entry.second.get(), sizeof(assets::Model));
        output.vector(Census::emitter_metadata_copies, model.bones);
        output.vector(Census::emitter_metadata_copies, model.proxies);
        output.vector(Census::emitter_metadata_copies, model.meshes);
    }
    // The memoized EnhancedMesh bindings: whole geometry copies kept for the life of UnitEmitters.
    for (const auto& entry : emitter_bindings_) {
        if (!output.step(Census::emitter_binding_copies)) break;
        output.string(Census::emitter_binding_copies, entry.first.first);
        if (!entry.second) continue;
        const auto& geometry = entry.second.value().binding.geometry;
        output.vector(Census::emitter_binding_copies, geometry.submeshes);
        for (const auto& mesh : geometry.submeshes) {
            if (!output.step(Census::emitter_binding_copies)) break;
            output.vector(Census::emitter_binding_copies, mesh.vertices);
            output.vector(Census::emitter_binding_copies, mesh.triangle_indices);
        }
    }
    for (const auto& entry : frames_) {
        const auto& [path, frames] = entry;
        if (!output.step(Census::emitter_metadata_copies)) break;
        output.range(Census::emitter_metadata_copies, &entry, sizeof(entry));
        output.string(Census::emitter_metadata_copies, path);
        output.vector(Census::emitter_metadata_copies, frames.bones);
        output.vector(Census::emitter_metadata_copies, frames.blast_phases);
    }
    for (const auto& ship : ships_) {
        if (!output.step(Census::emitter_binding_copies)) break;
        output.vector(Census::emitter_metadata_copies, ship.wanted);
        for (const auto& wanted : ship.wanted) {
            if (!output.step(Census::emitter_binding_copies)) break;
            if (!wanted.mesh) continue;
            output.vector(Census::emitter_binding_copies, wanted.mesh->geometry.submeshes);
            for (const auto& mesh : wanted.mesh->geometry.submeshes) {
                if (!output.step(Census::emitter_binding_copies)) break;
                output.vector(Census::emitter_binding_copies, mesh.vertices);
                output.vector(Census::emitter_binding_copies, mesh.triangle_indices);
            }
        }
    }
}
} // namespace eawr::presentation::godot_backend
