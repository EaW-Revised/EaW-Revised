#include "effect_mode_internal.hpp"
#include "render_profile_viewport.hpp"

namespace eawr::presentation::godot_backend {
namespace {

// Causes quote the authored texture name; reports carry only its file name.
[[nodiscard]] std::string redact(std::string text, const std::string_view authored) {
    const std::string_view name = file_name(authored);
    if (name.size() == authored.size() || authored.empty()) return text;
    for (std::size_t at = text.find(authored); at != std::string::npos; at = text.find(authored, at)) {
        text.replace(at, authored.size(), name);
        at += name.size();
    }
    return text;
}


} // namespace

void EffectMode::State::write_probe_emitters(std::ostream& output) const {
    for (std::size_t index = 0; index < plans.size(); ++index) {
        const particles::EmitterRenderPlan& plan = plans[index];
        const particles::EmitterDefinition& emitter = system.emitters[index];
        const auto texture = textures.find(canonical(file_name(plan.texture)));
        const bool resolved = texture != textures.end() && texture->second.texture.has_value();
        output << (index == 0 ? "\n    " : ",\n    ")
            << "{\"index\": " << index << ", \"name\": " << json(emitter.name)
            << ", \"creator_id\": " << emitter.creator_id
            << ", \"mesh_mode\": " << emitter.mesh_mode_raw
            << ", \"renderer_id\": " << plan.renderer_id
            << ", \"family\": " << json(to_string(plan.family))
            << ", \"adapter\": " << json(plan.drawable ? GodotParticleBackend::adapter_name(plan.family) : "")
            << ", \"blend_selector\": " << plan.blend_selector
            << ", \"program\": " << json(plan.program) << ", \"technique\": " << json(plan.technique)
            << ", \"blend\": " << json(to_string(plan.blend))
            << ", \"phase\": " << json(to_string(plan.phase))
            << ", \"depth_test\": " << (plan.depth_test ? "true" : "false")
            << ", \"depth_write\": " << (plan.depth_write ? "true" : "false")
            << ", \"sort_particles\": " << (plan.sort_particles ? "true" : "false")
            << ", \"texture\": " << json(file_name(plan.texture))
            << ", \"texture_authored_with_directory\": "
            << (file_name(plan.texture).size() != plan.texture.size() ? "true" : "false")
            << ", \"texture_logical_path\": " << json(resolved ? texture->second.logical_path : "")
            << ", \"texture_sha256\": " << json(resolved ? texture->second.sha256 : "")
            << ", \"parent_emitter\": "
            << (emitter.parent_emitter == particles::EmitterDefinition::no_parent
                ? std::string("null") : std::to_string(emitter.parent_emitter))
            << ", \"spawn_on_parent_death\": " << (emitter.spawn_on_parent_death ? "true" : "false")
            << ", \"legacy_route_supported\": false"
            << ", \"drawn\": " << (plan.drawable ? "true" : "false")
            << ", \"cause\": " << json(redact(plan.cause, plan.texture)) << "}";
    }
    output << (plans.empty() ? "" : "\n  ") << "],\n  \"frames\": [";
}

void EffectMode::State::write_probe_frames(std::ostream& output) const {
    for (std::size_t index = 0; index < records.size(); ++index) {
        const FrameRecord& record = records[index];
        std::size_t quads{};
        std::size_t triangles{};
        output << (index == 0 ? "\n    " : ",\n    ") << "{\"frame\": " << record.frame
            << ", \"index\": " << record.frame - 1
            << ", \"time\": " << record.time
            << ", \"advanced\": " << (record.advanced ? "true" : "false")
            << ", \"detached\": " << (record.stats.detached ? "true" : "false")
            << ", \"finished\": " << (record.stats.finished ? "true" : "false")
            << ", \"released\": " << (record.released ? "true" : "false")
            << ", \"resources\": " << record.resources
            << ", \"particles\": " << record.stats.particles
            << ", \"spawned\": " << record.stats.advance.spawned
            << ", \"killed\": " << record.stats.advance.killed
            << ", \"dropped\": " << record.stats.advance.dropped_at_capacity
            << ", \"child_instances_started\": " << record.stats.advance.child_instances_started
            << ", \"child_instances_detached\": " << record.stats.advance.child_instances_detached
            << ", \"death_bursts\": " << record.stats.advance.death_bursts
            << ", \"instances_dropped_at_capacity\": " << record.stats.advance.instances_dropped_at_capacity
            << ", \"emitter_particles\": [";
        for (std::size_t emitter = 0; emitter < record.stats.emitters.size(); ++emitter) {
            output << (emitter ? ", " : "") << record.stats.emitters[emitter].particles;
            quads += record.stats.emitters[emitter].quads;
            triangles += record.stats.emitters[emitter].triangles;
        }
        output << "], \"quads\": " << quads << ", \"triangles\": " << triangles
            << ", \"stream_hash\": " << json(particles::hex64(record.stats.hash)) << "}";
    }
}

} // namespace eawr::presentation::godot_backend
