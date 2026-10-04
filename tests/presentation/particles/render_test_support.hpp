#pragma once

#include "eawr/presentation/animation/animation.hpp"
#include "eawr/presentation/particles/attachment_lifecycle.hpp"
#include "eawr/presentation/particles/particles.hpp"
#include "eawr/presentation/particles/render.hpp"
#include "eawr/presentation/particles/proxy_binding.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace particles = eawr::presentation::particles;
namespace playback = eawr::presentation::animation;

namespace particle_render_contracts {

inline int failures{};
inline void expect(const bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
inline bool finite(const particles::Vec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
inline bool finite(const particles::Vec4 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z) && std::isfinite(value.w);
}
inline bool finite_stream(const particles::VertexStream& stream) {
    if (!finite(stream.bounds_min) || !finite(stream.bounds_max)) return false;
    for (const auto& vertex : stream.vertices) {
        if (!finite(vertex.position) || !finite(vertex.color) ||
            !std::isfinite(vertex.u) || !std::isfinite(vertex.v)) return false;
    }
    return true;
}
inline bool close(const float a, const float b, const float epsilon = 0.0001F) { return std::fabs(a - b) <= epsilon; }
inline float dot(const particles::Vec3 a, const particles::Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline particles::Vec3 minus(const particles::Vec3 a, const particles::Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

// A wholly original legacy particle ALO, written byte by byte like the CPU
// contract fixture, extended with the renderer fields the adapters consume.
using Bytes = std::vector<std::byte>;
inline void u32(Bytes& out, const std::uint32_t value) { for (unsigned shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::byte>((value >> shift) & 0xffU)); }
inline void f32(Bytes& out, const float value) { u32(out, std::bit_cast<std::uint32_t>(value)); }
inline void append(Bytes& out, const Bytes& value) { out.insert(out.end(), value.begin(), value.end()); }
inline void text(Bytes& out, const std::string& value) { for (const char ch : value) out.push_back(static_cast<std::byte>(ch)); out.push_back(std::byte{}); }
inline Bytes chunk(const std::uint32_t type, Bytes payload, const bool group = false) {
    Bytes out; u32(out, type); u32(out, static_cast<std::uint32_t>(payload.size()) | (group ? 0x80000000U : 0));
    append(out, payload); return out;
}
inline Bytes mini(const std::uint8_t type, Bytes payload) { Bytes out{static_cast<std::byte>(type), static_cast<std::byte>(payload.size())}; append(out, payload); return out; }
inline Bytes integer(const std::uint32_t value) { Bytes out; u32(out, value); return out; }
inline Bytes scalar(const float value) { Bytes out; f32(out, value); return out; }
inline Bytes flag(const bool value) { return Bytes{static_cast<std::byte>(value ? 1 : 0)}; }
inline Bytes old_group(const float x = 0.0F, const float y = 0.0F, const float z = 0.0F) {
    Bytes data; u32(data, 0); for (int i = 0; i < 12; ++i) f32(data, 0);
    f32(data, x); f32(data, y); f32(data, z);
    return chunk(0x1100, chunk(0x1101, std::move(data)), true);
}
inline Bytes track_header(const float first, const float last, const bool color) {
    Bytes data;
    if (color) { append(data, mini(2, Bytes{static_cast<std::byte>(first * 255)})); append(data, mini(3, Bytes{static_cast<std::byte>(last * 255)})); }
    else { append(data, mini(2, scalar(first))); append(data, mini(3, scalar(last))); }
    append(data, mini(4, integer(0))); return chunk(0, std::move(data));
}
struct LegacyEmitter final {
    std::string name{"render"};
    std::uint32_t blend{2};
    bool heat{};
    bool world{};
    bool tail{};
    bool no_depth{};
    bool depth_sort{};
    std::uint32_t primitive{1};
    float tail_size{12.0F};
    std::string texture{"p_synthetic_glow.tga"};
    std::string normal;
    std::uint32_t spawn_on_death{0xffffffffU};
    std::uint32_t spawn_during_life{0xffffffffU};
    float parent_link_strength{};
    float velocity_x{};
    std::uint32_t mesh_mode{};
};
inline Bytes legacy_emitter(const LegacyEmitter& spec) {
    Bytes properties;
    append(properties, mini(0x04, integer(spec.blend)));
    append(properties, mini(0x05, integer(spec.primitive)));
    append(properties, mini(0x2b, flag(spec.depth_sort)));
    append(properties, mini(0x0f, scalar(2.0F)));
    append(properties, mini(0x2a, integer(4)));
    append(properties, mini(0x10, integer(4)));
    append(properties, mini(0x3b, flag(spec.heat)));
    append(properties, mini(0x2e, flag(spec.world)));
    append(properties, mini(0x41, flag(spec.tail)));
    append(properties, mini(0x42, scalar(spec.tail_size)));
    append(properties, mini(0x46, flag(spec.no_depth)));
    if(spec.mesh_mode)append(properties,mini(0x34,integer(spec.mesh_mode)));
    if(spec.parent_link_strength!=0){
        append(properties, mini(0x28, scalar(spec.parent_link_strength)));
        append(properties, mini(0x43, flag(true)));
    }
    Bytes groups; append(groups, old_group(spec.velocity_x)); append(groups, old_group(2)); append(groups, old_group());
    Bytes tracks;
    for (int i = 0; i < 4; ++i) { append(tracks, track_header(1, 1, true)); append(tracks, chunk(1, {})); }
    for (int i = 0; i < 3; ++i) { append(tracks, track_header(i == 0 ? 2.0F : 0.0F, i == 0 ? 2.0F : 0.0F, false)); append(tracks, chunk(1, {})); }
    Bytes emitter; append(emitter, chunk(2, std::move(properties)));
    Bytes texture; text(texture, spec.texture); append(emitter, chunk(0x03, std::move(texture)));
    Bytes name; text(name, spec.name); append(emitter, chunk(0x16, std::move(name)));
    append(emitter, chunk(0x29, std::move(groups), true)); append(emitter, chunk(1, std::move(tracks), true));
    if (spec.spawn_on_death != 0xffffffffU || spec.spawn_during_life != 0xffffffffU) {
        Bytes links; append(links, mini(0x37, integer(spec.spawn_on_death))); append(links, mini(0x39, integer(spec.spawn_during_life)));
        append(emitter, chunk(0x36, std::move(links)));
    }
    if (!spec.normal.empty()) { Bytes normal; text(normal, spec.normal); append(emitter, chunk(0x45, std::move(normal))); }
    return chunk(0x700, std::move(emitter), true);
}
inline Bytes legacy_system(const std::vector<LegacyEmitter>& specs) {
    Bytes emitters; for (const auto& spec : specs) append(emitters, legacy_emitter(spec));
    Bytes root; Bytes name; text(name, "render-test"); append(root, chunk(0, std::move(name)));
    append(root, chunk(1, integer(1))); append(root, chunk(0x800, std::move(emitters), true));
    append(root, chunk(2, Bytes{std::byte{1}})); return chunk(0x900, std::move(root), true);
}

inline particles::ScalarTrack constant(const float value) { return {particles::Interpolation::linear, {{0, value}, {1, value}}}; }
inline particles::EmitterDefinition drawable_emitter() {
    particles::EmitterDefinition emitter;
    emitter.name = "programmatic"; emitter.particles_per_interval = 1; emitter.spawn_interval = 100;
    emitter.stop_time = 0.001F; emitter.lifetime = 10;
    emitter.red = emitter.green = emitter.blue = emitter.alpha = constant(1);
    emitter.size = constant(1); emitter.uv_index = constant(0); emitter.rotation_rate = constant(0);
    emitter.renderer_id = 22; emitter.blend_mode = 1; emitter.color_texture = "p_synthetic_glow.tga";
    return emitter;
}
// A spray that keeps spawning, so streams change frame to frame and random
// sampling is exercised by the determinism checks.
inline particles::EmitterDefinition spray_emitter() {
    auto emitter = drawable_emitter();
    emitter.spawn_interval = 1.0F; emitter.particles_per_interval = 40.0F; emitter.stop_time = 0.0F;
    emitter.lifetime = 1.5F; emitter.lifetime_variation = 0.3F;
    emitter.velocity.shape = particles::Shape::sphere; emitter.velocity.radius_min = 2; emitter.velocity.radius_max = 6;
    emitter.size = {particles::Interpolation::linear, {{0, 0.5F}, {1, 2.0F}}}; emitter.size_variation = 0.2F;
    emitter.alpha = {particles::Interpolation::linear, {{0, 1}, {1, 0}}};
    emitter.rotation_rate = constant(0.25F); emitter.modifier_ids = {7, 14, 49};
    return emitter;
}
inline particles::SystemDefinition mixed_system() {
    particles::SystemDefinition system;
    system.emitters.push_back(spray_emitter());
    auto second = spray_emitter(); second.blend_mode = 2; second.renderer_id = 28; second.name = "xy";
    system.emitters.push_back(second);
    auto kite = spray_emitter(); kite.renderer_id = 52; kite.name = "kite"; kite.tail_size = 3;
    system.emitters.push_back(kite);
    auto unsupported = spray_emitter(); unsupported.blend_mode = 5; unsupported.name = "depth-sprite";
    system.emitters.push_back(unsupported);
    return system;
}

class RecordingBackend final : public particles::RenderBackend {
public:
    std::uint64_t create_emitter(const particles::EmitterRenderPlan& plan) override {
        if (std::this_thread::get_id() != caller_) { ++wrong_thread; return 0; }
        if (refuse_emitter && plan.emitter_index == *refuse_emitter) return 0;
        const std::uint64_t id = next_++;
        live[id] = plan.emitter_index; ++created; return id;
    }
    void update_emitter(const std::uint64_t resource, const particles::VertexStream& stream) override {
        if (std::this_thread::get_id() != caller_) { ++wrong_thread; return; }
        if (!live.contains(resource)) { ++invalid_updates; return; }
        expect(finite_stream(stream), "backend uploads only finite vertices and bounds");
        record.push_back(particles::stream_hash(stream));
        last[resource]=stream;
        vertices += stream.vertices.size();
    }
    void destroy_emitter(const std::uint64_t resource) override {
        if (std::this_thread::get_id() != caller_) { ++wrong_thread; return; }
        if (live.erase(resource) == 0) ++invalid_destroys; else ++destroyed;
    }
    std::map<std::uint64_t, std::size_t> live;
    std::map<std::uint64_t, particles::VertexStream> last;
    std::vector<std::uint64_t> record;
    std::size_t created{}, destroyed{}, invalid_updates{}, invalid_destroys{}, vertices{};
    std::optional<std::size_t> refuse_emitter;
    std::atomic<std::size_t> wrong_thread{};
private:
    const std::thread::id caller_{std::this_thread::get_id()};
    std::uint64_t next_{1};
};

inline particles::CameraFrame test_camera() { return particles::camera_frame_from_render({0, 20, 60}, {0, 0, 0}, {0, 1, 0}); }

void test_offscreen_updates_and_bounds();

inline std::vector<std::uint64_t> run(RecordingBackend& backend, const std::uint32_t seed, const int frames) {
    particles::EffectRegistry registry(backend);
    const auto handle = registry.spawn(mixed_system(), seed, 512);
    expect(bool(handle), "effect spawns");
    std::vector<std::uint64_t> hashes;
    for (int frame = 0; frame < frames; ++frame) {
        const auto stats = registry.advance(handle.value(), 1.0F / 30.0F, test_camera());
        expect(bool(stats), "effect advances");
        hashes.push_back(stats.value().hash);
    }
    return hashes;
}

inline particles::MeshBinding test_mesh_binding(){
    particles::MeshBinding binding;
    binding.geometry.submeshes={{{{{3,0,0},{0,0,1}},{{9,0,0},{0,0,1}}},{}}};
    return binding;
}
inline particles::SystemDefinition mesh_system(const particles::MeshSpawnMode mode){
    particles::SystemDefinition system;
    auto emitter=drawable_emitter();
    emitter.creator_id=35;emitter.mesh_mode=mode;emitter.mesh_mode_raw=static_cast<std::uint32_t>(mode);
    emitter.mesh_surface_offset=0;emitter.stop_time=0;emitter.spawn_interval=1;
    emitter.lifetime=0.5F;
    system.emitters.push_back(emitter);
    return system;
}

void test_parser_retains_renderer_fields();
void test_death_burst_ignores_parent_velocity();
void test_parent_link_rejection();
void test_plan_policy();
void test_quad_geometry();
void test_finite_rotation_boundary();
void test_stream_validation();
void test_fixed_seed_streams();
void test_particle_detail_gates();
void test_legacy_sort_triangles_and_atlas();
void test_cpu_steady_state_allocates_nothing();
void test_particle_detail_batch();
void test_release_and_replacement_lifecycle();
void test_effect_brightness();
void test_emitter_glow_follows_turning_pose();
void test_present_allocates_nothing();
void test_legacy_moving_kite();
void test_camera_and_attachment_frames();
void test_mesh_registry_boundary();
void test_mesh_root_precedence();
void test_proxy_mesh_binding();
void test_no_detach_golden_unchanged();
void test_detach_release_branch();
void test_detach_drain_branch();
void test_stop_emission_drains_whatever_the_flag();
void test_detach_invalid_and_early_handles();
void test_detach_schedule_determinism();
void test_attachment_generation_seed();
void test_attachment_always_visible_matches_unmanaged();
void test_attachment_hide_drains_exactly_once();
void test_attachment_hide_releases_without_leave_particles();
void test_attachment_reappearance_policies();
void test_attachment_hidden_at_start_spawns_on_first_visible();
void test_attachment_moving_host();
void test_attachment_draining_bound();
void test_attachment_determinism();
void test_attachment_merge_stats();
void test_heat_pixel_change_bound();
void test_batch_matches_serial();
void test_attachment_batch_matches_serial();
void test_attachment_batch_requires_stats();
void test_batch_hashes_on_request();
void test_batch_work_counts();
void test_batch_rejects_repeats_and_unknown();
void test_batch_present_allocates_nothing();
} // namespace particle_render_contracts
