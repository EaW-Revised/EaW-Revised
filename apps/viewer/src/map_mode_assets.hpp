#pragma once

// Private to the map mode translation units (map_mode*.cpp): the mode's
// State, the particle provider it owns, and the helpers the units share.
#include "map_mode.hpp"
#include "capture_viewport.hpp"
#include "fog_mode.hpp"
#include "idle_clips.hpp"
#include "land_look.hpp"
#include "battle_input.hpp"
#include "live_session_view.hpp"
#include "shutdown_trace.hpp"
#include "live_fog_view.hpp"
#include "overview_fade_view.hpp"
#include "battle_audio.hpp"
#include "particle_workers.hpp"
#include "perf_trace.hpp"
#include "unit_emitters.hpp"
#include "space_environment.hpp"
#include "space_populate.hpp"
#include "space_fog.hpp"
#include "particle_adapter.hpp"
#include "map_camera.hpp"
#include "ui/perf_overlay_view.hpp"
#include "ui/tactical_hud.hpp"

#include "eawr/assets/map.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/presentation/animation/animation.hpp"
#include "eawr/presentation/godot/renderer.hpp"
#include "eawr/presentation/lighting/lighting.hpp"
#include "eawr/presentation/lighting/wind.hpp"
#include "eawr/presentation/particles/render.hpp"
#include "eawr/presentation/particles/map_attachment_owner.hpp"
#include "eawr/presentation/particles/map_effect_plan.hpp"
#include "eawr/presentation/particles/prewarmed_capacity.hpp"
#include "eawr/presentation/particles/proxy_binding.hpp"
#include "eawr/presentation/space/space.hpp"
#include "eawr/presentation/terrain/terrain.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/scene/space_population.hpp"
#include "eawr/sim/replay.hpp"
#include "eawr/sim/snapshot.hpp"
#include "eawr/vfs/vfs.hpp"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/input_event_with_modifiers.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <limits>
#include <utility>
#include <vector>

using namespace godot;

