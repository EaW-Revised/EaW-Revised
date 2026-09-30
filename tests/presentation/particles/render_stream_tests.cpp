#include "render_test_support.hpp"

#include <atomic>
#include <cstdlib>
#include <new>

// #439: counts every global allocation of this test binary, so a test can prove a hot path
// allocates nothing.
namespace {
std::atomic<std::size_t> global_allocations{0};
} // namespace

// GCC flags free() in a replaced operator delete as mismatched with new, though the replaced new
// allocates with malloc.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void* operator new(const std::size_t size) {
    global_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* const memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc();
}
void operator delete(void* const memory) noexcept { std::free(memory); }
void operator delete(void* const memory, std::size_t) noexcept { std::free(memory); }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace particle_render_contracts {
void test_parser_retains_renderer_fields() {
    const auto spec = [](std::string name, const std::uint32_t blend) {
        LegacyEmitter value; value.name = std::move(name); value.blend = blend; return value;
    };
    LegacyEmitter alpha = spec("alpha", 2); alpha.no_depth = true; alpha.normal = "p_synthetic_depth.tga";
    LegacyEmitter kite = spec("kite", 1); kite.tail = true; kite.tail_size = 7.5F;
    LegacyEmitter heat = spec("heat", 10); heat.heat = true; heat.world = true;
    LegacyEmitter parent = spec("parent", 1); parent.spawn_on_death = 3;
    LegacyEmitter child=spec("child",1);child.parent_link_strength=0.5F;
    const Bytes bytes = legacy_system({alpha, kite, heat, child, spec("wrapped", 30), parent});
    const auto loaded = particles::load_alo(bytes, "synthetic-render.alo");
    expect(bool(loaded), "render fixture parses");
    if (!loaded) return;
    const auto& emitters = loaded.value().emitters;
    expect(emitters.size() == 6, "six emitters parsed");
    expect(emitters[0].color_texture == "p_synthetic_glow.tga" && emitters[0].normal_texture == "p_synthetic_depth.tga", "colour and depth textures retained");
    expect(emitters[0].blend_mode == 2 && emitters[0].disable_depth_test && emitters[0].renderer_id == 22, "alpha billboard fields retained");
    expect(emitters[1].renderer_id == 52 && close(emitters[1].tail_size, 7.5F), "kite tail retained");
    expect(emitters[2].renderer_id == 38 && emitters[2].world_oriented && emitters[2].blend_mode == 10, "heat renderer fields retained");
    expect(emitters[4].blend_mode == 30, "out-of-table selector is kept verbatim, never wrapped");
    expect(emitters[3].parent_emitter == 5 && emitters[3].spawn_on_parent_death && emitters[3].cpu_ready, "parent death creator is CPU ready");
    expect(emitters[3].creator_id==40&&emitters[3].bursting&&
        close(emitters[3].inherited_velocity_scale,0.5F)&&!emitters[3].inherit_parent_velocity,
        "V1 death creator forces velocity inheritance off despite link strength");
    const auto plans = particles::plan_system(loaded.value());
    expect(plans[0].drawable && plans[0].blend == particles::Blend::alpha && !plans[0].depth_test && !plans[0].depth_write, "alpha selector follows PrimAlpha state and noDepthTest");
    expect(plans[1].drawable && plans[1].family == particles::RenderFamily::kites && plans[1].blend == particles::Blend::additive, "additive kite plan");
    expect(plans[2].drawable && plans[2].family == particles::RenderFamily::heat_saturation && plans[2].phase == particles::DrawPhase::heat, "heat plan draws in the heat phase");
    expect(plans[3].drawable, "parent death child receives a drawable plan");
    expect(!plans[4].drawable && plans[4].cause.find("outside") != std::string::npos, "out-of-table selector fails closed");
}

void test_death_burst_ignores_parent_velocity(){
    LegacyEmitter child;child.name="death-child";child.parent_link_strength=0.5F;
    LegacyEmitter parent;parent.name="moving-parent";parent.velocity_x=4.0F;parent.spawn_on_death=0;
    const auto loaded=particles::load_alo(legacy_system({child,parent}),"death-velocity.alo");
    expect(bool(loaded),"V1 death velocity fixture parses");
    if(!loaded)return;
    particles::CpuSystem cpu(loaded.value(),17,128);
    static_cast<void>(cpu.advance(0));
    const auto death=cpu.advance(2.0F);
    const auto found=std::find_if(cpu.particles().begin(),cpu.particles().end(),
        [](const particles::Particle& particle){return particle.emitter_index==0;});
    expect(death.death_bursts>0&&found!=cpu.particles().end(),"V1 parent death emits child particles");
    if(found!=cpu.particles().end())
        expect(close(found->velocity.x,0.0F),"V1 death child ignores nonzero parent velocity with 0x28 and 0x43 set");
}

void test_parent_link_rejection(){
    LegacyEmitter first;first.name="first";first.spawn_during_life=1;
    LegacyEmitter second;second.name="second";second.spawn_on_death=0;
    const auto cycle=particles::load_alo(legacy_system({first,second}),"cycle.alo");
    expect(!cycle&&cycle.error().code==particles::diagnostic_codes::structure,
        "V1 parent cycle is rejected at parse time");
    first.spawn_during_life=1;second.spawn_on_death=1;
    const auto duplicate=particles::load_alo(legacy_system({first,second}),"duplicate.alo");
    expect(!duplicate&&duplicate.error().code==particles::diagnostic_codes::structure,
        "V1 child cannot have two parent links");
    first.mesh_mode=3;second.mesh_mode=3;second.spawn_on_death=0;
    const auto mesh_cycle=particles::load_alo(legacy_system({first,second}),"mesh-cycle.alo");
    expect(!mesh_cycle&&mesh_cycle.error().code==particles::diagnostic_codes::structure,
        "mesh root precedence retains parent cycle rejection");
}

void test_plan_policy() {
    auto emitter = drawable_emitter();
    auto plan = particles::plan_emitter(emitter, 0);
    expect(plan.drawable && plan.family == particles::RenderFamily::billboard && plan.blend == particles::Blend::additive, "additive billboard");
    expect(plan.program == "Engine/PrimAdditive.fx" && plan.technique == "t1" && !plan.depth_write && plan.phase == particles::DrawPhase::transparent, "additive selector and depth-write policy");
    expect(!plan.sort_particles, "converted V1 renderers do not sort particles");
    emitter.blend_mode = 0; plan = particles::plan_emitter(emitter, 0);
    expect(plan.drawable && plan.blend == particles::Blend::opaque && plan.depth_write && plan.phase == particles::DrawPhase::opaque, "opaque selector writes depth");
    emitter.blend_mode = 3; plan = particles::plan_emitter(emitter, 0);
    expect(plan.drawable && plan.blend == particles::Blend::modulate, "modulate selector");
    emitter.blend_mode = 11; emitter.normal_texture = "bump.tga"; plan = particles::plan_emitter(emitter, 0);
    expect(plan.drawable && plan.blend == particles::Blend::bump_alpha && plan.normal_texture == "bump.tga"
        && plan.phase == particles::DrawPhase::transparent && !plan.depth_write, "bump alpha selector keeps its normal map");
    emitter.normal_texture.clear();
    for (const std::uint32_t selector : {4U, 5U, 6U, 7U, 8U, 9U, 12U, 13U}) {
        emitter.blend_mode = selector; plan = particles::plan_emitter(emitter, 0);
        expect(!plan.drawable && !plan.cause.empty(), "unsupported selector fails closed with a cause");
    }
    emitter.blend_mode = 10; plan = particles::plan_emitter(emitter, 0);
    expect(!plan.drawable, "PrimHeat outside the heat renderer fails closed");
    for (const std::uint32_t selector : {1U, 2U}) {
        emitter.blend_mode = selector; emitter.renderer_id = 38; plan = particles::plan_emitter(emitter, 0);
        expect(plan.drawable && plan.blend == particles::Blend::heat_distortion && plan.phase == particles::DrawPhase::heat
            && plan.program == "Engine/PrimHeat.fx" && plan.blend_selector == selector,
            "corpus heat selectors draw as screen distortion and keep the authored selector");
    }
    emitter.blend_mode = 3; plan = particles::plan_emitter(emitter, 0);
    expect(!plan.drawable && plan.cause.find("selector 3") != std::string::npos,
        "heat renderer with another selector fails closed");
    emitter.renderer_id = 24; plan = particles::plan_emitter(emitter, 0);
    expect(!plan.drawable && plan.cause.find("Chain") != std::string::npos, "renderer family without an adapter is named");
    emitter.renderer_id = 22; emitter.color_texture.clear(); plan = particles::plan_emitter(emitter, 0);
    expect(!plan.drawable, "an emitter with no texture is not drawn with a placeholder");
    expect(particles::legacy_blend_selectors().size() == 14, "fourteen legacy selectors are tabulated");
}

void test_quad_geometry() {
    particles::Particle particle; particle.position = {1, 2, 3}; particle.size = 2; particle.rotation = 0;
    particle.texcoords = {0.25F, 0.5F, 0.25F, 0.25F}; particle.color = {1, 0.5F, 0.25F, 0.75F};
    const particles::Particle list[] = {particle};
    const auto camera = test_camera();
    particles::VertexStream stream;
    auto plan = particles::plan_emitter(drawable_emitter(), 0);
    particles::build_stream(plan, list, camera, stream);
    expect(stream.quads == 1 && stream.vertices.size() == 4 && stream.indices.size() == 6, "one quad per particle");
    expect(stream.indices[0] == 0 && stream.indices[1] == 1 && stream.indices[2] == 2 && stream.indices[3] == 2 && stream.indices[4] == 1 && stream.indices[5] == 3, "alo-viewer index order");
    const particles::Vec3 forward = particles::Vec3{camera.right.y * camera.up.z - camera.right.z * camera.up.y,
        camera.right.z * camera.up.x - camera.right.x * camera.up.z, camera.right.x * camera.up.y - camera.right.y * camera.up.x};
    bool facing = true;
    for (const auto& vertex : stream.vertices) facing = facing && close(dot(minus(vertex.position, particle.position), forward), 0.0F, 0.001F);
    expect(facing, "billboard lies in the camera plane");
    expect(close(dot(minus(stream.vertices[0].position, particle.position), camera.right), -2) && close(dot(minus(stream.vertices[0].position, particle.position), camera.up), 2), "first corner is (-s,+s)");
    expect(close(stream.vertices[1].v, 0.75F) && close(stream.vertices[2].u, 0.5F) && close(stream.vertices[0].u, 0.25F), "texture cell mapping");
    expect(close(stream.vertices[3].color.w, 0.75F), "vertex colour carries alpha");

    auto emitter = drawable_emitter(); emitter.renderer_id = 28; plan = particles::plan_emitter(emitter, 0);
    stream.clear(); particles::build_stream(plan, list, camera, stream);
    bool flat = true; for (const auto& vertex : stream.vertices) flat = flat && close(vertex.position.z, 3);
    expect(flat, "XY-aligned quad stays in the world XY plane");

    emitter.renderer_id = 52; emitter.tail_size = 6; plan = particles::plan_emitter(emitter, 0);
    particles::Particle moving = particle; moving.velocity = {0, 0, 5};
    const particles::Particle kites[] = {moving};
    stream.clear(); particles::build_stream(plan, kites, camera, stream);
    const float tail = dot(minus(stream.vertices[0].position, moving.position), {0, 0, 1});
    expect(stream.quads == 1 && tail < -3.0F, "kite tail trails the velocity");

    particles::Particle other = particle; other.emitter_index = 1;
    const particles::Particle mixed[] = {particle, other};
    stream.clear(); plan = particles::plan_emitter(drawable_emitter(), 1);
    particles::build_stream(plan, mixed, camera, stream);
    expect(stream.quads == 1, "a stream only carries its own emitter's particles");
}

void test_finite_rotation_boundary() {
    particles::Particle particle;
    particle.position = {0, 0, 0};
    particle.size = 1;
    particle.rotation = std::numeric_limits<float>::max();
    particle.color = {1, 1, 1, 1};
    particle.texcoords = {0, 0, 1, 1};
    particles::VertexStream stream;
    particles::build_stream(particles::plan_emitter(drawable_emitter(), 0),
                            std::span{&particle, 1}, test_camera(), stream);
    expect(stream.vertices.empty() && stream.indices.empty() && stream.quads == 0,
           "finite rotation that overflows angle skips its whole quad");
}

void test_stream_validation() {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const float max = std::numeric_limits<float>::max();
    particles::Particle valid;
    valid.position = {0, 0, 0}; valid.size = 1; valid.rotation = 0;
    valid.color = {1, 1, 1, 1}; valid.texcoords = {0, 0, 1, 1};
    auto billboard = particles::plan_emitter(drawable_emitter(), 0);
    auto camera = test_camera();
    const auto skips = [&](const particles::EmitterRenderPlan& plan,
                           const particles::Particle& particle,
                           const particles::CameraFrame& frame) {
        particles::VertexStream stream;
        particles::build_stream(plan, std::span{&particle, 1}, frame, stream);
        return stream.vertices.empty() && stream.indices.empty() && stream.quads == 0 &&
               finite_stream(stream);
    };
    auto invalid = valid;
    invalid.rotation = nan;
    expect(skips(billboard, invalid, camera), "NaN rotation skips billboard");
    invalid.rotation = inf;
    expect(skips(billboard, invalid, camera), "infinite rotation skips billboard");
    invalid = valid; invalid.color.x = nan;
    expect(skips(billboard, invalid, camera), "NaN color skips billboard");
    invalid.color.x = inf;
    expect(skips(billboard, invalid, camera), "infinite color skips billboard");
    invalid = valid; invalid.texcoords.x = nan;
    expect(skips(billboard, invalid, camera), "NaN UV skips billboard");
    invalid.texcoords.x = inf;
    expect(skips(billboard, invalid, camera), "infinite UV skips billboard");
    invalid = valid; invalid.texcoords = {max, max, max, max};
    expect(skips(billboard, invalid, camera), "finite UV endpoint overflow skips billboard");
    invalid = valid; invalid.position.x = inf;
    expect(skips(billboard, invalid, camera), "infinite position skips billboard");
    invalid = valid; invalid.size = nan;
    expect(skips(billboard, invalid, camera), "NaN size skips billboard");
    invalid.size = max; invalid.position.x = max;
    expect(skips(billboard, invalid, camera), "finite position arithmetic overflow skips billboard");
    auto bad_camera = camera; bad_camera.right.x = nan;
    expect(skips(billboard, valid, bad_camera), "NaN consumed camera axis skips billboard");
    bad_camera = camera; bad_camera.up.z = inf;
    expect(skips(billboard, valid, bad_camera), "infinite consumed camera axis skips billboard");
    bad_camera = camera; bad_camera.right = {max, 0, 0};
    invalid = valid; invalid.size = 2;
    expect(skips(billboard, invalid, bad_camera), "finite camera-axis multiplication overflow skips billboard");

    auto xy_emitter = drawable_emitter(); xy_emitter.renderer_id = 28;
    const auto xy = particles::plan_emitter(xy_emitter, 0);
    bad_camera = camera; bad_camera.position.x = nan;
    bad_camera.right.x = nan; bad_camera.up.z = inf;
    invalid = valid; invalid.velocity.x = nan;
    particles::VertexStream stream;
    particles::build_stream(xy, std::span{&invalid, 1}, bad_camera, stream);
    expect(stream.quads == 1 && finite_stream(stream),
           "XY ignores camera and velocity fields it does not consume");
    stream.clear();
    particles::build_stream(billboard, std::span{&invalid, 1}, camera, stream);
    expect(stream.quads == 1 && finite_stream(stream),
           "billboard ignores particle velocity it does not consume");
    bad_camera = camera; bad_camera.position.x = nan;
    stream.clear();
    particles::build_stream(billboard, std::span{&valid, 1}, bad_camera, stream);
    expect(stream.quads == 1 && finite_stream(stream),
           "billboard ignores camera position it does not consume");

    auto heat_emitter = drawable_emitter(); heat_emitter.renderer_id = 38; heat_emitter.blend_mode = 10;
    const auto heat = particles::plan_emitter(heat_emitter, 0);
    expect(skips(heat, valid, particles::CameraFrame{camera.position, {nan, 0, 0}, camera.up}),
           "heat quad rejects consumed nonfinite camera axis");

    auto kite_emitter = drawable_emitter(); kite_emitter.renderer_id = 52;
    kite_emitter.tail_size = 2;
    auto kite = particles::plan_emitter(kite_emitter, 0);
    invalid = valid; invalid.velocity = {0, 0, 1}; invalid.velocity.x = nan;
    expect(skips(kite, invalid, camera), "kite rejects NaN velocity");
    invalid = valid; invalid.velocity.z = inf;
    expect(skips(kite, invalid, camera), "kite rejects infinite velocity");
    invalid = valid; invalid.velocity.z = max;
    expect(skips(kite, invalid, camera), "kite rejects finite velocity-length overflow");
    invalid = valid; invalid.velocity.z = 1;
    bad_camera = camera; bad_camera.position.x = inf;
    expect(skips(kite, invalid, bad_camera), "moving kite rejects consumed camera position");
    bad_camera = camera; bad_camera.right.x = nan; bad_camera.up.z = inf;
    stream.clear();
    invalid.rotation = nan;
    particles::build_stream(kite, std::span{&invalid, 1}, bad_camera, stream);
    expect(stream.quads == 1 && finite_stream(stream),
           "moving kite ignores rotation and unused camera axes");
    invalid = valid; invalid.rotation = nan;
    expect(skips(kite, invalid, bad_camera), "stationary kite rejects consumed fallback axes");
    invalid.rotation = 0;
    bad_camera = camera; bad_camera.position.x = nan;
    stream.clear();
    particles::build_stream(kite, std::span{&invalid, 1}, bad_camera, stream);
    expect(stream.quads == 1 && finite_stream(stream),
           "stationary kite ignores camera position");
    kite.tail_size = max;
    invalid.size = max;
    expect(skips(kite, invalid, camera), "finite kite tail overflow skips whole quad");

    auto second = valid; second.position.x = 10;
    invalid = valid; invalid.position.x = 1000; invalid.color.w = nan;
    const particles::Particle mixed[]{valid, invalid, second};
    stream.clear();
    particles::build_stream(xy, mixed, camera, stream);
    expect(stream.quads == 2 && stream.vertices.size() == 8 &&
           stream.indices == std::vector<std::uint32_t>({0, 1, 2, 2, 1, 3, 4, 5, 6, 6, 5, 7}) &&
           close(stream.bounds_min.x, -1) && close(stream.bounds_max.x, 11) && finite_stream(stream),
           "valid-invalid-valid keeps complete quads, indices and valid-only bounds");
    const auto hash = particles::stream_hash(stream);
    const auto bounds_min = stream.bounds_min, bounds_max = stream.bounds_max;
    particles::build_stream(xy, std::span{&invalid, 1}, camera, stream);
    expect(stream.quads == 2 && stream.vertices.size() == 8 && stream.indices.size() == 12 &&
           particles::stream_hash(stream) == hash &&
           stream.bounds_min.x == bounds_min.x && stream.bounds_max.x == bounds_max.x,
           "invalid particle leaves an existing stream unchanged");
}

void test_fixed_seed_streams() {
    RecordingBackend first, second, other;
    const auto a = run(first, 1234, 90);
    const auto b = run(second, 1234, 90);
    const auto c = run(other, 4321, 90);
    expect(a == b, "identical per-frame effect hashes across two fixed-seed runs");
    expect(first.record == second.record, "identical emitted vertex streams across two fixed-seed runs");
    expect(first.vertices == second.vertices && first.vertices > 0, "same non-empty vertex volume");
    expect(a != c, "a different seed produces a different stream");
}

void test_release_and_replacement_lifecycle() {
    RecordingBackend backend;
    {
        particles::EffectRegistry registry(backend);
        const auto first = registry.spawn(mixed_system(), 77, 256);
        expect(bool(first), "first instance spawns");
        const auto* plans = registry.plans(first.value());
        const auto drawable = static_cast<std::size_t>(std::count_if(plans->begin(), plans->end(), [](const auto& plan) { return plan.drawable; }));
        expect(drawable == 3 && !(*plans)[3].drawable, "depth sprite emitter is listed, three emitters drawn");
        expect(registry.live_backend_resources() == 3 && backend.live.size() == 3, "one backend resource per drawable emitter");
        std::vector<std::uint64_t> original;
        for (int frame = 0; frame < 20; ++frame) original.push_back(registry.advance(first.value(), 1.0F / 30.0F, test_camera()).value().hash);
        expect(bool(registry.release(first.value())), "release succeeds");
        expect(registry.live_backend_resources() == 0 && backend.live.empty() && registry.live_effects() == 0, "release frees every backend resource");
        const auto again = registry.release(first.value());
        expect(!again && again.error().code == particles::diagnostic_codes::unknown_effect, "double release is a diagnostic");
        expect(!registry.advance(first.value(), 0.1F, test_camera()), "a released handle cannot advance");

        const auto replacement = registry.spawn(mixed_system(), 77, 256);
        expect(bool(replacement) && replacement.value() != first.value(), "replacement gets a fresh handle");
        std::vector<std::uint64_t> replayed;
        for (int frame = 0; frame < 20; ++frame) replayed.push_back(registry.advance(replacement.value(), 1.0F / 30.0F, test_camera()).value().hash);
        expect(original == replayed, "replacement reproduces the released instance's streams");
        expect(registry.live_backend_resources() == 3, "replacement owns only its own resources");
        const auto kept = registry.spawn(mixed_system(), 5, 64);
        expect(bool(kept) && registry.live_effects() == 2, "second live instance");
    }
    expect(backend.live.empty() && backend.created == backend.destroyed, "registry destruction frees every remaining resource");
    expect(backend.invalid_updates == 0 && backend.invalid_destroys == 0, "no update or destroy reaches a freed resource");

    RecordingBackend refusing; refusing.refuse_emitter = 1;
    {
        particles::EffectRegistry registry(refusing);
        const auto handle = registry.spawn(mixed_system(), 3, 64);
        const auto& plan = (*registry.plans(handle.value()))[1];
        expect(!plan.drawable && !plan.cause.empty(), "backend refusal is reported per emitter");
        expect(registry.live_backend_resources() == 2 && !registry.diagnostics().empty(), "refused emitter owns nothing and leaves a diagnostic");
        const auto stats = registry.advance(handle.value(), 0.5F, test_camera());
        expect(stats && !stats.value().emitters[1].drawn && stats.value().emitters[1].quads == 0, "refused emitter is not drawn some other way");
    }
    expect(refusing.live.empty(), "refused-emitter instance leaks nothing");

    RecordingBackend peaks;
    {
        particles::EffectRegistry registry(peaks);
        const auto handle = registry.spawn(mixed_system(), 5, 256);
        const auto stats = registry.advance(handle.value(), 0.5F, test_camera());
        expect(bool(stats), "peak-alpha effect advances");
        for (const auto& [resource, stream] : peaks.last) {
            float peak = 0.0F;
            for (const auto& vertex : stream.vertices) peak = std::max(peak, vertex.color.w);
            const auto& emitter = stats.value().emitters[peaks.live.at(resource)];
            expect(stream.quads != 0 && peak > 0.0F && peak < 1.0F, "fading emitter uploads a partial alpha");
            expect(emitter.maximum_alpha == peak, "frame stats carry the uploaded stream's peak vertex alpha");
        }
        expect(stats.value().emitters[3].maximum_alpha == 0.0F, "an undrawn emitter reports no alpha");
    }
}

void test_effect_brightness() {
    RecordingBackend plain_backend;
    RecordingBackend bright_backend;
    particles::EffectRegistry plain(plain_backend);
    particles::EffectRegistry bright(bright_backend);
    const auto a = plain.spawn(mixed_system(), 21, 256);
    const auto b = bright.spawn(mixed_system(), 21, 256);
    expect(a && b, "brightness fixtures spawn");
    expect(bool(bright.set_brightness(b.value(), 1.0F)), "brightness 1 is accepted");
    const auto unit_a = plain.advance(a.value(), 0.25F, test_camera());
    const auto unit_b = bright.advance(b.value(), 0.25F, test_camera());
    expect(unit_a && unit_b && unit_a.value().hash == unit_b.value().hash, "brightness 1 leaves the streams unchanged");
    expect(bool(bright.set_brightness(b.value(), 0.2F)), "brightness 0.2 is accepted");
    const auto dim_a = plain.advance(a.value(), 1.0F / 30.0F, test_camera());
    const auto dim_b = bright.advance(b.value(), 1.0F / 30.0F, test_camera());
    expect(dim_a && dim_b && dim_a.value().particles == dim_b.value().particles, "brightness does not change the simulation");
    bool scaled = !plain_backend.last.empty() && plain_backend.last.size() == bright_backend.last.size();
    for (auto plain_stream = plain_backend.last.begin(), bright_stream = bright_backend.last.begin();
         scaled && plain_stream != plain_backend.last.end(); ++plain_stream, ++bright_stream) {
        const auto& p = plain_stream->second.vertices;
        const auto& q = bright_stream->second.vertices;
        scaled = p.size() == q.size();
        for (std::size_t index = 0; scaled && index < p.size(); ++index) {
            scaled = close(q[index].color.x, p[index].color.x * 0.2F) && close(q[index].color.y, p[index].color.y * 0.2F)
                && close(q[index].color.z, p[index].color.z * 0.2F) && close(q[index].color.w, p[index].color.w * 0.2F)
                && q[index].position.x == p[index].position.x;
        }
    }
    expect(scaled, "every vertex's RGBA is multiplied by the brightness, geometry unchanged");
    const auto negative = bright.set_brightness(b.value(), -0.5F);
    expect(!negative && negative.error().code == particles::diagnostic_codes::invalid_value, "a negative brightness is a diagnostic");
    expect(!bright.set_brightness(b.value(), std::numeric_limits<float>::quiet_NaN()), "a non-finite brightness is a diagnostic");
    expect(bool(bright.release(b.value())), "brightness fixture releases");
    const auto released = bright.set_brightness(b.value(), 1.0F);
    expect(!released && released.error().code == particles::diagnostic_codes::unknown_effect, "a released handle takes no brightness");
}

// #433: an engine glow (an emitter-linked particle on a nozzle bone) drawn at 60 frames per
// second while its ship turns 90 degrees in half a second and moves: the emitters advance on
// every other frame (the 30 Hz clock) and are presented on the frames between. On every drawn
// frame the uploaded glow stands where it was born in the bone's frame, in the pose drawn that
// frame.
void test_emitter_glow_follows_turning_pose() {
    RecordingBackend backend;
    particles::EffectRegistry registry(backend);
    particles::SystemDefinition system;
    // Spawned off the emitter's origin, as an EnhancedMesh engine glow spawns across its nozzle mesh.
    auto glow = drawable_emitter(); glow.translater_id = 26; glow.position.point = {0.0F, 2.0F, 0.0F};
    system.emitters.push_back(glow);
    const auto handle = registry.spawn(system, 3, 16);
    expect(bool(handle), "glow fixture spawns");
    const particles::Vec3 bone{-3.0F, 1.0F, 0.0F};  // the nozzle in the ship's model space
    float worst = 0.0F;
    std::size_t presents = 0, measured = 0;
    for (int frame = 0; frame <= 30; ++frame) {
        const float seconds = static_cast<float>(frame) / 60.0F;
        const float yaw = 1.5707963F * std::min(1.0F, seconds / 0.5F);
        const float c = std::cos(yaw), s = std::sin(yaw);
        const particles::Basis3 basis{{c, s, 0.0F}, {-s, c, 0.0F}, {0.0F, 0.0F, 1.0F}};
        const particles::Vec3 origin{100.0F * seconds + c * bone.x - s * bone.y, s * bone.x + c * bone.y, 0.0F};
        expect(bool(registry.set_frame(handle.value(), {origin, basis})), "glow frame is set every drawn frame");
        const std::size_t uploads = backend.record.size();
        if (frame % 2 == 0) {
            const auto stats = registry.advance(handle.value(), 1.0F / 30.0F, test_camera());
            expect(bool(stats) && stats.value().particles == 1 && stats.value().has_bounds, "the glow is drawn on a sample");
        } else {
            expect(bool(registry.present(handle.value(), test_camera())), "the glow is presented between samples");
            ++presents;
        }
        expect(backend.record.size() == uploads + 1 && backend.last.size() == 1, "every frame uploads the glow's stream");
        if (backend.last.empty() || backend.last.begin()->second.quads != 1) continue;
        const auto& bounds = backend.last.begin()->second;
        const particles::Vec3 centre{(bounds.bounds_min.x + bounds.bounds_max.x) * 0.5F,
                                     (bounds.bounds_min.y + bounds.bounds_max.y) * 0.5F,
                                     (bounds.bounds_min.z + bounds.bounds_max.z) * 0.5F};
        const particles::Vec3 glow_at{origin.x - s * 2.0F, origin.y + c * 2.0F, origin.z};
        const particles::Vec3 off = minus(centre, glow_at);
        worst = std::max(worst, std::sqrt(dot(off, off)));
        ++measured;
    }
    expect(presents == 15, "half the frames are drawn between advances");
    expect(measured == 31, "the glow's quad is measured on every frame");
    expect(worst < 0.01F, "the glow stands on its bone in the drawn pose on every frame of the turn");
    expect(backend.created == 1, "the glow is one emitter for the whole turn, never spawned again");
    const auto released = registry.present(particles::EffectHandle{9999}, test_camera());
    expect(!released && released.error().code == particles::diagnostic_codes::unknown_effect,
           "presenting an unknown handle is a diagnostic");
}

// #439: a frame drawn between samples sets and presents every running effect. With 150 live
// effects (a third of the spawned handles already released, so live handles are not contiguous)
// that frame allocates nothing once the streams have grown, and uploads each effect once.
class CountingBackend final : public particles::RenderBackend {
public:
    std::uint64_t create_emitter(const particles::EmitterRenderPlan&) override { return ++next_; }
    void update_emitter(std::uint64_t, const particles::VertexStream& stream) override {
        ++updates; quads += stream.quads;
    }
    void destroy_emitter(std::uint64_t) override {}
    std::size_t updates{}, quads{};
private:
    std::uint64_t next_{};
};

void test_present_allocates_nothing() {
    CountingBackend backend;
    particles::EffectRegistry registry(backend);
    particles::SystemDefinition system;
    auto glow = drawable_emitter(); glow.translater_id = 26; glow.position.point = {0.0F, 2.0F, 0.0F};
    system.emitters.push_back(glow);
    std::vector<particles::EffectHandle> handles;
    for (std::uint32_t index = 0; index < 225; ++index) {
        const auto handle = registry.spawn(system, index + 1U, 16);
        expect(bool(handle), "present fixture spawns");
        if (!handle) return;
        if (index % 3 == 0) expect(bool(registry.release(handle.value())), "present fixture releases a third");
        else handles.push_back(handle.value());
    }
    expect(handles.size() == 150 && registry.live_effects() == 150, "150 effects run");
    const auto camera = test_camera();
    const particles::Basis3 identity{{1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}};
    const particles::Basis3 quarter{{0.0F, 1.0F, 0.0F}, {-1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}};
    for (const particles::EffectHandle handle : handles) {
        expect(bool(registry.set_frame(handle, {{0.0F, 0.0F, 0.0F}, identity})), "present fixture frame");
        expect(bool(registry.advance(handle, 1.0F / 30.0F, camera)), "present fixture samples");
        expect(bool(registry.present(handle, camera)), "present fixture presents");
    }
    const std::size_t updates = backend.updates, quads = backend.quads;
    bool presented = true;
    const std::size_t before = global_allocations.load(std::memory_order_relaxed);
    for (const particles::EffectHandle handle : handles) {
        presented = bool(registry.set_frame(handle, {{1.0F, 0.0F, 0.0F}, quarter})) && presented;
        presented = bool(registry.present(handle, camera)) && presented;
    }
    const std::size_t allocated = global_allocations.load(std::memory_order_relaxed) - before;
    expect(presented, "every running effect is presented");
    expect(allocated == 0, "a frame presenting 150 effects allocates nothing");
    expect(backend.updates - updates == 150 && backend.quads - quads == 150, "each effect is uploaded once with its glow");
}

void test_camera_and_attachment_frames() {
    const auto camera = particles::camera_frame_from_render({0, 0, 10}, {0, 0, 0}, {0, 1, 0});
    // Render eye (0,0,10) is ALO (0,-10,0); render up +Y is ALO +Z.
    expect(close(camera.position.y, -10) && close(camera.up.z, 1) && close(camera.right.x, 1), "render camera converts to the ALO basis");

    playback::Matrix asset = playback::Player::identity_matrix();
    asset[0] = 0; asset[1] = 1; asset[4] = -1; asset[5] = 0; // 90 degrees about ALO Z
    asset[12] = 3; asset[13] = 4; asset[14] = 5;
    const auto frame = particles::emitter_frame_from_render(playback::Player::asset_to_render_transform(asset));
    expect(close(frame.origin.x, 3) && close(frame.origin.y, 4) && close(frame.origin.z, 5), "attachment origin round-trips to the ALO basis");
    expect(close(frame.basis.x.y, 1) && close(frame.basis.y.x, -1) && close(frame.basis.z.z, 1), "attachment basis round-trips to the ALO basis");

    // A bone that travels +10 X over one second, sampled on the fixed clock.
    eawr::assets::Model model;
    eawr::assets::Bone root; root.name = "root"; root.parent = -1;
    root.relative_transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    eawr::assets::Bone socket = root; socket.name = "socket"; socket.parent = 0; socket.relative_transform[11] = 2.0F;
    model.bones = {root, socket};
    eawr::assets::Animation animation;
    animation.stored_frame_count = 2; animation.playable_frame_count = 1;
    animation.frames_per_second = 1.0F; animation.duration_seconds = 1.0F;
    eawr::assets::AnimationTrack track; track.bone_index = 0; track.bone_name = "root";
    track.samples = {{{0, 0, 0}, {1, 1, 1}, {0, 0, 0, 1}, true}, {{10, 0, 0}, {1, 1, 1}, {0, 0, 0, 1}, true}};
    animation.tracks.push_back(track);
    const auto player = playback::Player::create(model, &animation);
    expect(bool(player), "synthetic attachment host binds");
    if (!player) return;

    RecordingBackend backend;
    particles::EffectRegistry registry(backend);
    particles::SystemDefinition system; auto emitter = drawable_emitter();
    emitter.spawn_interval = 0.1F; emitter.stop_time = 0.0F; emitter.lifetime = 0.05F;
    system.emitters.push_back(emitter);
    const auto handle = registry.spawn(system, 11, 64);
    constexpr float dt = 1.0F / 10.0F;
    bool followed = true;
    for (int step = 0; step < 8; ++step) {
        const float time = static_cast<float>(step) * dt;
        const auto pose = player.value().sample({time, playback::PlaybackMode::clamp, 0.0F});
        const auto attachment = player.value().attachment(pose.value(), "socket", playback::AttachmentSpace::model);
        expect(bool(attachment), "named attachment resolves");
        expect(bool(registry.set_frame(handle.value(), particles::emitter_frame_from_render(attachment.value().column_major))), "attachment frame applies");
        const auto stats = registry.advance(handle.value(), step == 0 ? 0.0F : dt, test_camera());
        expect(bool(stats), "attached effect advances");
        // The newest particle was born at the socket on this frame's clock.
        const float expected_x = 10.0F * time;
        const auto live = stats.value().particles;
        followed = followed && live >= 1 && stats.value().has_bounds
            && stats.value().bounds_min.x <= expected_x + 1.01F && stats.value().bounds_max.x >= expected_x - 1.01F
            && close(stats.value().bounds_min.z + stats.value().bounds_max.z, 4.0F, 0.01F);
    }
    expect(followed, "effect follows the animated attachment under fixed time");
    const auto missing = player.value().attachment(player.value().sample({}).value(), "absent", playback::AttachmentSpace::model);
    expect(!missing, "a missing attachment is a diagnostic, not an origin fallback");
}

} // namespace particle_render_contracts
