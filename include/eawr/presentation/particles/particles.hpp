#pragma once

#include "eawr/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::presentation::particles {

// Portable presentation-only values. These deliberately do not depend on sim math.
struct Vec3 final { float x{}, y{}, z{}; };
struct Vec4 final { float x{}, y{}, z{}, w{}; };
using Color = Vec4;
struct Basis3 final {
    Vec3 x{1.0F, 0.0F, 0.0F};
    Vec3 y{0.0F, 1.0F, 0.0F};
    Vec3 z{0.0F, 0.0F, 1.0F};
};

enum class PluginFamily : std::uint8_t { creator, translater, killer, renderer, modifier };
enum class Support : std::uint8_t { cpu, metadata_only, unsupported };

struct PluginInfo final {
    std::uint32_t id{};
    std::string_view name;
    PluginFamily family{};
    Support support{};
    std::string_view reason;
};

[[nodiscard]] std::span<const PluginInfo> plugin_catalog();
[[nodiscard]] const PluginInfo* find_plugin(std::uint32_t id);

enum class Interpolation : std::uint8_t { step, linear, smooth };
struct ScalarKey final { float time{}, value{}; };
struct ScalarTrack final {
    Interpolation interpolation{Interpolation::linear};
    std::vector<ScalarKey> keys;
};

enum class Shape : std::uint8_t { point, direction, sphere, range, spherical_range, cylinder, torus };
enum class MeshSpawnMode : std::uint8_t { disabled, random_vertex, random_surface, every_vertex };
struct MeshVertex final { Vec3 position{}, normal{}; };
struct MeshSubmesh final {
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> triangle_indices;
};
struct MeshGeometry final { std::vector<MeshSubmesh> submeshes; };
// Source Z-up affine mesh-bone frame. Independent of the emitter frame.
struct MeshFrame final { Vec3 origin{}; Basis3 basis{}; };
// Passed by value at the boundary; no caller storage or Pose callback is retained.
struct MeshBinding final { MeshGeometry geometry; MeshFrame frame; };
[[nodiscard]] core::Result<void> validate_mesh_binding(
    const MeshBinding& binding, MeshSpawnMode required_mode);
struct PropertyGroup final {
    Shape shape{Shape::point};
    Vec3 point{};
    Vec3 direction{};
    float magnitude_min{}, magnitude_max{};
    float radius_min{}, radius_max{};
    Vec3 range_min{}, range_max{};
    float angle_min{}, angle_max{};
    float spherical_radius_min{}, spherical_radius_max{};
    float cylinder_radius{}, cylinder_height_min{}, cylinder_height_max{};
    float torus_radius{}, tube_radius{};
};

struct EmitterDefinition final {
    std::string name;
    std::uint32_t creator_id{34};
    std::uint32_t mesh_mode_raw{};
    MeshSpawnMode mesh_mode{MeshSpawnMode::disabled};
    float mesh_surface_offset{0.5F};
    std::uint32_t translater_id{27};
    std::uint32_t killer_id{19};
    std::uint32_t renderer_id{22};
    std::vector<std::uint32_t> modifier_ids;

    // Shape creator / legacy-emitter conversion parameters.
    float particles_per_interval{1.0F};
    float spawn_interval{1.0F};
    bool bursting{};
    float start_delay{};
    float stop_time{}; // zero means no stop time
    // V1 skip time (0x33): seconds this emitter is pre-simulated before its
    // first presented frame. V1 freeze time (0x32): emitter-local seconds
    // after which the emitter stops updating (particles keep their last state
    // and no longer age, move or spawn). Zero disables either. The MIT
    // reference decodes both but applies neither; see docs/rendering.md.
    float skip_time{};
    float freeze_time{};
    PropertyGroup position;
    bool hollow_position{};
    PropertyGroup velocity;
    bool hollow_velocity{};
    bool local_velocity{true};
    bool inherit_parent_velocity{};
    float inherited_velocity_scale{};
    float max_inherited_velocity{3.402823466e38F};

