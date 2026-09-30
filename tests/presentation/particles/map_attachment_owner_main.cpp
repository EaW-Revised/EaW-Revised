#include "eawr/presentation/animation/animation.hpp"
#include "eawr/presentation/particles/attachment_lifecycle.hpp"
#include "eawr/presentation/particles/map_attachment_owner.hpp"
#include "eawr/presentation/particles/map_effect_plan.hpp"
#include "eawr/presentation/particles/particles.hpp"
#include "eawr/presentation/particles/render.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

// Contracts for the MapMode idle-clip attachment owner (P1 #29). Every model,
// clip, placement and particle system here is wholly synthetic.
namespace particles = eawr::presentation::particles;
namespace playback = eawr::presentation::animation;
namespace assets = eawr::assets;
namespace scene = eawr::scene;
namespace math = eawr::sim::math;

namespace {

int failures{};
void expect(const bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

class RecordingBackend final : public particles::RenderBackend {
public:
    std::uint64_t create_emitter(const particles::EmitterRenderPlan& plan) override {
        const std::uint64_t id = next_++;
        live[id] = plan.emitter_index;
        return id;
    }
    void update_emitter(const std::uint64_t resource, const particles::VertexStream& stream) override {
        if (!live.contains(resource)) { ++invalid; return; }
        record.push_back(particles::stream_hash(stream));
    }
    void destroy_emitter(const std::uint64_t resource) override {
        if (live.erase(resource) == 0) ++invalid;
    }
    std::map<std::uint64_t, std::size_t> live;
    std::vector<std::uint64_t> record;
    std::size_t invalid{};
private:
    std::uint64_t next_{1};
};

particles::CameraFrame test_camera() {
    return particles::camera_frame_from_render({0, 20, 60}, {0, 0, 0}, {0, 1, 0});
}

// Round-to-nearest binary32 to Q24 for these synthetic values. MapMode passes
// scene::fixed_from_binary32; the contracts below compare the owner against
// the plan built with the same converter, so only the path is under test.
eawr::core::Result<math::Fixed> test_fixed(const float value) {
    const double scaled = static_cast<double>(value) * static_cast<double>(math::Fixed::scale);
    if (!std::isfinite(scaled) || std::abs(scaled) >= 9.0e18) {
        eawr::core::Diagnostic diagnostic;
        diagnostic.code = "TEST-FIXED";
        diagnostic.message = "value outside Q24";
        return eawr::core::Result<math::Fixed>::failure(diagnostic);
    }
    return eawr::core::Result<math::Fixed>::success(math::Fixed::from_raw(std::llround(scaled)));
}

math::Fixed fixed(const double value) {
    return math::Fixed::from_raw(static_cast<std::int64_t>(value * static_cast<double>(math::Fixed::scale)));
}

// A 90 degree yaw, uniform scale and translation, exact in Q24.
math::Mat3x4 placement_matrix(const double scale, const double x, const double y, const double z) {
    math::Mat3x4 matrix;
    matrix.rows[0] = {fixed(0), fixed(-scale), fixed(0), fixed(x)};
    matrix.rows[1] = {fixed(scale), fixed(0), fixed(0), fixed(y)};
    matrix.rows[2] = {fixed(0), fixed(0), fixed(scale), fixed(z)};
    return matrix;
}

particles::ScalarTrack constant(const float value) {
    return {particles::Interpolation::linear, {{0, value}, {1, value}}};
}

// Particles are born at the host frame and live 0.5 s; leave_particles drains.
particles::SystemDefinition attached_spray(const bool leave_particles = true) {
    particles::EmitterDefinition emitter;
    emitter.name = "map-owner-spray";
    emitter.spawn_interval = 0.1F; emitter.particles_per_interval = 2; emitter.stop_time = 0.0F;
    emitter.lifetime = 0.5F;
    emitter.red = emitter.green = emitter.blue = emitter.alpha = constant(1);
    emitter.size = constant(1); emitter.uv_index = constant(0); emitter.rotation_rate = constant(0);
    emitter.renderer_id = 22; emitter.blend_mode = 1; emitter.color_texture = "p_synthetic_glow.tga";
    particles::SystemDefinition system;
    system.emitters.push_back(emitter);
    system.leave_particles = leave_particles;
    return system;
}

// Host model: Root, Socket (the admitted proxy's bone) and Latent (a bone
// hidden in bind pose, carrying a second proxy). The clip moves Socket +1
// ALO X per clip frame and scripts both bones' visibility.
struct Host final {
    assets::Model model;
    assets::Animation animation;
    std::optional<playback::Player> player;
    std::optional<playback::Player> bind;
    scene::Placement placement;
};

std::vector<bool> pattern(const std::size_t frames, const std::vector<std::pair<std::size_t, std::size_t>>& hidden) {
    std::vector<bool> visible(frames, true);
    for (const auto& [from, to] : hidden) {
        for (std::size_t frame = from; frame < to && frame < frames; ++frame) visible[frame] = false;
    }
    return visible;
}

Host make_host(const std::vector<bool>& socket_visible, const std::vector<bool>& latent_visible,
               const float fps = 10.0F, const double scale = 1.5, const float socket_step = 1.0F,
               const bool moving = true) {
    Host host;
    host.model.source.logical_path = "data/art/models/eawr_owner_host.alo";
    host.model.source.source_id = "synthetic:map-owner";
    assets::Bone root; root.name = "Root"; root.parent = -1;
    root.relative_transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    assets::Bone socket = root; socket.name = "Socket"; socket.parent = 0;
    assets::Bone latent = root; latent.name = "Latent"; latent.parent = 0; latent.visible = false;
    host.model.bones = {root, socket, latent};
    host.model.proxies = {{"eawr_owner_spark", 1, true, false}, {"eawr_owner_latent", 2, true, false}};
    const std::size_t frames = socket_visible.size();
    host.animation.stored_frame_count = static_cast<std::uint32_t>(frames);
    host.animation.playable_frame_count = host.animation.stored_frame_count - 1U;
    host.animation.frames_per_second = fps;
    host.animation.duration_seconds = static_cast<float>(host.animation.playable_frame_count) / fps;
    assets::AnimationTrack socket_track; socket_track.bone_index = 1; socket_track.bone_name = "Socket";
    assets::AnimationTrack latent_track; latent_track.bone_index = 2; latent_track.bone_name = "Latent";
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const float x = moving ? socket_step * static_cast<float>(frame) : 0.0F;
        socket_track.samples.push_back({{x, 0, 0}, {1, 1, 1}, {0, 0, 0, 1}, socket_visible[frame]});
        latent_track.samples.push_back({{0, 0, 0}, {1, 1, 1}, {0, 0, 0, 1}, latent_visible[frame]});
    }
    host.animation.tracks = {socket_track, latent_track};
    auto player = playback::Player::create(host.model, &host.animation);
    expect(bool(player), "synthetic host clip binds");
    if (player) host.player.emplace(std::move(player.value()));
    auto bind = playback::Player::create(host.model);
    expect(bool(bind), "synthetic host bind pose builds");
    if (bind) host.bind.emplace(std::move(bind.value()));

    host.placement.map_logical_path = "data/art/maps/eawr_owner_synthetic.ted";
    host.placement.scene_ordinal = 3;
    host.placement.record_ordinal = 7;
    host.placement.model_path = host.model.source.logical_path;
    host.placement.idle_animation = "data/art/models/eawr_owner_host_idle_00.ala";
    host.placement.idle_animation_status = "corpus_naming_observed";
    scene::Transform transform;
    transform.matrix = placement_matrix(scale, 160.0, 120.0, 4.0);
    host.placement.transform = transform;
    host.placement.effects = {{"eawr_owner_spark", "data/art/models/eawr_owner_spark.alo", 1, false},
                              {"eawr_owner_latent", "data/art/models/eawr_owner_latent.alo", 2, false}};
    return host;
}

// The bind-pose admission plan MapMode builds, with the same Q24 path.
particles::MapEffectPlan plan_host(const Host& host, const std::size_t capacity, const std::size_t budget) {
    std::vector<std::optional<math::Mat3x4>> frames(host.model.bones.size());
    const auto pose = host.bind->sample({});
    expect(bool(pose), "bind pose samples");
    for (std::size_t bone = 0; bone < frames.size(); ++bone) {
        frames[bone] = particles::fixed_model_frame(pose.value().bones[bone].model_asset, test_fixed);
    }
    const std::vector<particles::EffectEvidence> effects{
        {particles::EffectKind::particle, capacity}, {particles::EffectKind::particle, capacity}};
    const particles::MapEffectPlacementInput input{&host.model, &host.placement, frames, effects,
        particles::VisibilityEvidence::bind_pose, std::nullopt, std::nullopt, {}};
    return particles::plan_map_effects(std::span(&input, 1), 20260922U, budget);
}

struct Spawned final {
    std::vector<std::uint32_t> seeds;
};
particles::AttachmentLifecycle::Spawn spawner(const particles::SystemDefinition& system, const std::size_t capacity,
                                              Spawned* log = nullptr) {
    return [system, capacity, log](particles::EffectRegistry& registry, const std::uint32_t seed) {
        if (log) log->seeds.push_back(seed);
        return registry.spawn(system, seed, capacity);
    };
}

bool same_bits(const particles::EmitterFrame& left, const particles::EmitterFrame& right) {
    const auto bits = [](const particles::EmitterFrame& frame) {
        std::array<std::uint32_t, 12> out{};
        out[0] = std::bit_cast<std::uint32_t>(frame.origin.x);
        out[1] = std::bit_cast<std::uint32_t>(frame.origin.y);
        out[2] = std::bit_cast<std::uint32_t>(frame.origin.z);
        std::size_t index = 3;
        for (const particles::Vec3& axis : {frame.basis.x, frame.basis.y, frame.basis.z}) {
            out[index++] = std::bit_cast<std::uint32_t>(axis.x);
            out[index++] = std::bit_cast<std::uint32_t>(axis.y);
            out[index++] = std::bit_cast<std::uint32_t>(axis.z);
        }
        return out;
    };
    return bits(left) == bits(right);
}

struct OwnerRun final {
    std::vector<particles::MapOwnerEvent> events;
    std::vector<std::uint64_t> hashes;
    std::vector<std::size_t> live;
    std::vector<particles::EmitterFrame> frames;
    std::uint32_t generations{}, detaches{}, drains_released{}, drains_cut_short{};
    std::size_t peak_live{}, released_at_end{}, resources_after{}, backend_live_after{};
    std::vector<std::uint32_t> seeds;
    bool failed{};
    bool capacity_held{true};
};

OwnerRun drive(const Host& host, const particles::MapEffectRecord& record, const int samples,
               const particles::SystemDefinition& system, const std::size_t headroom) {
    OwnerRun run;
    RecordingBackend backend;
    particles::EffectRegistry registry(backend);
    Spawned log;
    particles::MapAttachmentOwner owner(registry, spawner(system, record.capacity, &log), record.seed,
        record.capacity, host.placement.transform->matrix, host.model.proxies[record.proxy_ordinal].bone, test_fixed);
    for (int index = 0; index < samples; ++index) {
        const auto sample = static_cast<std::uint32_t>(index);
        const auto pose = particles::map_owner_sample(*host.player, sample);
        expect(bool(pose), "clip samples");
        auto step = owner.step(sample, pose.value(), test_camera());
        if (!step) { run.failed = true; break; }
        run.hashes.push_back(step.value().stats.hash);
        run.live.push_back(step.value().live_instances);
        run.frames.push_back(*owner.last_frame());
        run.capacity_held = run.capacity_held && owner.live_capacity() <= record.capacity + headroom;
    }
    const auto& life = owner.lifecycle();
    run.events.assign(owner.events().begin(), owner.events().end());
    run.generations = life.generations(); run.detaches = life.detaches();
    run.drains_released = life.drains_released(); run.drains_cut_short = life.drains_cut_short();
    run.peak_live = owner.peak_live_instances();
    const auto released = owner.release_all();
    expect(bool(released), "owner release succeeds");
    run.released_at_end = released ? released.value() : 0;
    expect(bool(owner.release_all()) && owner.released(), "a second release is a no-op");
    run.resources_after = registry.live_backend_resources();
    run.backend_live_after = backend.live.size();
    run.seeds = log.seeds;
    return run;
}

// Visible on clip frames 0-5 and 14-23 of 31 at 10 fps. At 1/30 s the socket
// is hidden on samples 18-41 and 72-89.
std::vector<bool> reference_visibility() { return pattern(31, {{6, 14}, {24, 31}}); }

// T1: hide at 18, respawn at 42, one drain released, nothing left.
void test_owner_transition() {
    const Host host = make_host(reference_visibility(), std::vector<bool>(31, false));
    if (!host.player || !host.bind) return;
    const auto plan = plan_host(host, 64, 256);
    expect(plan.records.size() == 2, "plan has both proxies");
    const auto& record = plan.records[0];
    expect(record.status == particles::MapEffectStatus::admitted && record.emitter_frame.has_value(),
           "the visible proxy is admitted in bind pose");
    if (!record.emitter_frame) return;
    const std::size_t headroom = particles::map_owner_headroom(std::array{record.capacity},
        particles::map_owner_max_draining).value_or(0);
    const OwnerRun run = drive(host, record, 60, attached_spray(), headroom);
    expect(!run.failed, "sixty owner samples succeed");
    std::vector<std::uint32_t> spawns, detaches, drains;
    for (const auto& event : run.events) {
        if (event.spawned_generation) spawns.push_back(event.sample);
        if (event.detached) {
            detaches.push_back(event.sample);
            expect(*event.detached == particles::EffectDetachState::draining, "leave_particles drains on hide");
        }
        for (std::size_t count = 0; count < event.drains_released; ++count) drains.push_back(event.sample);
    }
    expect(spawns == std::vector<std::uint32_t>{0, 42}, "generations start at samples 0 and 42");
    expect(detaches == std::vector<std::uint32_t>{18}, "one detach at sample 18");
    expect(drains.size() == 1 && drains[0] > 18 && run.drains_released == 1, "the drain is released exactly once");
    expect(run.generations == 2 && run.detaches == 1 && run.drains_cut_short == 0, "counters match the events");
    expect(run.seeds == std::vector<std::uint32_t>{record.seed, particles::generation_seed(record.seed, 1)},
           "generation 1 takes generation_seed(record.seed, 1)");
    expect(run.peak_live <= 2 && *std::max_element(run.live.begin(), run.live.end()) <= 2,
           "at most the active generation and one drain are live");
    expect(run.live[17] == 1 && run.live[18] == 1, "the hidden sample keeps only the drain");
    expect(std::all_of(run.live.begin() + 19, run.live.begin() + 42,
                       [](const std::size_t live) { return live <= 1; }), "nothing spawns while hidden");
    expect(same_bits(run.frames[0], particles::source_emitter_frame(*record.emitter_frame)),
           "sample 0 equals the bind-pose admission frame bit for bit");
    expect(!same_bits(run.frames[42], run.frames[0]), "the reappeared generation follows the moved bone");
    expect(run.capacity_held, "live capacity stays within allocation plus headroom");
    expect(run.released_at_end == 1 && run.resources_after == 0 && run.backend_live_after == 0,
           "teardown releases the active generation and leaves 0 resources");

    const OwnerRun repeat = drive(host, record, 60, attached_spray(), headroom);
    expect(repeat.hashes == run.hashes && repeat.live == run.live && repeat.events.size() == run.events.size(),
           "a fixed seed and clip repeat every sample");
}

// Without leave_particles the hide releases at once and nothing drains.
void test_owner_release_without_drain() {
    const Host host = make_host(reference_visibility(), std::vector<bool>(31, false));
    if (!host.player) return;
    const auto plan = plan_host(host, 64, 256);
    const OwnerRun run = drive(host, plan.records[0], 60, attached_spray(false), 64);
    expect(!run.failed && run.generations == 2 && run.detaches == 1 && run.drains_released == 0,
           "an immediate release leaves no drain");
    expect(run.live[18] == 0 && run.live[41] == 0 && run.live[42] == 1, "hidden samples hold nothing");
    expect(run.resources_after == 0, "released cleanly");
}

// T2 case A: an always-visible, stationary clip reproduces the static path.
void test_owner_always_visible_matches_static() {
    const Host host = make_host(std::vector<bool>(31, true), std::vector<bool>(31, false), 10.0F, 1.5, 0.0F, false);
    if (!host.player) return;
    const auto plan = plan_host(host, 64, 256);
    const auto& record = plan.records[0];
    if (!record.emitter_frame) { expect(false, "stationary host admits"); return; }

    RecordingBackend owned_backend;
    particles::EffectRegistry owned_registry(owned_backend);
    particles::MapAttachmentOwner owner(owned_registry, spawner(attached_spray(), record.capacity), record.seed,
        record.capacity, host.placement.transform->matrix, 1, test_fixed);
    std::vector<std::uint64_t> owned;
    bool single = true;
    for (std::uint32_t sample = 0; sample < 60; ++sample) {
        const auto pose = particles::map_owner_sample(*host.player, sample);
        const auto step = owner.step(sample, pose.value(), test_camera());
        expect(bool(step), "always-visible owner step succeeds");
        if (!step) return;
        owned.push_back(step.value().stats.hash);
        single = single && step.value().live_instances == 1 && !step.value().detached;
    }
    expect(single && owner.lifecycle().generations() == 1 && owner.lifecycle().detaches() == 0
           && owner.events().size() == 1, "an always-visible owner keeps one generation and one event");

    // The static MapMode path: spawn and set_frame once, then advance with 0
    // and 1/30 s.
    RecordingBackend static_backend;
    particles::EffectRegistry static_registry(static_backend);
    const auto handle = static_registry.spawn(attached_spray(), record.seed, record.capacity);
    expect(bool(handle) && bool(static_registry.set_frame(handle.value(),
        particles::source_emitter_frame(*record.emitter_frame))), "static record spawns");
    std::vector<std::uint64_t> unmanaged;
    for (std::uint32_t sample = 0; sample < 60; ++sample) {
        unmanaged.push_back(static_registry.advance(handle.value(), particles::map_owner_delta(sample),
            test_camera()).value().hash);
    }
    expect(owned == unmanaged && owned_backend.record == static_backend.record,
           "an always-visible owner reproduces the static stream and uploads exactly");
    expect(bool(owner.release_all()) && owned_registry.live_backend_resources() == 0, "owned run releases");
    expect(bool(static_registry.release(handle.value())) && static_registry.live_backend_resources() == 0,
           "static run releases");
}

// T2 case B: a bind pose is not a visibility source. A Player without a clip
// samples the same pose at every time, so an owner fed from it sees no edge.
void test_bind_pose_has_no_edges() {
    const Host host = make_host(reference_visibility(), std::vector<bool>(31, false));
    if (!host.bind) return;
    RecordingBackend backend;
    particles::EffectRegistry registry(backend);
    particles::MapAttachmentOwner owner(registry, spawner(attached_spray(), 64), 99, 64,
        host.placement.transform->matrix, 1, test_fixed);
    const auto first = host.bind->sample({});
    bool identical = true;
    for (std::uint32_t sample = 0; sample < 90; ++sample) {
        const auto pose = particles::map_owner_sample(*host.bind, sample);
        identical = identical && pose.value().bones[1].model_asset == first.value().bones[1].model_asset
            && pose.value().bones[1].visible == first.value().bones[1].visible;
        expect(bool(owner.step(sample, pose.value(), test_camera())), "bind-pose step succeeds");
    }
    expect(identical, "a clipless Player samples the bind pose at every time");
    expect(owner.events().size() == 1 && owner.lifecycle().detaches() == 0 && owner.lifecycle().generations() == 1,
           "the bind pose yields zero visibility edges");
    expect(bool(owner.release_all()) && registry.live_backend_resources() == 0, "bind-pose owner releases");
}

// T3: headroom arithmetic, the exact budget edge, overflow of the sum.
void test_headroom_and_capacity_edge() {
    constexpr std::size_t capacity = 64;
    const std::array<std::size_t, 2> owned{capacity, capacity};
    const auto headroom = particles::map_owner_headroom(owned, particles::map_owner_max_draining);
    expect(headroom && *headroom == 2 * capacity, "one drain generation of headroom per owner");
    expect(particles::map_owner_headroom(std::array<std::size_t, 0>{}, 1) == std::optional<std::size_t>{0},
           "no owner reserves nothing");
    const std::array<std::size_t, 2> huge{std::numeric_limits<std::size_t>::max() - 1, 2};
    expect(!particles::map_owner_headroom(huge, 1), "a headroom sum that overflows is refused");
    expect(!particles::map_owner_headroom(std::array{std::numeric_limits<std::size_t>::max()}, 2),
           "a headroom product that overflows is refused");

    // Two admitted owners of capacity C: 2C allocated plus 2C headroom.
    Host host = make_host(reference_visibility(), reference_visibility());
    if (!host.bind) return;
    host.model.bones[2].visible = true;  // both proxies admitted for this case
    const auto exact = plan_host(host, capacity, 4 * capacity);
    expect(exact.records[0].status == particles::MapEffectStatus::admitted
           && exact.records[1].status == particles::MapEffectStatus::admitted
           && exact.allocated_capacity == 2 * capacity, "both proxies are admitted");
    expect(particles::map_owner_capacity_fits(exact.allocated_capacity, *headroom, 4 * capacity),
           "allocation plus headroom fits a budget of exactly 4C");
    const auto short_plan = plan_host(host, capacity, 4 * capacity - 1);
    expect(short_plan.allocated_capacity == 2 * capacity, "admission alone is unchanged at 4C-1");
    expect(!particles::map_owner_capacity_fits(short_plan.allocated_capacity, *headroom, 4 * capacity - 1),
           "a budget of 4C-1 fails closed before any owner exists");
    expect(!particles::map_owner_capacity_fits(10, 0, 9), "an allocation above budget never fits");
}

// T3: a bone toggling every clip frame (0.1 s) keeps live capacity bounded
// and cuts drains short.
void test_rapid_toggle_bound() {
    std::vector<bool> toggling(31);
    for (std::size_t frame = 0; frame < toggling.size(); ++frame) toggling[frame] = frame % 2 == 0;
    const Host host = make_host(toggling, std::vector<bool>(31, false));
    if (!host.player) return;
    const auto plan = plan_host(host, 64, 256);
    const auto& record = plan.records[0];
    const OwnerRun run = drive(host, record, 90, attached_spray(), record.capacity);
    expect(!run.failed, "rapid toggling steps succeed");
    expect(run.capacity_held && run.peak_live <= 2, "live capacity never exceeds allocation plus headroom");
    expect(run.drains_cut_short > 0 && run.generations > 10, "rapid toggling respawns and cuts drains short");
    expect(run.resources_after == 0 && run.backend_live_after == 0, "rapid toggling leaks nothing");
    const OwnerRun repeat = drive(host, record, 90, attached_spray(), record.capacity);
    expect(repeat.hashes == run.hashes && repeat.live == run.live && repeat.seeds == run.seeds,
           "rapid toggling is deterministic");
}

// T3: a composition that overflows Q24 on a later clip frame fails that
// sample and still releases cleanly.
void test_frame_overflow_fails_closed() {
    // Placement scale 2^16 with the socket moving 2^20 per clip frame: sample 0
    // composes at the origin, and the product passes the Q24 range (2^39)
    // before clip frame 8.
    const Host host = make_host(std::vector<bool>(31, true), std::vector<bool>(31, false), 10.0F,
                                65536.0, 1048576.0F);
    if (!host.player) return;
    RecordingBackend backend;
    particles::EffectRegistry registry(backend);
    particles::MapAttachmentOwner owner(registry, spawner(attached_spray(), 64), 5, 64,
        host.placement.transform->matrix, 1, test_fixed);
    bool failed = false;
    std::uint32_t failed_at = 0;
    for (std::uint32_t sample = 0; sample < 30 && !failed; ++sample) {
        const auto pose = particles::map_owner_sample(*host.player, sample);
        const auto step = owner.step(sample, pose.value(), test_camera());
        if (!step) {
            failed = true;
            failed_at = sample;
            expect(step.error().code == particles::diagnostic_codes::map_owner, "overflow names the owner code");
        }
    }
    expect(failed && failed_at > 0, "a later sample fails on Q24 overflow after sample 0 succeeded");
    expect(bool(owner.release_all()) && registry.live_backend_resources() == 0 && backend.live.empty(),
           "the failed owner releases to 0 resources");
    const auto pose = host.player->sample({});
    expect(!owner.step(0, pose.value(), test_camera()), "a released owner refuses further samples");

    particles::MapAttachmentOwner missing(registry, spawner(attached_spray(), 64), 5, 64,
        host.placement.transform->matrix, 9, test_fixed);
    expect(!missing.step(0, pose.value(), test_camera()) && registry.live_backend_resources() == 0,
           "an absent proxy bone fails before any spawn");
}

// Bind-hidden proxies stay unadmitted; the watch only counts their edges.
void test_hidden_proxy_watch() {
    const Host host = make_host(reference_visibility(), pattern(31, {{0, 3}, {5, 9}}));
    if (!host.player) return;
    const auto plan = plan_host(host, 64, 256);
    expect(plan.records[1].status == particles::MapEffectStatus::hidden
           && plan.records[1].cause == particles::MapEffectCause::hidden_bone
           && plan.records[1].capacity == 0, "the bind-hidden bone stays hidden and unallocated");
    particles::MapHiddenProxyWatch watch(2);
    for (std::uint32_t sample = 0; sample < 60; ++sample) {
        const auto pose = particles::map_owner_sample(*host.player, sample);
        expect(watch.observe(pose.value()), "watch observes");
    }
    // Clip frames 3-4 and 9-30 are visible: two hidden-to-visible edges.
    expect(watch.visible_edges() == 2, "the watch counts each hidden-to-visible edge");
    particles::MapHiddenProxyWatch absent(9);
    expect(!absent.observe(host.player->sample({}).value()), "an absent bone is not observed");
}

// L1: the owner clock samples n/30 s exactly across loop wraps. A clip of 7
// playable frames at 15 fps lasts 7/15 s, which binary32 cannot represent, so
// fmod-based sampling can land one frame early at a wrap. Sample n must read
// frame floor(n/2) mod 7 with fraction 0 or 1/2 on every sample.
void test_exact_clock_crosses_wraps() {
    std::vector<bool> visible(8);
    for (std::size_t frame = 0; frame < visible.size(); ++frame) visible[frame] = frame % 2 == 0;
    const Host host = make_host(visible, std::vector<bool>(8, false), 15.0F);
    if (!host.player) return;
    std::size_t wrong{};
    for (std::uint32_t sample = 0; sample < 600; ++sample) {
        const auto pose = particles::map_owner_sample(*host.player, sample);
        if (!pose) { ++wrong; continue; }
        const std::uint32_t position = sample * 15U % (7U * 30U);
        const std::uint32_t frame = position / 30U;
        const float x = static_cast<float>(frame) + static_cast<float>(position % 30U) / 30.0F;
        if (pose.value().bones[1].visible != visible[frame]
            || std::abs(pose.value().bones[1].local_asset[12] - x) > 1.0e-6F) {
            ++wrong;
        }
    }
    expect(wrong == 0, "every sample of a 7-frame 15 fps loop reads its exact frame across wraps");

    // The owner sees exactly that visibility. With even frames visible, a
    // sample read one frame early (as the binary32 path does at samples 30,
    // 60, 120, 240, 270, 480, 510 and 540) flips its visibility and moves an
    // edge, so the whole event stream must match the exact frame sequence.
    const Host owner_host = make_host(visible, std::vector<bool>(8, false), 15.0F);
    if (!owner_host.player || !owner_host.bind) return;
    const auto plan = plan_host(owner_host, 64, 256);
    const auto& record = plan.records[0];
    if (!record.emitter_frame) { expect(false, "wrapping host admits"); return; }
    const OwnerRun run = drive(owner_host, record, 600, attached_spray(), 64);
    std::vector<std::uint32_t> spawns, detaches, expected_spawns, expected_detaches;
    for (const auto& event : run.events) {
        if (event.spawned_generation) spawns.push_back(event.sample);
        if (event.detached) detaches.push_back(event.sample);
    }
    const auto shown = [&visible](const std::uint32_t sample) { return visible[sample * 15U % 210U / 30U]; };
    for (std::uint32_t sample = 0; sample < 600; ++sample) {
        const bool before = sample != 0 && shown(sample - 1);
        if (shown(sample) && !before) expected_spawns.push_back(sample);
        if (!shown(sample) && before) expected_detaches.push_back(sample);
    }
    expect(expected_detaches.size() > 100, "the alternating clip hides more than 100 times in 600 samples");
    expect(!run.failed && spawns == expected_spawns && detaches == expected_detaches,
           "every owner spawn and detach follows the exact frame across 42 loop wraps");
    expect(run.capacity_held && run.resources_after == 0, "the wrapping owner stays bounded and releases");
}

// sample_tick agrees with sample() wherever the binary32 time is exact
// enough, rejects a non-integral rate, and yields the bind pose without a clip.
void test_tick_sampling_contract() {
    const Host host = make_host(reference_visibility(), std::vector<bool>(31, false));
    if (!host.player || !host.bind) return;
    bool same = true;
    for (std::uint32_t sample = 0; sample < 90; ++sample) {
        const auto exact = particles::map_owner_sample(*host.player, sample);
        const auto timed = host.player->sample({particles::map_owner_sample_time(sample),
            playback::PlaybackMode::loop, 0.0F});
        same = same && exact && timed && exact.value().bones[1].visible == timed.value().bones[1].visible;
    }
    expect(same, "the 10 fps reference clip keeps its visibility on the exact clock");
    const std::uint64_t far = (std::uint64_t{1} << 40) + 7U;
    const auto late = host.player->sample_tick(far, 30);
    // 30 playable frames at 10 fps loop every 90 ticks of 1/30 s.
    const auto near = particles::map_owner_sample(*host.player, static_cast<std::uint32_t>(far % 90U));
    expect(late && near && late.value().bones[1].local_asset == near.value().bones[1].local_asset
           && late.value().bones[1].visible == near.value().bones[1].visible,
           "a 64-bit tick reduces to its loop position exactly");
    expect(!host.player->sample_tick(0, 0), "a zero tick rate is refused");
    const Host odd = make_host(std::vector<bool>(8, true), std::vector<bool>(8, false), 12.5F);
    if (odd.player) expect(!particles::map_owner_sample(*odd.player, 3), "a non-integral frame rate is refused");
    const auto bind = particles::map_owner_sample(*host.bind, 17);
    const auto reference = host.bind->sample({});
    expect(bind && reference && bind.value().bones[1].model_asset == reference.value().bones[1].model_asset
           && bind.value().bones[2].visible == reference.value().bones[2].visible,
           "a clipless player samples the bind pose on the exact clock");
}

void test_clock() {
    expect(particles::map_owner_delta(0) == 0.0F && particles::map_owner_delta(1) == 1.0F / 30.0F
           && particles::map_owner_delta(59) == 1.0F / 30.0F, "the clock's first advance is zero-delta");
    expect(particles::map_owner_sample_time(42) == 42.0F * (1.0F / 30.0F), "sample time is n steps of 1/30 s");
    // A capture's tick is its frame count: one sample per frame, sample 0 on frame 0.
    expect(particles::map_owner_samples_due(0, 0) == 1 && particles::map_owner_samples_due(1, 1) == 1
           && particles::map_owner_samples_due(59, 59) == 1, "a capture advances one sample per frame");
    // The live view's tick is real time: a 144 Hz frame (0.007 s) is often
    // not due a sample, and a 10 fps frame is due three (#186).
    expect(particles::map_owner_samples_due(5, 4) == 0 && particles::map_owner_samples_due(5, 5) == 1
           && particles::map_owner_samples_due(10, 12) == 3, "the live view advances to its real-time tick");
    expect(particles::map_owner_samples_due(0, 3) == 4, "a clock started late takes sample 0 and catches up");
    // A stalled frame catches up at most map_owner_catch_up_samples at a time.
    expect(particles::map_owner_samples_due(0, 299) == particles::map_owner_catch_up_samples
           && particles::map_owner_samples_due(270, 299) == 30 && particles::map_owner_samples_due(271, 299) == 29,
           "catch-up is bounded per frame");
    expect(particles::map_owner_samples_due(0, std::numeric_limits<std::uint32_t>::max())
               == particles::map_owner_catch_up_samples
           && particles::map_owner_samples_due(std::numeric_limits<std::uint32_t>::max(),
                  std::numeric_limits<std::uint32_t>::max()) == 1,
           "the owed count does not overflow at the end of the tick range");
}

} // namespace

int main() {
    test_clock();
    test_owner_transition();
    test_owner_release_without_drain();
    test_owner_always_visible_matches_static();
    test_bind_pose_has_no_edges();
    test_headroom_and_capacity_edge();
    test_rapid_toggle_bound();
    test_frame_overflow_fails_closed();
    test_hidden_proxy_watch();
    test_exact_clock_crosses_wraps();
    test_tick_sampling_contract();
    if (failures != 0) { std::cerr << failures << " map attachment owner contract(s) failed\n"; return 1; }
    std::cout << "map attachment owner contracts passed\n";
    return 0;
}