namespace eawr::presentation::godot_backend {
namespace map_mode_detail {

[[nodiscard]] std::string json(std::string_view value);
[[nodiscard]] bool ieq(std::string_view left, std::string_view right);
[[nodiscard]] std::string hash_bytes(std::span<const std::byte> bytes);
[[nodiscard]] std::optional<std::string> probe_reference(
    const vfs::Vfs& filesystem, std::string_view root,
    std::string_view name, std::span<const std::string_view> suffixes);
[[nodiscard]] assets::Texture placeholder_texture();
[[nodiscard]] std::string effect_name(terrain::SurfaceEffect effect);
[[nodiscard]] Ref<Image> decode_png(const std::vector<std::byte>& bytes);
[[nodiscard]] camera::TacticalFrame camera_frame(const FixedCamera& source);
[[nodiscard]] FixedCamera render_camera(const camera::TacticalFrame& source);
void inject_map_key(godot::Key code, bool pressed);

// Checks the land map camera self-test records; a run with any other count fails.
constexpr std::size_t map_camera_selftest_check_count = 13;
constexpr std::array<std::string_view, 2> texture_suffixes{".tga", ".dds"};
constexpr std::array<std::string_view, 1> model_suffixes{".alo"};

struct SlotRecord final {
    std::uint8_t slot{};
    std::string effect_program;
    std::string effect_technique;
    std::string effect_pass;
    std::string declared_primary;
    std::string resolved_primary;
    std::string declared_secondary;
    std::string resolved_secondary;
    std::uint64_t cells{};
    bool texture_resolved{};
};

struct ParticlePlacement final {
    struct Emitter final {
        std::size_t index{};
        std::string blend;
        std::uint64_t resource{};
        std::uint64_t fog_handle{};
        std::size_t quads{};
        std::uint8_t minimum_attenuation{255};
        std::uint8_t maximum_attenuation{};
        // Peak vertex alpha of the emitter's stream at the last advance.
        float maximum_alpha{};
        bool fog_bound{};
        std::string fog_source_sha256;
        std::uint32_t fog_team{};
        std::uint64_t fog_tick{};
        std::uint64_t fog_revision{};
    };
    std::string identity;
    std::string object_id;
    std::string logical_path;
    std::string sha256;
    bool confirmed_particle_model{};
    bool attached{};
    // Driven by an idle-clip owner (--eawr-map-effect-animation idle): its
    // generations are spawned and released by the owner, not by `handle`.
    bool owned{};
    std::size_t owner_index{};
    // Instances the owner held after its last sample (not reset by release).
    std::size_t live_instances{};
    std::size_t effect_plan_index{};
    std::uint32_t record_ordinal{};
    std::uint32_t seed{};
    std::uint32_t capacity{};
    std::int64_t scale_raw{};
    std::array<std::int64_t, 12> transform_raw{};
    particles::EmitterFrame frame;
    particles::EffectHandle handle{};
    particles::EffectFrameStats stats;
    std::array<float, 3> bounds_min{};
    std::array<float, 3> bounds_max{};
    bool has_bounds{};
    std::uint64_t changed_pixels{};
    std::vector<Emitter> emitters;
    std::string status{"unresolved"};
    std::vector<std::string> causes;
};

// Idle-clip owners handed from the plan builder (P1 #29,
// --eawr-map-effect-animation idle). One Player per placement is bound once;
// owned and watched entries name an attached plan record, its placement's
// clip, its proxy bone and the placement's Q24 transform.
struct MapOwnerInput final {
    struct Clip final {
        std::uint64_t scene_ordinal{};
        std::shared_ptr<const animation::Player> player;
    };
    struct Owned final {
        std::size_t plan_index{};
        std::size_t clip{};
        std::size_t bone{};
        sim::math::Mat3x4 placement{};
    };
    struct Watched final {
        std::size_t plan_index{};
        std::size_t clip{};
        std::size_t bone{};
    };
    std::vector<Clip> clips;
    std::vector<Owned> owned;
    std::vector<Watched> watched;
    // Admitted attached allocation plus the owners' drain headroom.
    std::size_t live_capacity_limit{};
};

// Dry-run backend for an idle-clip owner's system at prepare time, before any
// generation exists: it accepts exactly the plans the Godot backend accepts on
// drawability, material validation and texture resolution, and creates no
// RenderingServer resource. Shader compilation and texture upload are only
// exercised by the first real spawn.
class OwnerProbeBackend final : public particles::RenderBackend {
public:
    explicit OwnerProbeBackend(GodotParticleBackend::TextureResolver resolver) : resolver_(std::move(resolver)) {}
    std::uint64_t create_emitter(const particles::EmitterRenderPlan& plan) override;
    void update_emitter(std::uint64_t, const particles::VertexStream&) override {}
    void destroy_emitter(std::uint64_t) override {}
    std::string failure_cause() const override { return cause_; }

private:
    GodotParticleBackend::TextureResolver resolver_;
    std::string cause_;
    std::uint64_t next_{1};
};

// All state here belongs to the viewer. Nothing is submitted to the render
// snapshot or serialized into replay input.
class MapParticleProvider final {
public:
    MapParticleProvider(Node3D& host, const vfs::Vfs& filesystem,
        GodotRenderer* renderer = nullptr, FogMode* fog = nullptr);