    // Killer and modifier parameters used by the CPU port.
    float lifetime{1.0F};
    float lifetime_variation{};
    float inward_speed{};
    Vec3 acceleration{};
    bool acceleration_local{};
    float inward_acceleration{};
    float wind_response{};
    float terrain_elasticity{};
    ScalarTrack red, green, blue, alpha;
    ScalarTrack size;
    float size_variation{};
    ScalarTrack uv_index;
    std::uint32_t texture_size{64};
    ScalarTrack rotation_rate;
    bool random_rotation{};
    bool random_rotation_direction{};
    float random_rotation_average{};
    float random_rotation_variation{};
    Color color_variance{};
    bool grayscale_variance{};

    // Renderer data retained for the presentation adapters in render.hpp. The
    // legacy blend selector is kept exactly as stored: it is never wrapped or
    // clamped, so an out-of-table value fails closed at the adapter.
    std::uint32_t blend_mode{1};
    std::string color_texture;
    std::string normal_texture;
    bool disable_depth_test{};
    bool world_oriented{};
    float tail_size{50.0F};
    // V1 parent spawn relation. Each parent particle owns a child emitter instance.
    static constexpr std::uint32_t no_parent = 0xffffffffU;
    std::uint32_t parent_emitter{no_parent};
    bool spawn_on_parent_death{};

    bool cpu_ready{true};
    std::string unsupported_reason;
};

enum class AloParticleVersion : std::uint8_t { legacy_v1, plugin_v2 };
struct SystemDefinition final {
    AloParticleVersion version{AloParticleVersion::legacy_v1};
    bool leave_particles{true};
    std::vector<EmitterDefinition> emitters;
};

namespace diagnostic_codes {
inline constexpr std::string_view truncated = "EAWR-PARTICLE-0001";
inline constexpr std::string_view structure = "EAWR-PARTICLE-0002";
inline constexpr std::string_view limit = "EAWR-PARTICLE-0003";
inline constexpr std::string_view unsupported = "EAWR-PARTICLE-0004";
inline constexpr std::string_view invalid_value = "EAWR-PARTICLE-0005";
} // namespace diagnostic_codes

[[nodiscard]] core::Result<SystemDefinition> load_alo(
    std::span<const std::byte> bytes, std::string logical_path = {}
);

struct Particle final {
    std::uint64_t id{}; // stable across packed storage moves
    std::size_t emitter_index{};
    Vec3 position{};
    Vec3 velocity{};
    Vec3 acceleration{};
    Vec4 texcoords{0.0F, 0.0F, 1.0F, 1.0F};
    Color color{0.1F, 1.0F, 0.5F, 1.0F};
    float size{1.0F};
    float rotation{};
    float spawn_time{};
    float death_time{};
    // Per-particle plug-in state, public for snapshot/upload adapters only.
    float size_scale{1.0F};
    float rotation_direction{1.0F};
    Color color_offset{};
};

struct AdvanceStats final {
    std::size_t spawned{};
    std::size_t killed{};
    std::size_t dropped_at_capacity{};
    std::size_t child_instances_started{};
    std::size_t child_instances_detached{};
    std::size_t death_bursts{};
    std::size_t instances_dropped_at_capacity{};
};

class CpuSystem final {
public:
    explicit CpuSystem(SystemDefinition definition, std::uint32_t seed = 0x00c0ffeeU,
                       std::size_t max_particles = 65536U,
                       std::optional<MeshBinding> mesh_binding = std::nullopt);

    void set_origin(Vec3 origin);
    void set_basis(Basis3 basis);
    void set_wind(Vec3 acceleration);
    [[nodiscard]] core::Result<void> set_mesh_frame(const MeshFrame& frame);
    // The first valid advance first pre-simulates every emitter with a skip
    // time in fixed preroll_step increments, under the origin, basis and wind
    // set before it, then rebases all times so presentation time restarts at
    // zero. Pre-roll counts are included in that first advance's stats.
    [[nodiscard]] AdvanceStats advance(float delta_seconds);
    // BP-40: a linked particle (the Emitter translater, 26) keeps its position in its
    // emitter's frame, and FoC's renderer places it with the emitter's transform as it is
    // drawn; its velocity stays a world vector. Moves every such particle's position now by the emitter's rigid
    // motion since the last advance or follow (its rotation as well as its translation), so a
    // frame drawn between advances shows them where the emitter stands. The next advance then
    // moves them only by what the frame changes after this call.
    void follow_emitter();
    static constexpr float preroll_step = 1.0F / 30.0F;
    static constexpr float max_skip_seconds = 300.0F;

