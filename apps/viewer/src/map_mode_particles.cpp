#include "map_mode_internal.hpp"

namespace eawr::presentation::godot_backend {
namespace map_mode_detail {

const assets::Texture* MapParticleProvider::resolve_texture(const std::string_view authored) {
    const std::size_t slash = authored.find_last_of("/\\");
    const std::string_view name = slash == std::string_view::npos ? authored : authored.substr(slash + 1);
    const auto path = probe_reference(*filesystem_, "data/art/textures/", name, texture_suffixes);
    if (!path) return nullptr;
    auto found = textures_.find(*path);
    if (found == textures_.end()) {
        std::optional<assets::Texture> decoded;
        if (auto loaded = assets::load_texture(*filesystem_, *path)) decoded = std::move(loaded.value());
        found = textures_.emplace(*path, std::move(decoded)).first;
    }
    return found->second ? &*found->second : nullptr;
}

bool MapParticleProvider::prepare(const assets::Map& map, const scene::Scene& built,
    const std::uint32_t seed, const std::uint32_t capacity) {
    using Fixed = sim::math::Fixed;
    const auto is_particle_model = [](const scene::Placement& placement) {
        return std::any_of(placement.issues.begin(), placement.issues.end(),
            [](const scene::Issue& issue) { return issue.cause == scene::Cause::model_particle_system; });
    };
    const std::uint32_t confirmed = static_cast<std::uint32_t>(std::count_if(
        built.placements.begin(), built.placements.end(), is_particle_model));
    std::uint32_t confirmed_index{};
    for (const scene::Placement& placement : built.placements) {
        const bool particle_model = is_particle_model(placement);
        const bool missing_model = std::any_of(placement.issues.begin(), placement.issues.end(),
            [](const scene::Issue& issue) { return issue.cause == scene::Cause::model_not_in_vfs; });
        if (!particle_model && !missing_model) continue;
        ParticlePlacement record;
        record.identity = placement.map_logical_path + "#" + std::to_string(placement.record_ordinal);
        record.object_id = placement.object_id;
        if (placement.model_path.empty()) {
            record.logical_path = "data/art/models/";
            for (const char character : placement.model_declared) {
                const char folded = character >= 'A' && character <= 'Z'
                    ? static_cast<char>(character + ('a' - 'A')) : character;
                record.logical_path.push_back(folded == '\\' ? '/' : folded);
            }
        } else {
            record.logical_path = placement.model_path;
        }
        record.confirmed_particle_model = particle_model;
        record.record_ordinal = placement.record_ordinal;
        record.seed = seed + placement.record_ordinal;
        if (particle_model) {
            record.capacity = capacity / confirmed + (confirmed_index < capacity % confirmed ? 1U : 0U);
            ++confirmed_index;
        }
        record.scale_raw = placement.scale_raw;
        for (const auto& issue : placement.issues) {
            if (issue.cause != scene::Cause::model_particle_system) {
                record.causes.push_back(std::string(scene::to_string(issue.cause))
                    + (issue.detail.empty() ? "" : ": " + issue.detail));
            }
        }
        placements_.push_back(std::move(record));
        ParticlePlacement& current = placements_.back();
        if (!particle_model) {
            current.status = "unresolved_model_kind";
            failed_ = true;
            continue;
        }
        current.status = "failed";
        const auto fail = [&](const std::string& cause) {
            current.causes.push_back(cause);
            failed_ = true;
        };
        auto bytes = filesystem_->open(placement.model_path);
        if (!bytes) { fail(core::format_diagnostic(bytes.error())); continue; }
        current.sha256 = hash_bytes(bytes.value());
        if (current.capacity == 0) {
            fail("capacity_exhausted: aggregate map particle budget is smaller than the confirmed placement count");
            continue;
        }
        if (!current.causes.empty()) {
            failed_ = true;
            continue;
        }
        if (placement.record_ordinal >= map.placements.size()) {
            fail("TED record ordinal is outside the loaded placement stream");
            continue;
        }
        const assets::Placement& source = map.placements[placement.record_ordinal];
        if (!source.position) { fail("position_absent"); continue; }
        if (!source.orientation_degrees || source.orientation_status == assets::OrientationStatus::absent) {
            fail("orientation_absent"); continue;
        }
        if (source.orientation_status == assets::OrientationStatus::nonfinite) {
            fail("transform_nonfinite"); continue;
        }
        const std::array<float, 6> values{source.position->x, source.position->y,
            source.position->z, source.orientation_degrees->z, source.orientation_degrees->y,
            source.orientation_degrees->x};
        std::array<Fixed, 6> checked{};
        bool valid = true;
        for (std::size_t index = 0; index < values.size(); ++index) {
            auto value = scene::fixed_from_binary32(values[index]);
            if (!value) {
                fail(core::format_diagnostic(value.error()));
                valid = false;
                break;
            }
            checked[index] = value.value();
        }
        if (!valid) continue;
        auto height = sim::math::add(checked[2], Fixed::from_raw(placement.layer_z_adjust_raw));
        if (!height) { fail(core::format_diagnostic(height.error())); continue; }
        checked[2] = height.value();
        auto transform = scene::placement_transform(checked[0], checked[1], checked[2], checked[3], checked[4], checked[5],
            Fixed::from_raw(placement.scale_raw));
        if (!transform) { fail(core::format_diagnostic(transform.error())); continue; }
        const auto as_float = [](const Fixed value) {
            return static_cast<float>(static_cast<double>(value.raw()) / static_cast<double>(Fixed::scale));
        };
        for (std::size_t row = 0; row < 3; ++row) {
            for (std::size_t column = 0; column < 4; ++column) {
                current.transform_raw[row * 4 + column] = transform.value().rows[row][column].raw();
            }
        }
        const auto& m = transform.value().rows;
        current.frame.origin = {as_float(m[0][3]), as_float(m[1][3]), as_float(m[2][3])};
        current.frame.basis = {{as_float(m[0][0]), as_float(m[1][0]), as_float(m[2][0])},
            {as_float(m[0][1]), as_float(m[1][1]), as_float(m[2][1])},
            {as_float(m[0][2]), as_float(m[1][2]), as_float(m[2][2])}};
        auto system = particles::load_alo(bytes.value(), placement.model_path);
        if (!system) { fail(core::format_diagnostic(system.error())); continue; }
        if (system.value().emitters.empty()) { fail("particle system declares no emitters"); continue; }
        const auto before = backend_->fog_emitter_evidence();
        const std::uint64_t last_resource = before.empty() ? 0 : before.back().resource;
        auto handle = registry_->spawn(std::move(system.value()), current.seed, current.capacity);
        if (!handle) { fail(core::format_diagnostic(handle.error())); continue; }
        current.handle = handle.value();
        if (auto placed = registry_->set_frame(current.handle, current.frame); !placed) {
            fail(core::format_diagnostic(placed.error()));
            continue;
        }
        for (const auto& plan : *registry_->plans(current.handle)) {
            if (plan.drawable) {
                const auto resources = backend_->fog_emitter_evidence();
                const auto found = std::find_if(resources.begin(), resources.end(),
                    [&](const auto& item) {
                        return item.resource > last_resource && item.emitter_index == plan.emitter_index;
                    });
                current.emitters.push_back({.index = plan.emitter_index,
                    .blend = std::string(GodotParticleBackend::material_adapter_name(plan.blend)),
                    .resource = found == resources.end() ? 0 : found->resource,
                    .fog_handle = found == resources.end() ? 0 : found->fog_handle});
            }
            if (!plan.drawable) {
                std::string cause = plan.cause;
                const std::size_t slash = plan.texture.find_last_of("/\\");
                if (slash != std::string::npos) {
                    const std::string_view file = std::string_view(plan.texture).substr(slash + 1);
                    const std::string_view safe_file = file.empty() ? std::string_view("<unnamed>") : file;
                    for (std::size_t at = cause.find(plan.texture); at != std::string::npos;
                         at = cause.find(plan.texture, at + safe_file.size())) {
                        cause.replace(at, plan.texture.size(), safe_file);
                    }
                }
                fail("emitter " + std::to_string(plan.emitter_index) + ": " + cause);
            }
        }
        if (current.causes.empty()) current.status = "ready";
    }
    return !failed_;
}

particles::AttachmentLifecycle::Spawn MapParticleProvider::owned_spawner(const std::size_t placement,
    particles::SystemDefinition system, const std::size_t capacity,
    std::optional<particles::MeshBinding> mesh_binding) {
    return [this, placement, system = std::move(system), capacity,
            mesh_binding = std::move(mesh_binding)](particles::EffectRegistry& registry,
        const std::uint32_t seed) -> core::Result<particles::EffectHandle> {
        const auto before = backend_->fog_emitter_evidence();
        const std::uint64_t last_resource = before.empty() ? 0 : before.back().resource;
        auto handle = mesh_binding ? registry.spawn(system, seed, capacity, *mesh_binding)
                                   : registry.spawn(system, seed, capacity);
        if (!handle) return handle;
        std::vector<ParticlePlacement::Emitter> emitters;
        for (const auto& emitter : *registry.plans(handle.value())) {
            if (!emitter.drawable) {
                static_cast<void>(registry.release(handle.value()));
                return core::Result<particles::EffectHandle>::failure(particles::map_owner_diagnostic(
                    "emitter " + std::to_string(emitter.emitter_index) + ": " + emitter.cause));
            }
            const auto resources = backend_->fog_emitter_evidence();
            const auto found = std::find_if(resources.begin(), resources.end(),
                [&](const auto& item) {
                    return item.resource > last_resource && item.emitter_index == emitter.emitter_index;
                });
            emitters.push_back({.index = emitter.emitter_index,
                .blend = std::string(GodotParticleBackend::material_adapter_name(emitter.blend)),
                .resource = found == resources.end() ? 0 : found->resource,
                .fog_handle = found == resources.end() ? 0 : found->fog_handle});
        }
        // The report names the newest generation's emitter resources.
        placements_[placement].emitters = std::move(emitters);
        return handle;
    };
}

bool MapParticleProvider::prepare_attached(const particles::MapEffectPlan& plan, const scene::Scene& scene,
    const MapOwnerInput* owners) {
    if (owners) {
        for (const auto& clip : owners->clips) clips_.push_back({clip.player, std::nullopt});
        for (const auto& watched : owners->watched) {
            watches_.push_back({watched.plan_index, watched.clip, particles::MapHiddenProxyWatch(watched.bone)});
        }
        live_capacity_limit_ = owners->live_capacity_limit;
    }
    for (std::size_t index = 0; index < plan.records.size(); ++index) {
        const particles::MapEffectRecord& effect = plan.records[index];
        if (effect.status != particles::MapEffectStatus::admitted || !effect.emitter_frame) continue;
        start_attached(plan, index, scene, owners);
    }
    return !failed_;
}

bool MapParticleProvider::sync_attached(const particles::MapEffectPlan& before,
    const particles::MapEffectPlan& after, const scene::Scene& scene) {
    if (!owners_.empty()) {
        failed_ = true;
        return false;
    }
    const auto key = [](const particles::MapEffectRecord& record) {
        return std::pair<std::uint64_t, std::size_t>{record.scene_ordinal, record.proxy_ordinal};
    };
    std::map<std::pair<std::uint64_t, std::size_t>, std::size_t> admitted;
    for (std::size_t index = 0; index < after.records.size(); ++index) {
        const particles::MapEffectRecord& record = after.records[index];
        if (record.status == particles::MapEffectStatus::admitted && record.emitter_frame) {
            admitted.emplace(key(record), index);
        }
    }
    std::vector<bool> running(after.records.size());
    std::vector<ParticlePlacement> kept;
    kept.reserve(placements_.size());
    for (ParticlePlacement& placement : placements_) {
        if (placement.attached && placement.effect_plan_index < before.records.size()) {
            const particles::MapEffectRecord& old = before.records[placement.effect_plan_index];
            const auto found = admitted.find(key(old));
            if (found != admitted.end()) {
                const particles::MapEffectRecord& record = after.records[found->second];
                if (record.effect_logical_path == old.effect_logical_path && record.seed == old.seed
                    && record.capacity == old.capacity && record.scale_raw == old.scale_raw
                    && record.emitter_frame == old.emitter_frame) {
                    placement.effect_plan_index = found->second;
                    running[found->second] = true;
                    kept.push_back(std::move(placement));
                    continue;
                }
            }
            // Stopped, or never started: it leaves with its record. A failed
            // release stays listed with its cause.
            if (placement.handle != 0) {
                const auto released = registry_->release(placement.handle);
                placement.handle = 0;
                if (!released) {
                    placement.causes.push_back(core::format_diagnostic(released.error()));
                    placement.status = "failed";
                    failed_ = true;
                    kept.push_back(std::move(placement));
                }
            }
            continue;
        }
        kept.push_back(std::move(placement));
    }
    placements_ = std::move(kept);
    for (const auto& entry : admitted) {
        if (!running[entry.second]) start_attached(after, entry.second, scene, nullptr);
    }
    return !failed_;
}

void MapParticleProvider::start_attached(const particles::MapEffectPlan& plan, const std::size_t index,
    const scene::Scene& scene, const MapOwnerInput* owners) {
    const particles::MapEffectRecord& effect = plan.records[index];
    ParticlePlacement record;
    record.attached = true;
    record.effect_plan_index = index;
    record.identity = effect.map_logical_path + "#" + std::to_string(effect.record_ordinal)
        + "/proxy#" + std::to_string(effect.proxy_ordinal);
    record.object_id = effect.proxy_name;
    record.logical_path = effect.effect_logical_path;
    record.record_ordinal = effect.record_ordinal;
    record.seed = effect.seed;
    record.capacity = static_cast<std::uint32_t>(effect.capacity);
    record.scale_raw = effect.scale_raw;
    record.frame = particles::source_emitter_frame(*effect.emitter_frame);
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            record.transform_raw[row * 4 + column] = effect.emitter_frame->rows[row][column].raw();
        }
    }
    placements_.push_back(std::move(record));
    ParticlePlacement& current = placements_.back();
    const auto fail = [&](const std::string& cause) {
        current.status = "failed";
        current.causes.push_back(cause);
        failed_ = true;
    };
    const auto bytes = filesystem_->open(effect.effect_logical_path);
    if (!bytes) { fail(core::format_diagnostic(bytes.error())); return; }
    current.sha256 = hash_bytes(bytes.value());
    auto system = particles::load_alo(bytes.value(), effect.effect_logical_path);
    if (!system) { fail(core::format_diagnostic(system.error())); return; }
    if (system.value().emitters.empty()) { fail("particle system declares no emitters"); return; }
    const bool needs_mesh = std::any_of(system.value().emitters.begin(), system.value().emitters.end(),
        [&](const auto& emitter) {
            return system.value().version == particles::AloParticleVersion::legacy_v1
                && emitter.cpu_ready && emitter.creator_id == 35;
        });
    std::optional<particles::ProxyMeshBinding> mesh_binding;
    std::optional<particles::MeshFrame> mesh_frame;
    if (needs_mesh) {
        std::string binding_failure;
        do {
            auto model = assets::load_model(*filesystem_, effect.model_logical_path);
            if (!model) { binding_failure = core::format_diagnostic(model.error()); break; }
            auto binding = particles::bind_proxy_mesh(model.value(), effect.proxy_name);
            if (!binding) { binding_failure = core::format_diagnostic(binding.error()); break; }
            mesh_binding.emplace(std::move(binding.value()));
            const auto placement = std::find_if(scene.placements.begin(), scene.placements.end(),
                [&](const scene::Placement& item) { return item.scene_ordinal == effect.scene_ordinal; });
            if (placement == scene.placements.end() || !placement->transform) {
                binding_failure = "host placement has no transform"; break;
            }
            auto player = animation::Player::create(model.value());
            if (!player) { binding_failure = core::format_diagnostic(player.error()); break; }
            auto pose = player.value().sample({});
            if (!pose) { binding_failure = core::format_diagnostic(pose.error()); break; }
            auto owner = particles::map_owner_frame(pose.value(), mesh_binding->owner_bone,
                placement->transform->matrix, scene::fixed_from_binary32);
            if (!owner) { binding_failure = core::format_diagnostic(owner.error()); break; }
            mesh_frame = particles::MeshFrame{owner.value().origin, owner.value().basis};
        } while (false);
        if (!binding_failure.empty()) {
            mesh_binding.reset();
            // Keep independent emitters running. Descendants of a mesh
            // emitter have no source particle and must be skipped too.
            const auto& emitters = system.value().emitters;
            std::vector<bool> skipped(emitters.size());
            for (std::size_t emitter = 0; emitter < emitters.size(); ++emitter)
                skipped[emitter] = emitters[emitter].creator_id == 35;
            for (std::size_t pass = 0; pass < emitters.size(); ++pass) {
                for (std::size_t emitter = 0; emitter < emitters.size(); ++emitter) {
                    const auto parent = emitters[emitter].parent_emitter;
                    if (parent < skipped.size() && skipped[parent]) skipped[emitter] = true;
                }
            }
            std::vector<std::size_t> remap(emitters.size(), emitters.size());
            particles::SystemDefinition filtered = system.value();
            filtered.emitters.clear();
            for (std::size_t emitter = 0; emitter < emitters.size(); ++emitter) {
                if (skipped[emitter]) {
                    current.causes.push_back("emitter " + std::to_string(emitter)
                        + " skipped: mesh binding: " + binding_failure);
                } else {
                    remap[emitter] = filtered.emitters.size();
                    filtered.emitters.push_back(emitters[emitter]);
                }
            }
            for (auto& emitter : filtered.emitters) {
                if (emitter.parent_emitter < remap.size())
                    emitter.parent_emitter = static_cast<std::uint32_t>(remap[emitter.parent_emitter]);
            }
            system.value() = std::move(filtered);
            if (system.value().emitters.empty()) { current.status = "skipped"; return; }
        }
    }
    const auto plans = particles::plan_system(system.value());
    const auto unsupported = std::find_if(plans.begin(), plans.end(),
        [](const particles::EmitterRenderPlan& emitter) { return !emitter.drawable; });
    if (unsupported != plans.end()) {
        current.status = "unsupported";
        current.causes.push_back("emitter " + std::to_string(unsupported->emitter_index)
            + ": " + unsupported->cause);
        return;
    }
    const MapOwnerInput::Owned* owned = nullptr;
    if (owners) {
        for (const auto& entry : owners->owned) {
            if (entry.plan_index == index) owned = &entry;
        }
    }
    if (owned) {
        // Validate the system now: a clip that never shows the bone would
        // otherwise never spawn, and a non-drawable emitter would pass.
        std::string probe_failure;
        {
            OwnerProbeBackend probe([this](const std::string_view name) { return resolve_texture(name); });
            particles::EffectRegistry dry(probe);
            const auto handle = mesh_binding
                ? dry.spawn(system.value(), current.seed, current.capacity, mesh_binding->binding)
                : dry.spawn(system.value(), current.seed, current.capacity);
            if (!handle) {
                probe_failure = core::format_diagnostic(handle.error());
            } else {
                for (const auto& emitter : *dry.plans(handle.value())) {
                    if (!emitter.drawable) {
                        probe_failure = "emitter " + std::to_string(emitter.emitter_index) + ": " + emitter.cause;
                        break;
                    }
                }
                static_cast<void>(dry.release(handle.value()));
            }
        }
        if (!probe_failure.empty()) { fail("owner prepare probe: " + probe_failure); return; }
        // The first generation waits for the owner's first visible sample.
        const std::size_t placement = placements_.size() - 1;
        current.owned = true;
        current.owner_index = owners_.size();
        owners_.push_back({placement, owned->clip, particles::MapAttachmentOwner(*registry_,
            owned_spawner(placement, std::move(system.value()), current.capacity,
                mesh_binding ? std::optional<particles::MeshBinding>(mesh_binding->binding) : std::nullopt),
            current.seed, current.capacity, owned->placement, owned->bone,
            scene::fixed_from_binary32,
            mesh_binding ? std::optional<std::size_t>(mesh_binding->owner_bone) : std::nullopt)});
        current.status = "ready";
        return;
    }
    const auto before = backend_->fog_emitter_evidence();
    const std::uint64_t last_resource = before.empty() ? 0 : before.back().resource;
    auto handle = mesh_binding
        ? registry_->spawn(std::move(system.value()), current.seed, current.capacity, mesh_binding->binding)
        : registry_->spawn(std::move(system.value()), current.seed, current.capacity);
    if (!handle) { fail(core::format_diagnostic(handle.error())); return; }
    current.handle = handle.value();
    if (auto placed = registry_->set_frame(current.handle, current.frame); !placed) {
        fail(core::format_diagnostic(placed.error()));
        return;
    }
    if (mesh_frame) {
        if (auto placed = registry_->set_mesh_frame(current.handle, *mesh_frame); !placed) {
            fail(core::format_diagnostic(placed.error())); return;
        }
    }
    for (const auto& emitter : *registry_->plans(current.handle)) {
        if (!emitter.drawable) {
            fail("emitter " + std::to_string(emitter.emitter_index) + ": " + emitter.cause);
            continue;
        }
        const auto resources = backend_->fog_emitter_evidence();
        const auto found = std::find_if(resources.begin(), resources.end(),
            [&](const auto& item) {
                return item.resource > last_resource && item.emitter_index == emitter.emitter_index;
            });
        current.emitters.push_back({.index = emitter.emitter_index,
            .blend = std::string(GodotParticleBackend::material_adapter_name(emitter.blend)),
            .resource = found == resources.end() ? 0 : found->resource,
            .fog_handle = found == resources.end() ? 0 : found->fog_handle});
    }
    if (current.causes.empty()) current.status = "ready";

}

