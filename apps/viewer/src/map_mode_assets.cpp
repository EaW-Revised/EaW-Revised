#include "map_mode_internal.hpp"

namespace eawr::presentation::godot_backend {

map_mode_detail::MapParticleProvider::MapParticleProvider(Node3D& host, const vfs::Vfs& filesystem,
        GodotRenderer* renderer, FogMode* fog)
        : filesystem_(&filesystem), backend_(std::make_unique<GodotParticleBackend>(host,
              [this](const std::string_view name) { return resolve_texture(name); },
              renderer && fog ? GodotParticleBackend::FogCallbacks{
                  .register_material = [renderer](const RID& material, const RID& shader) {
                      return renderer->register_external_fog_material(material, shader);
                  },
                  .unregister_material = [renderer](const std::uint64_t handle) {
                      renderer->unregister_external_fog_material(handle);
                  },
                  .attenuation_at_source_xy = [fog](float x, float y) {
                      const auto* grid = fog->source().find(fog->team());
                      return grid ? presentation::fog::attenuation_at(*grid, {x, y}) : std::uint8_t{0};
                  },
              } : GodotParticleBackend::FogCallbacks{})),
          registry_(std::make_unique<particles::EffectRegistry>(*backend_)) {}

MapMode::State::State(Options value) : options(std::move(value)) {}

MapMode::State::~State() {
        shutdown_trace::mark("map mode teardown begins");
        space.reset();
        shutdown_trace::mark("space view freed");
        release_particles();
        particles.reset();
        shutdown_trace::mark("particles freed");
        renderer.reset();
        shutdown_trace::mark("renderer freed");
    }

void MapMode::State::refresh_fog_snapshots() {
        if (!fog || !snapshot) return;
        std::vector<sim::RenderInstance> all(snapshot->instances().begin(), snapshot->instances().end());
        snapshot = fog->snapshot(std::move(all));
        if (terrain_snapshot) {
            std::vector<sim::RenderInstance> terrain(
                terrain_snapshot->instances().begin(), terrain_snapshot->instances().end());
            terrain_snapshot = fog->snapshot(std::move(terrain));
        }
        if (fog_renderer_stream != fog->stream()) {
            renderer->reset_fog_stream(fog->stream());
            fog_renderer_stream = fog->stream();
        }
    }

void MapMode::State::build_attached_plan(scene::VfsAssetCache& cache, const SpacePopulation* hardpoints) {
        struct Candidate final {
            const assets::Model* model{};
            const scene::Placement* placement{};
            std::vector<std::optional<sim::math::Mat3x4>> frames;
            std::vector<particles::EffectEvidence> effects;
            particles::VisibilityEvidence visibility{particles::VisibilityEvidence::unknown};
            std::vector<std::uint8_t> hardpoint_hidden;
        };
        const scene::ObjectResolver resolve = [this](const std::string_view id, const data::Category category) -> std::optional<data::EffectiveObject> {
            if (!catalog) return std::nullopt;
            auto resolved = catalog->resolve(id, category);
            if (!resolved) return std::nullopt;
            return std::move(resolved.value());
        };
        std::vector<Candidate> candidates;
        candidates.reserve(scene->placements.size());
        std::map<std::string, particles::EffectKind> kinds;
        std::map<std::string, std::size_t> capacities;
        for (const scene::Placement& placement : scene->placements) {
            if (placement.model_path.empty()) continue;
            if (hardpoints && hardpoints->is_marker(placement.scene_ordinal)) continue;
            const assets::Model* model = cache.model(placement.model_path);
            if (model == nullptr || model->proxies.empty()) continue;
            Candidate candidate;
            candidate.model = model;
            candidate.placement = &placement;
            candidate.frames.resize(model->bones.size());
            candidate.effects.resize(model->proxies.size());
            if (auto player = animation::Player::create(*model)) {
                if (auto pose = player.value().sample({}); pose && pose.value().bones.size() == model->bones.size()) {
                    candidate.visibility = particles::VisibilityEvidence::bind_pose;
                    for (std::size_t bone = 0; bone < model->bones.size(); ++bone) {
                        candidate.frames[bone] = particles::fixed_model_frame(
                            pose.value().bones[bone].model_asset, scene::fixed_from_binary32);
                    }
                }
            }
            for (std::size_t ordinal = 0; ordinal < model->proxies.size() && ordinal < placement.effects.size(); ++ordinal) {
                const std::string& path = placement.effects[ordinal].resolved;
                if (path.empty()) continue;
                auto found = kinds.find(path);
                if (found == kinds.end()) {
                    particles::EffectKind kind = particles::EffectKind::unknown;
                    if (auto bytes = filesystem->open(path)) {
                        if (auto system = particles::load_alo(bytes.value(), path); system && !system.value().emitters.empty()) {
                            kind = particles::EffectKind::particle;
                            if (!options.attached_capacity_explicit)
                                capacities[path] = particles::continuous_capacity(system.value(), options.attached_capacity);
                        } else if (cache.model(path)) {
                            kind = particles::EffectKind::non_particle;
                        }
                    }
                    found = kinds.emplace(path, kind).first;
                }
                const auto capacity = capacities.find(path);
                candidate.effects[ordinal] = {found->second,
                    capacity == capacities.end() ? options.attached_capacity : capacity->second};
            }
            if (hardpoints && !placement.object_id.empty()) {
                if (const auto object = resolve(placement.object_id, data::Category::game_object)) {
                    const scene::SpaceObjectTags tags = scene::space_object_tags(*object, resolve);
                    candidate.hardpoint_hidden = scene::hidden_hardpoint_proxies(*model, tags.hardpoints,
                        hardpoints->hardpoint_states(placement.scene_ordinal));
                }
            }
            candidates.push_back(std::move(candidate));
        }
        std::vector<particles::MapEffectPlacementInput> inputs;
        inputs.reserve(candidates.size());
        for (const Candidate& candidate : candidates) {
            inputs.push_back({candidate.model, candidate.placement, candidate.frames, candidate.effects,
                candidate.visibility, options.effect_alt, options.effect_lod, candidate.hardpoint_hidden});
        }
        const std::size_t ordinary = static_cast<std::size_t>(scene->count(scene::Cause::model_particle_system));
        std::size_t budget = options.particle_capacity > ordinary
            ? options.particle_capacity - ordinary : 0;
        if (map_kind == "space" && !options.particle_capacity_explicit) {
            // Bound the automatic allocation; explicit capture controls still
            // exercise exhaustion. Include every candidate so TED order cannot
            // starve a later ambient field at the normal space-map density.
            constexpr std::size_t automatic_limit = 131072;
            std::size_t requested = 0;
            for (const auto& candidate : candidates)
                for (const auto& effect : candidate.effects)
                    requested = std::min(automatic_limit, requested + effect.requested_capacity);
            budget = std::max(budget, requested);
        }
        attached_plan = particles::plan_map_effects(inputs, options.particle_seed, budget);
        attached_budget = budget;
        if (effect_animation_idle()) bind_attached_clips(candidates);
    }

void MapMode::State::release_particles() {
        if (!particles) return;
        particles->release();
        particle_rids_after_release = particles->live_rids();
        particle_resources_after_release = particles->live_resources();
        if (renderer) particle_fog_consumers_after_release = renderer->fog_status().external_consumers;
    }

std::uint64_t map_mode_detail::OwnerProbeBackend::create_emitter(const particles::EmitterRenderPlan& plan) {
        if (!plan.drawable) { cause_ = "plan is not drawable: " + plan.cause; return 0U; }
        if (const auto valid = validate_material(GodotParticleBackend::material_for(plan)); !valid) {
            cause_ = valid.error().message;
            return 0U;
        }
        if (resolver_(plan.texture) == nullptr) {
            cause_ = "colour texture '" + plan.texture + "' did not resolve";
            return 0U;
        }
        return next_++;
    }

void map_mode_detail::MapParticleProvider::follow_lighting(const GodotRenderer& renderer) {
        const auto& lighting = renderer.lighting();
        if (!lighting) return;
        backend_->set_lighting({.toward_light = lighting->toward_light, .diffuse = lighting->sun_diffuse,
            .specular = lighting->specular, .fill = lighting->sph_fill});
    }

void map_mode_detail::MapParticleProvider::capture_fog_evidence(const GodotRenderer& renderer, const FogMode& fog) {
        const auto resources = backend_->fog_emitter_evidence();
        const auto consumers = renderer.external_fog_consumers();
        const auto selected = fog.source().find(fog.team());
        const auto& sources = fog.sources();
        const auto source = std::find_if(sources.begin(), sources.end(),
            [&](const auto& entry) { return entry.team == fog.team(); });
        for (ParticlePlacement& placement : placements_) {
            for (auto& emitter : placement.emitters) {
                emitter.fog_source_sha256 = source == sources.end() ? "" : source->sha256;
                emitter.fog_team = fog.team();
                emitter.fog_tick = fog.tick();
                emitter.fog_revision = selected ? selected->revision() : 0;
                const auto resource = std::find_if(resources.begin(), resources.end(),
                    [&](const auto& item) { return item.resource == emitter.resource; });
                const auto consumer = std::find_if(consumers.begin(), consumers.end(),
                    [&](const auto& item) { return item.handle == emitter.fog_handle; });
                if (resource != resources.end()) {
                    emitter.quads = resource->quads;
                    emitter.minimum_attenuation = resource->minimum_attenuation;
                    emitter.maximum_attenuation = resource->maximum_attenuation;
                }
                emitter.fog_bound = consumer != consumers.end() && consumer->bound;
            }
        }
    }

} // namespace eawr::presentation::godot_backend