    [[nodiscard]] bool prepare(const assets::Map& map, const scene::Scene& scene,
        std::uint32_t seed, std::uint32_t capacity);
    [[nodiscard]] bool prepare_attached(const particles::MapEffectPlan& plan, const scene::Scene& scene,
        const MapOwnerInput* owners = nullptr);
    // Follows a re-made attached plan (#136): records match by scene and
    // proxy ordinal. An emitter whose record stays admitted with the same
    // effect, seed, capacity and frame keeps running; the others are
    // released and dropped, and newly admitted records start now. Static
    // emitters only (the space path has no idle-clip owners).
    [[nodiscard]] bool sync_attached(const particles::MapEffectPlan& before, const particles::MapEffectPlan& after,
        const scene::Scene& scene);
    [[nodiscard]] bool advance(float delta, const particles::CameraFrame& camera);
    // Bump-mapped emitters are lit by the scene's sun and fill, as the
    // renderer's bump hull adapters are (#238).
    void follow_lighting(const GodotRenderer& renderer);
    void release();
    [[nodiscard]] const std::vector<ParticlePlacement>& placements() const { return placements_; }
    [[nodiscard]] std::vector<ParticlePlacement>& placements_mutable() { return placements_; }
    [[nodiscard]] std::size_t live_rids() const { return backend_->live_rids(); }
    [[nodiscard]] std::size_t live_resources() const { return registry_->live_backend_resources(); }
    [[nodiscard]] std::uint32_t frames() const { return frames_; }
    [[nodiscard]] bool failed() const { return failed_; }
    void capture_fog_evidence(const GodotRenderer& renderer, const FogMode& fog);
    [[nodiscard]] std::size_t active_count() const {
        return static_cast<std::size_t>(std::count_if(placements_.begin(), placements_.end(),
            [](const ParticlePlacement& placement) { return placement.handle != 0; }));
    }
    // The clock keeps running while any static instance is live or any owner
    // is unreleased, so an owner hidden at its first sample still advances.
    [[nodiscard]] bool has_work() const {
        return active_count() != 0 || std::any_of(owners_.begin(), owners_.end(),
            [](const Owned& owned) { return !owned.owner.released(); });
    }
    [[nodiscard]] bool has_owners() const { return !owners_.empty(); }
    // Anything drawn now, including an owner's drain with no active generation.
    [[nodiscard]] bool has_live() const {
        return active_count() != 0 || std::any_of(owners_.begin(), owners_.end(),
            [](const Owned& owned) { return owned.owner.live_instances() != 0; });
    }
    [[nodiscard]] const particles::MapAttachmentOwner* owner_for(const std::size_t plan_index) const {
        for (const Owned& owned : owners_) {
            if (placements_[owned.placement].effect_plan_index == plan_index) return &owned.owner;
        }
        return nullptr;
    }
    [[nodiscard]] std::optional<std::uint32_t> watch_edges(const std::size_t plan_index) const {
        for (const Watched& watched : watches_) {
            if (watched.plan_index == plan_index) return watched.watch.visible_edges();
        }
        return std::nullopt;
    }

private:
    struct Clip final {
        std::shared_ptr<const animation::Player> player;
        std::optional<animation::Pose> pose;
    };
    struct Owned final {
        std::size_t placement{};
        std::size_t clip{};
        particles::MapAttachmentOwner owner;
    };
    struct Watched final {
        std::size_t plan_index{};
        std::size_t clip{};
        particles::MapHiddenProxyWatch watch;
    };
    [[nodiscard]] particles::AttachmentLifecycle::Spawn owned_spawner(std::size_t placement,
        particles::SystemDefinition system, std::size_t capacity,
        std::optional<particles::MeshBinding> mesh_binding);
    [[nodiscard]] const assets::Texture* resolve_texture(std::string_view name);
    // Starts one admitted plan record (a placement is appended either way).
    void start_attached(const particles::MapEffectPlan& plan, std::size_t index, const scene::Scene& scene,
        const MapOwnerInput* owners);
    const vfs::Vfs* filesystem_;
    std::map<std::string, std::optional<assets::Texture>, std::less<>> textures_;
    std::unique_ptr<GodotParticleBackend> backend_;
    std::unique_ptr<particles::EffectRegistry> registry_;
    std::vector<ParticlePlacement> placements_;
    // Owners hold the registry but are not RAII: release() releases them
    // before any static handle, and before the registry and backend go away.
    std::vector<Clip> clips_;
    std::vector<Owned> owners_;
    std::vector<Watched> watches_;
    std::size_t live_capacity_limit_{};
    std::uint32_t frames_{};
    bool failed_{};
};

} // namespace map_mode_detail


} // namespace eawr::presentation::godot_backend