bool MapParticleProvider::advance(const float delta, const particles::CameraFrame& camera) {
    const std::uint32_t sample = frames_;
    ++frames_;
    // One pose per clip-bound placement per sample, at the exact n/30 s frame.
    for (Clip& clip : clips_) {
        auto pose = particles::map_owner_sample(*clip.player, sample);
        clip.pose.reset();
        if (pose) clip.pose = std::move(pose.value());
    }
    for (Watched& watched : watches_) {
        if (clips_[watched.clip].pose) static_cast<void>(watched.watch.observe(*clips_[watched.clip].pose));
    }
    std::size_t live_capacity{};
    const auto record_alpha = [](ParticlePlacement& placement) {
        for (auto& emitter : placement.emitters) {
            emitter.maximum_alpha = emitter.index < placement.stats.emitters.size()
                ? placement.stats.emitters[emitter.index].maximum_alpha : 0.0F;
        }
    };
    for (ParticlePlacement& placement : placements_) {
        if (placement.owned) {
            Owned& owned = owners_[placement.owner_index];
            if (owned.owner.released()) continue;
            const auto fail = [&](const std::string& cause) {
                placement.status = "failed";
                placement.causes.push_back("sample " + std::to_string(sample) + ": " + cause);
                failed_ = true;
            };
            const auto& pose = clips_[owned.clip].pose;
            if (!pose) { fail("idle clip did not sample"); continue; }
            auto step = owned.owner.step(sample, *pose, camera);
            if (!step) { fail(core::format_diagnostic(step.error())); continue; }
            placement.handle = owned.owner.lifecycle().active().value_or(0);
            placement.live_instances = step.value().live_instances;
            placement.stats = std::move(step.value().stats);
            placement.has_bounds = placement.stats.has_bounds;
            record_alpha(placement);
            live_capacity += owned.owner.live_capacity();
            if (placement.has_bounds) {
                const auto low = placement.stats.bounds_min;
                const auto high = placement.stats.bounds_max;
                placement.bounds_min = {low.x, low.z, -high.y};
                placement.bounds_max = {high.x, high.z, -low.y};
                placement.status = "drawn";
            } else if (placement.live_instances == 0) {
                placement.status = "hidden_by_clip";
            } else {
                // A live generation or drain before its first particle, between
                // bursts, or after its last particle died.
                placement.status = placement.stats.particles == 0 ? "live_no_particles" : "live_undrawn";
            }
            continue;
        }
        if (placement.attached && placement.handle != 0) live_capacity += placement.capacity;
        if (placement.handle == 0) continue;
        auto advanced = registry_->advance(placement.handle, delta, camera);
        if (!advanced) {
            placement.status = "failed";
            placement.causes.push_back(core::format_diagnostic(advanced.error()));
            failed_ = true;
            continue;
        }
        placement.stats = std::move(advanced.value());
        placement.has_bounds = placement.stats.has_bounds;
        record_alpha(placement);
        if (placement.has_bounds) {
            const auto low = placement.stats.bounds_min;
            const auto high = placement.stats.bounds_max;
            placement.bounds_min = {low.x, low.z, -high.y};
            placement.bounds_max = {high.x, high.z, -low.y};
            placement.status = "drawn";
        } else if (placement.stats.particles == 0) {
            placement.status = "live_no_particles";
        }
    }
    if (!owners_.empty() && live_capacity > live_capacity_limit_) {
        // Fail closed: the drain headroom reserved at admission was exceeded.
        for (ParticlePlacement& placement : placements_) {
            if (!placement.owned) continue;
            placement.status = "failed";
            placement.causes.push_back("sample " + std::to_string(sample) + ": live attached capacity "
                + std::to_string(live_capacity) + " exceeds allocation plus drain headroom "
                + std::to_string(live_capacity_limit_));
        }
        failed_ = true;
    }
    return !failed_;
}

