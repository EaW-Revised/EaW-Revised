#include "render_test_support.hpp"

namespace particle_render_contracts {
void test_mesh_registry_boundary(){
    RecordingBackend backend;
    particles::EffectRegistry registry(backend);
    auto system=mesh_system(particles::MeshSpawnMode::every_vertex);
    const auto missing=registry.spawn(system,17,1);
    expect(!missing&&missing.error().code==particles::diagnostic_codes::mesh_binding&&
        backend.created==0&&registry.live_effects()==0,
        "legacy three-argument spawn fails before backend creation when mesh binding is required");
    auto v2=system;v2.version=particles::AloParticleVersion::plugin_v2;
    v2.emitters[0].cpu_ready=false;
    const auto metadata=registry.spawn(v2,17,1);
    expect(metadata&&backend.created==0,"V2 mesh plugin remains metadata-only without a V1 binding");
    if(metadata)static_cast<void>(registry.release(metadata.value()));
    auto binding=test_mesh_binding();
    auto empty=binding;empty.geometry.submeshes.clear();
    expect(!registry.spawn(system,17,1,empty),"empty geometry is rejected");
    auto nan=binding;nan.geometry.submeshes[0].vertices[0].normal.x=std::numeric_limits<float>::quiet_NaN();
    expect(!registry.spawn(system,17,1,nan),"non-finite geometry is rejected");
    auto incomplete=binding;incomplete.geometry.submeshes[0].triangle_indices={0,1};
    expect(!registry.spawn(system,17,1,incomplete),"incomplete triangles are rejected");
    auto bad_index=binding;bad_index.geometry.submeshes[0].triangle_indices={0,1,2};
    expect(!registry.spawn(system,17,1,bad_index),"out-of-range triangle indices are rejected");
    auto bad_frame=binding;bad_frame.frame.basis.x.x=std::numeric_limits<float>::infinity();
    expect(!registry.spawn(system,17,1,bad_frame),"non-finite mesh frame is rejected");
    auto overflow=system;overflow.emitters[0].bursting=true;
    overflow.emitters[0].particles_per_interval=std::numeric_limits<float>::max();
    expect(!registry.spawn(overflow,17,1,binding),"every-vertex count multiplication overflow is rejected");
    overflow.emitters[0].mesh_mode=particles::MeshSpawnMode::random_vertex;
    expect(!registry.spawn(overflow,17,1,binding),"random-vertex burst count overflow is rejected");
    auto cycle=system;cycle.emitters[0].parent_emitter=0;
    expect(!registry.spawn(cycle,17,1,binding),"programmatic mesh parent cycle is rejected");
    auto surface=mesh_system(particles::MeshSpawnMode::random_surface);
    expect(!registry.spawn(surface,17,1,binding),
        "surface emission requires triangles in every submesh");
    expect(backend.created==0&&registry.live_effects()==0&&registry.live_backend_resources()==0,
        "all invalid mesh bindings fail without allocating backend resources");

    const auto first=registry.spawn(system,17,1,binding);
    expect(bool(first),"vertex-only mesh accepts absent triangles");
    if(!first)return;
    binding.geometry.submeshes[0].vertices.clear();
    auto fresh=test_mesh_binding();
    const auto second=registry.spawn(system,17,1,std::move(fresh));
    expect(bool(second),"second effect owns its own geometry and cursor");
    if(!second)return;
    const auto a0=registry.advance(first.value(),0,test_camera());
    expect(a0&&a0.value().advance.spawned==1&&close(backend.last[1].vertices[0].position.x,2),
        "first effect emits the original caller-destroyed vertex");
    const auto a1=registry.advance(first.value(),1,test_camera());
    expect(a1&&close(backend.last[1].vertices[0].position.x,8),
        "first effect cursor advances after prior particle death");
    const auto b0=registry.advance(second.value(),0,test_camera());
    expect(b0&&close(backend.last[2].vertices[0].position.x,2),
        "second effect begins at vertex zero independently");
    expect(!registry.set_mesh_frame(first.value(),{{std::numeric_limits<float>::quiet_NaN(),0,0},{}}),
        "non-finite mesh frame update fails explicitly");
    expect(!registry.set_frame(first.value(),{{std::numeric_limits<float>::infinity(),0,0},{}}),
        "non-finite emitter frame update fails explicitly");
    expect(bool(registry.set_frame(first.value(),{{100,0,0},{}})),
        "emitter frame can update independently");
    expect(bool(registry.set_mesh_frame(first.value(),{{10,0,0},{}})),
        "checked mesh frame update succeeds");
    const auto a2=registry.advance(first.value(),1,test_camera());
    expect(a2&&close(backend.last[1].vertices[0].position.x,12),
        "mesh update applies to future emission without adopting emitter origin");
    expect(!registry.set_mesh_frame(0,{}),"invalid mesh handle is rejected");
    expect(bool(registry.release(first.value()))&&bool(registry.release(second.value()))&&
        backend.live.empty()&&backend.created==backend.destroyed,
        "mesh effect release destroys each backend resource");
    const auto restarted=registry.spawn(system,17,1,test_mesh_binding());
    expect(restarted&&registry.advance(restarted.value(),0,test_camera())&&
        close(backend.last[3].vertices[0].position.x,2),
        "restarted effect repeats the fixed-seed initial vertex");
    auto random=mesh_system(particles::MeshSpawnMode::random_vertex);
    random.emitters[0].lifetime=0.5F;
    const auto random_first=registry.spawn(random,531,1,test_mesh_binding());
    expect(bool(random_first),"random mesh effect spawns for restart test");
    if(random_first){
        const auto h0=registry.advance(random_first.value(),0,test_camera());
        const auto h1=registry.advance(random_first.value(),1,test_camera());
        expect(h0&&h1,"random mesh effect advances for restart test");
        static_cast<void>(registry.release(random_first.value()));
        const auto replay=registry.spawn(random,531,1,test_mesh_binding());
        const auto r0=registry.advance(replay.value(),0,test_camera());
        const auto r1=registry.advance(replay.value(),1,test_camera());
        expect(r0&&r1&&h0.value().hash==r0.value().hash&&h1.value().hash==r1.value().hash,
            "same seed and binding reproduce random mesh streams after release and restart");
    }
}

void test_mesh_root_precedence(){
    LegacyEmitter parent;parent.spawn_during_life=1;parent.velocity_x=4;
    LegacyEmitter child;child.mesh_mode=3;child.parent_link_strength=0.5F;
    const auto loaded=particles::load_alo(legacy_system({parent,child}),"mesh-root.alo");
    expect(loaded&&loaded.value().emitters[1].creator_id==35&&
        loaded.value().emitters[1].parent_emitter==0,
        "mesh creator keeps precedence while parent metadata remains validated");
    if(!loaded)return;
    particles::CpuSystem cpu(loaded.value(),17,16,test_mesh_binding());
    expect(cpu.advance(0).spawned==3,
        "mesh creator registers as root rather than attaching one instance per parent");
    const auto mesh_particle=std::find_if(cpu.particles().begin(),cpu.particles().end(),
        [](const particles::Particle& particle){return particle.emitter_index==1;});
    expect(mesh_particle!=cpu.particles().end()&&close(mesh_particle->velocity.x,0),
        "mesh creator ignores Shape parent velocity inheritance");
}

void test_proxy_mesh_binding(){
    eawr::assets::Model host;
    const auto bone=[](const std::string& name,const std::int32_t parent){
        eawr::assets::Bone value;value.name=name;value.parent=parent;
        value.relative_transform={1,0,0,0,0,1,0,0,0,0,1,0};return value;
    };
    host.bones={bone("root",-1),bone("owner",0),bone("proxy",1),bone("other",0)};
    host.proxies={{"spark",2}};
    const auto vertex=[](float x,float y,float z,float nx,float ny,float nz){
        eawr::assets::Vertex value;value.position={x,y,z};value.normal={nx,ny,nz};return value;
    };
    eawr::assets::Mesh decoy;decoy.bone=0;
    eawr::assets::Submesh decoy_submesh;
    decoy_submesh.vertices={vertex(90,0,0,0,0,1)};
    decoy.submeshes={decoy_submesh};
    eawr::assets::Mesh first;first.bone=1;
    eawr::assets::Submesh a;a.vertices={vertex(1,2,3,0,1,0),vertex(4,5,6,1,0,0),vertex(7,8,9,0,0,1)};
    a.indices={0,1,2};
    eawr::assets::Submesh b;b.vertices={vertex(-2,3,4,0,0,1),vertex(-3,4,5,0,1,0),vertex(-4,5,6,1,0,0)};
    b.indices={2,1,0};first.submeshes={a,b};
    eawr::assets::Mesh second=first;second.submeshes[0].vertices[0].position.x=77;
    host.meshes={decoy,first,second};
    auto selected=particles::bind_proxy_mesh(host,"spark");
    expect(selected&&selected.value().mesh_index==1&&selected.value().owner_bone==1&&
        selected.value().proxy_bone==2,"proxy selects first mesh on immediate parent in model order");
    if(!selected)return;
    expect(selected.value().binding.geometry.submeshes.size()==2&&
        selected.value().binding.geometry.submeshes[0].vertices[0].position.x==1&&
        selected.value().binding.geometry.submeshes[1].vertices[0].position.x==-2&&
        selected.value().binding.geometry.submeshes[1].triangle_indices==std::vector<std::uint32_t>({2,1,0}),
        "proxy binding copies ordered submeshes, vertices, normals and widened indices");
    host.meshes.clear();
    expect(selected.value().binding.geometry.submeshes[0].vertices[0].normal.y==1,
        "proxy binding owns geometry after host meshes are destroyed");
    expect(!particles::bind_proxy_mesh(host,"absent"),"missing proxy is diagnosed");
    host.proxies.push_back({"spark",2});
    expect(!particles::bind_proxy_mesh(host,"spark"),"ambiguous proxy is diagnosed");
    host.proxies.back().bone=3;
    host.meshes={decoy};
    auto twin=particles::bind_proxy_mesh(host,std::size_t{1});
    expect(twin&&twin.value().proxy_bone==3&&twin.value().owner_bone==0&&twin.value().mesh_index==0,
        "a proxy named like another binds by its index, on its own bone's parent");
    expect(!particles::bind_proxy_mesh(host,std::size_t{2}),"proxy index out of range is diagnosed");
    host.proxies.pop_back();
    host.meshes.clear();
    expect(!particles::bind_proxy_mesh(host,"spark"),"missing immediate-parent mesh is diagnosed");

    // Two independent source-basis transforms expose accidental skinning,
    // render-basis conversion and proxy/owner frame conflation.
    playback::Pose pose;pose.bones.resize(4);
    for(auto& entry:pose.bones)entry.model_asset=playback::Player::identity_matrix();
    auto& owner=pose.bones[1].model_asset;
    owner[0]=0;owner[1]=2;owner[4]=-3;owner[5]=0;owner[12]=10;owner[13]=20;owner[14]=30;
    auto& proxy=pose.bones[2].model_asset;
    proxy[12]=-5;proxy[13]=7;proxy[14]=11;
    pose.bones[1].skin_asset=playback::Player::identity_matrix();
    particles::EmitterFrame emitter;particles::MeshFrame mesh;
    expect(bool(particles::proxy_mesh_frames(pose,selected.value(),emitter,mesh))&&
        close(emitter.origin.x,-5)&&close(emitter.origin.y,7)&&close(emitter.origin.z,11)&&
        close(mesh.origin.x,10)&&close(mesh.origin.y,20)&&close(mesh.origin.z,30)&&
        close(mesh.basis.x.y,2)&&close(mesh.basis.y.x,-3),
        "proxy and owner frames are independent affine source-Z-up matrices");
    expect(!particles::proxy_mesh_frames(playback::Pose{},selected.value(),emitter,mesh),
        "pose missing selected bones is diagnosed");
    auto normal_system=mesh_system(particles::MeshSpawnMode::every_vertex);
    normal_system.emitters[0].mesh_surface_offset=0.1F;
    particles::CpuSystem normal_cpu(normal_system,31,6,selected.value().binding);
    expect(bool(normal_cpu.set_mesh_frame(mesh))&&normal_cpu.advance(0).spawned==6&&
        close(normal_cpu.particles()[0].position.x,3.7F)&&
        close(normal_cpu.particles()[0].position.y,22)&&
        close(normal_cpu.particles()[0].position.z,33),
        "selected authored normal uses owner linear basis and source-Z-up offset without skinning");

    host.bones[2].relative_transform[11]=2.0F;
    eawr::assets::Animation clip;
    clip.stored_frame_count=2;clip.playable_frame_count=1;
    clip.frames_per_second=1;clip.duration_seconds=1;
    eawr::assets::AnimationTrack track;track.bone_index=1;track.bone_name="owner";
    track.samples={{{0,0,0},{1,1,1},{0,0,0,1},true},
                   {{4,0,0},{1,1,1},{0,0,0,1},true}};
    clip.tracks.push_back(track);
    auto player=playback::Player::create(host,&clip);
    expect(bool(player),"small owner animation binds to proxy host");
    if(!player)return;
    auto system=mesh_system(particles::MeshSpawnMode::every_vertex);
    system.emitters[0].lifetime=3;
    RecordingBackend backend;particles::EffectRegistry registry(backend);
    auto handle=registry.spawn(system,31,12,selected.value().binding);
    expect(bool(handle),"selected geometry spawns through graphical registry contract");
    if(!handle)return;
    for(int step=0;step<2;++step){
        const auto sampled=player.value().sample({static_cast<float>(step),playback::PlaybackMode::clamp,0});
        expect(bool(sampled),"owner animation pose samples");
        if(!sampled)return;
        expect(bool(particles::proxy_mesh_frames(sampled.value(),selected.value(),emitter,mesh)),
            "single sampled pose supplies both frames");
        expect(bool(registry.set_frame(handle.value(),emitter))&&
            bool(registry.set_mesh_frame(handle.value(),mesh)),
            "independent animated frames reach registry before advance");
        const auto advanced=registry.advance(handle.value(),static_cast<float>(step),test_camera());
        expect(advanced&&advanced.value().advance.spawned==6&&
            advanced.value().particles==static_cast<std::size_t>(6*(step+1)),
            "owner movement changes new births while older mesh particles survive");
        expect(close(mesh.origin.x,4.0F*step)&&close(emitter.origin.x,4.0F*step)&&
            close(emitter.origin.z,mesh.origin.z+2.0F),
            "animated proxy origin remains distinct from owner mesh origin");
    }
    expect(bool(registry.release(handle.value()))&&backend.live.empty()&&
        backend.created==backend.destroyed,
        "animated proxy registry releases all backend resources");
}

// Recorded from the pre-detach sources (base cdba900) with the same system,
// seed, step and camera; detach support must not move a single bit of it.
// Sampling uses the platform's sin/cos, so the bits are pinned per C runtime
// and architecture. Only MSVC x64 and glibc x86-64 (GCC 14 and Clang 18
// agree) were measured; every other target (ARM64, ARM64EC, 32-bit, other C
// runtimes) checks architecture-neutral invariants only and never claims a
// baseline it has not recorded.
void test_no_detach_golden_unchanged() {
#if defined(_WIN32) && defined(_M_X64) && !defined(_M_ARM64EC)
    constexpr const char* final_golden = "cf540386621096e7";
    constexpr const char* chain_golden = "441478cf9153b003";
#elif defined(__GLIBC__) && defined(__x86_64__)
    constexpr const char* final_golden = "302e180c7f1bec70";
    constexpr const char* chain_golden = "1bb59db7463d73dc";
#else
    constexpr const char* final_golden = nullptr;
    constexpr const char* chain_golden = nullptr;
#endif
    const auto chain_of = [](const std::vector<std::uint64_t>& hashes) {
        std::uint64_t chain = 0xcbf29ce484222325ULL;
        for (const std::uint64_t hash : hashes) chain = (chain ^ hash) * 0x100000001b3ULL;
        return chain;
    };
    RecordingBackend backend;
    const auto hashes = run(backend, 1234, 90);
    const std::uint64_t chain = chain_of(hashes);
    if constexpr (final_golden == nullptr) {
        RecordingBackend repeat_backend;
        const auto repeat = run(repeat_backend, 1234, 90);
        expect(hashes.size() == 90 && repeat == hashes && chain_of(repeat) == chain,
               "no-detach fixed-seed stream is reproducible on this unmeasured target");
        std::cout << "no-detach golden not recorded for this C runtime/architecture; invariants only\n";
        return;
    }
    expect(particles::hex64(hashes.back()) == final_golden && particles::hex64(chain) == chain_golden,
           "no-detach fixed-seed stream matches the pre-detach golden");
}

particles::SystemDefinition draining_system(const bool leave_particles) {
    auto system = mixed_system();
    system.leave_particles = leave_particles;
    return system;
}

void test_detach_release_branch() {
    RecordingBackend backend;
    particles::EffectRegistry registry(backend);
    const auto handle = registry.spawn(draining_system(false), 41, 256);
    expect(bool(handle), "leave-particles-false effect spawns");
    for (int frame = 0; frame < 10; ++frame)
        static_cast<void>(registry.advance(handle.value(), frame == 0 ? 0.0F : 1.0F / 30.0F, test_camera()));
    expect(registry.live_backend_resources() == 3 && backend.live.size() == 3, "resources exist before detach");
    const auto detached = registry.detach(handle.value());
    expect(detached && detached.value() == particles::EffectDetachState::released &&
           particles::to_string(detached.value()) == "released",
           "leave-particles false releases immediately");
    expect(registry.live_effects() == 0 && registry.live_backend_resources() == 0 && backend.live.empty() &&
           backend.destroyed == backend.created, "immediate release destroys every backend resource once");
    const auto updates = backend.record.size();
    expect(!registry.advance(handle.value(), 1.0F / 30.0F, test_camera()) && backend.record.size() == updates,
           "a released handle neither advances nor uploads");
    const auto again = registry.detach(handle.value());
    const auto release = registry.release(handle.value());
    expect(!again && again.error().code == particles::diagnostic_codes::unknown_effect &&
           !release && release.error().code == particles::diagnostic_codes::unknown_effect,
           "detach or release after immediate release is the unknown-handle diagnostic");
    expect(backend.invalid_destroys == 0 && backend.destroyed == backend.created, "no double cleanup");
}

void test_stop_emission_drains_whatever_the_flag() {
    RecordingBackend backend;
    particles::EffectRegistry registry(backend);
    const auto handle = registry.spawn(draining_system(false), 41, 256);
    expect(bool(handle), "leave-particles-false effect spawns for stop_emission");
    particles::EffectFrameStats last;
    for (int frame = 0; frame < 20; ++frame)
        last = registry.advance(handle.value(), frame == 0 ? 0.0F : 1.0F / 30.0F, test_camera()).value();
    expect(last.particles > 0 && !last.detached, "the effect is emitting before it stops");
    expect(bool(registry.stop_emission(handle.value())) && bool(registry.stop_emission(handle.value())),
           "stop_emission succeeds, twice");
    expect(registry.live_effects() == 1 && registry.live_backend_resources() == 3,
           "the instance and its resources stay, unlike a leave-particles-false detach");
    std::size_t previous = last.particles, spawned{}, frames{}, residual_quads{};
    bool monotonic = true;
    particles::EffectFrameStats stats;
    while (frames < 200) {
        stats = registry.advance(handle.value(), 1.0F / 30.0F, test_camera()).value();
        ++frames;
        spawned += stats.advance.spawned;
        monotonic = monotonic && stats.particles <= previous;
        previous = stats.particles;
        if (frames == 1) for (const auto& emitter : stats.emitters) residual_quads += emitter.quads;
        if (stats.finished) break;
    }
    expect(residual_quads > 0 && spawned == 0 && monotonic, "the residual particles are drawn and nothing new spawns");
    expect(stats.detached && stats.finished && stats.particles == 0, "the drain ends empty");
    expect(bool(registry.release(handle.value())) && registry.live_backend_resources() == 0 &&
           backend.live.empty() && backend.invalid_destroys == 0,
           "the owner's release frees the resources once");
    expect(!registry.stop_emission(handle.value()) &&
           registry.stop_emission(handle.value()).error().code == particles::diagnostic_codes::unknown_effect,
           "stop_emission of a released handle is the unknown-handle diagnostic");
}

void test_detach_drain_branch() {
    RecordingBackend backend;
    const auto created_before = backend.created;
    particles::EffectRegistry registry(backend);
    const auto handle = registry.spawn(draining_system(true), 41, 256);
    expect(bool(handle), "leave-particles-true effect spawns");
    particles::EffectFrameStats last;
    for (int frame = 0; frame < 20; ++frame)
        last = registry.advance(handle.value(), frame == 0 ? 0.0F : 1.0F / 30.0F, test_camera()).value();
    expect(!last.detached && !last.finished && last.particles > 0, "attached effect reports neither flag");
    const auto detached = registry.detach(handle.value());
    expect(detached && detached.value() == particles::EffectDetachState::draining &&
           particles::to_string(detached.value()) == "draining", "leave-particles true drains");
    expect(registry.live_effects() == 1 && registry.live_backend_resources() == 3 && backend.live.size() == 3,
           "draining keeps the instance and its backend resources");
    const auto repeated = registry.detach(handle.value());
    expect(repeated && repeated.value() == particles::EffectDetachState::draining, "repeated detach succeeds");
    std::size_t previous = last.particles, spawned{}, frames{}, residual_quads{};
    bool monotonic = true, flags = true;
    particles::EffectFrameStats stats;
    while (frames < 200) {
        stats = registry.advance(handle.value(), 1.0F / 30.0F, test_camera()).value();
        ++frames;
        spawned += stats.advance.spawned;
        monotonic = monotonic && stats.particles <= previous;
        previous = stats.particles;
        flags = flags && stats.detached && stats.finished == (stats.particles == 0);
        if (frames == 1) for (const auto& emitter : stats.emitters) residual_quads += emitter.quads;
        if (stats.finished) break;
    }
    expect(residual_quads > 0, "residual particles are still drawn after detach");
    expect(spawned == 0 && monotonic && flags, "draining spawns nothing and finishes only when empty");
    expect(stats.finished && stats.particles == 0 && !stats.has_bounds && frames < 200,
           "the drain reaches an empty finished frame");
    expect(registry.live_backend_resources() == 3 && backend.live.size() == 3,
           "a finished instance keeps its resources until the owner releases it");
    const auto after = registry.advance(handle.value(), 1.0F / 30.0F, test_camera());
    expect(after && after.value().finished && after.value().particles == 0, "a finished instance stays finished");
    expect(bool(registry.release(handle.value())) && registry.live_backend_resources() == 0 &&
           backend.live.empty() && backend.destroyed == backend.created - created_before,
           "owner release frees every retained resource once");
    const auto stale = registry.detach(handle.value());
    expect(!stale && stale.error().code == particles::diagnostic_codes::unknown_effect &&
           !registry.release(handle.value()) && backend.invalid_destroys == 0,
           "a released draining handle is unknown and never cleaned twice");
    const auto fresh = registry.spawn(draining_system(true), 41, 256);
    expect(fresh && fresh.value() > handle.value(), "handles are never recycled");
}

void test_detach_invalid_and_early_handles() {
    RecordingBackend backend;
    particles::EffectRegistry registry(backend);
    const auto diagnostics = registry.diagnostics().size();
    for (const particles::EffectHandle bogus : {particles::EffectHandle{0}, particles::EffectHandle{77}}) {
        const auto result = registry.detach(bogus);
        expect(!result && result.error().code == particles::diagnostic_codes::unknown_effect,
               "unknown handle detach is a diagnostic");
    }
    expect(registry.diagnostics().size() == diagnostics + 2 && backend.created == 0, "invalid detach touches nothing");

    // Detach before the first advance: the frame-zero root batch never spawns.
    const auto early = registry.spawn(draining_system(true), 41, 256);
    expect(early && registry.detach(early.value()) && registry.live_backend_resources() == 3,
           "early detach retains resources");
    const auto zero = registry.advance(early.value(), 0.0F, test_camera());
    expect(zero && zero.value().advance.spawned == 0 && zero.value().particles == 0 &&
           zero.value().detached && zero.value().finished,
           "detach before the zero-delta advance finishes empty");
    expect(bool(registry.release(early.value())) && backend.live.empty(), "early drained effect releases");

    // An attached, delayed effect is empty but scheduled and not finished.
    particles::SystemDefinition delayed;
    auto emitter = drawable_emitter(); emitter.start_delay = 0.5F;
    delayed.emitters.push_back(emitter);
    const auto waiting = registry.spawn(delayed, 41, 16);
    const auto empty = registry.advance(waiting.value(), 0.0F, test_camera());
    expect(empty && empty.value().particles == 0 && !empty.value().detached && !empty.value().finished,
           "empty but scheduled is not finished");
}

std::vector<std::uint64_t> scheduled_run(RecordingBackend& backend, const std::uint32_t seed,
                                         const int detach_frame, std::size_t* final_resources,
                                         bool* finished) {
    particles::EffectRegistry registry(backend);
    auto handle = registry.spawn(draining_system(true), seed, 512).value();
    std::vector<std::uint64_t> hashes;
    for (int frame = 0; frame < 90; ++frame) {
        if (frame == detach_frame) expect(bool(registry.detach(handle)), "scheduled detach succeeds");
        const auto stats = registry.advance(handle, frame == 0 ? 0.0F : 1.0F / 30.0F, test_camera());
        hashes.push_back(stats.value().hash);
        *finished = stats.value().finished;
    }
    expect(bool(registry.release(handle)), "scheduled run releases its effect");
    *final_resources = registry.live_backend_resources();
    return hashes;
}

void test_detach_schedule_determinism() {
    RecordingBackend first, second, attached;
    std::size_t first_resources{}, second_resources{}, attached_resources{};
    bool first_finished{}, second_finished{}, attached_finished{};
    const auto a = scheduled_run(first, 1234, 30, &first_resources, &first_finished);
    const auto b = scheduled_run(second, 1234, 30, &second_resources, &second_finished);
    const auto c = scheduled_run(attached, 1234, -1, &attached_resources, &attached_finished);
    expect(a == b && first.record == second.record, "fixed seed and detach frame reproduce the stream hashes");
    expect(std::equal(a.begin(), a.begin() + 30, c.begin()), "frames before the detach frame are unchanged");
    expect(a[30] != c[30], "the detach frame changes the stream");
    expect(first_finished && second_finished && !attached_finished,
           "the detached run finishes; the attached run never does");
    expect(first_resources == 0 && attached_resources == 0 && first.live.empty() && attached.live.empty(),
           "every run ends with zero resources");
}

// --- Attachment visibility lifecycle ------------------------------------------
} // namespace particle_render_contracts