    [[nodiscard]] float presentation_time() const { return time_; }
    [[nodiscard]] std::span<const Particle> particles() const { return particles_; }
    [[nodiscard]] std::size_t capacity() const { return max_particles_; }
    [[nodiscard]] std::size_t total_dropped() const { return total_dropped_; }
    [[nodiscard]] std::size_t live_child_instances() const;

    // Source ParticleSystemInstance::Detach with leave-particles set: every
    // root emitter stops scheduling future emission (ParticleEmitterInstance::
    // Detach). Live particles keep aging and moving, attached child emitters
    // keep spawning while their parents live, and parent death still detaches
    // children and fires death bursts. No particle is killed and no event is
    // synthesized; IDs and the random stream are untouched. Idempotent.
    void detach() noexcept { detached_ = true; }
    [[nodiscard]] bool detached() const noexcept { return detached_; }
    // True only once detached with no live particle and no child emission
    // left. An undetached system is never finished, even while it is empty
    // between scheduled root emissions.
    [[nodiscard]] bool finished() const noexcept {
        return detached_ && particles_.empty() && child_instances_.empty();
    }

private:
    struct EmitterState final { float next_spawn{}; bool active{true}; std::size_t submesh{}, vertex{}; };
    struct ChildInstance final {
        std::uint64_t id{};
        std::size_t emitter_index{};
        std::uint64_t parent_id{};
        Particle parent_snapshot{};
        float next_spawn{};
        float start_time{};
        bool active{true};
    };
    struct ParentEvent final { Particle parent; bool death{}; };

    [[nodiscard]] float random(float minimum, float maximum);
    [[nodiscard]] std::uint32_t random_index(std::uint32_t upper);
    [[nodiscard]] Vec3 sample(const PropertyGroup& group, bool hollow);
    void initialize_particle(Particle& particle, const EmitterDefinition& emitter,
                             std::size_t emitter_index, float spawn_time,
                             const Particle* parent = nullptr);
    void update_particle(Particle& particle, const EmitterDefinition& emitter, float delta);
    void spawn_batch(std::size_t emitter_index, float spawn_time, const Particle* parent,
                     AdvanceStats& stats, std::vector<ParentEvent>& events);
    void process_events(std::vector<ParentEvent>& events, AdvanceStats& stats);
    void step(float delta_seconds, AdvanceStats& stats);
    void step_segment(float delta_seconds, AdvanceStats& stats);
    // The emitter's rigid motion since previous_origin_/previous_basis_, applied to a particle
    // of the Emitter translater (26).
    struct EmitterMotion final {
        Vec3 translation{};
        std::optional<Basis3> rotation;  // absent while the basis is unchanged
    };
    [[nodiscard]] EmitterMotion emitter_motion() const;
    void follow(Particle& particle, const EmitterMotion& motion) const;
    void preroll(AdvanceStats& stats);
    [[nodiscard]] bool frozen(std::size_t emitter_index, float at_time) const;

    SystemDefinition definition_;
    std::vector<EmitterState> emitters_;
    std::vector<std::vector<std::size_t>> children_;
    std::vector<Particle> particles_;
    std::vector<ChildInstance> child_instances_;
    std::uint64_t next_particle_id_{1};
    std::uint64_t next_instance_id_{1};
    std::uint32_t random_state_{};
    std::size_t max_particles_{};
    std::size_t total_dropped_{};
    float time_{};
    bool detached_{};
    bool prerolled_{};
    // Presentation time at which each emitter's instance started; negative
    // after a pre-roll by that emitter's skip time.
    std::vector<float> emitter_start_;
    Vec3 origin_{};
    Vec3 previous_origin_{};
    Basis3 basis_{};
    Basis3 previous_basis_{};
    Vec3 wind_{};
    std::optional<MeshBinding> mesh_binding_;
    std::size_t mesh_vertex_count_{};
};

} // namespace eawr::presentation::particles