void MapParticleProvider::release() {
    // Owners first: each releases its active generation and drain once.
    for (Owned& owned : owners_) {
        ParticlePlacement& placement = placements_[owned.placement];
        const auto released = owned.owner.release_all();
        if (!released) {
            placement.causes.push_back(core::format_diagnostic(released.error()));
            failed_ = true;
        }
        // live_instances keeps its last-sample value: the capture check runs
        // after release and must still see what was drawn.
        placement.handle = 0;
    }
    for (ParticlePlacement& placement : placements_) {
        if (placement.handle == 0) continue;
        const auto released = registry_->release(placement.handle);
        if (!released) {
            placement.causes.push_back(core::format_diagnostic(released.error()));
            failed_ = true;
        }
        placement.handle = 0;
    }
}

} // namespace map_mode_detail

bool MapMode::State::sync_space_attached(Node3D& host, SpacePopulation& population) {
    const scene::Scene* populated = population.scene();
    if (populated == nullptr || !catalog) return false;
    scene::VfsAssetCache cache(*filesystem);
    const particles::MapEffectPlan before = std::move(attached_plan);
    scene = *populated;
    build_attached_plan(cache, &population);
    if (!particles && !attached_plan.records.empty()) {
        particles = std::make_unique<MapParticleProvider>(host, *filesystem);
    }
    const bool synced = !particles || particles->sync_attached(before, attached_plan, *scene);
    population.set_attached_effects(space_attached_effects());
    return synced;
}

SpacePopulation::AttachedEffects MapMode::State::space_attached_effects() const {
    SpacePopulation::AttachedEffects attached;
    if (!options.map_effects) {
        attached.cause = "--eawr-map-effects off";
    } else if (!catalog) {
        attached.cause = "the XML catalog did not load";
    } else if (!particles) {
        attached.cause = "no placement of the space scene declares an attached particle effect";
    } else {
        attached.composed = true;
        attached.records = attached_plan.records.size();
        for (const particles::MapEffectRecord& record : attached_plan.records) {
            attached.hardpoint_hidden += record.cause == particles::MapEffectCause::hardpoint_state ? 1U : 0U;
            attached.admitted += record.status == particles::MapEffectStatus::admitted ? 1U : 0U;
            attached.capacity_exhausted += record.cause == particles::MapEffectCause::capacity_exhausted ? 1U : 0U;
        }
        attached.cause = "the land attachment plan over the space scene, advanced by the map particle "
                         "provider with the space camera; per-placement status in map_particles";
    }
    return attached;
}

bool MapMode::State::verify_particles(const std::vector<std::byte>& with_effects,
    const std::vector<std::byte>& without_effects) {
    if (!particles) return true;
    const Ref<Image> on = decode_png(with_effects);
    const Ref<Image> off = decode_png(without_effects);
    if (on.is_null() || off.is_null() || on->get_width() != off->get_width()
        || on->get_height() != off->get_height()) {
        failure = "particle comparison PNGs could not be decoded at equal size";
        return false;
    }
    const int32_t width = on->get_width();
    const int32_t height = on->get_height();
    // Summed |dR| + |dG| + |dB| above which a pixel counts as changed.
    constexpr float changed_pixel_threshold = 0.04F;
    std::vector<std::uint8_t> inside(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    std::vector<std::uint8_t> changed(inside.size());
    for (int32_t y = 0; y < height; ++y) {
        for (int32_t x = 0; x < width; ++x) {
            const Color a = on->get_pixel(x, y);
            const Color b = off->get_pixel(x, y);
            const float delta = std::abs(a.r - b.r) + std::abs(a.g - b.g) + std::abs(a.b - b.b);
            changed[static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)] =
                delta > changed_pixel_threshold ? 1U : 0U;
        }
    }
    std::size_t expected{};
    std::size_t skipped_mesh{};
    particle_owners_empty = 0;
    particle_placements_empty = 0;
    particle_placements_outside_view = 0;
    particle_placements_subpixel_heat = 0;
    for (ParticlePlacement& placement : particles->placements_mutable()) {
        if (placement.status == "unsupported") continue;
        if (!placement.confirmed_particle_model && !placement.attached) continue;
        if (placement.status == "skipped") { ++skipped_mesh; continue; }
        if (placement.owned && !placement.has_bounds) {
            // An idle-clip owner may validly draw nothing at capture: hidden
            // by its clip with no generation or drain left, or live with no
            // particle yet (start delay, a late reappearance) or between
            // bursts. It is accounted empty only if its last sample held no
            // particle; it projects no region, so any pixel it changed counts
            // as outside every bound.
            if (placement.stats.particles != 0) {
                placement.causes.push_back("owner holds particles but drew no bounds at capture");
                failure = "particle placement " + placement.identity + " has particles but no drawn bounds";
                return false;
            }
            ++particle_owners_empty;
            continue;
        }
        if (!placement.has_bounds && placement.stats.particles == 0) {
            // A delayed or idle static emitter has no quads to submit yet.
            // Account for that sample before requiring drawn fog quads.
            ++particle_placements_empty;
            continue;
        }
        if (!placement.has_bounds) {
            placement.causes.push_back("captured frame has no drawn particle bounds");
            failure = "particle placement " + placement.identity + " has no drawn bounds";
            return false;
        }
        float left = 1e30F, right = -1e30F, top = 1e30F, bottom = -1e30F;
        for (int corner = 0; corner < 8; ++corner) {
            const std::array<float, 3> point{
                (corner & 1) ? placement.bounds_max[0] : placement.bounds_min[0],
                (corner & 2) ? placement.bounds_max[1] : placement.bounds_min[1],
                (corner & 4) ? placement.bounds_max[2] : placement.bounds_min[2]};
            const auto pixel = project(point);
            left = std::min(left, pixel[0]); right = std::max(right, pixel[0]);
            top = std::min(top, pixel[1]); bottom = std::max(bottom, pixel[1]);
        }
        const int32_t x0 = std::max(0, static_cast<int32_t>(std::floor(left)) - 2);
        const int32_t x1 = std::min(width - 1, static_cast<int32_t>(std::ceil(right)) + 2);
        const int32_t y0 = std::max(0, static_cast<int32_t>(std::floor(top)) - 2);
        const int32_t y1 = std::min(height - 1, static_cast<int32_t>(std::ceil(bottom)) + 2);
        if (x0 > x1 || y0 > y1) {
            placement.causes.push_back("projected effect bounds are outside the capture viewport");
            ++particle_placements_outside_view;
            continue;
        }
        if (fog) {
            if (!particle_fog_bound_at_capture || placement.emitters.empty()
                || std::any_of(placement.emitters.begin(), placement.emitters.end(),
                    [](const ParticlePlacement::Emitter& emitter) {
                        return emitter.resource == 0 || emitter.fog_handle == 0
                            || !emitter.fog_bound || emitter.quads == 0;
                    })) {
                placement.causes.push_back("particle emitter was not submitted with the selected fog grid");
                failure = "particle placement " + placement.identity + " lacks fog submission evidence";
                return false;
            }
        }
        ++expected;
        for (int32_t y = y0; y <= y1; ++y) {
            for (int32_t x = x0; x <= x1; ++x) {
                const std::size_t index = static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x);
                inside[index] = 1U;
                placement.changed_pixels += changed[index];
            }
        }
        // A faint additive or modulate effect under a fogged cell can fall
        // below the change threshold (a Naboo waterfall at 70/255), so an
        // effect whose every emitter is at least half darkened by fog is
        // excused and recorded. A nearly clear cell (254/255) is not.
        constexpr std::uint32_t excused_attenuation = 128;
        const bool fog_dimmed_effect = fog && !placement.emitters.empty() && std::all_of(
            placement.emitters.begin(), placement.emitters.end(),
            [](const ParticlePlacement::Emitter& emitter) {
                return emitter.maximum_attenuation <= excused_attenuation
                    && (emitter.blend == "eawr-particle-additive-v1"
                        || emitter.blend == "eawr-particle-modulate-v1");
            });
        // Heat only moves pixels already drawn, weighted by its own alpha:
        // heat_distortion_pixel_change_bound derives, from the shader, the
        // largest summed change one heat draw at its peak vertex alpha can
        // make to a pixel (bilinear sample, mix blend, 8-bit steps). An
        // effect whose every emitter is heat and stays below the change
        // threshold cannot be seen in this comparison even when drawn
        // correctly (a speeder's heat shimmer keys alpha at 5/255), so it is
        // excused and recorded. Heat that could reach the threshold is not.
        const bool subpixel_heat = !placement.emitters.empty() && std::all_of(
            placement.emitters.begin(), placement.emitters.end(),
            [width, height](const ParticlePlacement::Emitter& emitter) {
                return emitter.blend == "eawr-particle-heat-distortion-v1"
                    && particles::heat_distortion_pixel_change_bound(emitter.maximum_alpha,
                        GodotParticleBackend::heat_distortion_amount, width, height)
                        < changed_pixel_threshold;
            });
        if (placement.changed_pixels == 0 && fog_dimmed_effect) {
            placement.causes.push_back("fog-attenuated effect changed no pixels above the threshold");
        } else if (placement.changed_pixels == 0 && subpixel_heat) {
            placement.causes.push_back("heat distortion cannot change a pixel above the threshold at its peak vertex alpha");
            ++particle_placements_subpixel_heat;
        } else if (placement.changed_pixels == 0) {
            const auto center = project({
                (placement.bounds_min[0] + placement.bounds_max[0]) * 0.5F,
                (placement.bounds_min[1] + placement.bounds_max[1]) * 0.5F,
                (placement.bounds_min[2] + placement.bounds_max[2]) * 0.5F});
            if (center[0] < 0.0F || center[0] >= static_cast<float>(width)
                || center[1] < 0.0F || center[1] >= static_cast<float>(height)) {
                // A broad effect bound can touch the viewport while its live
                // particle cloud remains beyond the edge at this sample.
                placement.causes.push_back("projected effect center is outside the viewport and no pixels changed");
                ++particle_placements_outside_view;
                --expected;
                continue;
            }
            placement.causes.push_back("effects-off comparison changed no pixels inside projected bounds");
            failure = "particle placement " + placement.identity + " changed no pixels in its projected bounds";
            return false;
        }
    }
    for (std::size_t index = 0; index < changed.size(); ++index) {
        if (changed[index] && !inside[index]) ++particle_changed_outside;
    }
    if (particle_changed_outside != 0) {
        failure = "particle comparison changed pixels outside all projected effect bounds";
        return false;
    }
    // Every considered placement was verified in pixels or accounted for as
    // empty, outside the capture or skipped for a missing mesh binding; with
    // none, nothing authored was checked.
    if (expected == 0 && particle_owners_empty == 0 && particle_placements_empty == 0
        && particle_placements_outside_view == 0 && skipped_mesh == 0) {
        failure = "no map particle placement was drawn or accounted for at capture";
        return false;
    }
    return true;
}

} // namespace eawr::presentation::godot_backend
